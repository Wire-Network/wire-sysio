/// Contract tests for the OPP envelope dispute vote (sysio.chalg dispute-vote flow).
///
/// Covers the new chalg actions in isolation and against a minimally-bootstrapped OPP stack:
///   * opendispute  -- auth (sysio.msgch), >=3 candidates, no duplicate (outpost,epoch), pauses epoch
///   * votedispute  -- Tier-1 eligibility (sysio.roa::nodeowners), candidate validation, one-vote,
///                     dispute-must-be-open
///   * chkdispute   -- the Tier-1 tally rule: fast path (votes >= floor(N/2)+1 any time) and the
///                     post-deadline relaxation (cast >= Q AND a strict majority of cast votes),
///                     plus the non-resolving waits (sub-quorum, deadline tie). The fast-path resolve
///                     also exercises the winner dispatch (sysio.msgch::resolvedisp) + epoch unpause.
///
/// The full 3-way deliver -> auto-opendispute -> resolve -> slash path (and the slash economics) is
/// exercised end-to-end by the TS flow `flow-batch-operator-slashing`; here disputes are opened
/// directly as sysio.msgch so the chalg logic can be tested without standing up divergent deliveries.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>           // FC_REFLECT_ENUM for opp::types proto enums (mvo serialization)
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/types/types.pb.h>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/sha256.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

namespace {

/// SlugName mvo helper for v6 chain-registry action arguments.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

/// Build an `authority` whose active permission is the account's own active key plus a list of
/// `{actor, sysio.code}` co-signers — lets the listed contracts authorize inline actions as `account`.
authority active_with_code_authors(name account, const std::vector<name>& code_authors) {
   authority a(base_tester::get_public_key(account, "active"));
   a.accounts.push_back(permission_level_weight{{account, config::sysio_code_name}, 1});
   for (const auto& actor : code_authors) {
      a.accounts.push_back(permission_level_weight{{actor, config::sysio_code_name}, 1});
   }
   std::sort(a.accounts.begin(), a.accounts.end(),
      [](const permission_level_weight& l, const permission_level_weight& r) {
         return std::tie(l.permission.actor, l.permission.permission)
              < std::tie(r.permission.actor, r.permission.permission);
      });
   return a;
}

/// Encode a valid OPP Envelope wrapping a single benign attestation for `epoch_index`. Divergent
/// envelopes (distinct checksums) are produced by varying `tag`.
std::vector<char> encode_envelope(uint32_t epoch_index, const std::string& tag) {
   sysio::opp::Envelope env;
   env.set_epoch_index(epoch_index);
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1'775'612'516'983ULL);

   auto* msg     = env.add_messages();
   auto* payload = msg->mutable_payload();
   auto* att     = payload->add_attestations();
   att->set_type(AttestationType::ATTESTATION_TYPE_UNSPECIFIED);  // benign: dispatch is a no-op
   att->set_data(tag);
   att->set_data_size(static_cast<uint32_t>(tag.size()));

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

} // anonymous namespace

class sysio_dispute_tester : public tester {
public:
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto UWRIT_ACCOUNT  = "sysio.uwrit"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto ROA_ACCOUNT    = "sysio.roa"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr uint64_t ROA_NETWORK_GEN = 0;

   sysio_dispute_tester() {
      produce_blocks(2);

      create_accounts({
         CHALG_ACCOUNT, EPOCH_ACCOUNT, MSGCH_ACCOUNT, OPREG_ACCOUNT,
         UWRIT_ACCOUNT, CHAINS_ACCOUNT, BATCHOP
      });
      // Tier-1 voter accounts (node owners) and a Tier-2 account (ineligible voter). Created without
      // a roa policy so regnodeowner's reslimit creation does not collide.
      for (auto v : {"voter1"_n, "voter2"_n, "voter3"_n, "voter4"_n, "tier2"_n, "stranger"_n}) {
         create_account(v, config::system_account_name, false, true, /*include_roa_policy=*/false);
      }
      produce_blocks(2);

      deploy(CHALG_ACCOUNT,  contracts::chalg_wasm(),  contracts::chalg_abi(),  chalg_abi);
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(UWRIT_ACCOUNT,  contracts::uwrit_wasm(),  contracts::uwrit_abi(),  uwrit_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi);

      // sysio.roa is a genesis system account already running this build's code; load its abi.
      load_abi(ROA_ACCOUNT, roa_abi);

      // Deploy + initialize sysio.system (provides setemitcfg/addnodeowner -> nodecount.t1_count).
      set_code(config::system_account_name, contracts::system_wasm());
      set_abi (config::system_account_name, contracts::system_abi().data());
      produce_blocks();
      load_abi(config::system_account_name, system_abi);
      base_tester::push_action(config::system_account_name, "init"_n, config::system_account_name,
                               mvo()("version", 0)("core", std::string("4,SYS")));
      setup_emission_config();

      // Inline-action delegation web (depot/governance analogue):
      //   epoch.active  trusts chalg@code   (epoch::pause/unpause called by chalg)
      //   chalg.active  trusts msgch@code   (opendispute called by msgch) and epoch@code (slashop)
      //   msgch.active  trusts chalg@code   (resolvedisp called by chalg)
      //   opreg.active  trusts chalg/epoch  (slash / recorddel / termcheck)
      grant_code_authors(EPOCH_ACCOUNT, {CHALG_ACCOUNT, MSGCH_ACCOUNT});
      grant_code_authors(CHALG_ACCOUNT, {MSGCH_ACCOUNT, EPOCH_ACCOUNT});
      grant_code_authors(MSGCH_ACCOUNT, {CHALG_ACCOUNT});
      grant_code_authors(OPREG_ACCOUNT, {CHALG_ACCOUNT, EPOCH_ACCOUNT, MSGCH_ACCOUNT});
      produce_blocks();

      bootstrap_opp_stack();
   }

   // ── deploy / abi helpers ─────────────────────────────────────────────────

   void load_abi(name account, abi_serializer& out_ser) {
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed), true);
      out_ser.set_abi(std::move(parsed), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void deploy(name account, std::vector<uint8_t> wasm, std::vector<char> abi, abi_serializer& out_ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      load_abi(account, out_ser);
   }

   void grant_code_authors(name account, const std::vector<name>& code_authors) {
      set_authority(account, config::active_name,
                    active_with_code_authors(account, code_authors), config::owner_name);
   }

   void setup_emission_config() {
      auto cfg = mvo()
         ("t1_allocation", int64_t(7500000000000000))("t2_allocation", int64_t(1000000000000000))
         ("t3_allocation", int64_t(100000000000000))
         ("t1_duration", uint32_t(12u*30u*24u*3600u))("t2_duration", uint32_t(24u*30u*24u*3600u))
         ("t3_duration", uint32_t(36u*30u*24u*3600u))
         ("min_claimable", int64_t(10000000000))
         ("t5_distributable", int64_t(375000000000000000LL))("t5_floor", int64_t(125000000000000000LL))
         ("target_annual_decay_bps", uint16_t(6940))
         ("annual_initial_emission", int64_t(563150000000000LL*365))
         ("annual_max_emission", int64_t(3000000000000000LL*365))
         ("annual_min_emission", int64_t(100000000000000LL*365))
         ("compute_bps", uint16_t(4000))("capex_bps", uint16_t(2000))("governance_bps", uint16_t(1000))
         ("producer_bps", uint16_t(7000))("batch_op_bps", uint16_t(3000))
         ("standby_end_rank", uint32_t(28))
         ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));
      push(config::system_account_name, system_abi, config::system_account_name,
           "setemitcfg"_n, mvo()("cfg", cfg));
      produce_blocks();
   }

   // ── generic action push ──────────────────────────────────────────────────

   action_result push(name contract, abi_serializer& ser, name signer,
                      name action_name, const fc::variant_object& data) {
      try {
         std::string action_type = ser.get_action_type(action_name);
         action act;
         act.account = contract;
         act.name    = action_name;
         act.data    = ser.variant_to_binary(action_type, data,
                        abi_serializer::create_yield_function(abi_serializer_max_time));
         act.authorization = std::vector<permission_level>{{signer, config::active_name}};
         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));
         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         push_transaction(trx);
         // Land each successful action in its own block so repeated identical actions (e.g. a
         // second chkdispute / double-vote) get a distinct TaPoS ref and reach the contract guard
         // rather than being dropped as a duplicate transaction.
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   // ── OPP stack bootstrap: one batch op, one outpost, advance to epoch 1 ────

   void bootstrap_opp_stack() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("epoch_duration_sec", 60)
         ("operators_per_epoch", 1)
         ("batch_operator_minimum_active", 1)
         ("batch_op_groups", 1)
         ("epoch_retention_envelope_log_count", 200)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers", 21)("max_available_batch_ops", 63)("max_available_underwriters", 21)
         ("terminate_prune_delay_ms", 600000)("terminate_max_consecutive_misses", 5)
         ("terminate_max_pct_misses_24h", 5)("terminate_window_ms", uint64_t{24ULL*60*60*1000})
         ("req_prod_collat", fc::variants{})("req_batchop_collat", fc::variants{})
         ("req_uw_collat", fc::variants{})));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", BATCHOP.to_string())("type", OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped", true)));

      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", ChainKind::CHAIN_KIND_EVM)("code", codename_mvo("ETH"))
         ("external_chain_id", 31337)("name", std::string("ethereum-test"))("description", std::string{})));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "schbatchgps"_n, mvo()));
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "advance"_n, mvo()));
      produce_blocks();
   }

   uint64_t eth_code() const { return fc::slug_name{"ETH"}.value; }

   // ── node-owner / nodecount setup ─────────────────────────────────────────

   // sysio.roa is a genesis system account, already activated (network_gen 0) — no activateroa here.

   /// Register `owner` at `tier` in sysio.roa::nodeowners. forcereg -> regnodeowner also inline-bumps
   /// sysio.system::nodecount.t{tier}_count via addnodeowner (emitcfg is set in setup_emission_config),
   /// so chkdispute's N = nodecount.t1_count counts each Tier-1 owner registered here.
   void register_node_owner(name owner, uint8_t tier) {
      BOOST_REQUIRE_EQUAL(success(), push(ROA_ACCOUNT, roa_abi, ROA_ACCOUNT, "forcereg"_n, mvo()
         ("owner", owner.to_string())("tier", tier)));
   }

   // ── dispute action helpers ───────────────────────────────────────────────

   /// One candidate {checksum, operators[]} as an mvo for the opendispute payload.
   static fc::variant candidate(const fc::sha256& checksum, const std::vector<name>& ops) {
      return mvo()("checksum", checksum)("operators", ops);
   }

   action_result opendispute(uint64_t chain_code, uint32_t epoch_index,
                             const std::vector<fc::variant>& candidates, name signer = MSGCH_ACCOUNT) {
      return push(CHALG_ACCOUNT, chalg_abi, signer, "opendispute"_n, mvo()
         ("chain_code", chain_code)("epoch_index", epoch_index)("candidates", candidates));
   }

   action_result votedispute(name owner, uint64_t dispute_id, const fc::sha256& chosen) {
      return push(CHALG_ACCOUNT, chalg_abi, owner, "votedispute"_n, mvo()
         ("owner", owner.to_string())("dispute_id", dispute_id)("chosen_checksum", chosen));
   }

   action_result chkdispute(uint64_t dispute_id, name signer = BATCHOP) {
      return push(CHALG_ACCOUNT, chalg_abi, signer, "chkdispute"_n, mvo()("dispute_id", dispute_id));
   }

   action_result deliver(uint64_t chain_code, const std::vector<char>& data) {
      return push(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "deliver"_n, mvo()
         ("batch_op_name", BATCHOP.to_string())("chain_code", chain_code)("data", data));
   }

   // ── table reads ──────────────────────────────────────────────────────────

   fc::variant get_dispute(uint64_t id) {
      auto data = get_row_by_id(CHALG_ACCOUNT, CHALG_ACCOUNT, "disputes"_n, id);
      return data.empty() ? fc::variant() : chalg_abi.binary_to_variant("dispute_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   bool epoch_paused() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
      if (data.empty()) return false;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["is_paused"].as_bool();
   }

   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
   }

   abi_serializer chalg_abi, epoch_abi, msgch_abi, opreg_abi, uwrit_abi, chains_abi, roa_abi, system_abi;
};

// =============================================================================
//  opendispute
// =============================================================================
BOOST_AUTO_TEST_SUITE(sysio_dispute_tests)

BOOST_FIXTURE_TEST_CASE(opendispute_writes_row_and_pauses, sysio_dispute_tester) { try {
   const uint32_t epoch = current_epoch();
   std::vector<fc::variant> cands{
      candidate(fc::sha256::hash(std::string("a")), {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter1"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter2"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));

   auto d = get_dispute(1);
   BOOST_REQUIRE(!d.is_null());
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", d["status"].as_string());
   BOOST_REQUIRE_EQUAL(eth_code(), d["chain_code"].as_uint64());
   BOOST_REQUIRE_EQUAL(3u, d["candidates"].get_array().size());
   BOOST_REQUIRE(epoch_paused());   // opendispute inline-paused the epoch
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(opendispute_requires_msgch_auth, sysio_dispute_tester) { try {
   std::vector<fc::variant> cands{
      candidate(fc::sha256::hash(std::string("a")), {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter1"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter2"_n}),
   };
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.msgch"),
                       opendispute(eth_code(), current_epoch(), cands, /*signer=*/"voter1"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(opendispute_requires_three_candidates, sysio_dispute_tester) { try {
   std::vector<fc::variant> two{
      candidate(fc::sha256::hash(std::string("a")), {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter1"_n}),
   };
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: a dispute requires at least 3 candidate envelope versions"),
      opendispute(eth_code(), current_epoch(), two));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(opendispute_rejects_duplicate, sysio_dispute_tester) { try {
   const uint32_t epoch = current_epoch();
   std::vector<fc::variant> cands{
      candidate(fc::sha256::hash(std::string("a")), {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter1"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter2"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   produce_blocks();   // distinct TaPoS ref so the retry is not dropped as a duplicate transaction
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: a dispute already exists for this outpost+epoch"),
      opendispute(eth_code(), epoch, cands));
} FC_LOG_AND_RETHROW() }

// =============================================================================
//  votedispute
// =============================================================================

BOOST_FIXTURE_TEST_CASE(votedispute_tier1_accepted, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);

   const uint32_t epoch = current_epoch();
   auto cs_a = fc::sha256::hash(std::string("a"));
   std::vector<fc::variant> cands{
      candidate(cs_a, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter2"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter3"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_a));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votedispute_non_tier1_rejected, sysio_dispute_tester) { try {
   register_node_owner("tier2"_n, 2);   // a Tier-2 owner: registered but ineligible

   const uint32_t epoch = current_epoch();
   auto cs_a = fc::sha256::hash(std::string("a"));
   std::vector<fc::variant> cands{
      candidate(cs_a, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter2"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter3"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: only tier-1 node owners may vote on a dispute"),
      votedispute("tier2"_n, 1, cs_a));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votedispute_unregistered_rejected, sysio_dispute_tester) { try {
   const uint32_t epoch = current_epoch();
   auto cs_a = fc::sha256::hash(std::string("a"));
   std::vector<fc::variant> cands{
      candidate(cs_a, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter2"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter3"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: voter is not a registered node owner"),
      votedispute("stranger"_n, 1, cs_a));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votedispute_non_candidate_rejected, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   const uint32_t epoch = current_epoch();
   std::vector<fc::variant> cands{
      candidate(fc::sha256::hash(std::string("a")), {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter2"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter3"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: chosen checksum is not a candidate in this dispute"),
      votedispute("voter1"_n, 1, fc::sha256::hash(std::string("not-a-candidate"))));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votedispute_double_vote_rejected, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   const uint32_t epoch = current_epoch();
   auto cs_a = fc::sha256::hash(std::string("a"));
   std::vector<fc::variant> cands{
      candidate(cs_a, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter2"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter3"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_a));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: owner has already voted in this dispute"),
      votedispute("voter1"_n, 1, cs_a));
} FC_LOG_AND_RETHROW() }

// =============================================================================
//  chkdispute tally
// =============================================================================

// Fast path: with N=3 Tier-1 owners (Q=2), two votes for the delivered winning checksum resolve the
// dispute any time. Exercises winner dispatch (resolvedisp) + epoch unpause.
BOOST_FIXTURE_TEST_CASE(chkdispute_fast_path_resolves, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   register_node_owner("voter2"_n, 1);
   register_node_owner("voter3"_n, 1);   // N = 3, Q = 2

   const uint32_t epoch = current_epoch();

   auto winner_bytes = encode_envelope(epoch, "winner");
   auto cs_win = fc::sha256::hash(winner_bytes.data(), winner_bytes.size());

   std::vector<fc::variant> cands{
      candidate(cs_win, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("loser-1")), {"voter4"_n}),
      candidate(fc::sha256::hash(std::string("loser-2")), {"voter2"_n}),
   };
   // Open the dispute FIRST: the subsequent deliver then hits evalcons's open-dispute gate and
   // retains raw_data (no instant single-op consensus), so resolvedisp has the bytes to re-dispatch.
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE(epoch_paused());
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code(), winner_bytes));

   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_win));
   // One vote: below Q -> chkdispute does not resolve.
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", get_dispute(1)["status"].as_string());

   BOOST_REQUIRE_EQUAL(success(), votedispute("voter2"_n, 1, cs_win));
   // Two votes (= Q) for the winner -> resolves, dispatches, unpauses.
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));

   auto d = get_dispute(1);
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_RESOLVED", d["status"].as_string());
   BOOST_REQUIRE_EQUAL(cs_win.str(), d["winning_checksum"].as_string());
   BOOST_REQUIRE(!epoch_paused());
} FC_LOG_AND_RETHROW() }

// Sub-quorum: with N=3 (Q=2), a single vote never resolves before the deadline.
BOOST_FIXTURE_TEST_CASE(chkdispute_sub_quorum_waits, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   register_node_owner("voter2"_n, 1);
   register_node_owner("voter3"_n, 1);

   const uint32_t epoch = current_epoch();
   auto cs_a = fc::sha256::hash(std::string("a"));
   std::vector<fc::variant> cands{
      candidate(cs_a, {BATCHOP}),
      candidate(fc::sha256::hash(std::string("b")), {"voter4"_n}),
      candidate(fc::sha256::hash(std::string("c")), {"voter2"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_a));
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", get_dispute(1)["status"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

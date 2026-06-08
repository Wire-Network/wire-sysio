/// Contract tests for the OPP envelope dispute vote (sysio.chalg dispute-vote flow).
///
/// Covers the new chalg actions in isolation and against a minimally-bootstrapped OPP stack:
///   * opendispute  -- auth (sysio.msgch), >=3 candidates, no duplicate (outpost,epoch), pauses epoch
///   * votedispute  -- Tier-1 eligibility (sysio.roa::nodeowners), candidate validation, one-vote,
///                     dispute-must-be-open
///   * chkdispute   -- the Tier-1 tally rule: fast path (votes >= floor(N/2)+1 any time) and the
///                     post-deadline relaxation (cast >= Q AND a strict majority of cast votes),
///                     plus the non-resolving waits (sub-quorum before the deadline, tie after it).
///                     The fast-path and post-deadline resolves both exercise the winner dispatch
///                     (sysio.msgch::resolvedisp) + epoch unpause.
///   * slash gate   -- the ACTIVE-only scheduling filter (shared by sysio.epoch::advance's
///                     window-slide and schbatchgps) drops a SLASHED batch op from the eligible
///                     pool, so it is never re-placed where advance could attempt a second
///                     (aborting) slash of it (the no-cross-epoch-double-slash invariant). Also
///                     covers the widened slashop auth (callable by sysio.epoch) and opreg's
///                     already-slashed guard.
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

   /// Re-fire evalcons directly (as sysio.msgch, its own auth) for one (outpost, epoch) -- the same
   /// re-evaluation a post-quorum late delivery drives through deliver.
   action_result evalcons(uint64_t chain_code, uint32_t epoch_index) {
      return push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "evalcons"_n, mvo()
         ("chain_code", chain_code)("epoch_index", epoch_index));
   }

   // ── table reads ──────────────────────────────────────────────────────────

   /// `outpcons.epoch_index` for `chain_code` (the durable consensus marker), or -1 if no row.
   int64_t outpcons_epoch(uint64_t chain_code) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outpcons"_n, chain_code);
      if (data.empty()) return -1;
      auto v = msgch_abi.binary_to_variant("outpost_consensus_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["epoch_index"].as<uint32_t>();
   }

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

   /// Members of the active batch-op group (epoch_state.batch_op_groups[current_batch_op_group]).
   std::vector<name> current_group() {
      std::vector<name> members;
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
      if (data.empty()) return members;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      const uint64_t cur = v["current_batch_op_group"].as_uint64();
      const auto& groups = v["batch_op_groups"].get_array();
      if (cur >= groups.size()) return members;
      for (const auto& m : groups[cur].get_array()) members.push_back(m.as<name>());
      return members;
   }

   static bool group_contains(const std::vector<name>& g, name who) {
      return std::find(g.begin(), g.end(), who) != g.end();
   }

   /// opreg operator status as its enum spelling (e.g. "OPERATOR_STATUS_SLASHED"), or "" if absent.
   std::string operator_status(name op) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, op.value);
      if (data.empty()) return {};
      auto v = opreg_abi.binary_to_variant("operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["status"].as_string();
   }

   /// Slash `op` through sysio.chalg::slashop, authorised as `signer` (default sysio.epoch, the
   /// widened auth path). Lands in its own block like the other action helpers.
   action_result slashop(name op, name signer = EPOCH_ACCOUNT) {
      return push(CHALG_ACCOUNT, chalg_abi, signer, "slashop"_n, mvo()
         ("operator_acct", op.to_string())("reason", std::string("test non-canonical delivery")));
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

// Post-deadline relaxation: with N=4 (Q=3), a checksum that holds a STRICT MAJORITY of the cast votes
// (but fewer than Q, so the fast path never fires) resolves only AFTER the 24h deadline, provided a
// quorum of votes was cast (cast >= Q). Two of three cast votes -> resolves post-deadline; also
// exercises winner dispatch (resolvedisp) + unpause on the relaxed path.
BOOST_FIXTURE_TEST_CASE(chkdispute_post_deadline_majority_resolves, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   register_node_owner("voter2"_n, 1);
   register_node_owner("voter3"_n, 1);
   register_node_owner("voter4"_n, 1);   // N = 4, Q = 3

   const uint32_t epoch = current_epoch();

   auto winner_bytes = encode_envelope(epoch, "x");
   auto cs_x = fc::sha256::hash(winner_bytes.data(), winner_bytes.size());
   auto cs_y = fc::sha256::hash(std::string("y"));
   auto cs_z = fc::sha256::hash(std::string("z"));

   std::vector<fc::variant> cands{
      candidate(cs_x, {BATCHOP}),
      candidate(cs_y, {"voter1"_n}),
      candidate(cs_z, {"voter2"_n}),
   };
   // Open first so the subsequent deliver hits evalcons's open-dispute gate and retains raw_data,
   // giving resolvedisp the winning bytes to re-dispatch on resolution.
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code(), winner_bytes));

   // cast = 3 (x=2, y=1). x has a strict majority of cast but only 2 < Q=3 -> no fast path.
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_x));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter2"_n, 1, cs_x));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter3"_n, 1, cs_y));

   // Before the deadline the relaxed rule does not apply -> still open.
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", get_dispute(1)["status"].as_string());

   // Cross the 24h voting window, then crank: cast(3) >= Q(3) AND 2*votes_x(4) > cast(3) -> x wins.
   produce_block(fc::seconds(24 * 60 * 60 + 1));
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));

   auto d = get_dispute(1);
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_RESOLVED", d["status"].as_string());
   BOOST_REQUIRE_EQUAL(cs_x.str(), d["winning_checksum"].as_string());
   BOOST_REQUIRE(!epoch_paused());
} FC_LOG_AND_RETHROW() }

// Deadline tie: with N=4 (Q=3), an even split of all cast votes (2 vs 2) has a quorum but NO strict
// majority of cast, so even after the deadline the dispute does not resolve (no plurality / tie-break).
BOOST_FIXTURE_TEST_CASE(chkdispute_deadline_tie_waits, sysio_dispute_tester) { try {
   register_node_owner("voter1"_n, 1);
   register_node_owner("voter2"_n, 1);
   register_node_owner("voter3"_n, 1);
   register_node_owner("voter4"_n, 1);   // N = 4, Q = 3

   const uint32_t epoch = current_epoch();
   auto cs_x = fc::sha256::hash(std::string("x"));
   auto cs_y = fc::sha256::hash(std::string("y"));
   auto cs_z = fc::sha256::hash(std::string("z"));
   std::vector<fc::variant> cands{
      candidate(cs_x, {BATCHOP}),
      candidate(cs_y, {"voter1"_n}),
      candidate(cs_z, {"voter2"_n}),
   };
   BOOST_REQUIRE_EQUAL(success(), opendispute(eth_code(), epoch, cands));

   // cast = 4, split 2 (x) / 2 (y): 2*2 == cast, not a strict majority of cast.
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter1"_n, 1, cs_x));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter2"_n, 1, cs_x));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter3"_n, 1, cs_y));
   BOOST_REQUIRE_EQUAL(success(), votedispute("voter4"_n, 1, cs_y));

   produce_block(fc::seconds(24 * 60 * 60 + 1));
   BOOST_REQUIRE_EQUAL(success(), chkdispute(1));
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", get_dispute(1)["status"].as_string());
   BOOST_REQUIRE(epoch_paused());   // unresolved dispute keeps the epoch paused
} FC_LOG_AND_RETHROW() }

// No cross-epoch double slash: the ACTIVE-only scheduling filter -- shared verbatim by
// sysio.epoch::advance's window-slide and schbatchgps -- drops a SLASHED batch op from the eligible
// pool, so a slashed op is never re-placed into a future group where advance could attempt a second
// (aborting) slash of it. This exercises that filter directly via schbatchgps (advance's own slide is
// additionally gated by emissions readiness, out of scope here), plus the widened slashop auth
// (callable by sysio.epoch) and opreg's already-slashed guard. The full delivery-driven slash through
// advance is covered end-to-end by the TS flow flow-batch-operator-slashing.
BOOST_FIXTURE_TEST_CASE(slashed_op_excluded_from_scheduling_no_double_slash, sysio_dispute_tester) { try {
   constexpr auto BATCHOP_B = "batchop.b"_n;
   constexpr auto BATCHOP_C = "batchop.c"_n;
   for (auto op : {BATCHOP_B, BATCHOP_C})
      create_account(op, config::system_account_name, false, true, /*include_roa_policy=*/false);
   auto regop = [&](name op) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", op.to_string())("type", OperatorType::OPERATOR_TYPE_BATCH)("is_bootstrapped", true));
   };
   BOOST_REQUIRE_EQUAL(success(), regop(BATCHOP_B));

   // Single 2-member group; with a and b the only ACTIVE batch ops, both are placed.
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("epoch_duration_sec", 60)("operators_per_epoch", 2)("batch_operator_minimum_active", 2)
      ("batch_op_groups", 1)("epoch_retention_envelope_log_count", 200)));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "schbatchgps"_n, mvo()));
   auto g0 = current_group();
   BOOST_REQUIRE(group_contains(g0, BATCHOP));
   BOOST_REQUIRE(group_contains(g0, BATCHOP_B));

   // Slash batchop.b as sysio.epoch (the widened auth path).
   BOOST_REQUIRE_EQUAL(success(), slashop(BATCHOP_B));
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_SLASHED", operator_status(BATCHOP_B));

   // opreg aborts a second slash -- exactly the abort the scheduling exclusion below must prevent
   // advance from ever triggering.
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: operator already slashed"),
                       slashop(BATCHOP_B));

   // Bring c online so two ACTIVE ops remain, then re-schedule: the filter keeps the live ops
   // (a, c) and drops the slashed one (b).
   BOOST_REQUIRE_EQUAL(success(), regop(BATCHOP_C));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "schbatchgps"_n, mvo()));
   auto g1 = current_group();
   BOOST_REQUIRE(!group_contains(g1, BATCHOP_B));   // slashed -> never re-enters a group
   BOOST_REQUIRE(group_contains(g1, BATCHOP));
   BOOST_REQUIRE(group_contains(g1, BATCHOP_C));
} FC_LOG_AND_RETHROW() }

// Regression: after consensus, the winning delivery rows have raw_data cleared and the inbound
// message row is erased, so the idempotency guard must key off the DURABLE outpcons row. A
// post-quorum re-fire of evalcons (the path a late delivery takes through deliver) must be a benign
// no-op, not an empty-envelope decode that aborts -- which would also revert the late delivery and
// drop its envelope row, leaving epoch::advance nothing to classify/slash. Pre-fix this threw
// "failed to decode inbound OPP Envelope".
BOOST_FIXTURE_TEST_CASE(post_consensus_evalcons_refire_is_benign_noop, sysio_dispute_tester) { try {
   const uint32_t epoch = current_epoch();
   auto env = encode_envelope(epoch, "canonical");

   // 1-op group: the first deliver reaches all-delivered consensus, dispatches, clears raw_data,
   // erases the inbound message row, and records outpcons for this epoch.
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code(), env));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(epoch), outpcons_epoch(eth_code()));

   // Re-fire evalcons for the same (outpost, epoch): the winning checksum's raw_data is now empty.
   // The durable guard short-circuits it to a no-op instead of decoding empty bytes and aborting.
   BOOST_REQUIRE_EQUAL(success(), evalcons(eth_code(), epoch));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(epoch), outpcons_epoch(eth_code()));  // still recorded
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

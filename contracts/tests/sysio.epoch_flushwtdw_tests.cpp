/// Cross-contract tests for the `sysio.epoch::tickeconomy` ↔ `sysio.opreg::flushwtdw` integration:
/// a withdrawal matures on the economic clock AND waits for the consensus guard of each chain it
/// depends on: the chain closes `WITHDRAW_WAIT_EPOCHS` epochs after the request, or its current epoch is
/// decided (its winner accepted, no dispute open on it).
///
/// Data model: identity is now slug_name-keyed across opreg / chains.
/// The fixture deploys `sysio.chains` so the chain-of-record exists and
/// uses `regchain` (replacing the v5 `regoutpost`).

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/sha256.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "contracts.hpp"
#include "contract_test_support.hpp"
// Inbound envelopes must carry spec-derived semantic headers, or deliver refuses them.
#include "opp_envelope_oracle.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

class sysio_epoch_flushwtdw_tester : public tester {
public:
   static constexpr auto SYSIO_ACCOUNT  = config::system_account_name;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto DCLAIM_ACCOUNT = "sysio.dclaim"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto UWRIT_OP       = "uwrit.alice"_n;
   static constexpr auto UWRIT_OP_B     = "uwrit.bob"_n;
   /// `sysio.opreg::WITHDRAW_WAIT_ECONOMIC_EPOCHS`: economic epochs between a request and its release.
   static constexpr uint32_t WITHDRAW_WAIT_ECONOMIC_EPOCHS = 2;
   /// `sysio.opreg::WITHDRAW_WAIT_EPOCHS`: consensus epochs each waited-on chain closes before release.
   static constexpr uint32_t WITHDRAW_WAIT_EPOCHS = 2;
   /// `sysio.opreg::MAX_WTDW_FLUSH_PER_EPOCH`: rows one flush releases at most.
   static constexpr uint32_t MAX_WTDW_FLUSH_PER_EPOCH = 32;
   /// `sysio.opreg::MAX_WTDW_HELD_SKIPS_PER_FLUSH`: held rows one flush skips at most.
   static constexpr uint32_t MAX_WTDW_HELD_SKIPS_PER_FLUSH = 64;
   static constexpr auto UWRIT_OP_C     = "uwrit.carl"_n;
   static constexpr auto BATCHOP_ETH    = "batchop.eth"_n;    ///< a batch operator serving ETH alone
   static constexpr auto BATCHOP_SOL    = "batchop.sol"_n;    ///< a batch operator serving SOL alone
   static constexpr auto BATCHOP_BOTH   = "batchop.both"_n;   ///< a batch operator serving SOL and ETH
   static constexpr auto BATCHOP_PEER   = "batchop.peer"_n;   ///< a bootstrapped batch operator serving ETH
   /// Collateral a test operator bonds, and the part of it a test withdraws.
   static constexpr uint64_t OPERATOR_BOND       = 1'000'000;
   static constexpr uint64_t OPERATOR_WITHDRAWAL =   400'000;
   /// The `tickeconomy` refusal before the next economic boundary has passed.
   static constexpr std::string_view ECONOMIC_BOUNDARY_NOT_PASSED = "economic epoch boundary has not passed";

   /// Both clocks run at this duration, so it must be >= the economic clock's
   /// MIN_ECONOMIC_EPOCH_DURATION_SEC (60), the higher of the two floors.
   /// Each advance_one_epoch() crosses the boundary by producing
   /// `EPOCH_DURATION_SEC * 2 + 1` half-second blocks; empty-block production
   /// in the tester is cheap, so the floor doesn't materially slow the suite.
   static constexpr uint32_t EPOCH_DURATION_SEC = 60;

   sysio_epoch_flushwtdw_tester() {
      produce_blocks(2);

      // Create all sysio.* accounts BEFORE deploying sysio.system. The
      // BIOS-active create_account path leaves sysio.* accounts with
      // ram_bytes=-1 (unlimited). Once sysio.system is on the system
      // account, its `newaccount` handler transfers only 1144 bytes of
      // RAM to new accounts — too small for the sysio.token deploy
      // (~100KB). Doing creation first sidesteps that.
      //
      // sysio.dclaim / sysio.gov / sysio.ops are payepoch destinations for
      // the capital / governance / capex emission buckets; without them
      // the first pay epoch's WIRE transfer fails with "to account does
      // not exist". sysio.authex is auto-created by base_tester (see
      // tester.cpp), so it must NOT be re-listed here — duplicating it
      // throws `account_name_exists_exception`.
      create_accounts({
         TOKEN_ACCOUNT, EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT,
         CHALG_ACCOUNT, CHAINS_ACCOUNT, BATCHOP, UWRIT_OP,
         UWRIT_OP_B, UWRIT_OP_C, BATCHOP_ETH, BATCHOP_SOL, BATCHOP_BOTH, BATCHOP_PEER,
         DCLAIM_ACCOUNT, "sysio.gov"_n, "sysio.ops"_n
      });
      produce_blocks(2);

      // Deploy OPP contracts. These are privileged because epoch::advance
      // sends inline actions to opreg with `permission_level{epoch, owner}`,
      // which require_auth(epoch) accepts only when epoch is privileged.
      // epoch::advance also iterates sysio.chains::chains, so it must be
      // deployed for that cross-contract read to resolve.
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi);
      // A batch operator serves a chain only with its authex link for the chain's kind.
      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(), contracts::authex_abi(), authex_abi);
      deploy(DCLAIM_ACCOUNT, contracts::dclaim_wasm(), contracts::dclaim_abi(), dclaim_abi);
      produce_blocks(1);
      // A dispute opened on a chain's epoch holds the withdrawals waiting on it until its winner is accepted.
      deploy(CHALG_ACCOUNT,  contracts::chalg_wasm(),  contracts::chalg_abi(),  chalg_abi);
      produce_blocks(1);

      // sysio.system on the system account replaces the default BIOS contract.
      // sysio.epoch::advance reads sysio.system::emitcfg / t5state through its
      // emissions readiness gate; without these tables initialized the gate
      // returns CONFIG_MISSING and advance is a silent no-op, so wtdw rows
      // never mature. Deploy + init here, then push setemitcfg + initt5 in
      // bootstrap_for_flushwtdw() so each test starts from a clean gate-pass.
      set_code(SYSIO_ACCOUNT, contracts::system_wasm());
      set_abi(SYSIO_ACCOUNT, contracts::system_abi().data());
      produce_blocks(1);
      load_abi(SYSIO_ACCOUNT, sysio_abi);

      // The system contract's `init` action sets the core symbol used by
      // legacy stake/rex flows and asserts on a fixed "SYS" code. Emissions
      // hardcodes WIRE (9 decimals) independently of this argument; the
      // wtdw tests never touch stake/rex, so the value here is decorative.
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT,
         "init"_n, mvo()("version", 0)("core", "4,SYS")));

      // WIRE supply — the emissions gate's pay-epoch branch checks sysio's
      // WIRE balance against the period emission; issue plenty so every
      // test clears that gate.
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, TOKEN_ACCOUNT,
         "create"_n, mvo()
            ("issuer", SYSIO_ACCOUNT)
            ("maximum_supply", "1000000000.000000000 WIRE")));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, SYSIO_ACCOUNT,
         "issue"_n, mvo()
            ("to", SYSIO_ACCOUNT)
            ("quantity", "1000000000.000000000 WIRE")
            ("memo", "test bootstrap")));

      produce_blocks();
   }

   /// Load an ABI from a deployed account into the given serializer.
   /// Companion to `deploy()` for contracts that base_tester deploys for us
   /// (sysio.system goes through `set_code`/`set_abi` directly because the
   /// system account already exists with the BIOS contract).
   void load_abi(name account, abi_serializer& out_ser) {
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed_abi), true);
      out_ser.set_abi(std::move(parsed_abi),
                      fixture_abi_yield_function());
   }

   void deploy(name account, std::vector<uint8_t> wasm, std::vector<char> abi,
               abi_serializer& out_ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed_abi), true);
      out_ser.set_abi(std::move(parsed_abi),
                      fixture_abi_yield_function());
   }

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }

   /// Push an action against any deployed contract.
   action_result push(name contract, abi_serializer& ser, name signer,
                      name action_name, const fc::variant_object& data) {
      return sysio_system::test_support::push_contract_action(
         *this, contract, ser, signer, action_name, data);
   }

   /// Push setemitcfg with values mirroring `sysio_emissions_tester`'s
   /// defaults — `pay_cadence_epochs=1` means every epoch is a pay epoch
   /// and goes through the gate's `sysio_balance >= period_emission`
   /// check, which is why the constructor issues 1B WIRE to sysio. The
   /// numeric constants here track contracts/tests/emissions_tests.cpp;
   /// the gate only cares that `t5_distributable > t5_floor` and that the
   /// computed emission stays within the balance, so the exact values
   /// don't affect what the wtdw tests verify.
   action_result setemitcfg_defaults() {
      constexpr uint32_t SECONDS_PER_MONTH = 30u * 24u * 60u * 60u;
      return push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT, "setemitcfg"_n,
         mvo()("cfg", mvo()
            ("t1_allocation",          int64_t(7'500'000'000'000'000LL))
            ("t2_allocation",          int64_t(1'000'000'000'000'000LL))
            ("t3_allocation",          int64_t(  100'000'000'000'000LL))
            ("t1_duration",            12u * SECONDS_PER_MONTH)
            ("t2_duration",            24u * SECONDS_PER_MONTH)
            ("t3_duration",            36u * SECONDS_PER_MONTH)
            ("min_claimable",          int64_t(10'000'000'000LL))
            ("t5_distributable",       int64_t(375'000'000'000'000'000LL))
            ("t5_floor",               int64_t(125'000'000'000'000'000LL))
            ("target_annual_decay_bps", uint16_t(6940))
            ("annual_initial_emission", int64_t(563'150'000'000'000LL) * 365)
            ("annual_max_emission",     int64_t(3'000'000'000'000'000LL) * 365)
            ("annual_min_emission",     int64_t(100'000'000'000'000LL) * 365)
            ("compute_bps",            uint16_t(4000))
            ("capex_bps",              uint16_t(2000))
            ("governance_bps",         uint16_t(1000))
            ("producer_bps",           uint16_t(7000))
            ("batch_op_bps",           uint16_t(3000))
            ("standby_end_rank",       uint32_t(28))("standby_bps", uint16_t(800))
            ("epoch_log_retention_count", uint32_t(8640))
            ("pay_cadence_epochs",     uint16_t(1))));
   }

   /// The outpost chains the fixture registers, each with its own consensus epoch.
   static constexpr std::array<std::string_view, 2> CHAINS{"SOL", "ETH"};
   static constexpr std::string_view SOL_CHAIN = CHAINS[0];
   static constexpr std::string_view ETH_CHAIN = CHAINS[1];

   using outpost = sysio_system::test_support::served_chains::outpost;
   static constexpr outpost SOL_OUTPOST{SOL_CHAIN, ChainKind::CHAIN_KIND_SVM};
   static constexpr outpost ETH_OUTPOST{ETH_CHAIN, ChainKind::CHAIN_KIND_EVM};

   /// One-shot bootstrap: each chain's epoch config + opreg config + a bootstrapped
   /// batch op + a pending underwriter + sysio.chains rows.
   void bootstrap_for_flushwtdw() {
      for (const std::string_view chain : CHAINS) {
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
            "setconfig"_n, mvo()
               ("chain_code",                          chain)
               ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
               ("traffic_wait_sec",                    0)
               ("operators_per_epoch",                 1)
               ("batch_operator_minimum_active",       1)
               ("batch_op_groups",                     1)
               ("epoch_retention_envelope_log_count",  200)));
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "seteconomy"_n, mvo()
         ("epoch_duration_sec", EPOCH_DURATION_SEC)("start_time", fc::time_point_sec(control->head().block_time()))));

      // Emissions readiness gate prerequisites — without these tables the
      // gate at sysio.epoch::advance returns CONFIG_MISSING and every
      // advance is a silent no-op.
      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT,
         "initt5"_n, mvo()));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("terminate_prune_delay_ms",         600000)
            ("terminate_max_consecutive_misses", 5)
            ("terminate_max_pct_misses_24h",     5)
            ("terminate_min_pct_samples",        20)
            ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
            ("req_prod_collat",                  fc::variants{})
            ("req_batchop_collat",               fc::variants{})
            ("req_uw_collat",                    fc::variants{})));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          BATCHOP.to_string())
            ("type",             OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped",  true)));

      BOOST_REQUIRE_EQUAL(success(), regunderwriter(UWRIT_OP));

      // Register a SOLANA-class chain first to soak up the first slot, then
      // an Ethereum chain to host the deposits the rest of the test exercises.
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_SVM)
            ("code",              "SOL")
            ("external_chain_id", 1)
            ("name",              std::string("solana-test"))
            ("description",       std::string{})
            ("outpost", sysio_system::test_support::no_outpost_mvo())));
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_EVM)
            ("code",              "ETH")
            ("external_chain_id", 31337)
            ("name",              std::string("ethereum-test"))
            ("description",       std::string{})
            ("outpost", sysio_system::test_support::no_outpost_mvo())));

      sysio_system::test_support::served_chains::serve(*this, {BATCHOP}, {SOL_OUTPOST, ETH_OUTPOST});

      // Each chain's schedule window, then its genesis advance (sysio.epoch's authority stands in for msgch's).
      for (const std::string_view chain : CHAINS) {
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
            "schbatchgps"_n, mvo()("chain_code", chain)));
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
            "advance"_n, mvo()("chain_code", chain)));
      }
      produce_blocks();

      // Open economic epoch 1, as a relay's first crank after the genesis advance does, so a request
      // made now finds the crank current.
      tick_economy();
   }

   /// Step the wall clock past both clocks' next boundary.
   void cross_epoch_boundary() {
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
   }

   /// Close every chain's current consensus epoch, as sysio.msgch's chkcons would.
   void advance_consensus() {
      for (const std::string_view chain : CHAINS) advance_chain(chain);
   }

   /// Close `chain`'s current consensus epoch alone: record traffic since its epoch start
   /// (`epoch_state::mark_traffic`), which makes it due once its minimum has passed, and advance it.
   void advance_chain(std::string_view chain) {
      sysio_system::test_support::epoch_state::mark_traffic(*this, epoch_abi, chain);
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT, "advance"_n,
                                          mvo()("chain_code", chain)));
      produce_blocks();
   }

   /// Open the next economic epoch; its tick flushes matured withdrawals.
   void tick_economy() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "tickeconomy"_n,
         mvo()("epoch_index", economic_epoch() + 1)));
      produce_blocks();
   }

   /// Cross a boundary and step both clocks once.
   void advance_one_epoch() {
      cross_epoch_boundary();
      advance_consensus();
      tick_economy();
   }

   /// Close `epochs` consensus epochs of `chain` alone, crossing a boundary before each.
   void close_epochs(std::string_view chain, uint32_t epochs) {
      for (uint32_t i = 0; i < epochs; ++i) {
         const uint32_t before = current_epoch(chain);
         cross_epoch_boundary();
         advance_chain(chain);
         BOOST_REQUIRE_EQUAL(before + 1, current_epoch(chain));
      }
   }

   /// Tick the economic clock up to `epoch_index`, crossing a boundary whenever the next one has not passed.
   void tick_economy_to(uint32_t epoch_index) {
      while (economic_epoch() < epoch_index) {
         const action_result result = push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "tickeconomy"_n,
                                           mvo()("epoch_index", economic_epoch() + 1));
         if (result == success()) {
            produce_blocks();
            continue;
         }
         BOOST_REQUIRE(result.find(ECONOMIC_BOUNDARY_NOT_PASSED) != std::string::npos);
         cross_epoch_boundary();
      }
   }

   /// Register `account` as a non-bootstrapped batch operator serving `outposts` and bond `amount` WIRE.
   void register_serving_batch_operator(name account, const std::vector<outpost>& outposts, uint64_t amount) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account",          account.to_string())
         ("type",             OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped",  false)));
      sysio_system::test_support::served_chains::serve(*this, {account}, outposts);
      BOOST_REQUIRE_EQUAL(success(), fund_and_bond(account, "WIRE", "WIRE", amount));
      produce_blocks();
   }

   action_result pause_chain(std::string_view chain) {
      return push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, "pause"_n, mvo()("chain_code", chain));
   }

   action_result unpause_chain(std::string_view chain) {
      return push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, "unpause"_n, mvo()("chain_code", chain));
   }

   uint32_t economic_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "economystate"_n, "economystate"_n);
      if (data.empty()) return 0;
      return epoch_abi.binary_to_variant("economic_state", data, fixture_abi_yield_function())
         ["current_epoch_index"].as<uint32_t>();
   }

   /// Whether `chain`'s schedule window seats `account` in any group.
   bool seated(name account, std::string_view chain) {
      const auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, fc::slug_name{chain}.value);
      BOOST_REQUIRE(!data.empty());
      const auto state = epoch_abi.binary_to_variant("epoch_state", data, fixture_abi_yield_function());
      for (const auto& group : state["batch_op_groups"].get_array()) {
         for (const auto& member : group.get_array()) {
            if (name{member.as_string()} == account) return true;
         }
      }
      return false;
   }

   /// `chain`'s current consensus epoch index (0 before its genesis advance).
   uint32_t current_epoch(std::string_view chain = CHAINS.front()) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, fc::slug_name{chain}.value);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         fixture_abi_yield_function());
      return v["current_epoch_index"].as<uint32_t>();
   }

   /// Register a non-bootstrapped underwriter through opreg's privileged path.
   action_result regunderwriter(name account) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account",          account.to_string())
         ("type",             OperatorType::OPERATOR_TYPE_UNDERWRITER)
         ("is_bootstrapped",  false));
   }

   action_result slash(name account, std::string reason) {
      return push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n, mvo()
         ("account", account.to_string())
         ("reason",  reason));
   }

   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                     "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data,
         fixture_abi_yield_function());
   }

   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "withdraw_request", data,
         fixture_abi_yield_function());
   }

   /// Each chain withdrawal `request_id` waits on, by code, with the epoch index it waits for.
   std::map<std::string, uint32_t> consensus_waits(uint64_t request_id) {
      const fc::variant row = get_wtdw(request_id);
      BOOST_REQUIRE(!row.is_null());
      std::map<std::string, uint32_t> waits;
      for (const fc::variant& wait : row["consensus_waits"].get_array()) {
         waits[wait["chain_code"].as<fc::slug_name>().to_string()] = wait["eligible_at_epoch"].as<uint32_t>();
      }
      return waits;
   }

   /// The waits a request made now takes on `chains`: each one's current index + WITHDRAW_WAIT_EPOCHS.
   std::map<std::string, uint32_t> waits_from_now(std::initializer_list<std::string_view> chains) {
      std::map<std::string, uint32_t> waits;
      for (const std::string_view chain : chains) waits[std::string{chain}] = current_epoch(chain) + WITHDRAW_WAIT_EPOCHS;
      return waits;
   }

   // ---- Consensus on a chain's epoch ----

   /// Where an outpost chain's inbound stream stands: the next delivery continues from the last accepted
   /// envelope's digest and message id (raw 32-byte strings, empty at stream genesis).
   struct inbound_stream_tip {
      std::string envelope_digest;
      std::string message_id;
   };

   /// Each chain's inbound stream tip, by code.
   std::map<std::string, inbound_stream_tip> inbound_tips;

   /// Timestamp, in ms, of every test delivery's envelope and message.
   static constexpr uint64_t DELIVERY_TIMESTAMP_MS = 1'775'612'516'983ULL;

   /// Payload of a test delivery's one attestation; another payload makes a divergent envelope.
   static constexpr std::string_view DELIVERY_PAYLOAD = "withdrawal guard";

   /// An envelope for `chain`'s current epoch, chained from its inbound tip, carrying one attestation whose
   /// type dispatch ignores and whose data is `attestation_data`.
   std::vector<char> next_delivery(std::string_view chain, std::string_view attestation_data = DELIVERY_PAYLOAD) {
      const inbound_stream_tip& tip = inbound_tips[std::string{chain}];
      sysio::opp::Envelope      env;
      env.set_epoch_index(current_epoch(chain));
      env.set_epoch_envelope_index(1);
      env.set_epoch_timestamp(DELIVERY_TIMESTAMP_MS);
      if (!tip.envelope_digest.empty()) env.set_previous_envelope_hash(tip.envelope_digest);
      sysio::opp::MessagePayload* payload = env.add_messages()->mutable_payload();
      payload->set_version(1);
      sysio::opp::AttestationEntry* attestation = payload->add_attestations();
      const std::string data{attestation_data};
      attestation->set_type(AttestationType::ATTESTATION_TYPE_UNSPECIFIED);
      attestation->set_data(data);
      attestation->set_data_size(static_cast<uint32_t>(data.size()));
      oracle::finalize_header(*env.mutable_messages(0), tip.message_id, DELIVERY_TIMESTAMP_MS);
      std::vector<char> out(env.ByteSizeLong());
      BOOST_REQUIRE(env.SerializeToArray(out.data(), static_cast<int>(out.size())));
      return out;
   }

   /// `op`'s delivery of `envelope` to `chain`.
   action_result deliver(std::string_view chain, const std::vector<char>& envelope, name op = BATCHOP) {
      return push(MSGCH_ACCOUNT, msgch_abi, op, "deliver"_n, mvo()
         ("batch_op_name", op)("chain_code", fc::slug_name{chain}.value)("data", envelope));
   }

   /// Move `chain`'s inbound tip past `envelope`, its accepted winner.
   void accept(std::string_view chain, const std::vector<char>& envelope) {
      sysio::opp::Envelope env;
      BOOST_REQUIRE(env.ParseFromArray(envelope.data(), static_cast<int>(envelope.size())));
      inbound_tips[std::string{chain}] = {oracle::digest_bytes(oracle::epoch_digest(env)),
                                          env.messages(0).header().message_id()};
   }

   /// Decide `chain`'s current epoch: BATCHOP, the whole of its one-member group, delivers the next envelope,
   /// a strict majority at once.
   void decide_epoch(std::string_view chain) {
      const std::vector<char> envelope = next_delivery(chain);
      BOOST_REQUIRE_EQUAL(success(), deliver(chain, envelope));
      produce_blocks();
      BOOST_REQUIRE(epoch_decided(chain));
      accept(chain, envelope);
   }

   /// Whether `chain`'s `outpcons` row records an accepted winner for its current epoch.
   bool epoch_decided(std::string_view chain) {
      const auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outpcons"_n, fc::slug_name{chain}.value);
      if (data.empty()) return false;
      return msgch_abi.binary_to_variant("outpost_consensus_entry", data, fixture_abi_yield_function())
                ["epoch_index"].as<uint32_t>() == current_epoch(chain);
   }

   /// Open a dispute on `chain`'s current epoch as sysio.msgch does, between `checksums`; it pauses the chain.
   action_result open_dispute(std::string_view chain, const std::vector<fc::sha256>& checksums) {
      fc::variants candidates;
      for (const fc::sha256& checksum : checksums) {
         candidates.push_back(mvo()("checksum", checksum)("operators", std::vector<name>{BATCHOP}));
      }
      return push(CHALG_ACCOUNT, chalg_abi, MSGCH_ACCOUNT, "opendispute"_n, mvo()
         ("chain_code", fc::slug_name{chain}.value)("epoch_index", current_epoch(chain))("candidates", candidates));
   }

   /// Resolve dispute `dispute_id` for `winner`: the base tester's Tier-1 owner, the whole electorate, votes for
   /// it and the crank accepts it, lifting the chain's pause.
   void resolve_dispute(uint64_t dispute_id, const fc::sha256& winner) {
      BOOST_REQUIRE_EQUAL(success(), push(CHALG_ACCOUNT, chalg_abi, NODE_DADDY, "votedispute"_n, mvo()
         ("owner", NODE_DADDY)("dispute_id", dispute_id)("chosen_checksum", winner)));
      BOOST_REQUIRE_EQUAL(success(), push(CHALG_ACCOUNT, chalg_abi, BATCHOP, "chkdispute"_n,
                                          mvo()("dispute_id", dispute_id)));
      produce_blocks();
   }

   /// Whether `chain`'s epoch advancement is paused.
   bool paused(std::string_view chain) {
      const auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, fc::slug_name{chain}.value);
      BOOST_REQUIRE(!data.empty());
      return epoch_abi.binary_to_variant("epoch_state", data, fixture_abi_yield_function())["is_paused"].as_bool();
   }

   /// ETH's group size in the three-member tests: a strict majority is two.
   static constexpr uint32_t THREE_MEMBER_GROUP = 3;

   /// Seat ETH's next group with three members: BATCHOP_ETH, bonded and ACTIVE under a one-unit WIRE minimum
   /// and serving ETH alone, beside the bootstrapped BATCHOP and BATCHOP_PEER. ETH then closes one epoch so
   /// the slide seats that group on duty.
   void seat_three_member_eth_group() {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("terminate_prune_delay_ms",         600000)
         ("terminate_max_consecutive_misses", 5)
         ("terminate_max_pct_misses_24h",     5)
         ("terminate_min_pct_samples",        20)
         ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
         ("req_prod_collat",                  fc::variants{})
         ("req_batchop_collat",               fc::variants{fc::variant(mvo()
            ("chain_code", "WIRE")("token_code", "WIRE")("min_bond", 1)("config_timestamp_ms", 0))})
         ("req_uw_collat",                    fc::variants{})));
      register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
      BOOST_REQUIRE(get_operator(BATCHOP_ETH)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_ACTIVE);
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account",          BATCHOP_PEER.to_string())
         ("type",             OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped",  true)));
      sysio_system::test_support::served_chains::serve(*this, {BATCHOP_PEER}, {ETH_OUTPOST});
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("chain_code",                          ETH_CHAIN)
         ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
         ("traffic_wait_sec",                    0)
         ("operators_per_epoch",                 THREE_MEMBER_GROUP)
         ("batch_operator_minimum_active",       THREE_MEMBER_GROUP)
         ("batch_op_groups",                     1)
         ("epoch_retention_envelope_log_count",  200)));
      produce_blocks();
      close_epochs(ETH_CHAIN, 1);
      for (const name member : {BATCHOP_ETH, BATCHOP, BATCHOP_PEER}) BOOST_REQUIRE(seated(member, ETH_CHAIN));
   }

   /// `account`'s newest `recent_actions` entry.
   fc::variant last_action(name account) {
      const fc::variant op = get_operator(account);
      BOOST_REQUIRE(!op.is_null());
      const auto& actions = op["recent_actions"].get_array();
      BOOST_REQUIRE(!actions.empty());
      return actions.back();
   }

   /// `account`'s WIRE remit claim (`remitclaims{account, WIRE}`), 0 without a row.
   uint64_t wire_remit_claim(name account) {
      const auto data = get_row_by_id(OPREG_ACCOUNT, account, "remitclaims"_n, cn("WIRE").value);
      return data.empty() ? 0 : opreg_abi.binary_to_variant("remit_claim", data, fixture_abi_yield_function())
                                   ["balance"].as_uint64();
   }

   /// The `flushwtdw` refusal logged for a withdrawal whose operator was slashed while it waited.
   static constexpr std::string_view SLASHED_DURING_WAIT = "operator slashed during withdraw-wait window";

   /// Require withdrawal `request_id` of `account` flushed as a slashed operator's: erased, logged as a
   /// failed remit for that reason, and nothing credited.
   void require_dropped_as_slashed(name account, uint64_t request_id) {
      BOOST_REQUIRE(get_operator(account)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_SLASHED);
      BOOST_REQUIRE(get_wtdw(request_id).is_null());
      const fc::variant logged = last_action(account);
      BOOST_REQUIRE(!logged["success"].as_bool());
      BOOST_REQUIRE_EQUAL(std::string(SLASHED_DURING_WAIT), logged["error_message"].as_string());
      BOOST_REQUIRE_EQUAL(0u, wire_remit_claim(account));
   }

   /// The economic epoch withdrawal `request_id` becomes eligible at.
   uint32_t eligible_economic_epoch(uint64_t request_id) {
      const fc::variant row = get_wtdw(request_id);
      BOOST_REQUIRE(!row.is_null());
      return row["eligible_at_economic_epoch"].as<uint32_t>();
   }

   /// How many of the withdrawals with ids in [first_id, last_id] are still queued.
   uint32_t queued(uint64_t first_id, uint64_t last_id) {
      uint32_t n = 0;
      for (uint64_t id = first_id; id <= last_id; ++id) {
         if (!get_wtdw(id).is_null()) ++n;
      }
      return n;
   }

   /// The request id the next `flushwtdw` walk starts at (`opcounters.flush_resume_id`); 0 is the head.
   uint64_t flush_resume_id() {
      const auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "opcounters"_n, "opcounters"_n);
      if (data.empty()) return 0;
      return opreg_abi.binary_to_variant("op_counters", data, fixture_abi_yield_function())
         ["flush_resume_id"].as<uint64_t>();
   }

   uint64_t balance_of(name account, std::string_view chain_code,
                       std::string_view token_code) {
      auto op = get_operator(account);
      if (op.is_null()) return 0;
      const auto chain_v = cn(chain_code).value;
      const auto token_v = cn(token_code).value;
      const auto& arr = op["balances"].get_array();
      for (const auto& b : arr) {
         if (b["chain_code"].as<fc::slug_name>().value == chain_v &&
             b["token_code"].as<fc::slug_name>().value == token_v) {
            return b["balance"].as_uint64();
         }
      }
      return 0;
   }

   uint32_t count_attestations(sysio::opp::types::AttestationType expected,
                               uint64_t scan_until = 32) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT,
                                   "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant("attestation_entry", data,
            fixture_abi_yield_function());
         if (row["type"].as<sysio::opp::types::AttestationType>() == expected) ++n;
      }
      return n;
   }

   action_result fund_and_bond(name account, std::string_view chain, std::string_view token, uint64_t amount) {
      BOOST_REQUIRE_EQUAL("WIRE", chain);
      const auto quantity = asset(static_cast<int64_t>(amount), symbol(9, "WIRE"));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, SYSIO_ACCOUNT, "transfer"_n,
         mvo()("from", SYSIO_ACCOUNT)("to", account)("quantity", quantity)("memo", "bond funding")));
      return push(OPREG_ACCOUNT, opreg_abi, account, "deposit"_n,
         mvo()("account", account)("token_code", token)("amount", amount));
   }

   action_result withdraw(name account, std::string_view chain, std::string_view token, uint64_t amount) {
      BOOST_REQUIRE_EQUAL("WIRE", chain);
      return push(OPREG_ACCOUNT, opreg_abi, account, "withdraw"_n,
         mvo()("account", account)("token_code", token)("amount", amount));
   }

   void claim_exact(name account, uint64_t amount) {
      const auto before = get_currency_balance(TOKEN_ACCOUNT, symbol(9, "WIRE"), account).get_amount();
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, account, "claimremit"_n,
         mvo()("account", account)("token_code", "WIRE")));
      BOOST_CHECK_EQUAL(before + amount,
         get_currency_balance(TOKEN_ACCOUNT, symbol(9, "WIRE"), account).get_amount());
   }

   abi_serializer sysio_abi, token_abi, epoch_abi, opreg_abi, msgch_abi, chains_abi, authex_abi, dclaim_abi,
      chalg_abi;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_epoch_flushwtdw_tests)

BOOST_FIXTURE_TEST_CASE(flushwtdw_requires_epoch_auth, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   auto r = push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "flushwtdw"_n, mvo()("economic_epoch", 999));
   BOOST_REQUIRE(r.find("missing authority of sysio.epoch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(tick_releases_matured_native_withdraw, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 5'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT = 2'000'000;

   BOOST_REQUIRE_EQUAL(success(),
      fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));

   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "WIRE", "WIRE"));

   advance_one_epoch();
   advance_one_epoch();

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, "WIRE", "WIRE"));
   claim_exact(UWRIT_OP, WITHDRAW_AMOUNT);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(flushwtdw_credits_native_claim_without_outbound_attestation,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));

   // Satisfy the consensus waits, then flush directly far past the economic wait.
   for (uint32_t i = 0; i < WITHDRAW_WAIT_EPOCHS; ++i) {
      cross_epoch_boundary();
      advance_consensus();
   }
   constexpr uint32_t FUTURE_EPOCH = 100;
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT,
      "flushwtdw"_n, mvo()("economic_epoch", FUTURE_EPOCH)));

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, "WIRE", "WIRE"));

   BOOST_REQUIRE_EQUAL(count_attestations(
      sysio::opp::types::AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION), 0u);
   claim_exact(UWRIT_OP, WITHDRAW_AMOUNT);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(single_advance_leaves_immature_row_intact,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));

   advance_one_epoch();   // only one boundary of each clock; both waits are 2

   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(slashed_operator_withdraw_drops_silently,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));

   BOOST_REQUIRE_EQUAL(success(), slash(UWRIT_OP, "test slash"));
   uint64_t balance_after_slash = balance_of(UWRIT_OP, "WIRE", "WIRE");

   advance_one_epoch();
   advance_one_epoch();

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(balance_after_slash,
                       balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(economic_wait_alone_does_not_release, sysio_epoch_flushwtdw_tester) { try {
   // The economic clock runs ahead while consensus stands still: the consensus guard holds the
   // collateral until the request's deliveries could have been judged.
   bootstrap_for_flushwtdw();
   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));

   for (int i = 0; i < 4; ++i) {
      cross_epoch_boundary();
      tick_economy();
   }
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "WIRE", "WIRE"));

   // Two consensus closes satisfy the guard; the next economic tick releases it.
   advance_consensus();
   cross_epoch_boundary();
   advance_consensus();
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   tick_economy();
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(consensus_alone_does_not_release, sysio_epoch_flushwtdw_tester) { try {
   // Consensus closes without economic ticks release nothing: flushing runs on the economic clock,
   // and the economic wait must pass too.
   bootstrap_for_flushwtdw();
   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));
   const auto row = get_wtdw(1);
   BOOST_REQUIRE_EQUAL(economic_epoch(), row["requested_at_economic_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(economic_epoch() + WITHDRAW_WAIT_ECONOMIC_EPOCHS,
                       row["eligible_at_economic_epoch"].as<uint32_t>());
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(1));

   for (int i = 0; i < 3; ++i) {
      cross_epoch_boundary();
      advance_consensus();
   }
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   tick_economy();   // one economic epoch is still short of the wait
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   tick_economy();
   BOOST_REQUIRE(get_wtdw(1).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_request_while_the_crank_is_behind_waits_for_later_boundaries,
                        sysio_epoch_flushwtdw_tester) { try {
   // The economic wait counts boundaries after the request. With the crank behind, the catch-up ticks
   // for boundaries that had already passed at the request must not count toward it.
   bootstrap_for_flushwtdw();
   constexpr uint64_t INITIAL_DEPOSIT   = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT   =   400'000;
   constexpr uint32_t BOUNDARIES_BEHIND = 3;
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", INITIAL_DEPOSIT));
   for (uint32_t i = 0; i < BOUNDARIES_BEHIND; ++i) cross_epoch_boundary();
   const uint32_t ticked = economic_epoch();

   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", WITHDRAW_AMOUNT));
   const auto row = get_wtdw(1);
   const uint32_t requested = row["requested_at_economic_epoch"].as<uint32_t>();
   BOOST_REQUIRE_EQUAL(ticked + BOUNDARIES_BEHIND, requested);
   BOOST_REQUIRE_EQUAL(requested + WITHDRAW_WAIT_ECONOMIC_EPOCHS, row["eligible_at_economic_epoch"].as<uint32_t>());

   // Satisfy the consensus guard first, so only the economic wait holds the request.
   for (int i = 0; i < 2; ++i) {
      cross_epoch_boundary();
      advance_consensus();
   }
   while (economic_epoch() + 1 < requested + WITHDRAW_WAIT_ECONOMIC_EPOCHS) tick_economy();
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "WIRE", "WIRE"));
   tick_economy();
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(dispute_pause_holds_only_the_withdrawals_waiting_on_the_paused_chain,
                        sysio_epoch_flushwtdw_tester) { try {
   // A dispute pause freezes its chain's consensus index, and the paused epoch is never decided. A
   // withdrawal that waits on the paused chain stays held while the other chain keeps closing epochs and
   // the economic clock keeps ticking; one that waits only on the running chain is released. The held
   // ones release after the chain resumes.
   bootstrap_for_flushwtdw();
   constexpr std::string_view PAUSED_CHAIN  = ETH_CHAIN;
   constexpr std::string_view RUNNING_CHAIN = SOL_CHAIN;
   constexpr uint64_t         EVERY_CHAIN   = 1;   // the underwriter's request
   constexpr uint64_t         PAUSED_ONLY   = 2;
   constexpr uint64_t         RUNNING_ONLY  = 3;
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", OPERATOR_BOND));
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   register_serving_batch_operator(BATCHOP_SOL, {SOL_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_SOL, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE_EQUAL(success(), pause_chain(PAUSED_CHAIN));

   for (int i = 0; i < 4; ++i) {
      cross_epoch_boundary();
      advance_chain(RUNNING_CHAIN);
      tick_economy();
   }
   BOOST_REQUIRE(!get_wtdw(EVERY_CHAIN).is_null());
   BOOST_REQUIRE(!get_wtdw(PAUSED_ONLY).is_null());
   BOOST_REQUIRE(get_wtdw(RUNNING_ONLY).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_SOL, "WIRE", "WIRE"));
   BOOST_REQUIRE_EQUAL(current_epoch(PAUSED_CHAIN) + 4, current_epoch(RUNNING_CHAIN));

   BOOST_REQUIRE_EQUAL(success(), unpause_chain(PAUSED_CHAIN));
   advance_consensus();
   cross_epoch_boundary();
   advance_consensus();
   tick_economy();
   BOOST_REQUIRE(get_wtdw(EVERY_CHAIN).is_null());
   BOOST_REQUIRE(get_wtdw(PAUSED_ONLY).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_stalled_chain_does_not_hold_a_batch_operator_serving_only_another,
                        sysio_epoch_flushwtdw_tester) { try {
   // A batch operator waits only on the chains it serves: SOL stalls, and ETH alone releases it.
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(1));

   const uint32_t stalled = current_epoch(SOL_CHAIN);
   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(stalled, current_epoch(SOL_CHAIN));
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));
   claim_exact(BATCHOP_ETH, OPERATOR_WITHDRAWAL);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_batch_operator_serving_two_chains_waits_on_both, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_BOTH, {SOL_OUTPOST, ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_BOTH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(1));

   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   // One SOL epoch short of its wait still holds the request; the last one releases it.
   close_epochs(SOL_CHAIN, WITHDRAW_WAIT_EPOCHS - 1);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   close_epochs(SOL_CHAIN, 1);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_BOTH, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_left_member_waits_on_the_chain_only_while_seated, sysio_epoch_flushwtdw_tester) { try {
   // Leaving a chain stops new seats there. A request still waits on the chain while the member holds a seat
   // in its window, whose epochs are not yet judged, and not once the window has slid past its last seat.
   bootstrap_for_flushwtdw();
   // A one-unit WIRE minimum, so a bonded non-bootstrapped batch operator turns ACTIVE and the slide seats it.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
      ("terminate_prune_delay_ms",         600000)
      ("terminate_max_consecutive_misses", 5)
      ("terminate_max_pct_misses_24h",     5)
      ("terminate_min_pct_samples",        20)
      ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
      ("req_prod_collat",                  fc::variants{})
      ("req_batchop_collat",               fc::variants{fc::variant(mvo()
         ("chain_code", "WIRE")("token_code", "WIRE")("min_bond", 1)("config_timestamp_ms", 0))})
      ("req_uw_collat",                    fc::variants{})));
   register_serving_batch_operator(BATCHOP_BOTH, {SOL_OUTPOST, ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE(get_operator(BATCHOP_BOTH)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_ACTIVE);
   // The slide seats a non-bootstrapped operator first: SOL's next group is BATCHOP_BOTH alone.
   close_epochs(SOL_CHAIN, 1);
   BOOST_REQUIRE(seated(BATCHOP_BOTH, SOL_CHAIN));
   BOOST_REQUIRE_EQUAL(success(),
      sysio_system::test_support::served_chains::leave(*this, BATCHOP_BOTH, SOL_CHAIN));
   BOOST_REQUIRE_EQUAL(sysio_system::test_support::served_chains::status::left,
      sysio_system::test_support::served_chains::membership(*this, opreg_abi, BATCHOP_BOTH, SOL_CHAIN)
         ["status"].as_string());

   // Still seated on the chain it left: the request waits on it.
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_BOTH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(1));
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, BATCHOP_BOTH, "cancelwtdw"_n,
                                       mvo()("account", BATCHOP_BOTH)("request_id", 1)));

   // SOL's window slides past its seat and a LEFT member gets no new one: a request now waits on ETH alone,
   // and is released while SOL stands still.
   close_epochs(SOL_CHAIN, 1);
   BOOST_REQUIRE(!seated(BATCHOP_BOTH, SOL_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_BOTH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(2));
   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(eligible_economic_epoch(2));
   BOOST_REQUIRE(get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_non_batch_operator_waits_on_every_started_chain, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", OPERATOR_BOND));
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(1));

   close_epochs(SOL_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_chain_started_after_the_request_is_not_waited_on, sysio_epoch_flushwtdw_tester) { try {
   // A request waits only on the chains whose epochs had started when it was made. A chain activated
   // and started later holds the requests made after its start, not the ones made before, even from a
   // batch operator already serving it.
   bootstrap_for_flushwtdw();
   constexpr std::string_view LATE_CHAIN       = "BASE";
   constexpr uint32_t         LATE_EXTERNAL_ID = 8453;
   constexpr outpost          LATE_OUTPOST{LATE_CHAIN, ChainKind::CHAIN_KIND_EVM};
   constexpr uint64_t         BEFORE_START     = 1;
   constexpr uint64_t         SERVING_BEFORE   = 2;   // the batch operator's request, before the start
   constexpr uint64_t         AFTER_START      = 3;
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind",              ChainKind::CHAIN_KIND_EVM)
      ("code",              LATE_CHAIN)
      ("external_chain_id", LATE_EXTERNAL_ID)
      ("name",              std::string("base-test"))
      ("description",       std::string{})
      ("outpost",           sysio_system::test_support::no_outpost_mvo())));
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "activchain"_n,
                                       mvo()("code", LATE_CHAIN)));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("chain_code",                          LATE_CHAIN)
      ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
      ("traffic_wait_sec",                    0)
      ("operators_per_epoch",                 1)
      ("batch_operator_minimum_active",       1)
      ("batch_op_groups",                     1)
      ("epoch_retention_envelope_log_count",  200)));

   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", OPERATOR_BOND));
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST, LATE_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(BEFORE_START));
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(SERVING_BEFORE));

   // Start the late chain: the bootstrapped batch operator (already linked for EVM) serves it.
   BOOST_REQUIRE_EQUAL(success(), sysio_system::test_support::served_chains::join(*this, BATCHOP, LATE_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "schbatchgps"_n,
                                       mvo()("chain_code", LATE_CHAIN)));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "advance"_n,
                                       mvo()("chain_code", LATE_CHAIN)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, current_epoch(LATE_CHAIN));

   BOOST_REQUIRE_EQUAL(success(), regunderwriter(UWRIT_OP_B));
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP_B, "WIRE", "WIRE", OPERATOR_BOND));
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP_B, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN, LATE_CHAIN}) == consensus_waits(AFTER_START));

   // SOL and ETH close their waits while the late chain stalls at its first epoch.
   for (uint32_t i = 0; i < WITHDRAW_WAIT_EPOCHS; ++i) {
      cross_epoch_boundary();
      advance_consensus();
   }
   tick_economy_to(std::max(eligible_economic_epoch(BEFORE_START), eligible_economic_epoch(AFTER_START)));
   BOOST_REQUIRE(get_wtdw(BEFORE_START).is_null());
   BOOST_REQUIRE(get_wtdw(SERVING_BEFORE).is_null());
   BOOST_REQUIRE(!get_wtdw(AFTER_START).is_null());
   BOOST_REQUIRE_EQUAL(1u, current_epoch(LATE_CHAIN));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_held_withdrawal_does_not_block_a_later_matured_one, sysio_epoch_flushwtdw_tester) { try {
   // The flush skips a row a consensus wait still holds instead of stopping there.
   bootstrap_for_flushwtdw();
   constexpr uint64_t HELD     = 1;   // the underwriter's request, waiting on both chains
   constexpr uint64_t RELEASED = 2;   // the ETH-only batch operator's request
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", OPERATOR_BOND));
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));

   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(std::max(eligible_economic_epoch(HELD), eligible_economic_epoch(RELEASED)));
   BOOST_REQUIRE(!get_wtdw(HELD).is_null());
   BOOST_REQUIRE(get_wtdw(RELEASED).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND, balance_of(UWRIT_OP, "WIRE", "WIRE"));
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));
   BOOST_REQUIRE_EQUAL(0u, flush_resume_id());

   close_epochs(SOL_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(HELD).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(the_held_skip_budget_bounds_a_flush_and_the_next_resumes, sysio_epoch_flushwtdw_tester) { try {
   // More held rows than one flush may skip sit ahead of a releasable one: the first flush stops on a
   // held row, and the next resumes there and releases the row behind the held run.
   bootstrap_for_flushwtdw();
   constexpr uint32_t HELD_ROWS     = MAX_WTDW_HELD_SKIPS_PER_FLUSH + 1;
   constexpr uint64_t RELEASABLE_ID = uint64_t{HELD_ROWS} + 1;
   constexpr uint64_t HELD_DEPOSIT  = 1'000;

   std::vector<name> holders;
   for (uint32_t i = 0; i < HELD_ROWS; ++i) {
      holders.push_back(name{std::string{"uwskip."} + char('a' + i / 26) + char('a' + i % 26)});
   }
   create_accounts(holders);
   for (const name holder : holders) {
      BOOST_REQUIRE_EQUAL(success(), regunderwriter(holder));
      BOOST_REQUIRE_EQUAL(success(), fund_and_bond(holder, "WIRE", "WIRE", HELD_DEPOSIT));
      produce_blocks();
   }
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);

   // Every underwriter waits on both chains, so a stalled SOL holds it; the batch operator waits on ETH.
   for (const name holder : holders) {
      BOOST_REQUIRE_EQUAL(success(), withdraw(holder, "WIRE", "WIRE", HELD_DEPOSIT));
   }
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   produce_blocks();
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(RELEASABLE_ID));
   const uint32_t eligible = eligible_economic_epoch(RELEASABLE_ID);
   BOOST_REQUIRE_EQUAL(eligible, eligible_economic_epoch(1));

   close_epochs(ETH_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(eligible);
   BOOST_REQUIRE_EQUAL(HELD_ROWS, queued(1, HELD_ROWS));
   BOOST_REQUIRE(!get_wtdw(RELEASABLE_ID).is_null());
   BOOST_REQUIRE_EQUAL(uint64_t{HELD_ROWS}, flush_resume_id());   // stopped on the last held row

   tick_economy_to(eligible + 1);
   BOOST_REQUIRE(get_wtdw(RELEASABLE_ID).is_null());
   BOOST_REQUIRE_EQUAL(HELD_ROWS, queued(1, HELD_ROWS));
   BOOST_REQUIRE_EQUAL(0u, flush_resume_id());

   // Once SOL closes its wait the held rows drain, MAX_WTDW_FLUSH_PER_EPOCH per flush, each flush
   // resuming where the last stopped.
   close_epochs(SOL_CHAIN, WITHDRAW_WAIT_EPOCHS);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE_EQUAL(HELD_ROWS - MAX_WTDW_FLUSH_PER_EPOCH, queued(1, HELD_ROWS));
   BOOST_REQUIRE_EQUAL(uint64_t{MAX_WTDW_FLUSH_PER_EPOCH} + 1, flush_resume_id());
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE_EQUAL(HELD_ROWS - 2 * MAX_WTDW_FLUSH_PER_EPOCH, queued(1, HELD_ROWS));
   BOOST_REQUIRE_EQUAL(uint64_t{2 * MAX_WTDW_FLUSH_PER_EPOCH} + 1, flush_resume_id());
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE_EQUAL(0u, queued(1, HELD_ROWS));
   BOOST_REQUIRE_EQUAL(0u, flush_resume_id());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_decided_epoch_releases_at_the_request_epoch, sysio_epoch_flushwtdw_tester) { try {
   // The request's own epoch holds it while undecided; once that epoch's winner is accepted the economic wait
   // alone releases it, with the chain still at the request's epoch.
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(1));
   const uint32_t requested_at = current_epoch(ETH_CHAIN);

   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   decide_epoch(ETH_CHAIN);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(requested_at, current_epoch(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));
   claim_exact(BATCHOP_ETH, OPERATOR_WITHDRAWAL);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(an_idle_chain_decided_one_close_after_the_request_releases, sysio_epoch_flushwtdw_tester) { try {
   // A chain that closes one epoch after the request and then idles never reaches the request's index for it.
   // The decision of the request's epoch stops counting once the chain closes it; the next epoch holds the
   // request while undecided and releases it once decided, however long the chain then idles.
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   decide_epoch(ETH_CHAIN);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   const uint32_t requested_at = current_epoch(ETH_CHAIN);

   close_epochs(ETH_CHAIN, 1);
   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));

   decide_epoch(ETH_CHAIN);
   tick_economy_to(economic_epoch() + WITHDRAW_WAIT_ECONOMIC_EPOCHS);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(requested_at + 1, current_epoch(ETH_CHAIN));
   BOOST_REQUIRE_LT(current_epoch(ETH_CHAIN), requested_at + WITHDRAW_WAIT_EPOCHS);
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(each_waited_on_chain_passes_its_wait_on_its_own, sysio_epoch_flushwtdw_tester) { try {
   // A request waiting on two chains needs both waits passed: ETH's by its decided epoch, SOL's by closing its
   // two undecided epochs, one short of which still holds the request.
   bootstrap_for_flushwtdw();
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(UWRIT_OP, "WIRE", "WIRE", OPERATOR_BOND));
   BOOST_REQUIRE_EQUAL(success(), withdraw(UWRIT_OP, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({SOL_CHAIN, ETH_CHAIN}) == consensus_waits(1));
   const uint32_t eth_requested_at = current_epoch(ETH_CHAIN);

   decide_epoch(ETH_CHAIN);
   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   close_epochs(SOL_CHAIN, WITHDRAW_WAIT_EPOCHS - 1);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   close_epochs(SOL_CHAIN, 1);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(eth_requested_at, current_epoch(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(UWRIT_OP, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_disputed_epoch_holds_until_its_winner_is_accepted, sysio_epoch_flushwtdw_tester) { try {
   // A dispute on the request's epoch pauses the chain and leaves the epoch undecided, holding the request past
   // its economic wait; the resolution accepts the winner, which decides the epoch and releases it.
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   const uint32_t requested_at = current_epoch(ETH_CHAIN);

   // Opened before the delivery, the dispute keeps it from deciding the epoch and keeps its bytes on file.
   const std::vector<char> winner          = next_delivery(ETH_CHAIN);
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());
   BOOST_REQUIRE_EQUAL(success(), open_dispute(ETH_CHAIN, {winner_checksum, fc::sha256::hash(std::string("loser"))}));
   BOOST_REQUIRE(paused(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, winner));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_CHAIN));

   tick_economy_to(eligible_economic_epoch(1) + WITHDRAW_WAIT_ECONOMIC_EPOCHS);
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));

   resolve_dispute(1, winner_checksum);
   accept(ETH_CHAIN, winner);
   BOOST_REQUIRE(epoch_decided(ETH_CHAIN));
   BOOST_REQUIRE(!paused(ETH_CHAIN));
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(requested_at, current_epoch(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND - OPERATOR_WITHDRAWAL, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(an_open_dispute_holds_a_decided_epoch, sysio_epoch_flushwtdw_tester) { try {
   // The guard reads a decided epoch with an open dispute on it as holding. sysio.msgch never opens one on a
   // decided epoch; here it is opened directly. Resolving it lifts the hold.
   bootstrap_for_flushwtdw();
   register_serving_batch_operator(BATCHOP_ETH, {ETH_OUTPOST}, OPERATOR_BOND);
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   const std::vector<char> winner          = next_delivery(ETH_CHAIN);
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());
   decide_epoch(ETH_CHAIN);
   BOOST_REQUIRE_EQUAL(success(), open_dispute(ETH_CHAIN, {winner_checksum, fc::sha256::hash(std::string("loser"))}));

   tick_economy_to(eligible_economic_epoch(1));
   BOOST_REQUIRE(epoch_decided(ETH_CHAIN));
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   resolve_dispute(1, winner_checksum);
   tick_economy_to(economic_epoch() + 1);
   BOOST_REQUIRE(get_wtdw(1).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_divergent_deliverer_decided_against_is_slashed_before_its_withdrawal_flushes,
                        sysio_epoch_flushwtdw_tester) { try {
   // The withdrawer delivers a divergent envelope; the other two members decide the epoch on theirs, and
   // the acceptance slashes the withdrawer in that transaction. The decided epoch passes the request's wait,
   // so the next flush reaches it and drops it as a slashed operator's, crediting nothing.
   bootstrap_for_flushwtdw();
   seat_three_member_eth_group();
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));
   BOOST_REQUIRE(waits_from_now({ETH_CHAIN}) == consensus_waits(1));
   const uint32_t requested_at = current_epoch(ETH_CHAIN);

   const std::vector<char> divergent = next_delivery(ETH_CHAIN, "divergent");
   const std::vector<char> winner    = next_delivery(ETH_CHAIN);
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, divergent, BATCHOP_ETH));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, winner, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, winner, BATCHOP_PEER));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_CHAIN));
   accept(ETH_CHAIN, winner);
   BOOST_REQUIRE(get_operator(BATCHOP_ETH)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_SLASHED);
   BOOST_REQUIRE(!get_wtdw(1).is_null());

   tick_economy_to(eligible_economic_epoch(1));
   require_dropped_as_slashed(BATCHOP_ETH, 1);
   BOOST_REQUIRE_EQUAL(requested_at, current_epoch(ETH_CHAIN));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(a_dispute_loser_is_slashed_at_resolution_before_its_withdrawal_flushes,
                        sysio_epoch_flushwtdw_tester) { try {
   // Three distinct envelopes leave no version able to win, so the third delivery opens a dispute and pauses
   // ETH; the request is held past its economic wait. The Tier-1 vote accepts BATCHOP's envelope, and the
   // resolution slashes the withdrawer among the losers and decides the epoch; the next flush drops the
   // request as a slashed operator's, crediting nothing.
   bootstrap_for_flushwtdw();
   seat_three_member_eth_group();
   BOOST_REQUIRE_EQUAL(success(), withdraw(BATCHOP_ETH, "WIRE", "WIRE", OPERATOR_WITHDRAWAL));

   const std::vector<char> withdrawers = next_delivery(ETH_CHAIN, "withdrawer");
   const std::vector<char> winner      = next_delivery(ETH_CHAIN);
   const std::vector<char> peers       = next_delivery(ETH_CHAIN, "peer");
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, withdrawers, BATCHOP_ETH));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, winner, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE(!paused(ETH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_CHAIN, peers, BATCHOP_PEER));
   produce_blocks();
   BOOST_REQUIRE(paused(ETH_CHAIN));
   BOOST_REQUIRE(!epoch_decided(ETH_CHAIN));
   BOOST_REQUIRE(get_operator(BATCHOP_ETH)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_ACTIVE);

   tick_economy_to(eligible_economic_epoch(1) + WITHDRAW_WAIT_ECONOMIC_EPOCHS);
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(OPERATOR_BOND, balance_of(BATCHOP_ETH, "WIRE", "WIRE"));

   resolve_dispute(1, fc::sha256::hash(winner.data(), winner.size()));
   accept(ETH_CHAIN, winner);
   BOOST_REQUIRE(epoch_decided(ETH_CHAIN));
   BOOST_REQUIRE(!paused(ETH_CHAIN));
   BOOST_REQUIRE(get_operator(BATCHOP_ETH)["status"].as<OperatorStatus>() == OperatorStatus::OPERATOR_STATUS_SLASHED);
   tick_economy_to(economic_epoch() + 1);
   require_dropped_as_slashed(BATCHOP_ETH, 1);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

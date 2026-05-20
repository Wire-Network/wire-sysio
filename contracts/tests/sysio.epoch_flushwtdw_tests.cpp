/// Cross-contract tests for the `sysio.epoch::advance` ↔ `sysio.opreg::
/// flushwtdw` integration (Task 9 of the operator-collateral plan).
///
/// v6 data-model: identity is now slug_name-keyed across opreg / chains.
/// The fixture deploys `sysio.chains` so the chain-of-record exists and
/// uses `regchain` (replacing the v5 `regoutpost`).

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/attestations/attestations.pb.h>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

class sysio_epoch_flushwtdw_tester : public tester {
public:
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto UWRIT_ACCOUNT  = "sysio.uwrit"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto UWRIT_OP       = "uwrit.alice"_n;

   static constexpr uint32_t EPOCH_DURATION_SEC = 1;

   sysio_epoch_flushwtdw_tester() {
      produce_blocks(2);

      create_accounts({
         EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT, CHALG_ACCOUNT,
         CHAINS_ACCOUNT, UWRIT_ACCOUNT,
         BATCHOP, UWRIT_OP
      });
      produce_blocks(2);

      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi);
      // epoch::advance inlines into sysio.uwrit::chklocks — uwrit must
      // exist for the cross-contract call to resolve.
      deploy(UWRIT_ACCOUNT,  contracts::uwrit_wasm(),  contracts::uwrit_abi(),  uwrit_abi);

      produce_blocks();
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
                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }
   static fc::mutable_variant_object codename_mvo(std::string_view s) {
      return mvo()("value", fc::slug_name{s}.value);
   }

   /// Push an action against any deployed contract.
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
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   /// One-shot bootstrap: epoch config + opreg config + a bootstrapped
   /// batch op + a pending underwriter + sysio.chains rows.
   void bootstrap_for_flushwtdw() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
            ("operators_per_epoch",                 1)
            ("batch_operator_minimum_active",       1)
            ("batch_op_groups",                     1)
            ("epoch_retention_envelope_log_count",  200)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("max_available_producers",          21)
            ("max_available_batch_ops",          63)
            ("max_available_underwriters",       21)
            ("terminate_prune_delay_ms",         600000)
            ("terminate_max_consecutive_misses", 5)
            ("terminate_max_pct_misses_24h",     5)
            ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
            ("req_prod_collat",                  fc::variants{})
            ("req_batchop_collat",               fc::variants{})
            ("req_uw_collat",                    fc::variants{})));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          BATCHOP.to_string())
            ("type",             OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped",  true)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          UWRIT_OP.to_string())
            ("type",             OperatorType::OPERATOR_TYPE_UNDERWRITER)
            ("is_bootstrapped",  false)));

      // Register a SOLANA-class chain first to soak up the first slot, then
      // an Ethereum chain to host the deposits the rest of the test exercises.
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_SVM)
            ("code",              codename_mvo("SOL"))
            ("external_chain_id", 1)
            ("name",              std::string("solana-test"))
            ("description",       std::string{})));
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_EVM)
            ("code",              codename_mvo("ETH"))
            ("external_chain_id", 31337)
            ("name",              std::string("ethereum-test"))
            ("description",       std::string{})));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "schbatchgps"_n, mvo()));

      // Genesis advance — permissionless until current_epoch_index moves
      // off zero.
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "advance"_n, mvo()));

      produce_blocks();
   }

   /// Step the wall clock past `next_epoch_start` and call advance once.
   uint32_t advance_one_epoch() {
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT,
         "advance"_n, mvo()));
      produce_blocks();
      return current_epoch();
   }

   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT,
                                     "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
   }

   /// Direct opreg::depositinle, signed as opreg itself.
   /// v6 signature: codenames for chain and token, plus the actor identity.
   action_result depositinle(name account, std::string_view chain_code,
                             std::string_view token_code, uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "depositinle"_n, mvo()
         ("account",              account.to_string())
         ("chain_code",           codename_mvo(chain_code))
         ("token_code",           codename_mvo(token_code))
         ("amount",               amount)
         ("actor_chain",          ChainKind::CHAIN_KIND_EVM)
         ("actor_address",        std::vector<char>{})
         ("original_message_id",  std::string(64, '0')));
   }

   action_result withdrawinle(name account, std::string_view chain_code,
                              std::string_view token_code, uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "withdrawinle"_n, mvo()
         ("account",     account.to_string())
         ("chain_code",  codename_mvo(chain_code))
         ("token_code",  codename_mvo(token_code))
         ("amount",      amount));
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
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "withdraw_request", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   uint64_t balance_of(name account, std::string_view chain_code,
                       std::string_view token_code) {
      auto op = get_operator(account);
      if (op.is_null()) return 0;
      const auto chain_v = cn(chain_code).value;
      const auto token_v = cn(token_code).value;
      const auto& arr = op["balances"].get_array();
      for (const auto& b : arr) {
         if (b["chain_code"]["value"].as_uint64() == chain_v &&
             b["token_code"]["value"].as_uint64() == token_v) {
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
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["type"].as<sysio::opp::types::AttestationType>() == expected) ++n;
      }
      return n;
   }

   std::vector<std::vector<char>>
   collect_attestation_data(sysio::opp::types::AttestationType expected,
                            uint64_t scan_until = 32) {
      std::vector<std::vector<char>> out;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT,
                                   "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant("attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["type"].as<sysio::opp::types::AttestationType>() != expected) continue;
         out.push_back(row["data"].as<std::vector<char>>());
      }
      return out;
   }

   abi_serializer epoch_abi, opreg_abi, msgch_abi, chains_abi, uwrit_abi;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_epoch_flushwtdw_tests)

BOOST_FIXTURE_TEST_CASE(flushwtdw_requires_epoch_auth, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   auto r = push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "flushwtdw"_n, mvo()
      ("current_epoch", 999));
   BOOST_REQUIRE(r.find("missing authority of sysio.epoch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(advance_drains_matured_eth_withdraw, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 5'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT = 2'000'000;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, "ETH", "ETH", WITHDRAW_AMOUNT));

   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "ETH", "ETH"));

   advance_one_epoch();
   advance_one_epoch();

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, "ETH", "ETH"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(flushwtdw_direct_emits_withdraw_remit_attestation,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, "ETH", "ETH", WITHDRAW_AMOUNT));

   constexpr uint32_t FUTURE_EPOCH = 100;
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT,
      "flushwtdw"_n, mvo()("current_epoch", FUTURE_EPOCH)));

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, "ETH", "ETH"));

   BOOST_REQUIRE_GE(count_attestations(
      sysio::opp::types::AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION), 1u);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(single_advance_leaves_immature_row_intact,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, "ETH", "ETH", WITHDRAW_AMOUNT));

   advance_one_epoch();   // only one boundary — eligible_at_epoch is +2

   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT, balance_of(UWRIT_OP, "ETH", "ETH"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(slashed_operator_withdraw_drops_silently,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, "ETH", "ETH", WITHDRAW_AMOUNT));

   BOOST_REQUIRE_EQUAL(success(), slash(UWRIT_OP, "test slash"));
   uint64_t balance_after_slash = balance_of(UWRIT_OP, "ETH", "ETH");

   advance_one_epoch();
   advance_one_epoch();

   BOOST_REQUIRE(get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(balance_after_slash,
                       balance_of(UWRIT_OP, "ETH", "ETH"));
} FC_LOG_AND_RETHROW() }

/// The "clean protobuf" regression tests below verify that the bytes
/// emitted by `sysio.opreg::emit_*` parse as a standard protobuf
/// `OperatorAction` message. They were originally written against the v5
/// OperatorAction proto (with a `chain` ChainKind field and a
/// `TokenAmount.kind` TokenKind field). The v6 proto carries
/// `chain_code` (uint64) and `amount.token_code` (uint64) instead — same
/// shape, different field semantics — so the parse + field-1-tag-byte
/// invariant still holds.
BOOST_FIXTURE_TEST_CASE(flushwtdw_attestation_data_is_clean_protobuf,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, "ETH", "ETH", WITHDRAW_AMOUNT));

   constexpr uint32_t FUTURE_EPOCH = 100;
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT,
      "flushwtdw"_n, mvo()("current_epoch", FUTURE_EPOCH)));

   auto rows = collect_attestation_data(
      sysio::opp::types::AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION);
   BOOST_REQUIRE_EQUAL(rows.size(), 1u);
   const auto& bytes = rows.front();
   BOOST_REQUIRE(!bytes.empty());

   // First byte MUST be a valid protobuf field-1-varint tag (0x08 =
   // OperatorAction.action_type).
   BOOST_REQUIRE_EQUAL(static_cast<uint8_t>(bytes.front()), 0x08u);

   sysio::opp::attestations::OperatorAction oa;
   BOOST_REQUIRE(oa.ParseFromArray(bytes.data(),
                                   static_cast<int>(bytes.size())));
   BOOST_REQUIRE_EQUAL(
      static_cast<int>(oa.action_type()),
      static_cast<int>(sysio::opp::attestations::
                         OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(oa.amount().amount()),
                       WITHDRAW_AMOUNT);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(flushwtdw_multiple_attestations_all_clean_protobuf,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT  = 1'000'000;

   const std::array<uint64_t, 3> withdraws{ 100'000, 200'000, 300'000 };

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, "ETH", "ETH", INITIAL_DEPOSIT));
   for (auto amount : withdraws) {
      BOOST_REQUIRE_EQUAL(success(),
         withdrawinle(UWRIT_OP, "ETH", "ETH", amount));
   }

   constexpr uint32_t FUTURE_EPOCH = 100;
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT,
      "flushwtdw"_n, mvo()("current_epoch", FUTURE_EPOCH)));

   auto rows = collect_attestation_data(
      sysio::opp::types::AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION);
   BOOST_REQUIRE_EQUAL(rows.size(), withdraws.size());

   for (size_t i = 0; i < rows.size(); ++i) {
      const auto& bytes = rows[i];
      BOOST_REQUIRE(!bytes.empty());
      BOOST_REQUIRE_EQUAL(static_cast<uint8_t>(bytes.front()), 0x08u);

      sysio::opp::attestations::OperatorAction oa;
      BOOST_REQUIRE(oa.ParseFromArray(bytes.data(),
                                      static_cast<int>(bytes.size())));
      BOOST_REQUIRE_EQUAL(
         static_cast<int>(oa.action_type()),
         static_cast<int>(sysio::opp::attestations::
                            OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT));
      BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(oa.amount().amount()),
                          withdraws[i]);
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

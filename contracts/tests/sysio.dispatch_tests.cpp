/// Cross-contract dispatch tests for sysio.msgch's per-attestation-type
/// routing (Task 4 of the operator-collateral plan).
///
/// Unlike the single-contract tests in `sysio.msgch_tests.cpp` (which only
/// exercise the inbound envelope durability + outbound packing surface),
/// this fixture deploys the full inbound-handler set — opreg + uwrit +
/// reserve + epoch + chalg — alongside msgch and verifies that a delivered
/// envelope's individual attestations end up with the right downstream
/// side-effects:
///
///   * `OPERATOR_ACTION (DEPOSIT_REQUEST)`  → opreg::depositinle  → balance row
///   * `OPERATOR_ACTION (WITHDRAW_REQUEST)` → opreg::withdrawinle → wtdwqueue row
///   * `UNDERWRITE_INTENT_REJECT`      → uwrit::rcrdreject    → commit_entry RELEASED
///   * `REMIT_CONFIRM`                 → uwrit::release       → uwreq COMPLETED
///
/// The path under test is:
///
///   batch_op → msgch::deliver → msgch::evalcons (consensus)
///            → dispatch_attestation → inline downstream action
///
/// The fixture pins `operators_per_epoch=1` and a single batch-op group so
/// one delivery from the lone active batch operator immediately satisfies
/// the consensus check (`checksum_count == operators_per_group &&
/// total_deliveries == operators_per_group`). That keeps the test focused
/// on the dispatch surface rather than the consensus voting math.
///
/// Cross-contract permission grants — the depot wires these at deploy time
/// in production via `sysio.bios::setauth`; here we set them explicitly so
/// each contract's `require_auth(get_self())` check passes when the
/// neighbouring contract sends an inline action with its own active key.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

#include <fc/variant_object.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <magic_enum/magic_enum.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

namespace {

/// Build an `authority` whose active permission is the account's own
/// active key + a list of `{actor, sysio.code}` co-signers. Used so each
/// contract trusts its neighbours' inline actions.
/// Build an `authority` that mirrors what `updateauth` would emit at the
/// cluster level: the account's own active key plus a list of
/// `{caller, sysio.code}` co-signers so each `caller` may send inline
/// actions DECLARING this account's permission. Mirrors the
/// `wire-tools-ts/.../ClusterManager.ts` line-1714 pattern verbatim — only
/// `sysio.code` (not `active`) is added per caller, since contract-to-
/// contract inline auth in Antelope flows through the implicit
/// `{contract, sysio.code}` permission.
authority active_with_code_authors(name account, const std::vector<name>& code_authors) {
   authority a(base_tester::get_public_key(account, "active"));
   // Preserve `{self, sysio.code}` so the contract retains the ability to
   // send inline actions on its own behalf — `create_account(include_code=
   // true)` adds this entry by default; replacing the active permission
   // would otherwise drop it.
   a.accounts.push_back(permission_level_weight{
      {account, config::sysio_code_name}, 1});
   for (const auto& actor : code_authors) {
      a.accounts.push_back(permission_level_weight{
         {actor, config::sysio_code_name}, 1});
   }
   std::sort(a.accounts.begin(), a.accounts.end(),
      [](const permission_level_weight& l, const permission_level_weight& r) {
         return std::tie(l.permission.actor, l.permission.permission)
              < std::tie(r.permission.actor, r.permission.permission);
      });
   return a;
}

/// Encode an Envelope wrapping a single attestation. The depot uses
/// zpp_bits to decode; `google::protobuf::Message::SerializeToArray`
/// produces wire-format-compatible bytes that zpp_bits accepts.
std::vector<char> encode_envelope_with_one_attestation(
   uint32_t epoch_index,
   sysio::opp::types::AttestationType att_type,
   const std::string& att_data)
{
   sysio::opp::Envelope env;
   env.set_epoch_index(epoch_index);
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1'775'612'516'983ULL);

   auto* msg     = env.add_messages();
   auto* payload = msg->mutable_payload();
   auto* att     = payload->add_attestations();
   att->set_type(att_type);
   att->set_data(att_data);
   att->set_data_size(static_cast<uint32_t>(att_data.size()));

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

/// Render an EM public key into its canonical contract string —
/// "PUB_EM_" + hex(compressed_33_bytes). Mirrors the WASM-side
/// `sysio::pubkey_to_string` in `sysio.authex.hpp`.
std::string contract_em_pubkey_to_string(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return "PUB_EM_" + fc::to_hex(compressed.data(), compressed.size());
}

/// Build the createlink message string exactly as `sysio.authex::createlink`
/// composes it on-chain. Off-chain signer signs `keccak(msg)` for EM keys.
std::string build_link_message(
   const fc::crypto::public_key& pub_key,
   const std::string& account,
   sysio::opp::types::ChainKind chain_kind,
   uint64_t nonce)
{
   auto pub_key_str = contract_em_pubkey_to_string(pub_key);
   auto chain_kind_str = std::to_string(magic_enum::enum_integer(chain_kind));
   return pub_key_str + "|" + account + "|" + chain_kind_str + "|" +
          std::to_string(nonce) + "|createlink auth";
}

/// Extract the raw 33-byte compressed pubkey from an EM `public_key`.
/// Returned as `std::vector<char>` to fit directly into a
/// `ChainAddress.address` proto field.
std::vector<char> em_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return std::vector<char>(compressed.begin(), compressed.end());
}

/// Encode an OperatorAction attestation payload. The new schema carries the
/// operator's outpost-chain identity via `op_address` (chain_kind + raw
/// pubkey bytes); msgch dispatches by resolving that pubkey back to a WIRE
/// account name through `sysio.authex::links`'s `bypubkey` index.
std::string encode_operator_action(
   sysio::opp::attestations::OperatorAction_ActionType action_type,
   sysio::opp::types::ChainKind chain_kind,
   const std::vector<char>& op_pubkey_bytes,
   sysio::opp::types::TokenKind token_kind,
   int64_t amount)
{
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(action_type);

   auto* op_address = oa.mutable_op_address();
   op_address->set_kind(chain_kind);
   op_address->set_address(op_pubkey_bytes.data(), op_pubkey_bytes.size());

   auto* amt = oa.mutable_amount();
   amt->set_kind(token_kind);
   amt->set_amount(amount);

   std::string out;
   oa.SerializeToString(&out);
   return out;
}

} // anonymous namespace

class sysio_dispatch_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto UWRIT_ACCOUNT  = "sysio.uwrit"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto RESERV_ACCOUNT = "sysio.reserv"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto UWRIT_OP       = "uwrit.alice"_n;

   sysio_dispatch_tester() {
      produce_blocks(2);

      // `sysio.authex` is auto-created at genesis by the testing framework
      // (see `tester::create_accounts_for_<...>` in libraries/testing) — it
      // sits alongside `sysio` / `sysio.token` / etc as a system account.
      // Trying to `create_accounts` it again raises
      // `account_name_exists_exception`, so we just set_code/set_abi onto
      // the pre-existing account in `deploy()`.
      create_accounts({
         MSGCH_ACCOUNT, OPREG_ACCOUNT, UWRIT_ACCOUNT, EPOCH_ACCOUNT,
         RESERV_ACCOUNT, CHALG_ACCOUNT, TOKEN_ACCOUNT,
         BATCHOP, UWRIT_OP
      });
      produce_blocks(2);

      // Deploy each contract + privilege.
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(UWRIT_ACCOUNT,  contracts::uwrit_wasm(),  contracts::uwrit_abi(),  uwrit_abi);
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(RESERV_ACCOUNT, contracts::reserve_wasm(), contracts::reserve_abi(), reserv_abi);
      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(), contracts::authex_abi(), authex_abi);
      // chalg + token are referenced (auth checks, account name constants);
      // their full deployment isn't needed for the dispatch surface tested
      // here — the test fixture omits them and only registers the accounts.

      // Cross-contract permission grant — mirrors the production cluster's
      // bootstrap-time `updateauth` grant in `wire-tools-ts/.../
      // ClusterManager.ts`. Only `opreg.active` actually needs a delegation:
      // msgch's `dispatch_operator_action` declares
      // `permission_level{opreg, active}` (because opreg::depositinle /
      // opreg::withdrawinle both `require_auth(get_self()=opreg)`), so the
      // chain's inline-send check needs opreg.active to accept msgch's
      // `sysio.code`. All other cross-contract paths in the dispatch tree
      // — uwrit's calls to opreg::releaselock, epoch's to opreg::flushwtdw,
      // chalg's to opreg::slash, msgch's to uwrit::* — work without
      // delegation because those callees already `require_auth(caller)`.
      grant_code_authors(OPREG_ACCOUNT, {MSGCH_ACCOUNT});

      produce_blocks();
   }

   /// Deploy a contract + capture its abi_serializer for action encoding.
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

   void grant_code_authors(name account, const std::vector<name>& code_authors) {
      set_authority(account, config::active_name,
                    active_with_code_authors(account, code_authors),
                    config::owner_name);
   }

   /// Push an action against any contract using its captured abi_serializer.
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

   /// Generate an EM (secp256k1) key pair, sign a `createlink auth` message
   /// for `UWRIT_OP` against the Ethereum chain, push the createlink action,
   /// and cache the 33-byte compressed pubkey at `uwrit_op_eth_pubkey` so
   /// individual test cases can encode it into `OperatorAction.op_address`.
   ///
   /// Wire's authex contract signs and stores the **recovered** pubkey from
   /// the EM signature (the `verified_pub_key` in createlink), which is
   /// guaranteed to have the canonical y-parity prefix from libsecp256k1.
   /// We therefore re-derive the bytes from the contract's link record
   /// after the call, not from the locally generated key, so that the bytes
   /// we ship in `op_address` match what `bypubkey` indexed on the depot
   /// side.
   void create_uwrit_op_eth_authex_link() {
      using namespace fc::crypto;
      using namespace sysio::opp::types;

      auto priv = private_key::generate(private_key::key_type::em);
      auto pub  = priv.get_public_key();
      const uint64_t nonce = control->head().block_time().time_since_epoch().count() / 1000;

      auto msg = build_link_message(pub, UWRIT_OP.to_string(),
                                    ChainKind::CHAIN_KIND_ETHEREUM, nonce);
      auto msg_hash = keccak256::hash(msg);
      auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()),
                                      32));

      BOOST_REQUIRE_EQUAL(success(), push(AUTHEX_ACCOUNT, authex_abi, UWRIT_OP,
         "createlink"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_ETHEREUM)
            ("account",    UWRIT_OP.to_string())
            ("sig",        sig)
            ("pub_key",    pub)
            ("nonce",      nonce)));

      uwrit_op_eth_pubkey = em_pubkey_bytes(pub);
   }

   /// Bootstrap epoch + opreg with the minimum config that pins
   /// operators_per_group=1 (so a single deliver = consensus). Then register
   /// `BATCHOP` as a bootstrapped batch operator (so it lands in the active
   /// group via initgroups), `UWRIT_OP` as an underwriter (PENDING — its
   /// status is irrelevant for dispatch tests, only its existence matters
   /// for opreg::depositinle's `operator not found` check), bootstrap the
   /// UWRIT_OP↔Ethereum authex link (so msgch's `op_address` → WIRE-name
   /// resolution succeeds), register an Ethereum outpost, and advance the
   /// epoch to populate the consensus state.
   void bootstrap_for_dispatch() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  60)
            ("operators_per_epoch",                 1)
            ("batch_operator_minimum_active",       1)
            ("batch_op_groups",                     1)
            ("epoch_retention_envelope_log_count",  200)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("max_available_producers",   21)
            ("max_available_batch_ops",   63)
            ("max_available_underwriters",21)
            ("terminate_prune_delay_ms",  600000)));

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

      // Create UWRIT_OP's Ethereum authex link so msgch's inbound dispatch
      // can resolve `op_address` (33-byte EM compressed pubkey) back to
      // `UWRIT_OP`. Mirrors the harness flow in
      // `wire-tools-ts/.../ClusterManager.ts` Phase 19a and the unit-test
      // pattern in `sysio.authex_tests.cpp::make_eth_link`.
      create_uwrit_op_eth_authex_link();

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "regoutpost"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_ETHEREUM)
            ("chain_id",   31337)));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "initgroups"_n, mvo()));

      // Genesis advance — permissionless so anyone can sign; epoch just
      // needs the call to set current_epoch_index to 1.
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "advance"_n, mvo()));

      produce_blocks();
   }

   /// Deliver an envelope from the active batch operator. With
   /// operators_per_group=1, this is enough to reach consensus and trigger
   /// dispatch inline.
   action_result deliver(uint64_t outpost_id, const std::vector<char>& data) {
      return push(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "deliver"_n, mvo()
         ("batch_op_name", BATCHOP.to_string())
         ("outpost_id",    outpost_id)
         ("data",          data));
   }

   /// Read the running WIRE epoch index from epoch_state.
   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT,
                                     "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
   }

   /// Look up an opreg operator row by account name.
   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                     "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Look up an opreg wtdwqueue row by request_id (auto-incremented).
   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "withdraw_request", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Locate an operator's balance entry for a (chain, token_kind) pair.
   /// `balances` is a flat vector — scan it. Each row's typed enum is read
   /// out of the variant via `as<EnumT>()` (FC_REFLECT_ENUM in
   /// `sysio/opp/opp.hpp` provides the from_variant overloads).
   fc::variant find_balance(const fc::variant& op, ChainKind chain, TokenKind token_kind) {
      const auto& arr = op["balances"].get_array();
      for (const auto& b : arr) {
         if (b["chain"].as<ChainKind>() == chain &&
             b["token_kind"].as<TokenKind>() == token_kind) {
            return b;
         }
      }
      return fc::variant();
   }

   abi_serializer msgch_abi, opreg_abi, uwrit_abi, epoch_abi, reserv_abi, authex_abi;

   /// Captured during `bootstrap_for_dispatch` after `UWRIT_OP` registers an
   /// authex link for Ethereum. Holds the 33-byte EM compressed pubkey so
   /// `encode_operator_action` callers can populate `op_address.address`
   /// with bytes that msgch's `bypubkey` lookup will resolve back to
   /// `UWRIT_OP`.
   std::vector<char> uwrit_op_eth_pubkey;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_dispatch_tests)

/// End-to-end: an OPERATOR_ACTION(DEPOSIT_REQUEST) attestation arriving from
/// the Ethereum outpost is decoded, dispatched into `opreg::depositinle`, and
/// credits a balance row on the underwriter. Verifies the inbound dispatch
/// branch + the inline-permission grant on opreg.
BOOST_FIXTURE_TEST_CASE(dispatch_routes_deposit_to_opreg, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t DEPOSIT_AMOUNT = 1'000'000;

   auto operator_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_ETHEREUM,
      uwrit_op_eth_pubkey,
      sysio::opp::types::TOKEN_KIND_ETH,
      DEPOSIT_AMOUNT);

   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(),
      sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
      operator_payload);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*outpost_id=*/0, envelope));

   // Side-effect assertion: opreg now has an ETH balance row for UWRIT_OP
   // with the deposited amount. The presence of this row is the proof that
   // dispatch_attestation routed correctly into opreg::depositinle.
   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   auto bal = find_balance(op, ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH);
   BOOST_REQUIRE(!bal.is_null());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(DEPOSIT_AMOUNT),
                       bal["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// End-to-end: an OPERATOR_ACTION(WITHDRAW_REQUEST) attestation arriving
/// from the outpost is dispatched into `opreg::withdrawinle`. The underwriter
/// must already have a sufficient balance — bootstrap a deposit first via
/// the same dispatch path so the test exercises both branches.
BOOST_FIXTURE_TEST_CASE(dispatch_routes_withdraw_request_to_opreg, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t INITIAL_DEPOSIT = 5'000'000;
   constexpr int64_t WITHDRAW_AMOUNT = 2'000'000;

   // Deposit first, so available() covers the withdraw.
   auto deposit_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_ETHEREUM,
      uwrit_op_eth_pubkey,
      sysio::opp::types::TOKEN_KIND_ETH,
      INITIAL_DEPOSIT);
   BOOST_REQUIRE_EQUAL(success(), deliver(/*outpost_id=*/0,
      encode_envelope_with_one_attestation(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
         deposit_payload)));

   // Now an inbound WITHDRAW_REQUEST for a portion of the balance.
   auto wtdw_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST,
      sysio::opp::types::CHAIN_KIND_ETHEREUM,
      uwrit_op_eth_pubkey,
      sysio::opp::types::TOKEN_KIND_ETH,
      WITHDRAW_AMOUNT);
   BOOST_REQUIRE_EQUAL(success(), deliver(/*outpost_id=*/0,
      encode_envelope_with_one_attestation(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
         wtdw_payload)));

   // Side-effect: a row appears in opreg's wtdwqueue at request_id=1.
   auto row = get_wtdw(/*request_id=*/1);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(UWRIT_OP.to_string(),  row["account"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(WITHDRAW_AMOUNT),
                       row["amount"].as_uint64());
   BOOST_REQUIRE(ChainKind::CHAIN_KIND_ETHEREUM == row["chain"].as<ChainKind>());
   BOOST_REQUIRE(TokenKind::TOKEN_KIND_ETH      == row["token_kind"].as<TokenKind>());
} FC_LOG_AND_RETHROW() }

/// Negative case: unknown attestation types fall through silently. The
/// envelope is still recorded and consensus still advances, but no
/// downstream action runs. Pick `STAKE` — Task 4 explicitly defers staking
/// dispatch to a later task, so its branch is the canonical no-op branch.
BOOST_FIXTURE_TEST_CASE(dispatch_silently_drops_out_of_scope_types, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   // STAKE attestation with an empty payload — dispatch_attestation must
   // hit the fall-through arm and not crash the consensus.
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(),
      sysio::opp::types::ATTESTATION_TYPE_STAKE,
      std::string{});

   BOOST_REQUIRE_EQUAL(success(), deliver(/*outpost_id=*/0, envelope));

   // No opreg side-effect — operator's balances vector remains empty.
   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   const auto& balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(0u, balances.size());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

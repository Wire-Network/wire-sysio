/// Cross-contract dispatch tests for sysio.msgch's per-attestation-type
/// routing (Task 4 of the operator-collateral plan).
///
/// Data model: identity moved to slug_name-keyed registries. The dispatch
/// surface still routes `OPERATOR_ACTION` payloads into opreg, but the
/// payload schema now carries `chain_code` (slug_name uint64) instead of a
/// `ChainKind chain` field, and `TokenAmount.token_code` (slug_name uint64)
/// instead of `TokenAmount.kind` (TokenKind enum).

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain/kv_table_objects.hpp>   // kv_index / by_code_key for reading sysio.roa kv tables
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>
// The depot's swap kernel — tests re-derive both the quote settlement must pay
// and (via token_to_wire) the challenge-bond reference math.
#include <sysio.opp.common/amm_math.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <magic_enum/magic_enum.hpp>

#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>

#include "contracts.hpp"
#include "external_chain_simulator.hpp"
#include "contract_test_support.hpp"
#include "test_symbol.hpp"
// Canonical-encoding + header-derivation oracle: inbound envelopes must carry
// spec-derived semantic headers or apply_consensus drops them before dispatch.
#include "opp_envelope_oracle.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;
using sysio_system::test_support::codename_mvo;
using sysio_system::test_support::sign_createlink;

namespace {

constexpr uint64_t PROTOBUF_VARINT_PAYLOAD_MASK = 0x7fu;
constexpr uint32_t PROTOBUF_VARINT_PAYLOAD_BITS = 7u;
constexpr uint8_t  PROTOBUF_VARINT_CONTINUATION_BIT = 0x80u;
constexpr uint32_t PROTOBUF_FIELD_TAG_SHIFT = 3u;

/// The outpost custody a SyndicateLIQ or LIQYield fixture carries by default: the whole asset range, at
/// or above any outstanding shadow a dispatch test can reach.
constexpr uint64_t CUSTODY_COVERING_ANY_SUPPLY = static_cast<uint64_t>(sysio::chain::asset::max_amount);

/// One `chain_min_bond` entry for `sysio.opreg::setconfig`'s `req_*_collat`
/// vectors. `config_timestamp_ms` is stamped by the contract, so 0 here.
inline fc::variant chain_min_bond_mvo(std::string_view chain_code,
                                      std::string_view token_code,
                                      uint64_t min_bond) {
   return fc::variant(mvo()
      ("chain_code",          chain_code)
      ("token_code",          token_code)
      ("min_bond",            min_bond)
      ("config_timestamp_ms", uint64_t{0}));
}

/// One attestation of an envelope under construction: its type and its encoded payload.
using typed_attestation = std::pair<sysio::opp::types::AttestationType, std::string>;

/// Encode an Envelope wrapping `attestations` in order, each with its own type — the
/// general form the single-type encoders below delegate to. Used to fit several
/// attestations into a single delivery, since the depot deduplicates
/// per-(batch_op, outpost, epoch) — a second `deliver` from the same batch op in the
/// same epoch reverts as a duplicate.
/// Later epochs supply the accepted envelope digest and message ID to continue both inbound chains.
std::vector<char> encode_envelope_with_mixed_attestations(
   uint32_t epoch_index,
   const std::vector<typed_attestation>& attestations,
   const std::string& previous_envelope_hash = {},
   const std::string& previous_message_id = {})
{
   sysio::opp::Envelope env;
   env.set_epoch_index(epoch_index);
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1'775'612'516'983ULL);
   env.set_previous_envelope_hash(previous_envelope_hash);

   auto* msg     = env.add_messages();
   auto* payload = msg->mutable_payload();
   for (const auto& [att_type, att_data] : attestations) {
      auto* att = payload->add_attestations();
      att->set_type(att_type);
      att->set_data(att_data);
      att->set_data_size(static_cast<uint32_t>(att_data.size()));
   }

   oracle::finalize_header(*env.mutable_messages(0), previous_message_id, 1'775'612'516'983ULL);

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

/// Encode an Envelope wrapping N attestations of the same type.
std::vector<char> encode_envelope_with_attestations(
   uint32_t epoch_index,
   sysio::opp::types::AttestationType att_type,
   const std::vector<std::string>& att_datas,
   const std::string& previous_envelope_hash = {},
   const std::string& previous_message_id = {})
{
   std::vector<typed_attestation> attestations;
   attestations.reserve(att_datas.size());
   for (const auto& d : att_datas) attestations.emplace_back(att_type, d);
   return encode_envelope_with_mixed_attestations(
      epoch_index, attestations, previous_envelope_hash, previous_message_id);
}

/// Encode an Envelope wrapping a single attestation.
std::vector<char> encode_envelope_with_one_attestation(
   uint32_t epoch_index,
   sysio::opp::types::AttestationType att_type,
   const std::string& att_data)
{
   return encode_envelope_with_attestations(epoch_index, att_type, {att_data});
}

/// Mirrors the contract-internal `MAX_ENVELOPE_BYTES` protocol cap (32 KiB, shared with the
/// Ethereum and Solana outpost implementations). The contract constant lives in the msgch
/// translation unit — contract headers are not host-compilable — so tests keep this manual
/// mirror, same as the outbound packing tests in sysio.msgch_tests.cpp.
constexpr size_t MAX_ENVELOPE_BYTES = 32'768;

/// Encode a decodable envelope whose serialised size is EXACTLY `target_bytes`, padded with a
/// single out-of-scope STAKE attestation (dispatch drops it with no value-bearing effect). Probe
/// once with `target_bytes` of padding to measure the fixed protobuf overhead, then rebuild with
/// the pad shrunk by that overhead: at sizes near the 32 KiB envelope cap every nested length
/// prefix and the `data_size` varint sit in the same 3-byte width band (16 KiB .. 2 MiB), so the
/// second pass lands exactly on target — the final REQUIRE pins it.
std::vector<char> encode_envelope_padded_to(uint32_t epoch_index, size_t target_bytes) {
   auto probe = encode_envelope_with_one_attestation(
      epoch_index, sysio::opp::types::ATTESTATION_TYPE_STAKE, std::string(target_bytes, 'x'));
   BOOST_REQUIRE_GT(probe.size(), target_bytes);
   const size_t overhead = probe.size() - target_bytes;
   auto padded = encode_envelope_with_one_attestation(
      epoch_index, sysio::opp::types::ATTESTATION_TYPE_STAKE,
      std::string(target_bytes - overhead, 'x'));
   BOOST_REQUIRE_EQUAL(target_bytes, padded.size());
   return padded;
}

/// Extract the raw 33-byte compressed pubkey from an EM `public_key`.
std::vector<char> em_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return std::vector<char>(compressed.begin(), compressed.end());
}

/// Extract the 65-byte uncompressed EVM key emitted by BAR for NodeOwnerRegistration.
std::vector<char> em_uncompressed_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto uncompressed = shim.unwrapped().serialize_uncompressed();
   return std::vector<char>(uncompressed.begin(), uncompressed.end());
}

/// Encode an OperatorAction attestation payload (schema).
/// `chain_code` and `amount.token_code` are slug_name-packed uint64 values.
std::string encode_operator_action(
   sysio::opp::attestations::OperatorAction_ActionType action_type,
   sysio::opp::types::ChainKind op_address_chain,
   const std::vector<char>& op_pubkey_bytes,
   uint64_t chain_code_v,
   uint64_t token_code_v,
   int64_t amount)
{
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(action_type);

   auto* op_address = oa.mutable_op_address();
   op_address->set_kind(op_address_chain);
   op_address->set_address(op_pubkey_bytes.data(), op_pubkey_bytes.size());

   oa.set_chain_code(chain_code_v);

   auto* amt = oa.mutable_amount();
   amt->set_token_code(token_code_v);
   amt->set_amount(amount);

   std::string out;
   oa.SerializeToString(&out);
   return out;
}

/// Extract the raw 33-byte compressed point from a K1 `public_key` (no variant-index prefix) --
/// the form `WireKey.key` carries for a `WIRE_KEY_TYPE_K1` key. fc packs a K1 public_key as
/// [1-byte variant index 0][33-byte point]; strip the index byte.
std::vector<char> k1_pubkey_bytes(const fc::crypto::public_key& pk) {
   auto packed = fc::raw::pack(pk);
   BOOST_REQUIRE_EQUAL(packed.size(), 34u);   // index(1) + compressed point(33)
   return std::vector<char>(packed.begin() + 1, packed.end());
}

/// Encode a NodeOwnerRegistration attestation payload: the Wire account name + tier, the new
/// account's owner/active key as a `WireKey` (key_type + raw bytes), and the depositor's ETH key.
/// Leaves `actor.kind` unset because the exact source-outpost binding proves the chain and actor is
/// metadata only; `actor.address` remains populated so tests can prove it is not trusted as identity.
std::string encode_node_owner_registration(
   const std::string& account,
   uint32_t tier,
   sysio::opp::types::WireKeyType wire_key_type,
   const std::vector<char>& wire_key_bytes,
   const std::vector<char>& eth_pubkey_bytes,
   const std::vector<uint8_t>& eth_address)
{
   sysio::opp::attestations::NodeOwnerRegistration reg;
   reg.mutable_account()->set_name(account);
   reg.set_tier(tier);
   reg.set_actor_pub_key(eth_pubkey_bytes.data(), eth_pubkey_bytes.size());
   auto* actor = reg.mutable_actor();
   actor->set_address(eth_address.data(), eth_address.size());
   auto* wk = reg.mutable_wire_pub_key();
   wk->set_key_type(wire_key_type);
   wk->set_key(wire_key_bytes.data(), wire_key_bytes.size());

   std::string out;
   reg.SerializeToString(&out);
   return out;
}

/// Encode a SyndicateLIQ attestation payload: the emitting outpost, the syndicating user's
/// native pubkey (kind + bytes), the liq TokenAmount, the per-outpost sequence and the outpost's
/// live custody total.
std::string encode_syndicate_liq(uint64_t chain_code_v,
                                 sysio::opp::types::ChainKind user_kind,
                                 const std::vector<char>& user_pubkey,
                                 uint64_t token_code_v, int64_t amount, uint64_t sequence,
                                 uint64_t total_syndicated = CUSTODY_COVERING_ANY_SUPPLY)
{
   sysio::opp::attestations::SyndicateLIQ synd;
   synd.set_chain_code(chain_code_v);
   auto* user = synd.mutable_user();
   user->set_kind(user_kind);
   user->set_address(user_pubkey.data(), user_pubkey.size());
   auto* amt = synd.mutable_amount();
   amt->set_token_code(token_code_v);
   amt->set_amount(amount);
   synd.set_sequence(sequence);
   synd.set_total_syndicated(total_syndicated);

   std::string out;
   synd.SerializeToString(&out);
   return out;
}

/// Encode a LIQYield attestation payload: the outpost's claimed yield in its liq token,
/// the per-outpost sequence it shares with SyndicateLIQ, the outpost epoch of the report and the
/// outpost's live custody total.
std::string encode_liq_yield(uint64_t chain_code_v, uint64_t token_code_v, int64_t amount,
                             uint64_t sequence, uint64_t epoch,
                             uint64_t total_syndicated = CUSTODY_COVERING_ANY_SUPPLY)
{
   sysio::opp::attestations::LIQYield report;
   report.set_chain_code(chain_code_v);
   auto* amt = report.mutable_amount();
   amt->set_token_code(token_code_v);
   amt->set_amount(amount);
   report.set_sequence(sequence);
   report.set_epoch(epoch);
   report.set_total_syndicated(total_syndicated);

   std::string out;
   report.SerializeToString(&out);
   return out;
}

/// Encode a DesyndicateLIQ attestation payload — a depot -> outpost type; the tests echo one
/// inbound to prove the depot drops it.
std::string encode_desyndicate_liq(uint64_t chain_code_v,
                                   sysio::opp::types::ChainKind user_kind,
                                   const std::vector<char>& user_pubkey,
                                   uint64_t token_code_v, int64_t amount, uint64_t request_id)
{
   sysio::opp::attestations::DesyndicateLIQ desynd;
   desynd.set_chain_code(chain_code_v);
   auto* user = desynd.mutable_user();
   user->set_kind(user_kind);
   user->set_address(user_pubkey.data(), user_pubkey.size());
   auto* amt = desynd.mutable_amount();
   amt->set_token_code(token_code_v);
   amt->set_amount(amount);
   desynd.set_request_id(request_id);

   std::string out;
   desynd.SerializeToString(&out);
   return out;
}

} // anonymous namespace

/// Wire layout of an `auth.msg::onlinkauth` action, the payload sysio.system once acted on.
struct onlinkauth_notification {
   name            user;
   name            permission;
   public_key_type pub_key;
};
FC_REFLECT(onlinkauth_notification, (user)(permission)(pub_key))

class sysio_dispatch_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;

   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto DCLAIM_ACCOUNT = "sysio.dclaim"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto ROA_ACCOUNT    = "sysio.roa"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto UWRIT_OP       = "uwrit.alice"_n;
   // Pre-created claim account for the NodeOwnerRegistration test: a fresh single-key account, so
   // nodeownreg's active_key_matches succeeds when the claim carries that same key (existing-account
   // path -- exercises the dispatch decode + routing without the account-creation machinery).
   static constexpr auto CLAIM_ACCOUNT  = "claimacct"_n;
   static constexpr uint64_t ROA_NETWORK_GEN = 0;

   sysio_dispatch_tester() {
      produce_blocks(2);

      create_accounts({
         MSGCH_ACCOUNT, OPREG_ACCOUNT, EPOCH_ACCOUNT,
         CHALG_ACCOUNT, TOKEN_ACCOUNT, CHAINS_ACCOUNT,
         DCLAIM_ACCOUNT, BATCHOP, UWRIT_OP
      });
      // CLAIM_ACCOUNT with NO roa policy (include_roa_policy=false) so regnodeowner exercises the
      // fresh create-branch of increase_reslimit. (A pre-existing reslimit row would now be reconciled,
      // not rejected -- SEC-087 -- but this dispatch test keeps the clean create path.) include_code=true
      // leaves the standard <account>@sysio.code on active, which exercises active_key_matches against a
      // real (non-single-entry) authority.
      create_account(CLAIM_ACCOUNT, config::system_account_name,
                     /*multisig=*/false, /*include_code=*/true, /*include_roa_policy=*/false);
      produce_blocks(2);

      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),   contracts::msgch_abi(),   msgch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),   contracts::opreg_abi(),   opreg_abi);
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),   contracts::epoch_abi(),   epoch_abi);
      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(),  contracts::authex_abi(),  authex_abi);
      deploy(DCLAIM_ACCOUNT, contracts::dclaim_wasm(),  contracts::dclaim_abi(),  dclaim_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(),  contracts::chains_abi(),  chains_abi);
      // sysio.roa is a genesis system account already running this build's code (active, with the
      // sysio.acct policy), so re-deploying it would fail set_exact_code. Just load its on-chain abi
      // for the kv table reads below.
      {
         const auto* roa_acct = control->find_account_metadata(ROA_ACCOUNT);
         BOOST_REQUIRE(roa_acct != nullptr);
         abi_def roa_parsed;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(roa_acct->abi, roa_parsed), true);
         roa_abi.set_abi(std::move(roa_parsed),
                         abi_serializer::create_yield_function(abi_serializer_max_time));
      }

      // Production uses privileged system contracts and installs no cross-contract active grants.
      // deploy() already marked msgch privileged; explicitly preserve the genesis ROA privilege so
      // both msgch -> roa and roa -> authex exercise that exact authorization path.
      set_privileged(ROA_ACCOUNT);

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

   action_result push(name contract, abi_serializer& ser, name signer,
                      name action_name, const fc::variant_object& data) {
      return sysio_system::test_support::push_contract_action(
         *this, contract, ser, signer, action_name, data);
   }

   /** Push one ABI-encoded action and retain its trace/console output. */
   transaction_trace_ptr push_trace(name contract, abi_serializer& ser, name signer,
                                    name action_name, const fc::variant_object& data) {
      return sysio_system::test_support::push_contract_action_trace(
         *this, contract, ser, signer, action_name, data);
   }

   std::vector<char> create_eth_authex_link(name account) {
      return create_eth_authex_link(account, fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em));
   }

   /// Link `account` to the EM key behind `priv` through a user-created `createlink`.
   std::vector<char> create_eth_authex_link(name account, const fc::crypto::private_key& priv) {
      using namespace fc::crypto;
      using namespace sysio::opp::types;

      auto pub  = priv.get_public_key();
      const uint64_t nonce = control->head().block_time().time_since_epoch().count() / 1000;
      auto sig = sign_createlink(priv, account.to_string(), ChainKind::CHAIN_KIND_EVM, nonce);

      BOOST_REQUIRE_EQUAL(success(), push(AUTHEX_ACCOUNT, authex_abi, account,
         "createlink"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_EVM)
            ("account",    account.to_string())
            ("sig",        sig)
            ("pub_key",    pub)
            ("nonce",      nonce)));

      return em_pubkey_bytes(pub);
   }

   void create_uwrit_op_eth_authex_link() {
      uwrit_op_eth_pubkey = create_eth_authex_link(UWRIT_OP);
   }

   /// Push `sysio.opreg::setconfig` with the dispatch-suite defaults, varying
   /// the underwriter and (optionally) producer collateral requirements. Batch
   /// minimums stay empty (those operators are bootstrapped or unused here). The
   /// race resolver gates winner selection on ACTIVE UNDERWRITER, and
   /// `req_uw_collat` is what promotes UWRIT_OP to ACTIVE via
   /// `opreg::processuw`; the eligibility-gate tests tune these to make a
   /// candidate ACTIVE (a funded producer) or keep one inactive while funded.
   /// `prune_delay_ms` defaults to 10 minutes — far beyond any test's wall
   /// clock, so `prune` cases that want to exercise a gate OTHER than the delay
   /// lower it explicitly.
   action_result opreg_setconfig_collat(const fc::variants& req_uw_collat,
                                        const fc::variants& req_prod_collat = fc::variants{},
                                        uint64_t prune_delay_ms = 600000) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers",          21)
         ("max_available_batch_ops",          63)
         ("max_available_underwriters",       21)
         ("terminate_prune_delay_ms",         prune_delay_ms)
         ("terminate_max_consecutive_misses", 5)
         ("terminate_max_pct_misses_24h",     5)
         ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
         ("req_prod_collat",                  req_prod_collat)
         ("req_batchop_collat",               fc::variants{})
         ("req_uw_collat",                    req_uw_collat));
   }

   // `outpost_code` / `outpost_kind` name the single bootstrapped outpost. They default to the EVM
   // "ETH" chain used by the deposit/withdraw/swap/underwrite cases. The node-owner happy path passes
   // "ETHEREUM" so the source it binds against (msgch's NODE_OWNER_SRC_CHAIN) is the scheduled outpost
   // and reaches consensus; the non-EVM drop case passes an SVM "SOLANA" so its delivery also reaches
   // consensus and the drop is exercised at the source binding, not merely the consensus gate. The
   // outpost is registered before `schbatchgps`, so the batch op is scheduled for it.
   void bootstrap_for_dispatch(const std::string& outpost_code = "ETH",
                               ChainKind outpost_kind = ChainKind::CHAIN_KIND_EVM) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  60)
            ("operators_per_epoch",                 1)
            ("batch_operator_minimum_active",       1)
            ("batch_op_groups",                     1)
            ("epoch_retention_envelope_log_count",  200)));

      // A 1-unit ETH/ETH underwriter minimum: every swap-race test funds
      // UWRIT_OP's ETH bond, so this promotes it to ACTIVE via
      // `opreg::processuw` (it registers UNKNOWN — underwriters cannot be
      // bootstrapped). The race resolver now gates winner selection on ACTIVE
      // UNDERWRITER, so the happy-path winner tests need a genuinely-active
      // underwriter. Adds NO balance row — deposit-routing assertions (exact /
      // zero balances) are unaffected.
      BOOST_REQUIRE_EQUAL(success(),
         opreg_setconfig_collat(fc::variants{chain_min_bond_mvo("ETH", "ETH", 1)}));

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

      create_uwrit_op_eth_authex_link();

      // Chains are first-class registry rows.
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              outpost_kind)
            ("code",              outpost_code)
            ("external_chain_id", 31337)
            ("name",              std::string("outpost-test"))
            ("description",       std::string{})
            ("outpost", sysio_system::test_support::no_outpost_mvo())));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "schbatchgps"_n, mvo()));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "advance"_n, mvo()));

      produce_blocks();
   }

   action_result deliver(uint64_t chain_code, const std::vector<char>& data) {
      return push(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "deliver"_n, mvo()
         ("batch_op_name", BATCHOP.to_string())
         ("chain_code",    chain_code)
         ("data",          data));
   }

   transaction_trace_ptr deliver_trace(uint64_t chain_code,
                                       const std::vector<char>& data) {
      return push_trace(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "deliver"_n, mvo()
         ("batch_op_name", BATCHOP.to_string())
         ("chain_code",    chain_code)
         ("data",          data));
   }

   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT,
                                     "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
   }

   /// Permissionless consensus-and-time crank that triggers sysio.epoch::advance.
   action_result chkcons() {
      return push(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "chkcons"_n, mvo());
   }

   /// Read the sysio.epoch `blocklog` row for `epoch_index` (written by advance()'s emissions gate
   /// when it blocks). `retry_count` counts how many times advance re-attempted and re-blocked.
   fc::variant get_blocklog(uint32_t epoch_index) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "blocklog"_n, epoch_index);
      return data.empty() ? fc::variant() : epoch_abi.binary_to_variant(
         "blocklog_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
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

   /// Read a `sysio.msgch` inbound `envelopes` row (empty variant when absent).
   fc::variant get_envelope(uint64_t id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envelopes"_n, id);
      return data.empty() ? fc::variant() : msgch_abi.binary_to_variant(
         "envelope_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// The first queued outbound attestation of `attest_type`, or a null variant.
   /// `queueout` mints sequential ids, so a bounded forward scan finds any a test
   /// queued.
   fc::variant find_queued_attestation(const char* type_name, uint32_t type_value,
                                       uint64_t scan_until = 32) {
      for (uint64_t id = 0; id <= scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         // `type` is an AttestationType; accept either rendering so the helper
         // survives a change in how the enum reflects.
         const auto& t = row["type"];
         if ((t.is_string()  && t.as_string() == type_name) ||
             (t.is_integer() && t.as_uint64() == type_value)) return row;
      }
      return fc::variant();
   }

   // Read sysio.roa's kv tables (scoped by network_gen). `nodeowners` proves registration;
   // `nodeownerreg` is the audit row (status / reject_reason).
   fc::variant get_nodeowner(name acc) {
      const auto& db = control->db();
      auto key = chain::make_kv_scoped_key(ROA_NETWORK_GEN, acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA_ACCOUNT,
                  chain::compute_table_id(name("nodeowners").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end() && it->value.size()) {
         std::vector<char> data(it->value.data(), it->value.data() + it->value.size());
         return roa_abi.binary_to_variant("nodeowners", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      return fc::variant();
   }

   fc::variant get_nodeownerreg(name acc) {
      const auto& db = control->db();
      auto key = chain::make_kv_scoped_key(ROA_NETWORK_GEN, acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA_ACCOUNT,
                  chain::compute_table_id(name("nodeownerreg").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end() && it->value.size()) {
         std::vector<char> data(it->value.data(), it->value.data() + it->value.size());
         return roa_abi.binary_to_variant("nodeownerreg", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      return fc::variant();
   }

   fc::variant get_dclaim_row(name table, const char* type, uint64_t id) {
      auto data = get_row_by_id(DCLAIM_ACCOUNT, DCLAIM_ACCOUNT, table, id);
      return data.empty() ? fc::variant()
         : dclaim_abi.binary_to_variant(
              type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /** Produce a complete serialized fixed-size variant for a shim type. */

   // ── SEC-129 / WSA-223: real epoch-aging helpers ──────────────────────────
   //
   // The bootstrap advance gate-blocks on missing emissions state, so every
   // dispatch test normally runs at epoch 0. The UWREQ lifecycle sweep is
   // trigger-driven off real advances, so its tests configure emissions the
   // way emissions_tests.cpp does and then genuinely advance the epoch index.

   /// Push a sysio.system action (ABI resolved from chain state — the system
   /// account runs this build's genesis code, like sysio.roa above).
   action_result push_system(name signer, name action_name, const fc::variant_object& data) {
      try {
         base_tester::push_action(config::system_account_name, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   /// Deploy the full sysio.system contract (the genesis `sysio` account runs
   /// only the boot contract), create the T5 holding accounts payepoch
   /// transfers WIRE to, and set the emission config — i.e. everything
   /// `enable_epoch_advancement` does EXCEPT `initt5`. Split out so the
   /// emissions gate's "emitcfg present, t5state missing"
   /// (EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED) state is reachable on its
   /// own; every test that wants a genuinely-advancing epoch calls
   /// `enable_epoch_advancement` instead. Values mirror emissions_tests.cpp's
   /// defaults.
   void deploy_system_with_emitcfg() {
      set_code(config::system_account_name, contracts::system_wasm());
      set_abi(config::system_account_name, contracts::system_abi().data());
      BOOST_REQUIRE_EQUAL(success(), push_system(config::system_account_name, "init"_n,
         mvo()("version", 0)("core", "4,SYS")));
      produce_blocks();
      for (auto a : {"sysio.dclaim"_n, "sysio.gov"_n, "sysio.batch"_n, "sysio.ops"_n}) {
         if (!control->db().find<account_object, by_name>(a)) {
            create_accounts({a}, /*multisig=*/false, /*include_code=*/false,
                            /*include_roa_policy=*/false, /*include_ram_gift=*/true);
         }
      }
      produce_blocks();
      constexpr uint32_t seconds_per_month = 30u * 24u * 60u * 60u;
      BOOST_REQUIRE_EQUAL(success(), push_system(config::system_account_name, "setemitcfg"_n,
         mvo()("cfg", mvo()
            ("t1_allocation",             int64_t{7'500'000'000'000'000LL})
            ("t2_allocation",             int64_t{1'000'000'000'000'000LL})
            ("t3_allocation",             int64_t{100'000'000'000'000LL})
            ("t1_duration",               12u * seconds_per_month)
            ("t2_duration",               24u * seconds_per_month)
            ("t3_duration",               36u * seconds_per_month)
            ("min_claimable",             int64_t{10'000'000'000LL})
            ("t5_distributable",          int64_t{375'000'000'000'000'000LL})
            ("t5_floor",                  int64_t{125'000'000'000'000'000LL})
            ("target_annual_decay_bps",   uint16_t(6940))
            ("annual_initial_emission",   int64_t{563'150'000'000'000LL} * 365)
            ("annual_max_emission",       int64_t{3'000'000'000'000'000LL} * 365)
            ("annual_min_emission",       int64_t{100'000'000'000'000LL} * 365)
            ("compute_bps",               uint16_t(4000))
            ("capex_bps",                 uint16_t(2000))
            ("governance_bps",            uint16_t(1000))
            ("producer_bps",              uint16_t(7000))
            ("batch_op_bps",              uint16_t(3000))
            ("standby_end_rank",          uint32_t(28))("standby_bps", uint16_t(800))
            ("epoch_log_retention_count", uint32_t(8640))
            ("pay_cadence_epochs",        uint16_t(1)))));
      produce_blocks();
   }

   /// Make `sysio.epoch::advance` genuinely advance: everything
   /// `deploy_system_with_emitcfg` sets up, plus the t5 state the gate needs
   /// past its STATE_UNINITIALIZED check. Requires
   /// setup_wire_token() first (payepoch pays WIRE out of sysio's
   /// token balance).
   void enable_epoch_advancement() {
      deploy_system_with_emitcfg();
      BOOST_REQUIRE_EQUAL(success(), push_system(config::system_account_name, "initt5"_n,
         mvo()("start_time", time_point_sec(control->head().block_time()))));
      produce_blocks();
   }

   /// Cross one epoch boundary and advance. epoch_duration_sec is 60 in this
   /// fixture (bootstrap_for_dispatch); 124 half-second blocks = 62s crosses
   /// it. advance is pushed with the epoch contract's own authority (advance
   /// accepts sysio.msgch OR sysio.epoch post-genesis). Each successful
   /// advance fires the real inline maintenance chain — chklocks,
   /// pruneuwreqs(MAX_UWREQ_PRUNE_PER_EPOCH), drainfwq, buildenv — exactly as
   /// in production.
   void age_one_epoch() {
      produce_blocks(124);
      BOOST_REQUIRE_EQUAL(success(),
         push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "advance"_n, mvo()));
   }

   // ── sysio.synd inbound routing (SYNDICATE_LIQ / LIQ_YIELD) ────────────────

   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static inline const symbol LIQETH_SYM = symbol::from_string("9,LIQETH");
   /// One whole liq token in the depot's 9-decimal frame.
   static constexpr int64_t LIQ_UNIT = 1'000'000'000;

   /// Register `code` on sysio.tokens as `kind` at the depot's 9-decimal precision and bind
   /// it to `chain_code` (EVM address bytes; the registries only check the length).
   action_result regtoken(TokenKind kind, std::string_view code, std::string_view chain_code,
                          ChainKind family = ChainKind::CHAIN_KIND_EVM) {
      const std::vector<char> addr(family == ChainKind::CHAIN_KIND_SVM ? 32 : 20, '\x5a');
      auto r = push(TOKENS_ACCOUNT, tokens_abi, TOKENS_ACCOUNT, "regtoken"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("symbol_name", std::string(code))
         ("description", std::string{})("precision", 9)
         ("address", mvo()("kind", family)("address", addr)));
      if (r != success()) return r;
      return push(TOKENS_ACCOUNT, tokens_abi, TOKENS_ACCOUNT, "regctok"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(code))
         ("contract_addr", addr)("is_native", false));
   }

   /// Deploy sysio.tokens, sysio.liq and sysio.synd and register the bootstrapped outpost's liq
   /// token ("LIQETH" on ETH) with its shadow, plus two tokens the syndication intake must refuse: a
   /// plain ERC20 ("USDCETH") and a liq token nobody opened a shadow for ("LIQTWO").
   /// `bootstrap_for_dispatch` must have run first — registrations inside the epoch-0
   /// bootstrap window land ACTIVE.
   void setup_liq_for_dispatch(std::string_view chain = "ETH", std::string_view token = "LIQETH",
                               ChainKind family = ChainKind::CHAIN_KIND_EVM) {
      create_accounts({TOKENS_ACCOUNT, LIQ_ACCOUNT, SYND_ACCOUNT});
      produce_blocks();
      deploy(TOKENS_ACCOUNT, contracts::tokens_wasm(), contracts::tokens_abi(), tokens_abi);
      deploy(LIQ_ACCOUNT,    contracts::liq_wasm(),    contracts::liq_abi(),    liq_abi);
      deploy(SYND_ACCOUNT,   contracts::synd_wasm(),   contracts::synd_abi(),   synd_abi);
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ,   token, chain, family));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ,   "LIQTWO", chain, family));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_ERC20, "USDCETH", chain, family));
      BOOST_REQUIRE_EQUAL(success(), push(LIQ_ACCOUNT, liq_abi, LIQ_ACCOUNT, "create"_n, mvo()
         ("sym", symbol::from_string("9," + std::string(token)))("chain_code", codename_mvo(chain))
         ("token_code", codename_mvo(token))));
      produce_blocks();
   }

   fc::variant liq_row(name table, const char* type, name scope, uint64_t id) {
      auto data = get_row_by_id(LIQ_ACCOUNT, scope, table, id);
      return data.empty() ? fc::variant() : liq_abi.binary_to_variant(
         type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// `holder`'s LIQETH shadow balance; 0 without a row.
   int64_t liq_balance(name holder, symbol token = LIQETH_SYM) {
      const auto row = liq_row("accounts"_n, "account", holder, token.to_symbol_code().value);
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }

   /// The LIQETH shadow supply; 0 without a stat row.
   int64_t liq_supply(symbol token = LIQETH_SYM) {
      const auto row = liq_row("stat"_n, "currency_stats", LIQ_ACCOUNT, token.to_symbol_code().value);
      return row.is_null() ? 0 : row["supply"].as<asset>().get_amount();
   }

   /// LIQETH yield reported by the outpost and not yet queued to the swap; 0 without a row.
   int64_t liq_pending(symbol token = LIQETH_SYM) {
      const auto row = liq_row("liqpending"_n, "pending_yield", LIQ_ACCOUNT, token.to_symbol_code().value);
      return row.is_null() ? 0 : row["quantity"].as<asset>().get_amount();
   }

   /// Mint `amount` LIQETH shadow to `holder`, signed as sysio.synd, the ledger's only minter.
   action_result mint_shadow(name holder, uint64_t amount) {
      return push(LIQ_ACCOUNT, liq_abi, SYND_ACCOUNT, "mint"_n, mvo()
         ("to", holder)("token_code", codename_mvo("LIQETH"))("amount", amount));
   }

   fc::variant synd_decode(const char* type, const std::vector<char>& data) {
      return data.empty() ? fc::variant() : synd_abi.binary_to_variant(
         type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// sysio.synd's kv rows of `table`, in key order, as raw values.
   std::vector<std::vector<char>> synd_rows(name table) {
      const auto& kv_idx   = control->db().get_index<kv_index, by_code_key>();
      const auto  table_id = compute_table_id(table.to_uint64_t());
      std::vector<std::vector<char>> rows;
      for (auto itr = kv_idx.lower_bound(boost::make_tuple(SYND_ACCOUNT, table_id, std::string_view{}));
           itr != kv_idx.end() && itr->code == SYND_ACCOUNT && itr->table_id == table_id; ++itr)
         rows.emplace_back(itr->value.data(), itr->value.data() + itr->value.size());
      return rows;
   }

   /// Every held item, in arrival order.
   std::vector<fc::variant> synd_items() {
      std::vector<fc::variant> items;
      for (const auto& value : synd_rows("items"_n)) items.push_back(synd_decode("item_row", value));
      return items;
   }

   /// The sysio.synd envelope row of `(chain_code, token_code, epoch)`; null when there is none.
   fc::variant synd_envelope(std::string_view chain_code, std::string_view token_code, uint32_t epoch) {
      for (const auto& value : synd_rows("envelopes"_n)) {
         const auto row = synd_decode("envelope_row", value);
         if (row["chain_code"].as_string() == chain_code && row["token_code"].as_string() == token_code &&
             row["epoch_index"].as<uint32_t>() == epoch)
            return row;
      }
      return fc::variant();
   }

   /// The per-outpost inbound cursor (`last_sequence`, `last_epoch`); null before any message is held.
   fc::variant synd_cursor(std::string_view chain_code) {
      return synd_decode("synd_cursor", get_row_by_id(SYND_ACCOUNT, SYND_ACCOUNT, "syndcursors"_n,
                                                      fc::slug_name{chain_code}.value));
   }

   /// The canonical digest of the last envelope sysio.msgch accepted from `chain_code`.
   fc::sha256 accepted_envelope_digest(uint64_t chain_code) {
      const auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outpcons"_n, chain_code);
      BOOST_REQUIRE(!data.empty());
      return msgch_abi.binary_to_variant("outpost_consensus_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time))["envelope_digest"].as<fc::sha256>();
   }

   /// The data of every sysio.synd `action_name` action `trace` executed, decoded, in execution order.
   std::vector<fc::variant> synd_action_data(const transaction_trace_ptr& trace, name action_name) {
      std::vector<fc::variant> decoded;
      for (const auto& at : trace->action_traces)
         if (at.receiver == SYND_ACCOUNT && at.act.account == SYND_ACCOUNT && at.act.name == action_name)
            decoded.push_back(synd_decode(action_name.to_string().c_str(), at.act.data));
      return decoded;
   }

   /// The sysio.synd actions `trace` executed, in execution order.
   static std::vector<name> synd_actions(const transaction_trace_ptr& trace) {
      std::vector<name> actions;
      for (const auto& at : trace->action_traces)
         if (at.receiver == SYND_ACCOUNT && at.act.account == SYND_ACCOUNT) actions.push_back(at.act.name);
      return actions;
   }

   /// Every action's console in `trace`, inline actions included: msgch's own drops print on
   /// `deliver`, a downstream contract's on the inline action msgch sent it.
   static std::string all_console(const transaction_trace_ptr& trace) {
      std::string console;
      for (const auto& action_trace : trace->action_traces) {
         console += action_trace.console;
      }
      return console;
   }

   void setup_wire_token() {
      deploy(TOKEN_ACCOUNT, contracts::token_wasm(), contracts::token_abi(), token_abi);
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", "sysio")("maximum_supply", "1000000000.000000000 WIRE")));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
         "issue"_n, mvo()("to", "sysio")("quantity", "1000000000.000000000 WIRE")("memo", "seed")));
   }

   abi_serializer msgch_abi, opreg_abi, epoch_abi, authex_abi, dclaim_abi,
                  chains_abi, roa_abi, token_abi, tokens_abi, liq_abi, synd_abi;

   std::vector<char> uwrit_op_eth_pubkey;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_dispatch_tests)

BOOST_FIXTURE_TEST_CASE(dispatch_silently_drops_out_of_scope_types, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const auto eth_code = fc::slug_name{"ETH"}.value;
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(),
      sysio::opp::types::ATTESTATION_TYPE_STAKE,
      std::string{});

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));

   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   const auto& balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(0u, balances.size());
} FC_LOG_AND_RETHROW() }

// A second `deliver` from the SAME operator for the same outpost+epoch must REVERT, not land as a
// recorded no-op: a reverted transaction is never included in a block and bills no CPU/NET, whereas
// the previous soft print-and-return shape charged the operator and consumed block space for zero
// state change. Matching deliveries from DISTINCT operators are not duplicates -- they are the
// consensus tally itself (covered by the dispute/consensus suites).
BOOST_FIXTURE_TEST_CASE(deliver_duplicate_from_same_operator_reverts, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const auto eth_code = fc::slug_name{"ETH"}.value;
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(),
      sysio::opp::types::ATTESTATION_TYPE_STAKE,
      std::string{});

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));
   // Cross a block boundary so the re-submission is a distinct transaction —
   // an identical push in the same block is rejected as tx_duplicate before
   // the contract runs, which would mask the guard under test.
   produce_blocks();
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("operator already delivered for this outpost+epoch"),
      deliver(/*chain_code=*/eth_code, envelope));
} FC_LOG_AND_RETHROW() }

// The inbound `deliver` boundary enforces the same 32 KiB protocol envelope cap the outbound
// `buildenv` packer obeys (and that the Ethereum/Solana outposts enforce on their side): a
// decodable current-epoch envelope one byte over the cap must revert before anything is hashed
// or stored. Without the contract-level cap, the generic chain ceilings (~512 KiB inline-action,
// 256 KiB KV value) would admit inbound envelopes WIRE's own packer could never emit.
BOOST_FIXTURE_TEST_CASE(deliver_oversized_envelope_reverts, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const auto eth_code = fc::slug_name{"ETH"}.value;
   auto oversized = encode_envelope_padded_to(current_epoch(), MAX_ENVELOPE_BYTES + 1);

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("inbound envelope exceeds MAX_ENVELOPE_BYTES"),
      deliver(/*chain_code=*/eth_code, oversized));
   // Reverted before the emplace: no envelope row landed.
   BOOST_REQUIRE(get_envelope(1).is_null());
} FC_LOG_AND_RETHROW() }

// Boundary companion: an envelope exactly AT the cap is accepted and stored (and, with this
// suite's single-operator group, immediately reaches consensus and dispatches). Guards against
// an off-by-one regression turning the cap check exclusive.
BOOST_FIXTURE_TEST_CASE(deliver_envelope_at_size_cap_succeeds, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const auto eth_code = fc::slug_name{"ETH"}.value;
   auto boundary = encode_envelope_padded_to(current_epoch(), MAX_ENVELOPE_BYTES);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, boundary));
   auto row = get_envelope(1);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(BATCHOP.to_string(), row["batch_op_name"].as_string());
   BOOST_REQUIRE_EQUAL(current_epoch(), row["epoch_index"].as<uint32_t>());
} FC_LOG_AND_RETHROW() }

// NodeOwnerRegistration: msgch decodes the attestation and inline-sends sysio.roa::newnameduser then
// nodeownreg. CLAIM_ACCOUNT pre-exists with a single-key active, so newnameduser no-ops and the
// claim's matching wire key drives nodeownreg's existing-account path to CONFIRMED (registers the
// owner and inline-records the depositor's ETH link in sysio.authex). Exercises the full dispatch:
// proto decode (account name + WireKey + ETH key) -> routing -> both roa actions -> recordlink.
BOOST_FIXTURE_TEST_CASE(dispatch_routes_node_owner_reg_to_roa, sysio_dispatch_tester) { try {
   // Node-owner NFT deposits originate on the Ethereum outpost (code "ETHEREUM", matching the launch
   // bootstrap config and msgch's NODE_OWNER_SRC_CHAIN). Bootstrap it as the scheduled source outpost
   // so the delivery below reaches consensus and dispatches.
   bootstrap_for_dispatch("ETHEREUM");

   const auto eth_code = fc::slug_name{"ETHEREUM"}.value;
   // The claim must carry CLAIM_ACCOUNT's own active key so nodeownreg's active_key_matches passes.
   auto wire_key = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   // BAR supplies the depositor's ETH key as an uncompressed 65-byte point.
   auto eth_pub = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_uncompressed_pubkey_bytes(eth_pub);
   auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_pub);

   // Seed a pre-link reward at the key-derived address, then deliberately put a conflicting address
   // in the redundant actor field. Dispatch must derive from actor_pub_key and sweep the real row.
   const std::vector<char> native_address(eth_address.begin(), eth_address.end());
   BOOST_REQUIRE_EQUAL(success(), push(DCLAIM_ACCOUNT, dclaim_abi, MSGCH_ACCOUNT, "onreward"_n, mvo()
      ("chain_code", eth_code)("staker_wire_account", std::string{})
      ("reward_chain", ChainKind::CHAIN_KIND_EVM)("staker_native_addr", native_address)
      ("reward_amount", uint64_t{4321})("reward_epoch_index", uint32_t{7})
      ("external_epoch_ref", uint64_t{100})("share_bps", uint32_t{10000})));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   std::fill(eth_address.begin(), eth_address.end(), uint8_t{0xA5});

   auto payload = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes, eth_address);
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));

   // Registered at the claimed tier, audited CONFIRMED.
   auto reg = get_nodeowner(CLAIM_ACCOUNT);
   BOOST_REQUIRE(!reg.is_null());
   BOOST_REQUIRE_EQUAL(reg["tier"].as<uint32_t>(), 2u);
   auto audit = get_nodeownerreg(CLAIM_ACCOUNT);
   BOOST_REQUIRE(!audit.is_null());
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(),
                       sysio_system::test_support::nodeownerreg::status_confirmed);
   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   auto pending = get_dclaim_row("pclaims"_n, "pending_claim", CLAIM_ACCOUNT.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 4321);
} FC_LOG_AND_RETHROW() }

// Only ATTESTATION_TYPE_NODE_OWNER_REG reaches the node-owner handler. The outbound-only types
// (OPERATORS, BATCH_OPERATOR_GROUPS, SWAP_REVERT, DEPOSIT_REVERT) are dropped on receipt even when
// the payload is a well-formed NodeOwnerRegistration from the node-owner source outpost: a payload
// that would register CLAIM_ACCOUNT under the real type must leave no owner row and no audit row.
BOOST_FIXTURE_TEST_CASE(outbound_only_types_never_reach_node_owner_dispatch, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch("ETHEREUM");

   const auto eth_code = fc::slug_name{"ETHEREUM"}.value;
   auto wire_key = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   auto eth_pub = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_uncompressed_pubkey_bytes(eth_pub);
   auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_pub);
   const auto payload = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes, eth_address);

   std::vector<typed_attestation> entries;
   for (auto type : {ATTESTATION_TYPE_OPERATORS, ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS,
                     ATTESTATION_TYPE_SWAP_REVERT, ATTESTATION_TYPE_DEPOSIT_REVERT}) {
      entries.emplace_back(type, payload);
   }
   const auto trace = deliver_trace(eth_code,
      encode_envelope_with_mixed_attestations(current_epoch(), entries));
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);

   BOOST_CHECK(get_nodeowner(CLAIM_ACCOUNT).is_null());
   BOOST_CHECK(get_nodeownerreg(CLAIM_ACCOUNT).is_null());
} FC_LOG_AND_RETHROW() }

/// One slot and two fresh-name claims must commit the first, reject the second, and keep epochs moving.
BOOST_FIXTURE_TEST_CASE(dispatch_node_owner_tier_cap_preserves_epoch_progress, sysio_dispatch_tester) { try {
   namespace owners = sysio_system::test_support::nodeowners;
   namespace audit = sysio_system::test_support::nodeownerreg;
   bootstrap_for_dispatch("ETHEREUM");
   setup_wire_token();
   enable_epoch_advancement();
   owners::fill_tier1(*this, roa_abi, owners::tier1_cap - 1);

   constexpr auto first = "claimb"_n;
   constexpr auto second = "claimc"_n;
   const auto eth_code = fc::slug_name{"ETHEREUM"}.value;
   const auto epoch = current_epoch();
   std::vector<std::string> claims;
   for (const auto owner : {first, second}) {
      const auto eth_pub = fc::crypto::private_key::generate(
         fc::crypto::private_key::key_type::em).get_public_key();
      claims.push_back(encode_node_owner_registration(
         owner.to_string(), owners::tier1, sysio::opp::types::WIRE_KEY_TYPE_K1,
         k1_pubkey_bytes(get_public_key(owner, "active")), em_uncompressed_pubkey_bytes(eth_pub),
         fc::crypto::ethereum::address_to_bytes(eth_pub)));
   }
   const auto envelope = encode_envelope_with_attestations(
      epoch, sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, claims);
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code, envelope));
   produce_block();

   BOOST_REQUIRE(!get_nodeowner(first).is_null());
   BOOST_CHECK_EQUAL(get_nodeownerreg(first)["status"].as<uint64_t>(), audit::status_confirmed);
   BOOST_REQUIRE(get_nodeowner(second).is_null());
   const auto rejected = get_nodeownerreg(second);
   BOOST_REQUIRE(!rejected.is_null());
   BOOST_CHECK_EQUAL(rejected["status"].as<uint64_t>(), audit::status_rejected);
   BOOST_CHECK_EQUAL(rejected["reason"].as<uint64_t>(), audit::reason_tier_cap_reached);
   BOOST_CHECK((control->db().find<account_object, by_name>(second) == nullptr));
   BOOST_CHECK_EQUAL(owners::count(*this, roa_abi, owners::tier1), owners::tier1_cap);
   BOOST_REQUIRE(!get_envelope(1).is_null());

   // Use the consensus crank, so a rolled-back consensus row cannot be hidden by a privileged advance.
   produce_block(fc::seconds(120));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_block();
   BOOST_REQUIRE_EQUAL(current_epoch(), epoch + 1);

   // A later epoch must also accept an envelope and reach consensus while the tier remains full.
   sysio::opp::Envelope accepted;
   BOOST_REQUIRE(accepted.ParseFromArray(envelope.data(), static_cast<int>(envelope.size())));
   const auto next = encode_envelope_with_attestations(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, {claims.back()},
      oracle::digest_bytes(oracle::epoch_digest(accepted)), accepted.messages(0).header().message_id());
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code, next));
   produce_block();
   BOOST_REQUIRE(!get_envelope(2).is_null());
   BOOST_CHECK_EQUAL(get_nodeownerreg(second)["reason"].as<uint64_t>(), audit::reason_tier_cap_reached);
   BOOST_CHECK((control->db().find<account_object, by_name>(second) == nullptr));
   produce_block(fc::seconds(120));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_block();
   BOOST_CHECK_EQUAL(current_epoch(), epoch + 2);
} FC_LOG_AND_RETHROW() }

// BAR's NodeOwnerRegistration contract emits a 65-byte uncompressed SEC1 key. A compressed EM key
// is well-formed protobuf but unusable identity input: dispatch must soft-drop it while committing
// the consensus envelope, with no sysio.roa registration or audit side effect.
BOOST_FIXTURE_TEST_CASE(node_owner_reg_with_compressed_actor_key_is_dropped, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch("ETHEREUM");
   const auto eth_code = fc::slug_name{"ETHEREUM"}.value;
   auto wire_key = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   auto eth_pub = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   auto compressed_eth_key = em_pubkey_bytes(eth_pub);
   auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_pub);
   auto payload = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, compressed_eth_key, eth_address);
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));
   BOOST_REQUIRE(!get_envelope(1).is_null());
   BOOST_REQUIRE(get_nodeowner(CLAIM_ACCOUNT).is_null());
   BOOST_REQUIRE(get_nodeownerreg(CLAIM_ACCOUNT).is_null());
} FC_LOG_AND_RETHROW() }

// WSA-005: node-owner registration is bound to the EXACT Ethereum source outpost (NODE_OWNER_SRC_CHAIN
// = "ETHEREUM"), not merely to the EVM family. A claim proven-delivered from a DIFFERENT active EVM
// outpost — here the fixture's "ETH" chain — is dropped, with no Wire account / node-owner state
// created. This is the precise hole a CHAIN_KIND_EVM family gate would leave open: a second, unrelated
// EVM operator quorum (Polygon / Base / Arbitrum / …) forging an NFT deposit the Ethereum outpost
// never saw. deliver() still reaches consensus, so the drop is the source binding, not a missing
// delivery. The matched-chain control is `dispatch_routes_node_owner_reg_to_roa` above.
BOOST_FIXTURE_TEST_CASE(node_owner_reg_from_other_evm_outpost_is_dropped, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // registers "ETH" — an EVM outpost, but NOT the node-owner source
   // Register the real node-owner source too, so the ONLY thing wrong below is the delivering outpost.
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_EVM)("code", "ETHEREUM")
      ("external_chain_id", 1)("name", std::string("ethereum-mainnet"))("description", std::string{})
      ("outpost", sysio_system::test_support::no_outpost_mvo())));
   const auto other_evm = fc::slug_name{"ETH"}.value;   // active EVM outpost, but not "ETHEREUM"

   auto wire_key  = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   auto eth_pub   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_uncompressed_pubkey_bytes(eth_pub);
   auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_pub);
   auto payload   = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes, eth_address);
   auto envelope  = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   // Proven outpost = "ETH" (EVM, but not "ETHEREUM"); the exact-chain binding drops it. deliver()
   // still succeeds (no throw) and reaches consensus.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/other_evm, envelope));

   // Nothing was sent to sysio.roa: no node-owner registration and no audit row.
   BOOST_REQUIRE(get_nodeowner(CLAIM_ACCOUNT).is_null());
   BOOST_REQUIRE(get_nodeownerreg(CLAIM_ACCOUNT).is_null());
} FC_LOG_AND_RETHROW() }

// WSA-005 (cross-VM-family case): NodeOwnerRegistration carries no chain code, so msgch binds it to the
// exact Ethereum source outpost. A registration proven-delivered from a NON-EVM outpost (SOLANA) is
// dropped too — complementing `node_owner_reg_from_other_evm_outpost_is_dropped` (wrong EVM chain).
BOOST_FIXTURE_TEST_CASE(node_owner_reg_from_non_evm_outpost_is_dropped, sysio_dispatch_tester) { try {
   // Bootstrap SOLANA (SVM) as the scheduled outpost so its delivery reaches consensus and the drop is
   // exercised at the source binding, not the consensus gate.
   bootstrap_for_dispatch("SOLANA", ChainKind::CHAIN_KIND_SVM);
   const auto sol_code = fc::slug_name{"SOLANA"}.value;

   auto wire_key  = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   auto eth_pub   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_uncompressed_pubkey_bytes(eth_pub);
   auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_pub);
   auto payload   = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes, eth_address);
   auto envelope  = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   // Proven outpost = SOLANA (SVM), not "ETHEREUM"; the exact-chain binding drops it. deliver() still
   // succeeds (no throw) and reaches consensus.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/sol_code, envelope));

   // Nothing was sent to sysio.roa: no node-owner registration and no audit row.
   BOOST_REQUIRE(get_nodeowner(CLAIM_ACCOUNT).is_null());
   BOOST_REQUIRE(get_nodeownerreg(CLAIM_ACCOUNT).is_null());
} FC_LOG_AND_RETHROW() }

/// Node-owner claims on a chain running sysio.system. A tier-2/3 claim lets the NFT holder pick any valid 1-12 char
/// name and control the account created for it; these cases pin what that control must not reach.
class node_owner_claim_tester : public sysio_dispatch_tester {
public:
   static constexpr auto     NODE_OWNER_SOURCE_CHAIN = "ETHEREUM";
   static constexpr uint32_t CLAIM_TIER              = 2;
   static constexpr auto     AUTH_MSG_ACCOUNT        = "auth.msg"_n;
   static constexpr auto     ONLINKAUTH_ACTION       = "onlinkauth"_n;
   static constexpr auto     AUTH_EXT_PERMISSION     = "auth.ext"_n;
   static constexpr auto     OTHER_PERMISSION        = "session"_n;
   static constexpr auto     RESERVED_SYSTEM_NAME    = "sysio.pwn"_n;
   static constexpr auto     PAYER_ACCOUNT           = "payer"_n;
   /// Never created; only names the key a notification offers.
   static constexpr auto     REPLACEMENT_KEY_NAME    = "replacement"_n;
   static constexpr auto     SYSTEM_INIT_ACTION      = "init"_n;
   static constexpr auto     ADDPOLICY_ACTION        = "addpolicy"_n;
   static constexpr auto     SELF_POLICY_WEIGHT      = "0.1000 SYS";

   /// Parent and authority of each of an account's permissions, keyed by permission name.
   using permission_set = std::map<name, std::pair<name, authority>>;

   /// Deploy and initialize sysio.system; the base dispatch fixture runs without it.
   void deploy_system_contract() {
      set_code(config::system_account_name, contracts::system_wasm());
      set_abi(config::system_account_name, contracts::system_abi().data());
      produce_block();
      base_tester::push_action(config::system_account_name, SYSTEM_INIT_ACTION, config::system_account_name,
                               mvo()("version", 0)("core", CORE_SYM_STR));
      produce_block();
   }

   /// Deliver a NodeOwnerRegistration for `account` through the Ethereum outpost, as BAR.commitNode emits it.
   void claim_node_owner(name account, const public_key_type& wire_key) {
      const auto eth_key = fc::crypto::private_key::generate(
         fc::crypto::private_key::key_type::em).get_public_key();
      const auto eth_address = fc::crypto::ethereum::address_to_bytes(eth_key);
      const auto payload = encode_node_owner_registration(
         account.to_string(), CLAIM_TIER, sysio::opp::types::WIRE_KEY_TYPE_K1,
         k1_pubkey_bytes(wire_key), em_uncompressed_pubkey_bytes(eth_key), eth_address);
      const auto envelope = encode_envelope_with_one_attestation(
         current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);
      BOOST_REQUIRE_EQUAL(success(), deliver(fc::slug_name{NODE_OWNER_SOURCE_CHAIN}.value, envelope));
      produce_blocks(2);
   }

   /// The claimant issues a policy to itself from its tier budget; PAYER_ACCOUNT pays for the transaction.
   void issue_own_policy(name owner) {
      signed_transaction trx;
      trx.actions.emplace_back(get_action(config::roa_account_name, ADDPOLICY_ACTION,
         vector<permission_level>{{PAYER_ACCOUNT, config::sysio_payer_name}, {PAYER_ACCOUNT, config::active_name},
                                  {owner, config::active_name}},
         mvo()("owner", owner)("issuer", owner)("net_weight", SELF_POLICY_WEIGHT)("cpu_weight", SELF_POLICY_WEIGHT)
              ("ram_weight", SELF_POLICY_WEIGHT)("time_block", 0)("network_gen", ROA_NETWORK_GEN)));
      set_transaction_headers(trx);
      trx.sign(get_private_key(PAYER_ACCOUNT, "active"), control->get_chain_id());
      trx.sign(get_private_key(owner, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }

   /// Code a claimant can deploy on its account: it forwards every action it receives to sysio as a notification.
   static std::vector<uint8_t> notify_system_account_wasm() {
      std::ostringstream wast;
      wast << R"((module
         (import "env" "require_recipient" (func $require_recipient (param i64)))
         (func (export "apply") (param i64 i64 i64)
            (call $require_recipient (i64.const 0x)"
           << std::hex << config::system_account_name.to_uint64_t() << R"())
         )
      ))";
      return wast_to_wasm(wast.str());
   }

   /// Push auth.msg::onlinkauth as auth.msg; true when the transaction committed and the notification reached sysio.
   bool notify_onlinkauth(name user, name permission, const public_key_type& key) {
      signed_transaction trx;
      trx.actions.emplace_back(vector<permission_level>{{AUTH_MSG_ACCOUNT, config::active_name}}, AUTH_MSG_ACCOUNT,
                               ONLINKAUTH_ACTION, fc::raw::pack(onlinkauth_notification{user, permission, key}));
      set_transaction_headers(trx);
      trx.sign(get_private_key(AUTH_MSG_ACCOUNT, "active"), control->get_chain_id());
      bool delivered = false;
      try {
         const auto trace = push_transaction(trx);
         delivered = std::ranges::any_of(trace->action_traces, [](const action_trace& at) {
            return at.receiver == config::system_account_name && at.act.name == ONLINKAUTH_ACTION;
         });
      } catch (const fc::exception&) {
         delivered = false;
      }
      produce_block();
      return delivered;
   }

   /// Native newaccount creates only the account_object; metadata appears once code, abi or privilege is set.
   bool account_exists(name account) const {
      return control->db().find<account_object, by_name>(account) != nullptr;
   }

   /// Every permission of `account`, for before/after comparison.
   permission_set permissions_of(name account) const {
      permission_set out;
      const auto& db  = control->db();
      const auto& idx = db.get_index<permission_index, by_owner>();
      for (auto it = idx.lower_bound(boost::make_tuple(account)); it != idx.end() && it->owner == account; ++it) {
         const name parent = it->parent._id == 0 ? name{} : db.get<permission_object, by_id>(it->parent).name;
         out.emplace(it->name, std::make_pair(parent, it->auth.to_authority()));
      }
      return out;
   }
};

// A tier-2/3 claim can take the name auth.msg and deploy code there, so an auth.msg::onlinkauth notification is
// attacker-controlled. It must reach sysio as an ordinary notification and change nothing: not sysio's owner, active,
// auth.ext or any other permission, and not a user's.
BOOST_FIXTURE_TEST_CASE(claimed_auth_msg_notification_changes_no_permission, node_owner_claim_tester) { try {
   bootstrap_for_dispatch(NODE_OWNER_SOURCE_CHAIN);
   create_account(PAYER_ACCOUNT);
   deploy_system_contract();

   claim_node_owner(AUTH_MSG_ACCOUNT, get_public_key(AUTH_MSG_ACCOUNT, "active"));
   const auto audit = get_nodeownerreg(AUTH_MSG_ACCOUNT);
   BOOST_REQUIRE(!audit.is_null());
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), sysio_system::test_support::nodeownerreg::status_confirmed);
   issue_own_policy(AUTH_MSG_ACCOUNT);
   set_code(AUTH_MSG_ACCOUNT, notify_system_account_wasm());
   produce_block();

   const auto replacement_key = get_public_key(REPLACEMENT_KEY_NAME, "active");
   const auto sysio_before    = permissions_of(config::system_account_name);
   const auto user_before     = permissions_of(CLAIM_ACCOUNT);
   for (const auto target : {config::system_account_name, CLAIM_ACCOUNT}) {
      for (const auto permission : {config::owner_name, config::active_name, AUTH_EXT_PERMISSION, OTHER_PERMISSION}) {
         BOOST_CHECK_MESSAGE(notify_onlinkauth(target, permission, replacement_key),
                             "onlinkauth for " << target.to_string() << "@" << permission.to_string()
                                               << " did not commit as a plain notification");
      }
   }
   BOOST_CHECK_MESSAGE(permissions_of(config::system_account_name) == sysio_before, "sysio permissions changed");
   BOOST_CHECK_MESSAGE(permissions_of(CLAIM_ACCOUNT) == user_before, "claimacct permissions changed");
} FC_LOG_AND_RETHROW() }

// No node owner may claim a name under the reserved sysio. prefix: the depot's claim is rejected as NAME_INVALID and
// no account is created.
BOOST_FIXTURE_TEST_CASE(node_owner_claim_rejects_reserved_system_name, node_owner_claim_tester) { try {
   bootstrap_for_dispatch(NODE_OWNER_SOURCE_CHAIN);
   deploy_system_contract();

   claim_node_owner(RESERVED_SYSTEM_NAME, get_public_key(RESERVED_SYSTEM_NAME, "active"));

   const auto audit = get_nodeownerreg(RESERVED_SYSTEM_NAME);
   BOOST_REQUIRE(!audit.is_null());
   BOOST_CHECK_EQUAL(audit["status"].as<uint64_t>(), sysio_system::test_support::nodeownerreg::status_rejected);
   BOOST_CHECK_EQUAL(audit["reason"].as<uint64_t>(), sysio_system::test_support::nodeownerreg::reason_name_invalid);
   BOOST_CHECK(get_nodeowner(RESERVED_SYSTEM_NAME).is_null());
   BOOST_CHECK(!account_exists(RESERVED_SYSTEM_NAME));
} FC_LOG_AND_RETHROW() }

/// Regression: a non-advancing advance() must not permanently strand the epoch.
///
/// When every active outpost has reached consensus and the wall clock has passed, chkcons triggers
/// sysio.epoch::advance. advance can legally return WITHOUT bumping the epoch when emissions are not
/// ready -- its emissions gate records a block and returns gracefully, it does not throw. The earlier
/// chkcons cleared per-outpost consensus_reached BEFORE calling advance, so that graceful return
/// committed the cleared state and nothing re-armed it (apply_consensus does not re-fire for an
/// already-complete delivery set) -- permanently stalling advancement even once emissions later became
/// ready.
///
/// This fixture never deploys sysio.system, so the emissions gate always blocks (CONFIG_MISSING):
/// exactly the non-advancing path. After consensus, each chkcons must re-attempt advance, which the
/// gate records as blocklog.retry_count. Pre-fix, the second chkcons bailed at the consensus gate
/// (retry_count would stay 1) and the epoch was stuck forever.
BOOST_FIXTURE_TEST_CASE(chkcons_survives_non_advancing_advance, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   const auto eth_code = fc::slug_name{"ETH"}.value;

   // This fixture deploys no sysio.system, so even the genesis advance in bootstrap gate-blocked; the
   // chain sits at `epoch0`. advance() always targets the next epoch, whose blocklog row counts gate
   // re-attempts.
   const uint32_t epoch0 = current_epoch();
   const uint32_t target = epoch0 + 1;

   // operators_per_epoch == 1, so a single delivery is Option-A unanimous consensus; apply_consensus
   // records the outpcons row for ETH at epoch0.
   auto operator_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM,
      uwrit_op_eth_pubkey,
      /*chain_code_v=*/ eth_code,
      /*token_code_v=*/ eth_code,
      /*amount=*/ 1'000'000);
   auto envelope = encode_envelope_with_one_attestation(
      epoch0,
      sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
      operator_payload);
   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));
   produce_blocks();   // land the deliver in its own block (this fixture's push does not)

   auto retry_count = [&]() -> uint32_t {
      auto bl = get_blocklog(target);
      return bl.is_null() ? 0u : bl["retry_count"].as<uint32_t>();
   };
   const uint32_t rc0 = retry_count();

   // Pass the wall clock (epoch_duration_sec == 60) so chkcons will fire advance, then refresh the head
   // so the subsequent push is stamped against the post-skip time and does not expire.
   produce_block(fc::seconds(120));
   produce_blocks();

   // First chkcons fires advance, which gate-blocks on missing emissions and returns without advancing.
   // The epoch is unchanged and the gate bumps retry_count -- confirming advance was actually attempted.
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(current_epoch(), epoch0);
   const uint32_t rc1 = retry_count();
   BOOST_REQUIRE_EQUAL(rc1, rc0 + 1);

   // REGRESSION: the per-outpost consensus signal must survive the non-advancing advance, so the second
   // chkcons re-attempts advance (retry_count -> rc1 + 1). Pre-fix, chkcons cleared consensus_reached
   // before calling advance, so this second call bailed at the consensus gate and never re-attempted --
   // retry_count would stay at rc1 and the epoch would be permanently stranded.
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(current_epoch(), epoch0);
   BOOST_REQUIRE_EQUAL(retry_count(), rc1 + 1);
} FC_LOG_AND_RETHROW() }

// ═════════════════════════════════════════════════════════════════════════
// msgch::bootstrap emissions guards. Missing emissions config is a bootstrap DEFECT, not an
// operational state: without these guards bootstrap's inline genesis advance would gate-block
// SILENTLY (the emissions gate never throws, by design) and the misconfiguration would surface
// only as "epoch stuck at 0". The three cases below cover both guards independently plus the
// fully-configured pass-through. advance()'s own soft block-and-retry behavior for the ECONOMIC
// gate reasons is covered above (chkcons_survives_non_advancing_advance) and in emissions_tests.
// ═════════════════════════════════════════════════════════════════════════

// Guard 1 — emitcfg missing. This fixture deploys no sysio.system at all (setemitcfg / initt5
// never ran), which is exactly the never-configured deployment shape.
BOOST_FIXTURE_TEST_CASE(bootstrap_requires_emissions_config, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("msgch::bootstrap: emissions config missing -- sysio.system::setemitcfg must run before bootstrap"),
      push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "bootstrap"_n, mvo()));
} FC_LOG_AND_RETHROW() }

// Guard 2 — emitcfg present, t5state missing (the half-configured deployment: setemitcfg ran,
// initt5 was forgotten). Proves the SECOND check is independently reachable and reads t5state,
// not emitcfg again: guard 1 passes here, so a wrong table/account lookup or a shadowed copy of
// the first check would surface as the WRONG diagnostic (or no throw at all).
BOOST_FIXTURE_TEST_CASE(bootstrap_requires_emissions_state, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   deploy_system_with_emitcfg();   // setemitcfg only -- initt5 deliberately not run
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("msgch::bootstrap: emissions state uninitialized -- sysio.system::initt5 must run before bootstrap"),
      push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "bootstrap"_n, mvo()));
} FC_LOG_AND_RETHROW() }

// Both guards pass — the genesis 0 -> 1 advance actually happens. Guards that always threw (or a
// gate that never passes) would look identical to guards 1 and 2 above, so this is the case that
// pins them as guards rather than a permanent bootstrap block.
BOOST_FIXTURE_TEST_CASE(bootstrap_advances_genesis_epoch_when_emissions_configured,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token();   // payepoch pays WIRE out of sysio's token balance
   enable_epoch_advancement();        // setemitcfg + initt5

   // bootstrap_for_dispatch's own advance ran while emissions were unconfigured, so it
   // gate-blocked and left a blocklog row for the target epoch.
   BOOST_REQUIRE_EQUAL(current_epoch(), 0u);
   BOOST_REQUIRE(!get_blocklog(1).is_null());

   BOOST_REQUIRE_EQUAL(success(),
      push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "bootstrap"_n, mvo()));
   produce_blocks();

   // The inline genesis advance passed the gate: epoch 0 -> 1, and the gate cleared the stale
   // blocklog row on its way through.
   BOOST_REQUIRE_EQUAL(current_epoch(), 1u);
   BOOST_REQUIRE(get_blocklog(1).is_null());
} FC_LOG_AND_RETHROW() }

// A forged/invalid delivery cannot strand the epoch. SEC-102's semantic-header check runs at
// INGRESS (msgch::deliver's inbound_envelope_valid gate), so a forged envelope reverts on delivery
// and records no envelope row -- it can never reach the consensus tally and leave a phantom
// consensus_reached with no outpcons row. A subsequent VALID delivery reaches consensus normally and
// chkcons proceeds to ATTEMPT advance (retry_count bumps) rather than waiting forever for a consensus
// row that an accepted-then-soft-dropped invalid winner would have left missing. Drives the
// production chkcons gate, not a direct epoch::advance.
BOOST_FIXTURE_TEST_CASE(forged_delivery_does_not_strand_chkcons, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   const auto eth_code = fc::slug_name{"ETH"}.value;
   const uint32_t epoch0 = current_epoch();
   const uint32_t target = epoch0 + 1;

   auto operator_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM,
      uwrit_op_eth_pubkey,
      /*chain_code_v=*/ eth_code,
      /*token_code_v=*/ eth_code,
      /*amount=*/ 1'000'000);
   const auto valid = encode_envelope_with_one_attestation(
      epoch0,
      sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
      operator_payload);

   // A forged copy: corrupt payload_checksum so the semantic header no longer recomputes.
   std::vector<char> forged;
   {
      sysio::opp::Envelope env;
      BOOST_REQUIRE(env.ParseFromArray(valid.data(), static_cast<int>(valid.size())));
      auto* h = env.mutable_messages(0)->mutable_header();
      std::string c = h->payload_checksum();
      c[0] ^= 0x01;
      h->set_payload_checksum(c);
      forged.resize(env.ByteSizeLong());
      env.SerializeToArray(forged.data(), static_cast<int>(forged.size()));
   }

   // Forged delivery is rejected at ingress -- nothing recorded, no phantom consensus.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(eth_code, forged));
   produce_blocks();

   auto retry_count = [&]() -> uint32_t {
      auto bl = get_blocklog(target);
      return bl.is_null() ? 0u : bl["retry_count"].as<uint32_t>();
   };

   // The valid delivery (operators_per_epoch == 1 => Option-A unanimous) reaches consensus normally.
   BOOST_REQUIRE_EQUAL(success(), deliver(eth_code, valid));
   produce_blocks();
   const uint32_t rc0 = retry_count();

   // Pass the wall clock so chkcons fires advance; confirm it ATTEMPTED advance (retry_count bumps),
   // i.e. it was NOT stranded waiting on a missing consensus row. Advance itself gate-blocks on
   // emissions (this fixture deploys no sysio.system), exactly as in chkcons_survives_non_advancing_advance.
   produce_block(fc::seconds(120));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(current_epoch(), epoch0);
   BOOST_REQUIRE_EQUAL(retry_count(), rc0 + 1);
} FC_LOG_AND_RETHROW() }

;

// ── SYNDICATE_LIQ / LIQ_YIELD -> sysio.synd ─────────────────────────────────

// SYNDICATE_LIQ routes to sysio.synd::onsynd with the envelope's identity, whether the user's key is
// AuthX-linked or not: sysio.synd holds every syndication as an item of the accepted envelope,
// minted into its own sysio.liq row, and no account is credited at intake. The per-outpost sequence
// is consumed only by a message that is held -- a replay is dropped, a gap is admitted. A malformed
// payload and an echoed DESYNDICATE_LIQ are dropped too, nothing aborts the envelope, and after the
// last attestation sysio.msgch closes the envelope with one `closeenv`.
BOOST_FIXTURE_TEST_CASE(msgch_routes_syndicate_liq_to_synd,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   // An EVM key nobody has linked yet.
   const auto stranger_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   const auto stranger     = em_pubkey_bytes(stranger_key.get_public_key());
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const uint32_t epoch = current_epoch();

   const auto env = encode_envelope_with_mixed_attestations(epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 5 * LIQ_UNIT, 1)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   encode_syndicate_liq(eth, EVM, stranger,            liqeth, 3 * LIQ_UNIT, 2)},
      // sequence 2 again: a replay, dropped by sysio.synd
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 4 * LIQ_UNIT, 2)},
      // not a SyndicateLIQ at all
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   std::string(1, '\x0a')},
      // a depot -> outpost type echoed back inbound
      {ATTESTATION_TYPE_DESYNDICATE_LIQ, encode_desyndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 1 * LIQ_UNIT, 77)},
      // a gap after the last admitted sequence is fine
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 1 * LIQ_UNIT, 9)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto console = all_console(trace);

   // Four onsynd (the malformed one never leaves msgch), then exactly one closeenv, last.
   const auto actions = synd_actions(trace);
   BOOST_REQUIRE_EQUAL(5u, actions.size());
   for (size_t i = 0; i < 4; ++i) BOOST_CHECK_EQUAL("onsynd"_n, actions[i]);
   BOOST_CHECK_EQUAL("closeenv"_n, actions[4]);
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onsynd: DROP -- replayed sequence"));

   // The linked and the unlinked key are held alike; nobody is credited.
   const auto items = synd_items();
   BOOST_REQUIRE_EQUAL(3u, items.size());
   BOOST_CHECK(uwrit_op_eth_pubkey == items[0]["pubkey"].as<std::vector<char>>());
   BOOST_CHECK(stranger == items[1]["pubkey"].as<std::vector<char>>());
   BOOST_CHECK(uwrit_op_eth_pubkey == items[2]["pubkey"].as<std::vector<char>>());
   BOOST_CHECK_EQUAL(5 * LIQ_UNIT, items[0]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(3 * LIQ_UNIT, items[1]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(1 * LIQ_UNIT, items[2]["amount"].as<int64_t>());
   for (const auto& item : items) {
      BOOST_CHECK_EQUAL("SYNDICATION", item["kind"].as_string());
      BOOST_CHECK_EQUAL("CHAIN_KIND_EVM", item["chain_kind"].as_string());
      BOOST_CHECK_EQUAL(epoch, item["epoch_index"].as<uint32_t>());
   }
   BOOST_CHECK_EQUAL(9 * LIQ_UNIT, liq_balance(SYND_ACCOUNT));
   BOOST_CHECK_EQUAL(0, liq_balance(UWRIT_OP));
   BOOST_CHECK_EQUAL(9 * LIQ_UNIT, liq_supply());
   BOOST_CHECK_EQUAL(0, liq_pending());
   const auto cursor = synd_cursor("ETH");
   BOOST_REQUIRE(!cursor.is_null());
   BOOST_CHECK_EQUAL(9u, cursor["last_sequence"].as<uint64_t>());
   BOOST_CHECK_EQUAL(0u, cursor["last_epoch"].as<uint64_t>());

   // The envelope carries the digest msgch accepted, and closeenv left it WAITING.
   const auto envelope = synd_envelope("ETH", "LIQETH", epoch);
   BOOST_REQUIRE(!envelope.is_null());
   BOOST_CHECK_EQUAL("WAITING", envelope["state"].as_string());
   BOOST_CHECK_EQUAL(9 * LIQ_UNIT, envelope["synd_total"].as<int64_t>());
   BOOST_CHECK_EQUAL(3u, envelope["item_count"].as<uint32_t>());
   BOOST_CHECK_EQUAL(accepted_envelope_digest(eth), envelope["digest"].as<fc::sha256>());

   // Linking the stranger's key later credits nothing at intake: the item stays held.
   create_accounts({"stranger"_n});
   produce_blocks();
   create_eth_authex_link("stranger"_n, stranger_key);
   BOOST_CHECK_EQUAL(0, liq_balance("stranger"_n));
   BOOST_CHECK_EQUAL(3u, synd_items().size());
   BOOST_CHECK_EQUAL(9 * LIQ_UNIT, liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW() }

// Retired payloads must be inert and must not prevent current LIQ traffic in
// the same envelope from reaching the syndication contract.
BOOST_FIXTURE_TEST_CASE(retired_attestations_do_not_mutate_collateral_or_block_liq,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();
   const auto eth = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   std::vector<typed_attestation> entries;
   for (auto type : {ATTESTATION_TYPE_OPERATOR_ACTION, ATTESTATION_TYPE_SWAP_REQUEST,
                     ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT,
                     ATTESTATION_TYPE_RESERVE_CREATE, ATTESTATION_TYPE_RESERVE_CREATE_CANCEL}) {
      entries.emplace_back(type, std::string(1, '\x0a'));
   }
   entries.emplace_back(ATTESTATION_TYPE_SYNDICATE_LIQ,
      encode_syndicate_liq(eth, ChainKind::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
                          liqeth, LIQ_UNIT, 1));
   const auto trace = deliver_trace(eth,
      encode_envelope_with_mixed_attestations(current_epoch(), entries));
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   BOOST_CHECK(get_operator(UWRIT_OP)["balances"].get_array().empty());
   BOOST_CHECK(get_wtdw(0).is_null());
   BOOST_REQUIRE_EQUAL(1u, synd_items().size());
   BOOST_CHECK_EQUAL(LIQ_UNIT, liq_supply());
   BOOST_CHECK_EQUAL(LIQ_UNIT, liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW() }

// sysio.msgch forwards each message's outpost custody total verbatim as the last argument of
// `onsynd` and `onyield`: every syndication and yield report of one envelope carries its own reading.
BOOST_FIXTURE_TEST_CASE(msgch_forwards_the_custody_total_to_synd,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   // The custody the outpost read after each lock and after the claim, 1 base unit apart from any
   // amount so a forwarded amount cannot pass for the total.
   const uint64_t first_total  = 5 * LIQ_UNIT + 1;
   const uint64_t second_total = 8 * LIQ_UNIT + 1;
   const uint64_t yield_total  = 10 * LIQ_UNIT + 1;

   const auto env = encode_envelope_with_mixed_attestations(current_epoch(), {
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 5 * LIQ_UNIT, 1, first_total)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 3 * LIQ_UNIT, 2, second_total)},
      {ATTESTATION_TYPE_LIQ_YIELD, encode_liq_yield(eth, liqeth, 2 * LIQ_UNIT, 3, 42, yield_total)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);

   const auto syndications = synd_action_data(trace, "onsynd"_n);
   BOOST_REQUIRE_EQUAL(2u, syndications.size());
   BOOST_CHECK_EQUAL(5 * LIQ_UNIT, syndications[0]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(first_total, syndications[0]["total_syndicated"].as_uint64());
   BOOST_CHECK_EQUAL(3 * LIQ_UNIT, syndications[1]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(second_total, syndications[1]["total_syndicated"].as_uint64());
   const auto reports = synd_action_data(trace, "onyield"_n);
   BOOST_REQUIRE_EQUAL(1u, reports.size());
   BOOST_CHECK_EQUAL(2 * LIQ_UNIT, reports[0]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(yield_total, reports[0]["total_syndicated"].as_uint64());
   // Forwarding changes nothing about intake: both syndications and the report are held.
   BOOST_CHECK_EQUAL(3u, synd_items().size());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_supply());
} FC_LOG_AND_RETHROW() }

// The user's key family must be the proven outpost's own. A Solana-family key inside an
// Ethereum envelope is dropped by msgch before it reaches sysio.synd, whether an account has linked
// it or not; the EVM syndication beside them is still held and is the only sequence consumed.
BOOST_FIXTURE_TEST_CASE(syndicate_liq_refuses_a_key_of_another_chain_family,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   constexpr auto SVM = ChainKind::CHAIN_KIND_SVM;
   // A Solana key the underwriter has linked, and one nobody has.
   const auto linked_sol_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed).get_public_key();
   const auto linked_sol_raw = linked_sol_key.get<fc::crypto::ed::public_key_shim>().serialize();
   const std::vector<char> linked_sol(linked_sol_raw.begin(), linked_sol_raw.end());
   BOOST_REQUIRE_EQUAL(success(), push(
      AUTHEX_ACCOUNT, authex_abi, AUTHEX_ACCOUNT, "recordlink"_n, mvo()
         ("account", UWRIT_OP)("chain_kind", SVM)("pub_key", linked_sol_key)("native_address", linked_sol)));
   produce_block();
   const auto stranger_sol_raw = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed)
                                    .get_public_key().get<fc::crypto::ed::public_key_shim>().serialize();
   const std::vector<char> stranger_sol(stranger_sol_raw.begin(), stranger_sol_raw.end());

   const auto env = encode_envelope_with_mixed_attestations(current_epoch(), {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, SVM, linked_sol,          liqeth, 5 * LIQ_UNIT, 1)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, SVM, stranger_sol,        liqeth, 3 * LIQ_UNIT, 2)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 2 * LIQ_UNIT, 3)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto console = all_console(trace);

   const auto actions = synd_actions(trace);
   BOOST_REQUIRE_EQUAL(2u, actions.size());
   BOOST_CHECK_EQUAL("onsynd"_n, actions[0]);
   BOOST_CHECK_EQUAL("closeenv"_n, actions[1]);
   const auto items = synd_items();
   BOOST_REQUIRE_EQUAL(1u, items.size());
   BOOST_CHECK(uwrit_op_eth_pubkey == items[0]["pubkey"].as<std::vector<char>>());
   BOOST_CHECK_EQUAL(2 * LIQ_UNIT, liq_balance(SYND_ACCOUNT));
   BOOST_CHECK_EQUAL(0, liq_balance(UWRIT_OP));
   BOOST_CHECK_EQUAL(2 * LIQ_UNIT, liq_supply());
   const auto cursor = synd_cursor("ETH");
   BOOST_REQUIRE(!cursor.is_null());
   BOOST_CHECK_EQUAL(3u, cursor["last_sequence"].as<uint64_t>());
   const std::string dropped = "msgch::dispatch_syndicate_liq: DROP attestation -- user kind CHAIN_KIND_SVM";
   const auto first = console.find(dropped);
   BOOST_REQUIRE_NE(std::string::npos, first);
   BOOST_CHECK_NE(std::string::npos, console.find(dropped, first + dropped.size()));
} FC_LOG_AND_RETHROW() }

// LIQ_YIELD routes to sysio.synd::onyield, which holds the amount as a number: nothing is minted and
// nothing pends, and the report's outpost epoch is stamped on the cursor. Every refusal is dropped
// without aborting the envelope: by msgch, a payload claiming another chain, a token that is not an
// active liq token (a plain ERC20, an unregistered code), a non-positive or oversized amount and a
// malformed payload; by sysio.synd, a liq token with no shadow, an unlinked pubkey of the wrong
// shape and a replayed sequence.
BOOST_FIXTURE_TEST_CASE(liq_yield_is_held_by_synd_and_refusals_are_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();

   const auto eth     = fc::slug_name{"ETH"}.value;
   const auto solana  = fc::slug_name{"SOLANA"}.value;
   const auto liqeth  = fc::slug_name{"LIQETH"}.value;
   const auto liqtwo  = fc::slug_name{"LIQTWO"}.value;
   const auto usdceth = fc::slug_name{"USDCETH"}.value;
   const auto liqnone = fc::slug_name{"LIQNONE"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   constexpr int64_t oversized = std::numeric_limits<int64_t>::max();
   const std::vector<char> evm_address(20, '\x0c');   // an address, not the 33-byte pubkey a link holds
   const uint32_t epoch = current_epoch();

   const auto env = encode_envelope_with_mixed_attestations(epoch, {
      {ATTESTATION_TYPE_LIQ_YIELD,     encode_liq_yield(eth, liqeth, 7 * LIQ_UNIT, 1, 42)},
      // claims another chain than the proven outpost
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(solana, EVM, uwrit_op_eth_pubkey, liqeth, 1 * LIQ_UNIT, 2)},
      // an ERC20, an unregistered code, a liq token without a shadow
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, usdceth, 1 * LIQ_UNIT, 3)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqnone, 1 * LIQ_UNIT, 4)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqtwo,  1 * LIQ_UNIT, 5)},
      // amounts the fail-closed gate refuses
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, -1,        6)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, oversized, 7)},
      // not a pubkey the chain family could ever link
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, evm_address, liqeth, 1 * LIQ_UNIT, 8)},
      // yield refusals: another chain, not a liq token, a replayed sequence, malformed
      {ATTESTATION_TYPE_LIQ_YIELD,     encode_liq_yield(solana, liqeth,  1 * LIQ_UNIT, 9,  43)},
      {ATTESTATION_TYPE_LIQ_YIELD,     encode_liq_yield(eth,    usdceth, 1 * LIQ_UNIT, 10, 43)},
      {ATTESTATION_TYPE_LIQ_YIELD,     encode_liq_yield(eth,    liqeth,  1 * LIQ_UNIT, 1,  44)},
      {ATTESTATION_TYPE_LIQ_YIELD,     std::string(1, '\x0a')},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto console = all_console(trace);

   const auto items = synd_items();
   BOOST_REQUIRE_EQUAL(1u, items.size());
   BOOST_CHECK_EQUAL("YIELD", items[0]["kind"].as_string());
   BOOST_CHECK_EQUAL(7 * LIQ_UNIT, items[0]["amount"].as<int64_t>());
   BOOST_CHECK_EQUAL(0, liq_pending());
   BOOST_CHECK_EQUAL(0, liq_supply());
   BOOST_CHECK_EQUAL(0, liq_balance(SYND_ACCOUNT));
   BOOST_CHECK_EQUAL(0, liq_balance(UWRIT_OP));
   const auto cursor = synd_cursor("ETH");
   BOOST_REQUIRE(!cursor.is_null());
   BOOST_CHECK_EQUAL(1u,  cursor["last_sequence"].as<uint64_t>());
   BOOST_CHECK_EQUAL(42u, cursor["last_epoch"].as<uint64_t>());
   const auto envelope = synd_envelope("ETH", "LIQETH", epoch);
   BOOST_REQUIRE(!envelope.is_null());
   BOOST_CHECK_EQUAL("WAITING", envelope["state"].as_string());
   BOOST_CHECK_EQUAL(7 * LIQ_UNIT, envelope["yield_total"].as<int64_t>());
   BOOST_CHECK_EQUAL(0u, envelope["synd_total"].as<uint64_t>());

   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_syndicate_liq: DROP attestation -- payload chain_code="));
   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_syndicate_liq: DROP attestation -- token is not an active liq token"));
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onsynd: DROP -- token_code has no shadow symbol"));
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onsynd: DROP -- pubkey does not fit the chain family"));
   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_liq_yield: DROP attestation -- payload chain_code="));
   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_liq_yield: DROP attestation -- token is not an active liq token"));
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onyield: DROP -- replayed sequence"));
} FC_LOG_AND_RETHROW() }

// Review Focus 3: an envelope whose attestations carry no syndication value -- here an echoed
// DESYNDICATE_LIQ and syndication messages msgch itself drops -- sends sysio.synd nothing, not even
// a `closeenv`, and leaves no row there.
BOOST_FIXTURE_TEST_CASE(an_envelope_without_syndication_value_touches_synd_not_at_all,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();

   const auto eth     = fc::slug_name{"ETH"}.value;
   const auto solana  = fc::slug_name{"SOLANA"}.value;
   const auto liqeth  = fc::slug_name{"LIQETH"}.value;
   const auto usdceth = fc::slug_name{"USDCETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const auto env = encode_envelope_with_mixed_attestations(current_epoch(), {
      {ATTESTATION_TYPE_DESYNDICATE_LIQ,
       encode_desyndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 1 * LIQ_UNIT, 77)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ,   encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, usdceth, 1 * LIQ_UNIT, 1)},
      {ATTESTATION_TYPE_LIQ_YIELD,       encode_liq_yield(solana, liqeth, 1 * LIQ_UNIT, 2, 1)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);

   BOOST_CHECK(synd_actions(trace).empty());
   for (auto table : { "envelopes"_n, "items"_n, "ledger"_n, "syndcursors"_n })
      BOOST_CHECK(synd_rows(table).empty());
   BOOST_CHECK(get_row_by_account(SYND_ACCOUNT, SYND_ACCOUNT, "syndcounters"_n, "syndcounters"_n).empty());
} FC_LOG_AND_RETHROW() }

// Without a token registry there is no active liq token, so the envelope is delivered and the
// attestations dropped before msgch would send anything to a sysio.liq that does not exist.
BOOST_FIXTURE_TEST_CASE(liq_attestations_are_dropped_without_a_token_registry,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   const auto env = encode_envelope_with_mixed_attestations(current_epoch(), {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, ChainKind::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
                                                            liqeth, 1 * LIQ_UNIT, 1)},
      {ATTESTATION_TYPE_LIQ_YIELD,     encode_liq_yield(eth, liqeth, 1 * LIQ_UNIT, 2, 1)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto console = all_console(trace);
   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_syndicate_liq: DROP attestation -- token is not an active liq token"));
   BOOST_CHECK_NE(std::string::npos, console.find(
      "msgch::dispatch_liq_yield: DROP attestation -- token is not an active liq token"));
} FC_LOG_AND_RETHROW() }

// ── The emergency stop (sysio.andon) ─────────────────────────────────────────

// A pulled sysio.andon cord freezes nothing on the envelope path: sysio.msgch accepts the envelope, routes
// every syndication to sysio.synd, which holds it exactly as it does unfrozen, and closes the envelope with
// `closeenv`, whose queue step prints that it waits and aborts nothing.
BOOST_FIXTURE_TEST_CASE(a_pulled_andon_cord_leaves_deliveries_running, sysio_dispatch_tester) { try {
   namespace andon = sysio_system::test_support::andon;
   bootstrap_for_dispatch();
   setup_liq_for_dispatch();
   abi_serializer andon_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), andon::pull(*this, andon_abi));

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const uint32_t epoch = current_epoch();
   const auto env = encode_envelope_with_mixed_attestations(epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 5 * LIQ_UNIT, 1)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 3 * LIQ_UNIT, 2)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto actions = synd_actions(trace);
   BOOST_REQUIRE_EQUAL(3u, actions.size());
   BOOST_CHECK_EQUAL("closeenv"_n, actions[2]);
   BOOST_CHECK_NE(std::string::npos,
                  all_console(trace).find("sysio.synd::queue: the andon cord is pulled; releases, deliveries and "
                                          "burns wait for the clear"));
   BOOST_REQUIRE_EQUAL(2u, synd_items().size());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_balance(SYND_ACCOUNT));
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_supply());
   BOOST_CHECK_EQUAL("WAITING", synd_envelope("ETH", "LIQETH", epoch)["state"].as_string());
} FC_LOG_AND_RETHROW() }

// A pulled sysio.andon cord leaves `sysio.epoch::advance` running: the genesis advance inline from
// `sysio.msgch::bootstrap` moves the epoch from 0 to 1 exactly as it does unfrozen.
BOOST_FIXTURE_TEST_CASE(a_pulled_andon_cord_leaves_advance_running, sysio_dispatch_tester) { try {
   namespace andon = sysio_system::test_support::andon;
   bootstrap_for_dispatch();
   // Deployed before sysio.system is set up, while a new account's code needs no RAM grant.
   abi_serializer andon_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), andon::pull(*this, andon_abi));
   setup_wire_token();
   enable_epoch_advancement();

   BOOST_REQUIRE_EQUAL(current_epoch(), 0u);
   BOOST_REQUIRE_EQUAL(success(), push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "bootstrap"_n, mvo()));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(current_epoch(), 1u);
   BOOST_REQUIRE(get_blocklog(1).is_null());
} FC_LOG_AND_RETHROW() }

// ── The solvency check (sysio.synd) through a delivered envelope ─────────────

// Review focus 3: sysio.msgch sends an envelope's messages to sysio.synd as inline actions, and each
// `onsynd`'s inline mint runs before the next message (depth-first), so each syndication is compared with
// the outstanding after its own mint. Two syndications carrying the custody read after each lock are both
// exact; the yield report after them carries the custody after its claim and reads as an excess of the
// claimed yield. Nothing is recorded and the cord stays clear.
BOOST_FIXTURE_TEST_CASE(each_syndication_of_an_envelope_is_compared_after_its_own_mint,
                        sysio_dispatch_tester) { try {
   namespace andon = sysio_system::test_support::andon;
   bootstrap_for_dispatch();
   abi_serializer andon_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   setup_liq_for_dispatch();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const auto env = encode_envelope_with_mixed_attestations(current_epoch(), {
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 5 * LIQ_UNIT, 1, 5 * LIQ_UNIT)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 3 * LIQ_UNIT, 2, 8 * LIQ_UNIT)},
      {ATTESTATION_TYPE_LIQ_YIELD, encode_liq_yield(eth, liqeth, 2 * LIQ_UNIT, 3, 42, 10 * LIQ_UNIT)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   const auto console = all_console(trace);
   BOOST_CHECK_EQUAL(std::string::npos, console.find("sysio.synd::onsynd: EXCESS"));
   BOOST_CHECK_EQUAL(std::string::npos, console.find("SHORTFALL"));
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onyield: EXCESS -- reported " +
                                                  std::to_string(10 * LIQ_UNIT) + " outstanding " +
                                                  std::to_string(8 * LIQ_UNIT)));
   BOOST_CHECK(synd_rows("mismatch"_n).empty());
   BOOST_CHECK(get_row_by_account(andon::account, andon::account, "cord"_n, "cord"_n).empty());
   BOOST_CHECK_EQUAL(3u, synd_items().size());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_supply());
} FC_LOG_AND_RETHROW() }

// Review focus 4: a custody shortfall in a delivered envelope writes its `mismatch` row and pulls the depot
// cord from inside the consensus transaction; the envelope is accepted, both syndications are held, and
// the epoch still advances through the consensus crank.
BOOST_FIXTURE_TEST_CASE(a_custody_shortfall_pulls_the_cord_and_the_epoch_advances, sysio_dispatch_tester) { try {
   namespace andon = sysio_system::test_support::andon;
   bootstrap_for_dispatch();
   // Deployed before sysio.system is set up, while a new account's code needs no RAM grant.
   abi_serializer andon_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   setup_liq_for_dispatch();
   setup_wire_token();
   enable_epoch_advancement();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const uint32_t epoch = current_epoch();
   // The second syndication carries 7 against 8 outstanding after its mint.
   const auto env = encode_envelope_with_mixed_attestations(epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 5 * LIQ_UNIT, 1, 5 * LIQ_UNIT)},
      {ATTESTATION_TYPE_SYNDICATE_LIQ,
       encode_syndicate_liq(eth, EVM, uwrit_op_eth_pubkey, liqeth, 3 * LIQ_UNIT, 2, 7 * LIQ_UNIT)},
   });
   const auto trace = deliver_trace(/*proven=*/ eth, env);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   produce_block();   // land the delivery before the clock is moved past its expiration
   const auto console = all_console(trace);
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onsynd: SHORTFALL -- reported " +
                                                  std::to_string(7 * LIQ_UNIT) + " outstanding " +
                                                  std::to_string(8 * LIQ_UNIT)));
   BOOST_CHECK_NE(std::string::npos,
                  console.find("sysio.synd::onsynd: CORD PULLED -- custody shortfall ETH LIQETH seq 2"));

   const auto rows = synd_rows("mismatch"_n);
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   const auto row = synd_decode("mismatch_row", rows[0]);
   BOOST_CHECK_EQUAL("ETH", row["chain_code"].as_string());
   BOOST_CHECK_EQUAL("LIQETH", row["token_code"].as_string());
   BOOST_CHECK_EQUAL(epoch, row["epoch_index"].as<uint32_t>());
   BOOST_CHECK_EQUAL(2u, row["sequence"].as_uint64());
   BOOST_CHECK_EQUAL("SYNDICATION", row["kind"].as_string());
   BOOST_CHECK_EQUAL(7 * LIQ_UNIT, row["reported"].as<int64_t>());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, row["expected"].as<int64_t>());

   const auto cord = andon_abi.binary_to_variant("cord_state",
      get_row_by_account(andon::account, andon::account, "cord"_n, "cord"_n),
      abi_serializer::create_yield_function(abi_serializer_max_time));
   BOOST_REQUIRE(cord["pulled"].as_bool());
   BOOST_CHECK_EQUAL("custody shortfall ETH LIQETH seq 2", cord["reason"].as_string());

   BOOST_CHECK_EQUAL(2u, synd_items().size());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_supply());
   BOOST_CHECK_EQUAL(8 * LIQ_UNIT, liq_balance(SYND_ACCOUNT));
   BOOST_CHECK_EQUAL("WAITING", synd_envelope("ETH", "LIQETH", epoch)["state"].as_string());
   BOOST_REQUIRE(!get_envelope(1).is_null());

   // The frozen depot still reaches consensus and advances.
   produce_block(fc::seconds(120));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_block();
   BOOST_CHECK_EQUAL(current_epoch(), epoch + 1);
} FC_LOG_AND_RETHROW() }

// Review focus 4 with work pending: the cord a shortfall pulls inline from `onsynd` freezes the very
// queue step `closeenv` runs later in the same delivered envelope. The earlier envelope of the pair was
// partly bonded, challenged and ruled VALID, so the next step would record the ruling, claim the hold-bond
// award from sysio.bond, forward the challenger's share and release the syndication to its linked account.
// Frozen, that step releases nothing, sends no claim and only records VALID with the share pending; the
// epoch still advances. After the clear, one crank releases the syndication and forwards the share.
BOOST_FIXTURE_TEST_CASE(a_shortfall_freezes_the_same_envelopes_queue_step, sysio_dispatch_tester) { try {
   namespace andon = sysio_system::test_support::andon;
   constexpr auto BOND_ACCOUNT = "sysio.bond"_n;
   constexpr auto USER         = "synd.user"_n;
   constexpr auto UNDERWRITER  = "synd.bonder"_n;
   constexpr auto CHALLENGER   = "synd.chal"_n;
   bootstrap_for_dispatch();
   // Deployed before sysio.system is set up, while a new account's code needs no RAM grant.
   abi_serializer andon_abi, bond_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   setup_liq_for_dispatch();
   create_accounts({BOND_ACCOUNT, USER, UNDERWRITER, CHALLENGER});
   produce_blocks();
   deploy(BOND_ACCOUNT, contracts::bond_wasm(), contracts::bond_abi(), bond_abi);
   const auto user_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   create_eth_authex_link(USER, user_key);
   const auto user_pubkey = em_pubkey_bytes(user_key.get_public_key());
   // Buckets wide open, so only the cord stops a release.
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi, config::system_account_name, "setconfig"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("LIQETH"))("synd_fee_bps", 0)
      ("desynd_fee_bps", 0)("synd_burst", 1'000'000 * LIQ_UNIT)("synd_refill", 1'000'000 * LIQ_UNIT)
      ("desynd_burst", 0)("desynd_refill", 0)("window_sec", 10800)("bounty", 0)("challenge_extra", 0)));
   setup_wire_token();
   enable_epoch_advancement();

   const auto eth    = fc::slug_name{"ETH"}.value;
   const auto liqeth = fc::slug_name{"LIQETH"}.value;
   constexpr auto EVM = ChainKind::CHAIN_KIND_EVM;
   const auto executed = [](const transaction_trace_ptr& trace, name code, name action_name) {
      return std::any_of(trace->action_traces.begin(), trace->action_traces.end(), [&](const auto& at) {
         return at.receiver == code && at.act.account == code && at.act.name == action_name;
      });
   };

   // Envelope A: 10 for the linked user, exact custody. Its closeenv step requests the underwriting.
   const uint32_t first_epoch = current_epoch();
   const auto first = encode_envelope_with_mixed_attestations(first_epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, user_pubkey, liqeth, 10 * LIQ_UNIT, 1,
                                                            10 * LIQ_UNIT)},
   });
   BOOST_REQUIRE_EQUAL(success(), deliver(eth, first));
   produce_block();
   const uint64_t id = synd_envelope("ETH", "LIQETH", first_epoch)["request_id"].as_uint64();
   BOOST_REQUIRE_GT(id, 0u);
   BOOST_REQUIRE_EQUAL("REQUESTED", synd_envelope("ETH", "LIQETH", first_epoch)["state"].as_string());

   // 4 of the 10 bonded, then a challenge and a VALID ruling: the hold bond's unbonded 6/10 is owed back.
   BOOST_REQUIRE_EQUAL(success(), mint_shadow(UNDERWRITER, 4 * LIQ_UNIT));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi, UNDERWRITER, "accept"_n, mvo()
      ("underwriter", UNDERWRITER)("request_id", id)("amount", 4 * LIQ_UNIT)));
   BOOST_REQUIRE_EQUAL(success(), mint_shadow(CHALLENGER, 5 * LIQ_UNIT));
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi, CHALLENGER, "challenge"_n, mvo()
      ("challenger", CHALLENGER)("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("LIQETH"))
      ("epoch_index", first_epoch)));
   const int64_t hold_bond = 10 * LIQ_UNIT / 10;           // 1000 bps of the 10 covered
   const int64_t share     = hold_bond * 6 / 10;           // the 6 of 10 no bond covered
   const int64_t challenger_before = liq_balance(CHALLENGER);
   BOOST_REQUIRE_EQUAL(5 * LIQ_UNIT - hold_bond, challenger_before);
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi, config::system_account_name, "rslvvalid"_n,
                                       mvo()("request_id", id)));
   produce_block();   // land the pushes above before the clock is moved past their expiration
   BOOST_REQUIRE_EQUAL("HELD", synd_envelope("ETH", "LIQETH", first_epoch)["state"].as_string());

   produce_block(fc::seconds(120));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_block();
   const uint32_t second_epoch = current_epoch();
   BOOST_REQUIRE_EQUAL(first_epoch + 1, second_epoch);

   // Envelope B: 1 more, carrying 15 against 10 + 4 + 5 + 1 = 20 outstanding.
   sysio::opp::Envelope accepted;
   BOOST_REQUIRE(accepted.ParseFromArray(first.data(), static_cast<int>(first.size())));
   const auto second = encode_envelope_with_mixed_attestations(second_epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, EVM, user_pubkey, liqeth, 1 * LIQ_UNIT, 2,
                                                            15 * LIQ_UNIT)},
   }, oracle::digest_bytes(oracle::epoch_digest(accepted)), accepted.messages(0).header().message_id());
   const auto trace = deliver_trace(eth, second);
   BOOST_REQUIRE(trace != nullptr);
   BOOST_REQUIRE(!trace->except);
   produce_block();   // land the delivery before the clock is moved past its expiration
   const auto console = all_console(trace);
   BOOST_CHECK_NE(std::string::npos, console.find("sysio.synd::onsynd: SHORTFALL -- reported " +
                                                  std::to_string(15 * LIQ_UNIT) + " outstanding " +
                                                  std::to_string(20 * LIQ_UNIT)));
   BOOST_CHECK(executed(trace, andon::account, "pull"_n));
   BOOST_CHECK(executed(trace, SYND_ACCOUNT, "closeenv"_n));
   BOOST_CHECK_NE(std::string::npos,
                  console.find("sysio.synd::queue: the andon cord is pulled; releases, deliveries and burns wait "
                               "for the clear"));
   // Nothing left custody: no release, no claim, no forward.
   BOOST_CHECK(!executed(trace, BOND_ACCOUNT, "claim"_n));
   BOOST_CHECK(!executed(trace, LIQ_ACCOUNT, "transfer"_n));
   BOOST_CHECK_EQUAL(0, liq_balance(USER));
   BOOST_CHECK_EQUAL(challenger_before, liq_balance(CHALLENGER));
   auto envelope = synd_envelope("ETH", "LIQETH", first_epoch);
   BOOST_CHECK_EQUAL("VALID", envelope["outcome"].as_string());
   BOOST_CHECK(envelope["share_pending"].as_bool());
   BOOST_CHECK_EQUAL(share, envelope["hold_share"].as<int64_t>());
   BOOST_CHECK_EQUAL(0u, envelope["released"].as_uint64());
   BOOST_CHECK_EQUAL(1u, synd_rows("mismatch"_n).size());
   const auto cord_of = [&] {
      return andon_abi.binary_to_variant("cord_state",
         get_row_by_account(andon::account, andon::account, "cord"_n, "cord"_n),
         abi_serializer::create_yield_function(abi_serializer_max_time));
   };
   BOOST_REQUIRE(cord_of()["pulled"].as_bool());

   // The frozen depot still reaches consensus and advances.
   produce_block(fc::seconds(120));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkcons());
   produce_block();
   BOOST_CHECK_EQUAL(second_epoch + 1, current_epoch());

   // Cleared: one crank records nothing new, claims the award, forwards the share and releases the 10.
   BOOST_REQUIRE_EQUAL(success(), andon::clear(*this, andon_abi));
   BOOST_REQUIRE(!cord_of()["pulled"].as_bool());
   const auto crank = push_trace(SYND_ACCOUNT, synd_abi, CHALLENGER, "crank"_n, mvo()("limit", 100));
   BOOST_REQUIRE(crank != nullptr);
   BOOST_REQUIRE(!crank->except);
   produce_block();
   BOOST_CHECK(executed(crank, BOND_ACCOUNT, "claim"_n));
   BOOST_CHECK_EQUAL(10 * LIQ_UNIT, liq_balance(USER));
   BOOST_CHECK_EQUAL(challenger_before + share, liq_balance(CHALLENGER));
   envelope = synd_envelope("ETH", "LIQETH", first_epoch);
   BOOST_CHECK_EQUAL("DONE", envelope["state"].as_string());
   BOOST_CHECK(!envelope["share_pending"].as_bool());
} FC_LOG_AND_RETHROW() }

// The flow the batch_operator_plugin underwriter drives, with zero fees and bounty and one account bonding every
// request in full. The request a delivery opens carries the statement the underwriter verifies (the outpost, the
// epoch and the digest `sysio.msgch` accepted); bonding releases the syndication on the next queue step; once the
// challenge window passes anyone approves, and the underwriter's claim returns the whole bond.
BOOST_FIXTURE_TEST_CASE(a_bond_releases_the_envelope_and_comes_back_after_the_window, sysio_dispatch_tester) {
try {
   namespace andon = sysio_system::test_support::andon;
   constexpr auto     BOND_ACCOUNT = "sysio.bond"_n;
   constexpr auto     USER         = "synd.user"_n;
   constexpr auto     UNDERWRITER  = "synd.bonder"_n;
   constexpr uint32_t window_sec   = 10800;
   bootstrap_for_dispatch();
   abi_serializer andon_abi, bond_abi;
   andon::deploy(*this, andon_abi, contracts::andon_wasm(), contracts::andon_abi());
   setup_liq_for_dispatch();
   create_accounts({BOND_ACCOUNT, USER, UNDERWRITER});
   produce_blocks();
   deploy(BOND_ACCOUNT, contracts::bond_wasm(), contracts::bond_abi(), bond_abi);
   const auto user_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   create_eth_authex_link(USER, user_key);
   const auto user_pubkey = em_pubkey_bytes(user_key.get_public_key());
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi, config::system_account_name, "setconfig"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("LIQETH"))("synd_fee_bps", 0)
      ("desynd_fee_bps", 0)("synd_burst", 1'000'000 * LIQ_UNIT)("synd_refill", 1'000'000 * LIQ_UNIT)
      ("desynd_burst", 0)("desynd_refill", 0)("window_sec", window_sec)("bounty", 0)("challenge_extra", 0)));
   setup_wire_token();
   enable_epoch_advancement();
   const auto request_of = [&](uint64_t id) {
      return bond_abi.binary_to_variant("request_row", get_row_by_id(BOND_ACCOUNT, BOND_ACCOUNT, "requests"_n, id),
                                        abi_serializer::create_yield_function(abi_serializer_max_time));
   };

   const auto     eth    = fc::slug_name{"ETH"}.value;
   const auto     liqeth = fc::slug_name{"LIQETH"}.value;
   const uint32_t epoch  = current_epoch();
   const auto     delivered = encode_envelope_with_mixed_attestations(epoch, {
      {ATTESTATION_TYPE_SYNDICATE_LIQ, encode_syndicate_liq(eth, ChainKind::CHAIN_KIND_EVM, user_pubkey, liqeth,
                                                            10 * LIQ_UNIT, 1, 10 * LIQ_UNIT)},
   });
   BOOST_REQUIRE_EQUAL(success(), deliver(eth, delivered));
   produce_block();

   const uint64_t id = synd_envelope("ETH", "LIQETH", epoch)["request_id"].as_uint64();
   BOOST_REQUIRE_GT(id, 0u);
   auto request = request_of(id);
   BOOST_REQUIRE_EQUAL("OPEN", request["state"].as_string());
   BOOST_REQUIRE_EQUAL("sysio.synd", request["issuer"].as_string());
   BOOST_REQUIRE_EQUAL("oppenvelope", request["schema"].as_string());
   BOOST_REQUIRE_EQUAL(10 * LIQ_UNIT, request["covered"].as_int64());
   // The statement: chain code, epoch, accepted digest and token code, little-endian, 52 bytes.
   std::vector<char> statement(52);
   auto* p = reinterpret_cast<unsigned char*>(statement.data());
   boost::endian::store_little_u64(p, eth);
   boost::endian::store_little_u32(p + 8, epoch);
   const auto digest = accepted_envelope_digest(eth);
   std::memcpy(p + 12, digest.data(), 32);
   boost::endian::store_little_u64(p + 44, liqeth);
   BOOST_CHECK(statement == request["statement"].as<std::vector<char>>());
   // The underwriter bonds a statement only when the outpost's record of the envelope carries its digest. An outpost
   // records keccak256 of the canonical bytes it emits (`envelope_hash` empty): the same digest.
   sysio::opp::Envelope emitted;
   BOOST_REQUIRE(emitted.ParseFromArray(delivered.data(), static_cast<int>(delivered.size())));
   BOOST_CHECK(std::memcmp(oracle::epoch_digest(emitted).data(), digest.data(), 32) == 0);
   BOOST_CHECK_EQUAL(0, liq_balance(USER));   // held until underwritten

   // The underwriter bonds the whole request; a crank releases the syndication rather than the next envelope's
   // step.
   BOOST_REQUIRE_EQUAL(success(), mint_shadow(UNDERWRITER, 10 * LIQ_UNIT));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi, UNDERWRITER, "accept"_n, mvo()
      ("underwriter", UNDERWRITER)("request_id", id)("amount", 10 * LIQ_UNIT)));
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi, UNDERWRITER, "crank"_n, mvo()("limit", 16)));
   produce_block();
   BOOST_CHECK_EQUAL("BONDED", request_of(id)["state"].as_string());
   BOOST_CHECK_EQUAL(10 * LIQ_UNIT, liq_balance(USER));
   BOOST_CHECK_EQUAL(0, liq_balance(UNDERWRITER));
   BOOST_CHECK_EQUAL("DONE", synd_envelope("ETH", "LIQETH", epoch)["state"].as_string());

   // The bond stays at risk for the whole window, then anyone approves and the underwriter claims it back.
   BOOST_CHECK_EQUAL(wasm_assert_msg("challenge window has not passed"),
                     push(BOND_ACCOUNT, bond_abi, USER, "approve"_n, mvo()("request_id", id)));
   produce_block(fc::seconds(window_sec));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi, USER, "approve"_n, mvo()("request_id", id)));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi, UNDERWRITER, "claim"_n, mvo()
      ("request_id", id)("account", UNDERWRITER)));
   produce_block();
   BOOST_CHECK_EQUAL("APPROVED", request_of(id)["state"].as_string());
   BOOST_CHECK_EQUAL(10 * LIQ_UNIT, liq_balance(UNDERWRITER));
   BOOST_CHECK_EQUAL(10 * LIQ_UNIT, liq_balance(USER));
} FC_LOG_AND_RETHROW() }

// The host-side simulator feeds canonical protobuf envelopes into the production
// msgch -> epoch consensus -> synd -> liq path. No patched OPP or dispatch stub.
BOOST_AUTO_TEST_CASE(generic_external_simulator_drives_production_dispatch) {
   for (const auto& a : sysio::testing::external::Assets) {
      BOOST_TEST_CONTEXT("chain=" << a.chain << " token=" << a.token) {
         sysio_dispatch_tester t;
         t.bootstrap_for_dispatch(a.chain, a.kind);
         t.setup_liq_for_dispatch(a.chain, a.token, a.kind);
         sysio::testing::external::chain outpost(a);
         const auto token = symbol::from_string(std::string("9,") + a.token);
         const auto key = a.kind == ChainKind::CHAIN_KIND_EVM ? t.uwrit_op_eth_pubkey : std::vector<char>(32, '\x42');
         const auto deposit = outpost.deposit(key, 9 * t.LIQ_UNIT);
         const auto yield = outpost.yield(2 * t.LIQ_UNIT);
         const auto epoch = t.current_epoch();
         const auto bytes = outpost.envelope(epoch, {deposit, yield, deposit});
         const auto trace = t.deliver_trace(fc::slug_name{a.chain}.value, bytes);
         BOOST_REQUIRE(trace != nullptr);
         BOOST_REQUIRE(!trace->except);
         BOOST_REQUIRE_EQUAL(2u, t.synd_items().size());
         BOOST_CHECK_EQUAL(9 * t.LIQ_UNIT, t.liq_supply(token));
         BOOST_CHECK_EQUAL(9 * t.LIQ_UNIT, t.liq_balance(t.SYND_ACCOUNT, token));
         BOOST_CHECK_EQUAL(0, t.liq_pending(token));
         BOOST_CHECK_EQUAL(2u, t.synd_cursor(a.chain)["last_sequence"].as_uint64());
         const auto envelope = t.synd_envelope(a.chain, a.token, epoch);
         BOOST_REQUIRE(!envelope.is_null());
         BOOST_CHECK_EQUAL("WAITING", envelope["state"].as_string());
         BOOST_CHECK_EQUAL(t.accepted_envelope_digest(fc::slug_name{a.chain}.value),
                           envelope["digest"].as<fc::sha256>());
         BOOST_CHECK(t.synd_rows("mismatch"_n).empty());
      }
   }
}

// A real staking-reward envelope crosses msgch -> dclaim -> system -> token.
// Small treasury liquidity deliberately reaches the second fundclaim cap.
BOOST_FIXTURE_TEST_CASE(generic_staking_rewards_fund_claim_and_dedupe_through_dispatch, sysio_dispatch_tester) {
   using namespace sysio::testing::external;
   bootstrap_for_dispatch(First.chain, First.kind);
   deploy(TOKEN_ACCOUNT, contracts::token_wasm(), contracts::token_abi(), token_abi);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, TOKEN_ACCOUNT, "create"_n,
      mvo()("issuer", "sysio")("maximum_supply", "100.000000000 WIRE")));
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name, "issue"_n,
      mvo()("to", "sysio")("quantity", "100.000000000 WIRE")("memo", "reward funding")));
   enable_epoch_advancement();
   external::chain outpost(First);
   constexpr uint64_t Reward = 10 * Unit;
   sysio::opp::attestations::StakingReward reward;
   reward.set_chain_code(fc::slug_name{First.chain}.value);
   reward.mutable_staker_wire_account()->set_name(UWRIT_OP.to_string());
   reward.set_share_bps(10'000);
   reward.set_reward_epoch_index(current_epoch());
   reward.set_external_epoch_ref(1);
   reward.mutable_reward_amount()->set_token_code(fc::slug_name{"WIRE"}.value);
   reward.mutable_reward_amount()->set_amount(Reward);
   reward.mutable_staker_native_address()->set_kind(First.kind);
   const std::vector<char> address(20, '\x31');
   reward.mutable_staker_native_address()->set_address(address.data(), address.size());
   const auto body = reward.SerializeAsString();
   const auto bytes = outpost.envelope(current_epoch(), {
      {ATTESTATION_TYPE_STAKING_REWARD, body}, {ATTESTATION_TYPE_STAKING_REWARD, body}});
   const auto trace = deliver_trace(fc::slug_name{First.chain}.value, bytes);
   BOOST_REQUIRE(trace && !trace->except);
   const auto wire = symbol::from_string("9,WIRE");
   const auto balance = [&](name account) { return get_currency_balance(TOKEN_ACCOUNT, wire, account).get_amount(); };
   BOOST_REQUIRE_EQUAL(Reward, get_dclaim_row("pclaims"_n, "pending_claim", UWRIT_OP.to_uint64_t())
      ["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(Reward, balance(DCLAIM_ACCOUNT));
   BOOST_REQUIRE_EQUAL(90 * Unit, balance(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), push(DCLAIM_ACCOUNT, dclaim_abi, UWRIT_OP, "claim"_n,
      mvo()("wire_account", UWRIT_OP)));
   BOOST_REQUIRE_EQUAL(Reward, balance(UWRIT_OP));
   BOOST_REQUIRE_EQUAL(0, balance(DCLAIM_ACCOUNT));
   BOOST_REQUIRE(get_dclaim_row("pclaims"_n, "pending_claim", UWRIT_OP.to_uint64_t()).is_null());
   // The soak's launch import has its own funding path: pre-fund, import an
   // unlinked identity, authenticate the link, then claim the exact WIRE credit.
   constexpr uint64_t Imported = 5 * Unit;
   const auto private_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   const auto native = fc::crypto::ethereum::address_to_bytes(private_key.get_public_key());
   const std::vector<char> native_address(native.begin(), native.end());
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name, "transfer"_n,
      mvo()("from", "sysio")("to", DCLAIM_ACCOUNT)("quantity", asset(Imported, wire))("memo", "import backing")));
   BOOST_REQUIRE_EQUAL(success(), push(DCLAIM_ACCOUNT, dclaim_abi, DCLAIM_ACCOUNT, "importseed"_n,
      mvo()("chain", First.kind)("credits", fc::variants{
         mvo()("native_address", native_address)("wire_atomic", int64_t(Imported))})));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   create_eth_authex_link(CLAIM_ACCOUNT, private_key);
   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   BOOST_REQUIRE_EQUAL(Imported, get_dclaim_row("pclaims"_n, "pending_claim", CLAIM_ACCOUNT.to_uint64_t())
      ["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), push(DCLAIM_ACCOUNT, dclaim_abi, CLAIM_ACCOUNT, "claim"_n,
      mvo()("wire_account", CLAIM_ACCOUNT)));
   BOOST_REQUIRE_EQUAL(Imported, balance(CLAIM_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, balance(DCLAIM_ACCOUNT));
   const auto funding_left = balance(config::system_account_name);
   // A new reward at the authenticated downstream boundary exceeds liquid
   // treasury funds. Credit is retained, but funding cannot overspend custody.
   BOOST_REQUIRE_EQUAL(success(), push(DCLAIM_ACCOUNT, dclaim_abi, MSGCH_ACCOUNT, "onreward"_n,
      mvo()("chain_code", fc::slug_name{First.chain}.value)("staker_wire_account", UWRIT_OP.to_string())
         ("reward_chain", First.kind)("staker_native_addr", address)("reward_amount", 100 * Unit)
         ("reward_epoch_index", current_epoch())("external_epoch_ref", 2)("share_bps", 10'000)));
   BOOST_REQUIRE_EQUAL(funding_left, balance(DCLAIM_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, balance(config::system_account_name));
   BOOST_REQUIRE_EQUAL(100 * Unit, get_dclaim_row("pclaims"_n, "pending_claim", UWRIT_OP.to_uint64_t())
      ["balance"].as<asset>().get_amount());
}
BOOST_AUTO_TEST_SUITE_END()

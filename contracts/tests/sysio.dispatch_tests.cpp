/// Cross-contract dispatch tests for sysio.msgch's per-attestation-type
/// routing (Task 4 of the operator-collateral plan).
///
/// v6 data-model: identity moved to slug_name-keyed registries. The dispatch
/// surface still routes `OPERATOR_ACTION` payloads into opreg, but the
/// payload schema now carries `chain_code` (slug_name uint64) instead of a
/// `ChainKind chain` field, and `TokenAmount.token_code` (slug_name uint64)
/// instead of `TokenAmount.kind` (TokenKind enum).

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/permission_object.hpp>
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
#include <magic_enum/magic_enum.hpp>

#include "contracts.hpp"
// Canonical-encoding + header-derivation oracle: inbound envelopes must carry
// spec-derived semantic headers or apply_consensus drops them before dispatch.
#include "opp_envelope_oracle.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

namespace {

/// SlugName mvo helper for v6 action arguments.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

/// One `chain_min_bond` entry for `sysio.opreg::setconfig`'s `req_*_collat`
/// vectors. `config_timestamp_ms` is stamped by the contract, so 0 here.
inline fc::variant chain_min_bond_mvo(std::string_view chain_code,
                                      std::string_view token_code,
                                      uint64_t min_bond) {
   return fc::variant(mvo()
      ("chain_code",          codename_mvo(chain_code))
      ("token_code",          codename_mvo(token_code))
      ("min_bond",            min_bond)
      ("config_timestamp_ms", uint64_t{0}));
}

/// Build an `authority` whose active permission is the account's own
/// active key + a list of `{actor, sysio.code}` co-signers.
authority active_with_code_authors(name account, const std::vector<name>& code_authors) {
   authority a(base_tester::get_public_key(account, "active"));
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

/// Encode an Envelope wrapping a single attestation.
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

   oracle::finalize_header(*env.mutable_messages(0), {}, 1'775'612'516'983ULL);

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

/// Encode an Envelope wrapping N attestations of the same type. Used to fit
/// multiple OPERATOR_ACTIONs into a single delivery, since the depot
/// deduplicates per-(batch_op, outpost, epoch) — a second `deliver` from
/// the same batch op in the same epoch reverts as a duplicate.
std::vector<char> encode_envelope_with_attestations(
   uint32_t epoch_index,
   sysio::opp::types::AttestationType att_type,
   const std::vector<std::string>& att_datas)
{
   sysio::opp::Envelope env;
   env.set_epoch_index(epoch_index);
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1'775'612'516'983ULL);

   auto* msg     = env.add_messages();
   auto* payload = msg->mutable_payload();
   for (const auto& d : att_datas) {
      auto* att = payload->add_attestations();
      att->set_type(att_type);
      att->set_data(d);
      att->set_data_size(static_cast<uint32_t>(d.size()));
   }

   oracle::finalize_header(*env.mutable_messages(0), {}, 1'775'612'516'983ULL);

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

/// Mirrors the contract-internal `MAX_ENVELOPE_BYTES` protocol cap (64 KiB, shared with the
/// Ethereum and Solana outpost implementations). The contract constant lives in the msgch
/// translation unit — contract headers are not host-compilable — so tests keep this manual
/// mirror, same as the outbound packing tests in sysio.msgch_tests.cpp.
constexpr size_t MAX_ENVELOPE_BYTES = 65'536;

/// Encode a decodable envelope whose serialised size is EXACTLY `target_bytes`, padded with a
/// single out-of-scope STAKE attestation (dispatch drops it with no value-bearing effect). Probe
/// once with `target_bytes` of padding to measure the fixed protobuf overhead, then rebuild with
/// the pad shrunk by that overhead: at sizes near the 64 KiB envelope cap every nested length
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

/// Render an EM public key into its canonical contract string —
/// "PUB_EM_" + hex(compressed_33_bytes).
std::string contract_em_pubkey_to_string(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return "PUB_EM_" + fc::to_hex(compressed.data(), compressed.size());
}

/// Build the createlink message string exactly as `sysio.authex::createlink`
/// composes it on-chain.
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
std::vector<char> em_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return std::vector<char>(compressed.begin(), compressed.end());
}

/// Encode an OperatorAction attestation payload (v6 schema).
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
std::string encode_node_owner_registration(
   const std::string& account,
   uint32_t tier,
   sysio::opp::types::WireKeyType wire_key_type,
   const std::vector<char>& wire_key_bytes,
   const std::vector<char>& eth_pubkey_bytes)
{
   sysio::opp::attestations::NodeOwnerRegistration reg;
   reg.mutable_account()->set_name(account);
   reg.set_tier(tier);
   reg.set_actor_pub_key(eth_pubkey_bytes.data(), eth_pubkey_bytes.size());
   auto* wk = reg.mutable_wire_pub_key();
   wk->set_key_type(wire_key_type);
   wk->set_key(wire_key_bytes.data(), wire_key_bytes.size());

   std::string out;
   reg.SerializeToString(&out);
   return out;
}

/// Encode a SwapRequest attestation payload — the bytes `createuwreq` decodes.
std::string encode_swap_request(
   sysio::opp::types::ChainKind actor_kind,
   const std::vector<char>&     actor_addr,
   uint64_t src_chain_code_v, uint64_t src_token_code_v, uint64_t src_reserve_code_v,
   int64_t  src_amount,
   uint64_t dst_chain_code_v, uint64_t dst_token_code_v, uint64_t dst_reserve_code_v,
   uint64_t target_amount, uint32_t tolerance_bps,
   sysio::opp::types::ChainKind recipient_kind,
   const std::vector<char>&     recipient_addr)
{
   sysio::opp::attestations::SwapRequest sr;
   auto* actor = sr.mutable_actor();
   actor->set_kind(actor_kind);
   actor->set_address(actor_addr.data(), actor_addr.size());
   auto* amt = sr.mutable_source_amount();
   amt->set_token_code(src_token_code_v);
   amt->set_amount(src_amount);
   sr.set_source_chain_code(src_chain_code_v);
   sr.set_source_reserve_code(src_reserve_code_v);
   sr.set_target_chain_code(dst_chain_code_v);
   sr.set_target_token_code(dst_token_code_v);
   sr.set_target_reserve_code(dst_reserve_code_v);
   sr.set_target_amount(target_amount);
   sr.set_target_tolerance_bps(tolerance_bps);
   auto* rcpt = sr.mutable_recipient();
   rcpt->set_kind(recipient_kind);
   rcpt->set_address(recipient_addr.data(), recipient_addr.size());
   const std::vector<char> tx_id{'\x01', '\x02', '\x03', '\x04'};
   sr.set_source_tx_id(tx_id.data(), tx_id.size());

   std::string out;
   sr.SerializeToString(&out);
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

   /// `sysio.uwrit::uw_config::fee_bps`'s default (0.1% per spoke) — the rate in
   /// force whenever a case does not push its own `uwrit::setconfig`. Tests that
   /// re-derive a swap quote must charge the same fee the contract did.
   static constexpr uint32_t kDefaultUwritFeeBps = 10;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
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
         MSGCH_ACCOUNT, OPREG_ACCOUNT, UWRIT_ACCOUNT, EPOCH_ACCOUNT,
         RESERV_ACCOUNT, CHALG_ACCOUNT, TOKEN_ACCOUNT, CHAINS_ACCOUNT,
         BATCHOP, UWRIT_OP
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
      deploy(UWRIT_ACCOUNT,  contracts::uwrit_wasm(),   contracts::uwrit_abi(),   uwrit_abi);
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),   contracts::epoch_abi(),   epoch_abi);
      deploy(RESERV_ACCOUNT, contracts::reserve_wasm(), contracts::reserve_abi(), reserv_abi);
      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(),  contracts::authex_abi(),  authex_abi);
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

      grant_code_authors(OPREG_ACCOUNT, {MSGCH_ACCOUNT});
      // NodeOwnerRegistration delegations (the production analogue is wired in ClusterManager):
      // msgch -> sysio.roa (newnameduser/nodeownreg), and sysio.roa -> sysio.authex (recordlink).
      grant_code_authors(ROA_ACCOUNT,    {MSGCH_ACCOUNT});
      grant_code_authors(AUTHEX_ACCOUNT, {ROA_ACCOUNT});

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

   void grant_code_authors(name account, const std::vector<name>& code_authors) {
      set_authority(account, config::active_name,
                    active_with_code_authors(account, code_authors),
                    config::owner_name);
   }

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

   void create_uwrit_op_eth_authex_link() {
      using namespace fc::crypto;
      using namespace sysio::opp::types;

      auto priv = private_key::generate(private_key::key_type::em);
      auto pub  = priv.get_public_key();
      const uint64_t nonce = control->head().block_time().time_since_epoch().count() / 1000;

      auto msg = build_link_message(pub, UWRIT_OP.to_string(),
                                    ChainKind::CHAIN_KIND_EVM, nonce);
      auto msg_hash = keccak256::hash(msg);
      auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()),
                                      32));

      BOOST_REQUIRE_EQUAL(success(), push(AUTHEX_ACCOUNT, authex_abi, UWRIT_OP,
         "createlink"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_EVM)
            ("account",    UWRIT_OP.to_string())
            ("sig",        sig)
            ("pub_key",    pub)
            ("nonce",      nonce)));

      uwrit_op_eth_pubkey = em_pubkey_bytes(pub);
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

      // v6: chains are first-class registry rows.
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              outpost_kind)
            ("code",              codename_mvo(outpost_code))
            ("external_chain_id", 31337)
            ("name",              std::string("outpost-test"))
            ("description",       std::string{})));

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

   /// Find an operator's balance entry for a (chain_code, token_code) pair.
   fc::variant find_balance(const fc::variant& op,
                            std::string_view chain_code,
                            std::string_view token_code) {
      const auto chain_v = fc::slug_name{chain_code}.value;
      const auto token_v = fc::slug_name{token_code}.value;
      const auto& arr = op["balances"].get_array();
      for (const auto& b : arr) {
         if (b["chain_code"]["value"].as_uint64() == chain_v &&
             b["token_code"]["value"].as_uint64() == token_v) {
            return b;
         }
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

   // ── uwrit swap-race helpers (direct msgch-auth action calls) ──

   action_result depositinle_credit(name account, std::string_view chain_code,
                                    std::string_view token_code, uint64_t amount) {
      // depositinle does require_auth(get_self()); sign as opreg for a direct call.
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "depositinle"_n, mvo()
         ("account",             account.to_string())
         ("chain_code",          codename_mvo(chain_code))
         ("token_code",          codename_mvo(token_code))
         ("amount",              amount)
         ("actor_chain",         ChainKind::CHAIN_KIND_EVM)
         ("actor_address",       std::vector<char>(20, '\x06'))
         ("original_message_id", fc::sha256()));
   }

   action_result createuwreq_direct(uint64_t attestation_id, uint64_t chain_code_v,
                                    const std::string& swap_request_bytes) {
      return push(UWRIT_ACCOUNT, uwrit_abi, MSGCH_ACCOUNT, "createuwreq"_n, mvo()
         ("attestation_id", attestation_id)
         ("type",           AttestationType::ATTESTATION_TYPE_SWAP_REQUEST)
         ("chain_code",     chain_code_v)
         ("data",           std::vector<char>(swap_request_bytes.begin(),
                                              swap_request_bytes.end())));
   }

   action_result rcrdcommit_direct(uint64_t uwreq_id, name underwriter,
                                   uint64_t outpost_chain_code,
                                   std::string_view from_chain, std::string_view from_token,
                                   std::string_view reserve, const std::vector<char>& uic_bytes) {
      return push(UWRIT_ACCOUNT, uwrit_abi, MSGCH_ACCOUNT, "rcrdcommit"_n, mvo()
         ("uwreq_id",        uwreq_id)
         ("underwriter",     underwriter.to_string())
         ("chain_code",      outpost_chain_code)
         ("from_chain_code", codename_mvo(from_chain))
         ("from_token_code", codename_mvo(from_token))
         ("reserve_code",    codename_mvo(reserve))
         ("uic_bytes",       uic_bytes));
   }

   /// Build + sign an UnderwriteIntentCommit so it passes verify_uic_signature:
   /// serialize with signature EMPTY (matching the contract's blank-then-rehash;
   /// set uw_ext_chain_addr.kind non-zero with an empty address per the
   /// proto3/zpp encoder-parity rule), sha256 the RAW bytes, sign with the
   /// underwriter's active key, embed the packed signature, serialize.
   std::vector<char> make_signed_uic(name underwriter, uint64_t uwreq_id,
                                     uint64_t chain_code_v,
                                     uint64_t token_code_v, uint64_t reserve_code_v) {
      sysio::opp::attestations::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter.to_string());
      uic.mutable_uw_ext_chain_addr()->set_kind(sysio::opp::types::CHAIN_KIND_EVM);
      uic.set_uw_request_id(uwreq_id);
      uic.set_token_code(token_code_v);
      uic.set_chain_code(chain_code_v);
      uic.set_reserve_code(reserve_code_v);

      std::string blanked;
      uic.SerializeToString(&blanked);
      const auto digest = fc::sha256::hash(blanked.data(), blanked.size());

      const auto sig       = get_private_key(underwriter, "active").sign(digest);
      const auto sig_bytes = fc::raw::pack(sig);
      uic.set_signature(sig_bytes.data(), sig_bytes.size());

      std::string full;
      uic.SerializeToString(&full);
      return std::vector<char>(full.begin(), full.end());
   }

   /// Build an UnderwriteIntentCommit carrying an arbitrary raw `uw_account.name`
   /// string — including names that are not constructible as a `sysio::name`
   /// (uppercase, hyphen, over-long). No signature is embedded: the dispatch path
   /// validates the account name and drops a malformed UIC before rcrdcommit or
   /// signature verification is ever reached, so a valid signature is unnecessary.
   std::vector<char> make_uic_raw_name(const std::string& raw_name, uint64_t uwreq_id,
                                       uint64_t chain_code_v,
                                       uint64_t token_code_v, uint64_t reserve_code_v) {
      sysio::opp::attestations::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(raw_name);
      uic.mutable_uw_ext_chain_addr()->set_kind(sysio::opp::types::CHAIN_KIND_EVM);
      uic.set_uw_request_id(uwreq_id);
      uic.set_token_code(token_code_v);
      uic.set_chain_code(chain_code_v);
      uic.set_reserve_code(reserve_code_v);

      std::string full;
      uic.SerializeToString(&full);
      return std::vector<char>(full.begin(), full.end());
   }

   fc::variant get_uwreq(uint64_t id) {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, id);
      return data.empty() ? fc::variant() : uwrit_abi.binary_to_variant(
         "uw_request_t", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Next attestation id `sysio.msgch::mint_att_id` will return: the `attseq` singleton's
   /// `next`, or 1 before the row is materialised. Outbound queueouts share the same sequence
   /// (e.g. reserve registration emits RESERVE_READY), so a test that needs the ids an inbound
   /// envelope's attestations will mint must read the counter rather than assume it starts at 1.
   uint64_t next_att_id() {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attseq"_n, 0);
      if (data.empty()) return 1;
      auto v = msgch_abi.binary_to_variant("att_seq_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["next"].as_uint64();
   }

   /// Read a collateral lock row by lock_id (uwrit `locks` KV table). lock_ids
   /// are allocated from 1 (uwcounters default), so the first swap's source +
   /// destination locks are ids 1 and 2.
   fc::variant get_lock(uint64_t lock_id) {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "locks"_n, lock_id);
      return data.empty() ? fc::variant() : uwrit_abi.binary_to_variant(
         "lock_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Drive sysio.opreg::slash (CHALG-authorized economic punishment). Flips the
   /// operator to SLASHED and immediately debits the unlocked (slashable-now)
   /// portion of each balance; the locked portion is settled later by
   /// releaselock as each lock's challenge window closes.
   action_result slash_op(name account, const std::string& reason) {
      return push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n, mvo()
         ("account", account.to_string())
         ("reason",  reason));
   }

   /// Direct sysio.opreg::releaselock call (UWRIT-authorized). In production
   /// this is fanned out one-per-lock by sysio.uwrit::chklocks at epoch advance;
   /// the tests call it directly to exercise the deferred-slash settlement math.
   action_result releaselock_direct(name account, std::string_view chain_code,
                                    std::string_view token_code, uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, UWRIT_ACCOUNT, "releaselock"_n, mvo()
         ("account",    account.to_string())
         ("chain_code", codename_mvo(chain_code))
         ("token_code", codename_mvo(token_code))
         ("amount",     amount));
   }

   /// Administratively terminate an operator (`sysio.opreg::terminate`,
   /// self-authorized). Remits the unlocked portion of every balance and leaves
   /// the locked remainder for `releaselock` — the state WNS-01's prune gate
   /// must protect.
   action_result terminate_op(name account, const std::string& reason) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "terminate"_n, mvo()
         ("account", account.to_string())
         ("reason",  reason));
   }

   /// The permissionless `sysio.opreg::prune` crank.
   action_result opreg_prune() {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "prune"_n, mvo());
   }

   /// Walk `sysio.reserv::reserves` and return the row matching the slug_name
   /// triple. The table is KV-keyed by a checksum256, which `get_row_by_id`
   /// (uint64 keys only) cannot reach — same scan `sysio.reserv_tests` uses.
   fc::variant find_reserve(std::string_view chain_code,
                            std::string_view token_code,
                            std::string_view reserve_code) {
      const auto target_chain   = fc::slug_name{chain_code}.value;
      const auto target_token   = fc::slug_name{token_code}.value;
      const auto target_reserve = fc::slug_name{reserve_code}.value;

      const auto& db       = control->db();
      const auto  table_id = chain::compute_table_id("reserves"_n.to_uint64_t());
      const auto& kv_idx   = db.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(RESERV_ACCOUNT, table_id, std::string_view{}));
      for (; itr != kv_idx.end() && itr->code == RESERV_ACCOUNT
             && itr->table_id == table_id; ++itr) {
         std::vector<char> raw(itr->value.size());
         if (!raw.empty()) std::memcpy(raw.data(), itr->value.data(), raw.size());
         try {
            auto row = reserv_abi.binary_to_variant("reserve_row", raw,
               abi_serializer::create_yield_function(abi_serializer_max_time));
            if (row["chain_code"]["value"].as_uint64()   == target_chain &&
                row["token_code"]["value"].as_uint64()   == target_token &&
                row["reserve_code"]["value"].as_uint64() == target_reserve) {
               return row;
            }
         } catch (...) {
            // Not a reserve_row — skip.
         }
      }
      return fc::variant();
   }

   /// The depot's own quote for a swap, computed with the SAME kernel the
   /// contract uses (`sysio.uwrit::swap_quote` -> `opp::amm::quote_swap`) over
   /// the live reserve rows. Tests assert settlement against this rather than
   /// against a hardcoded number, so they pin the INVARIANT ("dst_amount is the
   /// curve's output") instead of one fixture's arithmetic.
   uint64_t expected_quote(std::string_view src_chain, std::string_view src_token,
                           std::string_view src_reserve, uint64_t src_amount,
                           std::string_view dst_chain, std::string_view dst_token,
                           std::string_view dst_reserve, uint32_t fee_bps) {
      const auto src = find_reserve(src_chain, src_token, src_reserve);
      const auto dst = find_reserve(dst_chain, dst_token, dst_reserve);
      BOOST_REQUIRE(!src.is_null());
      BOOST_REQUIRE(!dst.is_null());
      return sysio::opp::amm::quote_swap(
         /*src_is_wire*/ false,
         src["reserve_chain_amount"].as_uint64(), src["reserve_wire_amount"].as_uint64(),
         static_cast<uint32_t>(src["connector_weight_bps"].as_uint64()),
         /*dst_is_wire*/ false,
         dst["reserve_chain_amount"].as_uint64(), dst["reserve_wire_amount"].as_uint64(),
         static_cast<uint32_t>(dst["connector_weight_bps"].as_uint64()),
         src_amount, fee_bps);
   }

   /// Register one ACTIVE reserve with ample balanced liquidity (1e12 / 1e12,
   /// 50% connector weight). Shared by the bootstrap pair below and by the
   /// same-(chain, token) multi-reserve swap tests, which add a second reserve
   /// on an already-registered (chain, token) pair.
   action_result regreserve_active(std::string_view c, std::string_view t, std::string_view r) {
      return push(RESERV_ACCOUNT, reserv_abi, RESERV_ACCOUNT, "regreserve"_n, mvo()
         ("chain_code",             codename_mvo(c))
         ("token_code",             codename_mvo(t))
         ("reserve_code",           codename_mvo(r))
         ("name",                   std::string(c))
         ("description",            std::string{})
         ("initial_chain_amount",   uint64_t{1'000'000'000'000ull})
         ("initial_wire_amount",    uint64_t{1'000'000'000'000ull})
         ("source_token_precision", uint32_t{9})
         ("connector_weight_bps",   uint32_t{5000})
         ("is_private",             false)
         ("owner",                  ""));
   }

   /// Deploy sysio.token, issue a WIRE supply to the treasury, and seed two
   /// ACTIVE bootstrap reserves (ETH/ETH and SOLANA/SOL) with ample balanced
   /// liquidity so try_select_winner's reserve-liquidity gate passes and the
   /// race reaches try_build_swap_remit. Bootstrap window is open (epoch 0:
   /// bootstrap_for_dispatch's advance() gate-blocks on missing emissions).
   void setup_wire_token_and_reserves() {
      deploy(TOKEN_ACCOUNT, contracts::token_wasm(), contracts::token_abi(), token_abi);
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", "sysio")("maximum_supply", "1000000000.000000000 WIRE")));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
         "issue"_n, mvo()("to", "sysio")("quantity", "1000000000.000000000 WIRE")("memo", "seed")));

      BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH",    "ETH", "PRIMARY"));
      BOOST_REQUIRE_EQUAL(success(), regreserve_active("SOLANA", "SOL", "PRIMARY"));
   }

   /// Register the WIRE depot chain (`is_depot = (kind == CHAIN_KIND_WIRE)`), so a
   /// swap whose destination leg is `WIRE` is treated as a to-WIRE swap (depot
   /// destination, no destination UIC, settled inline via reserv::paywire).
   void register_wire_depot() {
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_WIRE)
            ("code",              codename_mvo("WIRE"))
            ("external_chain_id", 0)
            ("name",              std::string("wire-depot"))
            ("description",       std::string{})));
   }

   /// Register SOLANA, seed ACTIVE ETH+SOLANA reserves, credit UWRIT_OP collateral,
   /// and create a PENDING ETH->SOLANA uwreq under `att_id` with source leg
   /// (ETH, ETH, PRIMARY) and destination leg (SOLANA, SOL, PRIMARY). Shared by the
   /// UIC leg-binding and account-name-validation cases below. `bootstrap_for_dispatch`
   /// must have been called first.
   void setup_eth_to_sol_uwreq(uint64_t att_id) {
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
         ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
      setup_wire_token_and_reserves();
      BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000'000));
      BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000'000));

      const auto eth       = fc::slug_name{"ETH"}.value;
      const auto sol_chain = fc::slug_name{"SOLANA"}.value;
      const auto sol_token = fc::slug_name{"SOL"}.value;
      const auto primary   = fc::slug_name{"PRIMARY"}.value;
      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary, 100, sol_chain, sol_token, primary, 100,
         5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(att_id, /*proven=*/ eth, sr));
      BOOST_REQUIRE(!get_uwreq(att_id).is_null());
   }

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
            ("standby_end_rank",          uint32_t(28))
            ("epoch_log_retention_count", uint32_t(8640))
            ("pay_cadence_epochs",        uint16_t(1)))));
      produce_blocks();
   }

   /// Make `sysio.epoch::advance` genuinely advance: everything
   /// `deploy_system_with_emitcfg` sets up, plus the t5 state the gate needs
   /// past its STATE_UNINITIALIZED check. Requires
   /// setup_wire_token_and_reserves() first (payepoch pays WIRE out of sysio's
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

   abi_serializer msgch_abi, opreg_abi, uwrit_abi, epoch_abi, reserv_abi, authex_abi, chains_abi, roa_abi,
                  token_abi;

   std::vector<char> uwrit_op_eth_pubkey;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_dispatch_tests)

BOOST_FIXTURE_TEST_CASE(dispatch_routes_deposit_to_opreg, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t DEPOSIT_AMOUNT = 1'000'000;
   const auto eth_code = fc::slug_name{"ETH"}.value;

   auto operator_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM,
      uwrit_op_eth_pubkey,
      /*chain_code_v=*/ eth_code,
      /*token_code_v=*/ eth_code,
      DEPOSIT_AMOUNT);

   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(),
      sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
      operator_payload);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));

   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   auto bal = find_balance(op, "ETH", "ETH");
   BOOST_REQUIRE(!bal.is_null());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(DEPOSIT_AMOUNT),
                       bal["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(dispatch_routes_withdraw_request_to_opreg, sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t INITIAL_DEPOSIT = 5'000'000;
   constexpr int64_t WITHDRAW_AMOUNT = 2'000'000;
   const auto eth_code = fc::slug_name{"ETH"}.value;

   // The depot dedups per-(batch_op, chain_code, epoch) — a second `deliver`
   // from the same batch op in the same epoch reverts as a duplicate. To
   // exercise both dispatch branches in one test, both attestations ride a
   // single envelope.
   auto deposit_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM,
      uwrit_op_eth_pubkey,
      eth_code, eth_code,
      INITIAL_DEPOSIT);

   auto wtdw_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM,
      uwrit_op_eth_pubkey,
      eth_code, eth_code,
      WITHDRAW_AMOUNT);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code,
      encode_envelope_with_attestations(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
         {deposit_payload, wtdw_payload})));

   auto row = get_wtdw(/*request_id=*/1);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(UWRIT_OP.to_string(),  row["account"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(WITHDRAW_AMOUNT),
                       row["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth_code, row["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth_code, row["token_code"]["value"].as_uint64());
} FC_LOG_AND_RETHROW() }

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

// ───────────────────────────── WSA-005: inbound source-chain binding ─────────────────────────────
//
// A consensus envelope is delivered for exactly ONE proven source outpost (the `deliver` chain_code,
// validated against `sysio.chains`). Every value-bearing attestation it carries embeds its own chain
// identifier; that identifier MUST equal the proven outpost. These tests drive a payload chain that
// diverges from the proven outpost and assert the depot applies NO value-bearing effect — and never
// throws (a throw inside the evalcons dispatch chain stalls consensus chain-wide).

// OPERATOR_ACTION: a DEPOSIT_REQUEST and a WITHDRAW_REQUEST proven-delivered from the ETH outpost but
// whose payloads claim a different active chain (SOLANA) are both dropped — no operator collateral is
// credited and no withdraw is queued. The matched-chain control is `dispatch_routes_deposit_to_opreg`
// / `dispatch_routes_withdraw_request_to_opreg` above, which DO credit/queue.
BOOST_FIXTURE_TEST_CASE(operator_action_mismatched_source_chain_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // ETH outpost + UWRIT_OP (EVM authex link)

   const auto eth_code = fc::slug_name{"ETH"}.value;
   const auto sol_code = fc::slug_name{"SOLANA"}.value;
   constexpr int64_t DEPOSIT_AMOUNT  = 1'000'000;
   constexpr int64_t WITHDRAW_AMOUNT =   400'000;

   // SOLANA is a real, active outpost, so the ONLY thing wrong with the payloads below is that they
   // were proven-delivered from ETH rather than SOLANA — the exact WSA-005 forgery.
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));

   auto deposit_sol = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
      /*chain_code_v=*/ sol_code, /*token_code_v=*/ sol_code, DEPOSIT_AMOUNT);
   auto withdraw_sol = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
      /*chain_code_v=*/ sol_code, /*token_code_v=*/ sol_code, WITHDRAW_AMOUNT);

   // Proven outpost = ETH; both payloads claim SOLANA. deliver() must SUCCEED (the binding drops the
   // attestations inside dispatch — it must not abort the envelope).
   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code,
      encode_envelope_with_attestations(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, {deposit_sol, withdraw_sol})));

   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   BOOST_REQUIRE_EQUAL(0u, op["balances"].get_array().size());   // no collateral on any chain
   BOOST_REQUIRE(get_wtdw(/*request_id=*/1).is_null());          // no withdraw queued
} FC_LOG_AND_RETHROW() }

// SWAP_REQUEST: a swap whose `source_chain_code` does not match the proven delivering outpost must be
// refunded (SwapRevert) and create NO uwreq — settling it would draw against the named source reserve
// while the user's deposit sits on a different chain. Same SwapRequest delivered from its real source
// outpost is the control: it creates the uwreq.
BOOST_FIXTURE_TEST_CASE(swap_request_mismatched_source_chain_is_refunded,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // ETH source outpost
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
   setup_wire_token_and_reserves();   // ACTIVE ETH/ETH/PRIMARY + SOLANA/SOL/PRIMARY reserves
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000'000));

   const auto eth       = fc::slug_name{"ETH"}.value;
   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;

   // A fully valid ETH->SOLANA swap (source leg = ETH).
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      sol_chain, sol_token, primary, /*target*/ 100,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));

   // Mismatch: proven delivering outpost = SOLANA, but source_chain_code = ETH -> refund, no uwreq.
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(/*att_id*/ 9001, /*proven=*/ sol_chain, sr));
   BOOST_REQUIRE(get_uwreq(9001).is_null());

   // Control: same SwapRequest proven-delivered from its real source outpost (ETH) -> uwreq created.
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(/*att_id*/ 9002, /*proven=*/ eth, sr));
   BOOST_REQUIRE(!get_uwreq(9002).is_null());
} FC_LOG_AND_RETHROW() }

// UNDERWRITE_INTENT_COMMIT: the same signed dest-leg (SOLANA) commit, delivered through the FULL
// deliver->evalcons->apply_consensus->dispatch path, is recorded only when its proven outpost matches
// `uic.chain_code`. Delivered from ETH it is dropped (no commit); delivered from SOLANA it lands.
BOOST_FIXTURE_TEST_CASE(underwrite_commit_mismatched_source_chain_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000'000));

   const auto eth       = fc::slug_name{"ETH"}.value;
   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 9100;

   // Create the uwreq (ETH source, SOLANA dest) via the proven ETH outpost.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, 100, sol_chain, sol_token, primary, 100,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, /*proven=*/ eth, sr));
   BOOST_REQUIRE(!get_uwreq(ATT_ID).is_null());

   // One signed dest-leg (SOLANA) UIC, wrapped in an envelope. The outpost it is proven-delivered
   // from is the ONLY thing that varies between the two deliveries below.
   const auto uic_sol = make_signed_uic(UWRIT_OP, ATT_ID,
                                        /*chain_code*/ sol_chain, sol_token, primary);
   const auto uic_env = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT,
      std::string(uic_sol.begin(), uic_sol.end()));

   auto dest_committed = [&]() {
      auto req = get_uwreq(ATT_ID);
      for (const auto& c : req["commits_by"].get_array())
         if (c["underwriter"].as_string() == UWRIT_OP.to_string() &&
             c["dest_received_at_ms"].as_uint64() != 0)
            return true;
      return false;
   };

   // Mismatch: a SOLANA-leg commit proven-delivered from the ETH outpost is dropped — no commit.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ eth, uic_env));
   BOOST_REQUIRE(!dest_committed());

   // Control: the SAME commit proven-delivered from the SOLANA outpost is recorded.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ sol_chain, uic_env));
   BOOST_REQUIRE(dest_committed());
} FC_LOG_AND_RETHROW() }

// SEC-13/WSA-027: two chains of the SAME VM family (ETH + a second EVM chain)
// must be disambiguated by EXACT chain_code at the depot — never collapsed onto
// one ChainKind. A commit for the SECOND EVM chain's leg, proven-delivered from
// the FIRST EVM chain's outpost, is dropped (its chain_code != the proven
// outpost); delivered from the second EVM outpost it lands. This is the
// same-family analogue of underwrite_commit_mismatched_source_chain_is_dropped
// and pins the two-same-kind-chain invariant WSA-027 is about.
BOOST_FIXTURE_TEST_CASE(underwrite_commit_two_evm_chains_route_per_chain,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // ETH (EVM) source outpost
   // A SECOND active EVM chain — same VM family, distinct chain_code.
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_EVM)("code", codename_mvo("POLYGON"))
      ("external_chain_id", 137)("name", std::string("polygon-test"))("description", std::string{})));
   // SOLANA is registered only because the shared reserve-setup helper seeds a
   // SOLANA/SOL reserve; it is otherwise unused by this two-EVM scenario.
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("POLYGON", "POL", "PRIMARY"));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",     "ETH", 1'000'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "POLYGON", "POL", 1'000'000'000));

   const auto eth     = fc::slug_name{"ETH"}.value;
   const auto polygon = fc::slug_name{"POLYGON"}.value;
   const auto pol_tok = fc::slug_name{"POL"}.value;
   const auto primary = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 9200;

   // ETH -> POLYGON swap (both EVM). Proven source outpost = ETH.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, 100, polygon, pol_tok, primary, 100,
      5000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0c'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, /*proven=*/ eth, sr));
   BOOST_REQUIRE(!get_uwreq(ATT_ID).is_null());

   // One signed dest-leg (POLYGON) UIC; the proven delivering outpost is the
   // only thing that varies between the two deliveries below.
   const auto uic_pol = make_signed_uic(UWRIT_OP, ATT_ID, /*chain_code*/ polygon, pol_tok, primary);
   const auto uic_env = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT,
      std::string(uic_pol.begin(), uic_pol.end()));

   auto dest_committed = [&]() {
      auto req = get_uwreq(ATT_ID);
      for (const auto& c : req["commits_by"].get_array())
         if (c["underwriter"].as_string() == UWRIT_OP.to_string() &&
             c["dest_received_at_ms"].as_uint64() != 0)
            return true;
      return false;
   };

   // Mismatch: a POLYGON-leg commit proven-delivered from the ETH outpost (same
   // VM family, different chain_code) is dropped — exact chain_code != proven ETH.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ eth, uic_env));
   BOOST_REQUIRE(!dest_committed());

   // Control: the SAME commit proven-delivered from the POLYGON outpost lands.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ polygon, uic_env));
   BOOST_REQUIRE(dest_committed());
} FC_LOG_AND_RETHROW() }

// A decode-clean UIC whose `uw_account.name` is nonempty but not a constructible account name
// (uppercase, hyphen, over-long) must be dropped inside dispatch WITHOUT throwing: constructing
// `name{}` from it would abort the evalcons/apply_consensus transaction and stall consensus
// chain-wide. All three malformed names ride one envelope so a single consensus delivery exercises
// them (a second deliver from the same operator+epoch would revert as a duplicate).
BOOST_FIXTURE_TEST_CASE(underwrite_commit_invalid_account_name_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9200;
   setup_eth_to_sol_uwreq(ATT_ID);

   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;

   const auto uic_upper  = make_uic_raw_name("BADNAME",        ATT_ID, sol_chain, sol_token, primary);
   const auto uic_hyphen = make_uic_raw_name("bad-name",       ATT_ID, sol_chain, sol_token, primary);
   const auto uic_long   = make_uic_raw_name("abcdefghijklmn", ATT_ID, sol_chain, sol_token, primary);
   const auto env = encode_envelope_with_attestations(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT,
      {std::string(uic_upper.begin(),  uic_upper.end()),
       std::string(uic_hyphen.begin(), uic_hyphen.end()),
       std::string(uic_long.begin(),   uic_long.end())});

   // deliver() must SUCCEED — the malformed names are dropped inside dispatch, never thrown.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ sol_chain, env));
   // ...and nothing was recorded against the request.
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
} FC_LOG_AND_RETHROW() }

// A UIC whose (chain_code, token_code, reserve_code) triple matches neither the source nor the
// destination leg of the pending request must leave commits_by untouched — no inert commit_entry,
// no mutation. rcrdcommit is msgch-authorized; call it directly.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_unmatched_leg_leaves_commits_empty,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9300;
   setup_eth_to_sol_uwreq(ATT_ID);   // src=(ETH,ETH,PRIMARY) dst=(SOLANA,SOL,PRIMARY)

   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;
   const auto uic = make_signed_uic(UWRIT_OP, ATT_ID, sol_chain, sol_token, primary);

   // (SOLANA, ETH, PRIMARY): source-chain differs from the dest leg, token differs from the source
   // leg — matches neither.
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, sol_chain, "SOLANA", "ETH", "PRIMARY", uic));
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
} FC_LOG_AND_RETHROW() }

// Repeating unmatched commits with distinct underwriter names must not grow the row — the
// pre-mutation leg guard is what closes the storage/scan-cost bloat vector.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_unmatched_distinct_underwriters_do_not_grow_row,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9301;
   setup_eth_to_sol_uwreq(ATT_ID);

   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;
   const auto uic = make_signed_uic(UWRIT_OP, ATT_ID, sol_chain, sol_token, primary);

   for (name uw : {"uwtwo"_n, "uwthree"_n, "uwfour"_n}) {
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(ATT_ID, uw, sol_chain, "SOLANA", "ETH", "PRIMARY", uic));
   }
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
} FC_LOG_AND_RETHROW() }

// Control for the leg-branch refactor: a UIC that DOES match the source leg still records a
// commit_entry with the source arrival slot armed.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_matched_source_leg_is_recorded,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9302;
   setup_eth_to_sol_uwreq(ATT_ID);

   const auto eth     = fc::slug_name{"ETH"}.value;
   const auto primary = fc::slug_name{"PRIMARY"}.value;
   const auto uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);

   // (ETH, ETH, PRIMARY) is the source leg.
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", uic));

   const auto commits = get_uwreq(ATT_ID)["commits_by"].get_array();
   BOOST_REQUIRE_EQUAL(1u, commits.size());
   BOOST_REQUIRE_EQUAL(UWRIT_OP.to_string(), commits[0]["underwriter"].as_string());
   BOOST_REQUIRE(commits[0]["source_received_at_ms"].as_uint64() != 0);
} FC_LOG_AND_RETHROW() }

// Even on a matched leg, a UIC whose account name is a valid `name` but NOT a registered ACTIVE
// underwriter must not create a commit_entry: only active underwriters can win, so gating entry
// creation on activation bounds commits_by to the legitimate racer set and blocks the matched-leg
// name-varying bloat vector (varying valid account names would otherwise append one entry per name).
BOOST_FIXTURE_TEST_CASE(rcrdcommit_matched_leg_non_underwriter_names_do_not_grow_row,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9303;
   setup_eth_to_sol_uwreq(ATT_ID);   // src=(ETH,ETH,PRIMARY)

   const auto eth = fc::slug_name{"ETH"}.value;
   const std::vector<char> uic(8, '\x00');   // opaque: dropped before the bytes are ever read

   // (ETH, ETH, PRIMARY) IS the source leg, so these clear the leg-binding guard; each name is a
   // valid sysio::name but none is a registered underwriter, so all are dropped before mutation.
   for (name uw : {"alice"_n, "bob"_n, "carol"_n}) {
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(ATT_ID, uw, eth, "ETH", "ETH", "PRIMARY", uic));
   }
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
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

// The inbound `deliver` boundary enforces the same 64 KiB protocol envelope cap the outbound
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
   // Depositor's ETH key (EM, 33-byte compressed).
   auto eth_pub = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_pubkey_bytes(eth_pub);

   auto payload = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes);
   auto envelope = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code, envelope));

   // Registered at the claimed tier, audited CONFIRMED.
   auto reg = get_nodeowner(CLAIM_ACCOUNT);
   BOOST_REQUIRE(!reg.is_null());
   BOOST_REQUIRE_EQUAL(reg["tier"].as<uint32_t>(), 2u);
   auto audit = get_nodeownerreg(CLAIM_ACCOUNT);
   BOOST_REQUIRE(!audit.is_null());
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), 0u);  // CONFIRMED
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
      ("kind", ChainKind::CHAIN_KIND_EVM)("code", codename_mvo("ETHEREUM"))
      ("external_chain_id", 1)("name", std::string("ethereum-mainnet"))("description", std::string{})));
   const auto other_evm = fc::slug_name{"ETH"}.value;   // active EVM outpost, but not "ETHEREUM"

   auto wire_key  = k1_pubkey_bytes(get_public_key(CLAIM_ACCOUNT, "active"));
   auto eth_pub   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   auto eth_bytes = em_pubkey_bytes(eth_pub);
   auto payload   = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes);
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
   auto eth_bytes = em_pubkey_bytes(eth_pub);
   auto payload   = encode_node_owner_registration(
      CLAIM_ACCOUNT.to_string(), /*tier=*/2,
      sysio::opp::types::WIRE_KEY_TYPE_K1, wire_key, eth_bytes);
   auto envelope  = encode_envelope_with_one_attestation(
      current_epoch(), sysio::opp::types::ATTESTATION_TYPE_NODE_OWNER_REG, payload);

   // Proven outpost = SOLANA (SVM), not "ETHEREUM"; the exact-chain binding drops it. deliver() still
   // succeeds (no throw) and reaches consensus.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/sol_code, envelope));

   // Nothing was sent to sysio.roa: no node-owner registration and no audit row.
   BOOST_REQUIRE(get_nodeowner(CLAIM_ACCOUNT).is_null());
   BOOST_REQUIRE(get_nodeownerreg(CLAIM_ACCOUNT).is_null());
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
   setup_wire_token_and_reserves();   // payepoch pays WIRE out of sysio's token balance
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

// #5-residual: a race winner lacking a destination-chain authex link must be
// DISQUALIFIED (skipped, uwreq left PENDING), reached via try_build_swap_remit
// BEFORE any CONFIRMED / reserve write so nothing throws in evalcons.
BOOST_FIXTURE_TEST_CASE(swap_winner_without_dst_authex_link_is_disqualified,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // registers ETH + UWRIT_OP (EVM link only)

   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
      "regchain"_n, mvo()
         ("kind",              ChainKind::CHAIN_KIND_SVM)
         ("code",              codename_mvo("SOLANA"))
         ("external_chain_id", 900)
         ("name",              std::string("solana-test"))
         ("description",       std::string{})));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t sol_chain = fc::slug_name{"SOLANA"}.value;
   const uint64_t sol_token = fc::slug_name{"SOL"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID    = 5000;
   constexpr int64_t  SRC_AMOUNT = 100;
   constexpr uint64_t DST_AMOUNT = 100;

   setup_wire_token_and_reserves();

   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000));

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      sol_chain, sol_token, primary, DST_AMOUNT,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, sol_chain, sol_token, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, sol_chain, "SOLANA", "SOL", "PRIMARY", dst_uic));

   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   bool found = false;
   for (const auto& c : req["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string()) {
         found = true;
         BOOST_REQUIRE_EQUAL("UNDERWRITE_STATUS_DISQUALIFIED", c["status"].as_string());
         BOOST_REQUIRE(c["reason"].as_string().find(
            "no authex link for the destination") != std::string::npos);
      }
   }
   BOOST_REQUIRE(found);
} FC_LOG_AND_RETHROW() }

// [P1] WSA-041: a zero AMM quote from ACTIVE reserves must FAIL CLOSED, not skip
// the variance check. createuwreq overloaded a zero `swap_quote` to mean "no LP
// provisioned, skip variance" — but a zero quote also arises from a degenerate
// ACTIVE reserve (extreme connector weights, a drained side, or — as here — a
// `src_amount` so small the weighted-Bancor kernel floors the output to 0). With
// variance skipped, the caller-supplied target_amount flowed straight into the
// uwreq and on to settlement, letting a ~0 input debit an arbitrary amount.
// Post-fix: when every required reserve is ACTIVE, a zero quote emits SWAP_REVERT
// and creates NO uwreq. (try_select_winner and drainfwq carry the same guard.)
BOOST_FIXTURE_TEST_CASE(swap_zero_quote_from_active_reserve_fails_closed,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // registers ETH

   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
      "regchain"_n, mvo()
         ("kind",              ChainKind::CHAIN_KIND_SVM)
         ("code",              codename_mvo("SOLANA"))
         ("external_chain_id", 900)
         ("name",              std::string("solana-test"))
         ("description",       std::string{})));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t sol_chain = fc::slug_name{"SOLANA"}.value;
   const uint64_t sol_token = fc::slug_name{"SOL"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 6100;

   // Seeds ETH/ETH and SOLANA/SOL ACTIVE reserves at 1e12/1e12.
   setup_wire_token_and_reserves();

   // src_amount = 1 against a 1e12 reserve floors token_to_wire to 0, so the
   // chained quote is 0 — but both reserves are ACTIVE. The (huge) target must
   // NOT slip through on a skipped variance check.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 1,
      sol_chain, sol_token, primary, /*target_amount*/ 900'000'000'000ull,
      /*tolerance_bps*/ 5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
   // createuwreq never throws (it emits SWAP_REVERT and returns).
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   // Fail closed: no uwreq was created from the unpriceable, ACTIVE-reserve swap.
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// [P0] WNS-02 (CertiK "Wire Network - Sysio Audit 1", Critical): a swap must
// settle on the AMM QUOTE, never on the caller's `target_amount`.
//
// The attack: `createuwreq` stored `dst_amount = sr.target_amount` — a number
// the caller puts in the SwapRequest — and the only guard computed the allowed
// deviation as a percentage of that SAME caller-supplied target
// (`allowed = target * target_tolerance_bps / 10000`). At 10000 bps the check
// reduces to `|quote - target| <= target`, which every target above the quote
// satisfies. `try_select_winner` then locked and `sysio.reserv::applyswap` paid
// out `req.dst_amount` verbatim, so a negligible source deposit drained the
// destination reserve.
//
// Post-fix, both halves are closed and this pins each:
//   (a) the allowance is a fraction of the QUOTE, so the drain request is
//       refused outright — no uwreq exists to settle;
//   (b) even for a target the tolerance legitimately admits, the row carries
//       the quote, so the settlement amount is not caller-controlled at all.
BOOST_FIXTURE_TEST_CASE(swap_settles_on_amm_quote_not_caller_target,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_eth_to_sol_uwreq(/*att_id*/ 9500);   // registers SOLANA, reserves, collateral

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t sol_chain = fc::slug_name{"SOLANA"}.value;
   const uint64_t sol_token = fc::slug_name{"SOL"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr int64_t SRC_AMOUNT = 1'000'000;

   const uint64_t quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                         "SOLANA", "SOL", "PRIMARY", kDefaultUwritFeeBps);
   BOOST_REQUIRE(quote > 0);

   auto submit = [&](uint64_t att_id, uint64_t target, uint32_t tolerance_bps) {
      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary, SRC_AMOUNT,
         sol_chain, sol_token, primary, target,
         tolerance_bps, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(att_id, eth, sr));
   };

   // (a) THE EXPLOIT, verbatim: a tiny source deposit naming most of the
   // destination reserve, with the 100% tolerance that used to make the check
   // pass unconditionally. Refused at ingestion — no uwreq, nothing to settle.
   constexpr uint64_t DRAIN_TARGET = 900'000'000'000ull;   // 90% of the 1e12 reserve
   submit(/*att_id*/ 9501, DRAIN_TARGET, /*tolerance_bps*/ 10'000);
   BOOST_REQUIRE(get_uwreq(9501).is_null());

   // A tolerance ABOVE 100% must not buy the caller anything either — the
   // allowance is clamped, so the same drain is still refused.
   submit(/*att_id*/ 9502, DRAIN_TARGET, /*tolerance_bps*/ 1'000'000);
   BOOST_REQUIRE(get_uwreq(9502).is_null());

   // (b) A target the tolerance genuinely admits (1.5x the quote at 100%
   // tolerance: |quote - target| = 0.5*quote <= quote). The request is accepted
   // — and the row still carries the QUOTE, not the inflated target.
   const uint64_t inflated_target = quote + quote / 2;
   submit(/*att_id*/ 9503, inflated_target, /*tolerance_bps*/ 10'000);
   const auto accepted = get_uwreq(9503);
   BOOST_REQUIRE(!accepted.is_null());
   BOOST_REQUIRE_EQUAL(quote, accepted["dst_amount"].as_uint64());
   BOOST_REQUIRE_LT(accepted["dst_amount"].as_uint64(), inflated_target);
} FC_LOG_AND_RETHROW() }

// [P0] WNS-02, settlement half: the destination reserve is debited by the AMM
// quote and by nothing else. The row-level assertion above proves `dst_amount`
// is the quote; this drives the full race to CONFIRMED and reads the reserve
// books, so the guarantee is pinned where the funds actually move
// (`sysio.reserv::applyswap`), which is where the drain happened.
BOOST_FIXTURE_TEST_CASE(swap_settlement_debits_destination_reserve_by_the_quote,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   // Both legs on ETH (PRIMARY -> SECOND): UWRIT_OP's only authex link is EVM,
   // so an ETH destination is what lets the race actually reach settlement.
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID     = 9601;
   constexpr int64_t  SRC_AMOUNT = 1'000'000;

   // One bucket funds both legs, so it must cover src + quote.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));

   const uint64_t quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                         "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   BOOST_REQUIRE(quote > 0);

   const uint64_t dst_chain_before =
      find_reserve("ETH", "ETH", "SECOND")["reserve_chain_amount"].as_uint64();
   const uint64_t src_chain_before =
      find_reserve("ETH", "ETH", "PRIMARY")["reserve_chain_amount"].as_uint64();

   // Target 20% above the quote — inside a 100% tolerance, so the race resolves
   // and the ONLY thing that can decide the payout is the curve.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      eth, eth, secondary, /*target_amount*/ quote + quote / 5,
      /*tolerance_bps*/ 10'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED", req["status"].as_string());
   BOOST_REQUIRE_EQUAL(UWRIT_OP.to_string(), req["winner"].as_string());
   // The CONFIRMED row records the price the books moved at.
   BOOST_REQUIRE_EQUAL(quote, req["dst_amount"].as_uint64());

   // Destination reserve gave up exactly the quote; the source reserve took in
   // exactly the source amount. Neither is the caller's target.
   const uint64_t dst_chain_after =
      find_reserve("ETH", "ETH", "SECOND")["reserve_chain_amount"].as_uint64();
   const uint64_t src_chain_after =
      find_reserve("ETH", "ETH", "PRIMARY")["reserve_chain_amount"].as_uint64();
   BOOST_REQUIRE_EQUAL(dst_chain_before - quote, dst_chain_after);
   BOOST_REQUIRE_EQUAL(src_chain_before + static_cast<uint64_t>(SRC_AMOUNT), src_chain_after);

   // The destination lock — the collateral the winner has at risk — is sized to
   // the delivered amount, so the challenge window covers the real obligation.
   BOOST_REQUIRE_EQUAL(quote, get_lock(2)["amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

// [P0] Review follow-up (PR #550): the slippage bound must NOT compound across
// the two checkpoints. The row's `dst_amount` is re-priced at race resolution,
// so measuring race-time drift against it would compare the settlement quote
// with the PREVIOUS quote instead of with what the user actually asked for —
// letting the price walk one tolerance-step per checkpoint. The reviewer's
// example: target 100 at 10% accepts an ingestion quote of 91 (9 <= 9.1) and
// then a settlement quote of 83 (8 <= 8.3), delivering 17% below target.
//
// `target_amount` is retained on the row precisely so this check anchors on the
// user's ORIGINAL bound, whatever path the price took.
BOOST_FIXTURE_TEST_CASE(swap_slippage_bound_does_not_compound_across_checkpoints,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID       = 9800;
   constexpr int64_t  SRC_AMOUNT   = 1'000'000;
   constexpr uint32_t TOLERANCE_BPS = 1'000;   // 10%

   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));

   // Target ~9% above the ingestion quote — inside the 10% bound, so the request
   // is accepted and the row records the quote.
   const uint64_t ingestion_quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                                   "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   BOOST_REQUIRE(ingestion_quote > 0);
   const uint64_t target = ingestion_quote + (ingestion_quote * 9 / 100);

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      eth, eth, secondary, target,
      TOLERANCE_BPS, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE(!req.is_null());
      BOOST_REQUIRE_EQUAL(ingestion_quote, req["dst_amount"].as_uint64());
      BOOST_REQUIRE_EQUAL(target,          req["target_amount"].as_uint64());
   }

   // Move the destination reserve so the price walks a second step down: debit
   // 7% of its token side (UWRIT-authorized, the same primitive settlement uses).
   BOOST_REQUIRE_EQUAL(success(), push(RESERV_ACCOUNT, reserv_abi, UWRIT_ACCOUNT, "debit"_n, mvo()
      ("chain_code",   codename_mvo("ETH"))
      ("token_code",   codename_mvo("ETH"))
      ("reserve_code", codename_mvo("SECOND"))
      ("amount",       uint64_t{70'000'000'000})));

   // Pin the scenario: the drift is inside tolerance of the PREVIOUS quote (so
   // the compounding rule would have let it settle) but outside tolerance of the
   // user's TARGET (so the anchored rule must reject). If the fixture's
   // arithmetic ever drifts out of this window the test says so here, rather
   // than silently asserting nothing.
   const uint64_t settle_quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                                "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   const uint64_t allowed = settle_quote * TOLERANCE_BPS / 10'000;
   BOOST_REQUIRE_LE(ingestion_quote - settle_quote, allowed);   // old rule: would pass
   BOOST_REQUIRE_GT(target - settle_quote,          allowed);   // new rule: must reject

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_REJECTED", req["status"].as_string());
   bool drift = false;
   for (const auto& c : req["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string())
         drift = c["reason"].as_string().find("variance drift") != std::string::npos;
   }
   BOOST_REQUIRE(drift);
} FC_LOG_AND_RETHROW() }

// [P1] Review follow-up (PR #550): an under-bonded candidate must not be able to
// terminally close someone else's swap. The variance / unpriceable verdicts
// refund the user and end the request, so they run only after a candidate has
// proved it could actually settle. An ACTIVE underwriter can clear the role
// minimum yet lack collateral for a particular swap; if it could trigger a
// terminal refund during transient drift, any such operator would hold a
// denial-of-service over other people's swaps.
BOOST_FIXTURE_TEST_CASE(swap_underbonded_candidate_cannot_terminally_reject,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID       = 9850;
   constexpr int64_t  SRC_AMOUNT   = 1'000'000;
   constexpr uint32_t TOLERANCE_BPS = 1'000;   // 10%

   // Clears the 1-unit role minimum (so the candidate is an ACTIVE underwriter)
   // but is nowhere near the src + dst aggregate this swap needs.
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 10));

   const uint64_t ingestion_quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                                   "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      eth, eth, secondary, ingestion_quote,
      TOLERANCE_BPS, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   // Drift the price far outside the tolerance — the request IS terminally
   // doomed, but this candidate must not be the one to close it.
   BOOST_REQUIRE_EQUAL(success(), push(RESERV_ACCOUNT, reserv_abi, UWRIT_ACCOUNT, "debit"_n, mvo()
      ("chain_code",   codename_mvo("ETH"))
      ("token_code",   codename_mvo("ETH"))
      ("reserve_code", codename_mvo("SECOND"))
      ("amount",       uint64_t{500'000'000'000})));

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

   // Still PENDING (not REJECTED): the candidate was disqualified on bond, and
   // the request stays open for an underwriter that can actually settle it.
   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   bool bond_dq = false;
   for (const auto& c : req["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string()) {
         BOOST_REQUIRE_EQUAL("UNDERWRITE_STATUS_DISQUALIFIED", c["status"].as_string());
         bond_dq = c["reason"].as_string().find("insufficient bond") != std::string::npos;
      }
   }
   BOOST_REQUIRE(bond_dq);
} FC_LOG_AND_RETHROW() }

// [P0] WNS-01 (CertiK "Wire Network - Sysio Audit 1", Critical): `opreg::prune`
// must not erase a TERMINATED operator whose collateral has not settled.
//
// `terminate` remits only the immediately-unlocked portion and deliberately
// LEAVES the locked remainder on the row, because `releaselock` settles it when
// `chklocks` frees each lock. `prune` gated on `terminated_at` + the prune delay
// alone, so any caller — it is permissionless — could erase the row first;
// `releaselock` then returns at its missing-operator check and the retained
// collateral is stranded with no attestation that could ever release it.
BOOST_FIXTURE_TEST_CASE(prune_keeps_terminated_operator_until_collateral_settles,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   // A 1s prune delay (vs the fixture's 10min) so the delay gate is satisfied
   // within the test — isolating the SETTLEMENT gate as the only thing holding
   // the row. The underwriter minimum is re-declared verbatim: setconfig
   // replaces the whole config.
   BOOST_REQUIRE_EQUAL(success(),
      opreg_setconfig_collat(fc::variants{chain_min_bond_mvo("ETH", "ETH", 1)},
                             fc::variants{}, /*prune_delay_ms*/ 1'000));
   // A 2-minute lock window, so `chklocks` can expire it inside the test.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", kDefaultUwritFeeBps)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 0)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID     = 9700;
   constexpr int64_t  SRC_AMOUNT = 1'000'000;

   // Both legs on ETH — UWRIT_OP is authex-linked on EVM only, so this is the
   // shape whose race actually reaches CONFIRMED and writes locks.
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));
   enable_epoch_advancement();

   // Win a race so UWRIT_OP holds live locks on both legs.
   const uint64_t quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                         "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   BOOST_REQUIRE(quote > 0);
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      eth, eth, secondary, /*target_amount*/ quote,
      /*tolerance_bps*/ 10'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));
   BOOST_REQUIRE(!get_uwreq(ATT_ID).is_null());
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED",
                       get_uwreq(ATT_ID)["status"].as_string());
   BOOST_REQUIRE(!get_lock(1).is_null());
   BOOST_REQUIRE(!get_lock(2).is_null());

   // Terminate: the unlocked portion is remitted, the locked portion stays on
   // the row for `releaselock`.
   BOOST_REQUIRE_EQUAL(success(), terminate_op(UWRIT_OP, "audit-test termination"));
   {
      const auto op = get_operator(UWRIT_OP);
      BOOST_REQUIRE(!op.is_null());
      BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_TERMINATED", op["status"].as_string());
      uint64_t retained = 0;
      for (const auto& b : op["balances"].get_array()) retained += b["balance"].as_uint64();
      BOOST_REQUIRE_GT(retained, 0u);   // exactly the locked collateral
   }

   // Past the prune delay, with locks still live: the row MUST survive. This is
   // the vulnerability — pre-fix the delay alone was sufficient to erase it.
   produce_blocks(4);   // > 1s at 0.5s/block
   BOOST_REQUIRE_EQUAL(success(), opreg_prune());
   BOOST_REQUIRE(!get_operator(UWRIT_OP).is_null());

   // Let the 2-minute lock window elapse. Each advance runs `chklocks` inline,
   // which frees the expired locks and fans out `opreg::releaselock` — the
   // deferred remit that drains the retained balance.
   age_one_epoch();
   age_one_epoch();
   age_one_epoch();
   BOOST_REQUIRE(get_lock(1).is_null());
   BOOST_REQUIRE(get_lock(2).is_null());
   {
      const auto op = get_operator(UWRIT_OP);
      BOOST_REQUIRE(!op.is_null());
      for (const auto& b : op["balances"].get_array())
         BOOST_REQUIRE_EQUAL(0u, b["balance"].as_uint64());
   }

   // Settled — now the row is genuinely disposable and prune erases it.
   BOOST_REQUIRE_EQUAL(success(), opreg_prune());
   BOOST_REQUIRE(get_operator(UWRIT_OP).is_null());
} FC_LOG_AND_RETHROW() }

// [P0] Review follow-up (PR #550): `prune` is not the only path that destroys a
// TERMINATED row — `regoperator` REPLACES one to allow re-registration, and it
// strands collateral the same way. A terminated underwriter re-registering while
// its locks are live would swap the retained balance row for a fresh healthy
// one; `chklocks` then fans out `releaselock`, which settles only SLASHED /
// TERMINATED operators, sees a healthy row, and no-ops. Every terminated-row
// erase path carries the settlement precondition.
BOOST_FIXTURE_TEST_CASE(reregistration_blocked_until_collateral_settles,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", kDefaultUwritFeeBps)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 0)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID     = 9900;
   constexpr int64_t  SRC_AMOUNT = 1'000'000;

   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));
   enable_epoch_advancement();

   // Win a race so the operator holds live locks.
   const uint64_t quote = expected_quote("ETH", "ETH", "PRIMARY", SRC_AMOUNT,
                                         "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, SRC_AMOUNT,
      eth, eth, secondary, quote,
      /*tolerance_bps*/ 10'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));
   BOOST_REQUIRE(!get_lock(1).is_null());

   BOOST_REQUIRE_EQUAL(success(), terminate_op(UWRIT_OP, "audit-test termination"));

   auto reregister = [&]() {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account",         UWRIT_OP.to_string())
         ("type",            OperatorType::OPERATOR_TYPE_UNDERWRITER)
         ("is_bootstrapped", false));
   };

   // Locks live + balance retained: re-registration must be refused, so the
   // terminated row (and the collateral it accounts for) survives intact.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator has unsettled collateral: a terminated "
            "operator may only re-register once its balances are drained and no underwriting "
            "locks remain"),
      reregister());
   {
      const auto op = get_operator(UWRIT_OP);
      BOOST_REQUIRE(!op.is_null());
      BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_TERMINATED", op["status"].as_string());
      uint64_t retained = 0;
      for (const auto& b : op["balances"].get_array()) retained += b["balance"].as_uint64();
      BOOST_REQUIRE_GT(retained, 0u);
   }

   // Let the locks expire; `chklocks` -> `releaselock` drains the balance.
   age_one_epoch();
   age_one_epoch();
   age_one_epoch();
   BOOST_REQUIRE(get_lock(1).is_null());

   // Settled — re-registration is allowed again, with a clean row.
   BOOST_REQUIRE_EQUAL(success(), reregister());
   const auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   BOOST_REQUIRE(op["status"].as_string() != "OPERATOR_STATUS_TERMINATED");
} FC_LOG_AND_RETHROW() }

// [P1] regression (r3444213199) + WNS-02: an oversized to-WIRE target_amount
// must never reach settlement. Originally the target WAS the settlement amount,
// so a value near UINT64_MAX wrapped `dst_amount + to_wire_fee`, slipped into
// paywire, and aborted its `asset(static_cast<int64_t>(dst_amount))` inside
// evalcons — a chain-wide consensus stall — which the downstream
// `asset::max_amount` guard was added to catch.
//
// With settlement priced by the curve (WNS-02) the row never carries the target
// at all, and the variance check refuses these at INGRESS: the allowance is a
// fraction of the quote, so a target orders of magnitude above it fails for any
// tolerance. That is strictly stronger than the old outcome (no uwreq is created
// at all, versus one created then terminally rejected after a commit), and it
// makes the downstream asset-bound guard unreachable defense-in-depth — kept in
// the contract, but no longer constructible from a SwapRequest.
BOOST_FIXTURE_TEST_CASE(swap_to_wire_oversized_target_is_refused_at_ingress,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();          // ETH source outpost + UWRIT_OP (EVM link)
   register_wire_depot();             // WIRE depot => to-WIRE destination
   setup_wire_token_and_reserves();   // ACTIVE ETH/ETH/PRIMARY source reserve w/ WIRE

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   // Large enough that the source leg prices well above the kernel's floor, so
   // the refusal is the variance check and not the unpriceable-reserve gate.
   constexpr int64_t SRC_AMOUNT = 1'000'000;
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));

   // A valid existing WIRE recipient, so nothing earlier in the path rejects.
   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());

   constexpr uint64_t ASSET_MAX = (uint64_t{1} << 62) - 1;
   auto run = [&](uint64_t att_id, uint64_t target) {
      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary, SRC_AMOUNT,
         wire, wire, primary, target,
         // 100x the meaningful maximum: `variance_allowance` clamps it to 100%,
         // so even the widest tolerance a caller can name cannot admit a target
         // this far from the quote. Pre-fix, this tolerance let it straight in.
         /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_WIRE, rcpt);
      // createuwreq never throws — it emits SWAP_REVERT and returns.
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(att_id, eth, sr));
      BOOST_REQUIRE(get_uwreq(att_id).is_null());
   };

   run(7001, ASSET_MAX + 1);        // target == asset::max_amount + 1 (boundary)
   run(7002, uint64_t{1} << 63);    // high bit set (negative as int64)
   run(7003, ~uint64_t{0});         // UINT64_MAX — wrapped dst_amount + fee pre-fix
} FC_LOG_AND_RETHROW() }

// Regression (r3444212152): a candidate whose UIC signature does not recover to
// its active/owner key must be DISQUALIFIED so the race state converges — not
// silently left INTENT_SUBMITTED, keeping the uwreq pending/noisy until another
// underwriter wins. The handler stays non-throwing (no consensus stall).
BOOST_FIXTURE_TEST_CASE(swap_candidate_with_invalid_uic_signature_is_disqualified,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   register_wire_depot();             // to-WIRE: a single (source) required leg

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 7200;
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000}));

   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      wire, wire, primary, /*target*/ 50,
      5000, ChainKind::CHAIN_KIND_WIRE, rcpt);
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   // Source UIC signed by the WRONG account (batchop.a): the recovered key does
   // not match UWRIT_OP's active/owner permission, so verify_uic_signature
   // returns false. The push must still succeed (non-throwing).
   const auto bad_uic = make_signed_uic(BATCHOP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", bad_uic));

   const auto req = get_uwreq(ATT_ID);
   // Race left open (PENDING) but the bad candidate is DISQUALIFIED.
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   bool found = false;
   for (const auto& c : req["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string()) {
         found = true;
         BOOST_REQUIRE_EQUAL("UNDERWRITE_STATUS_DISQUALIFIED", c["status"].as_string());
         BOOST_REQUIRE(c["reason"].as_string().find("signature") != std::string::npos);
      }
   }
   BOOST_REQUIRE(found);
} FC_LOG_AND_RETHROW() }

// ── Underwriter role + activation gate at commit ingestion ───────────────────
//
// Only an ACTIVE UNDERWRITER (opreg type == UNDERWRITER && status == ACTIVE) can
// win a race, so `rcrdcommit` refuses to record a commit_entry for anything else:
// a UIC from a non-underwriter, or a not-yet-active underwriter, is dropped before
// any mutation. This bounds `commits_by` to the registered active-underwriter set,
// so a matched-leg UIC cannot append one row per attacker-chosen valid name.
// `try_select_winner` keeps the same eligibility check as defensive depth. Both
// cases below carry real ETH bond and a valid self-signed UIC; the ingestion gate
// drops them (no row, no lock) and leaves the race PENDING (reclaimable) for a
// genuine winner. The positive control — an ACTIVE underwriter that wins — is
// `swap_same_token_legs_exact_balance_wins`.

// A non-underwriter operator that is fully ACTIVE and bonded — a funded PRODUCER —
// cannot have a commit recorded even with a valid self-signed UIC. Activating it
// (status ACTIVE) isolates the op.type half of the gate: a status-only check would
// let it through, so this test fails if the type check is dropped.
BOOST_FIXTURE_TEST_CASE(swap_commit_non_underwriter_type_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();          // ETH source outpost + UWRIT_OP
   register_wire_depot();             // to-WIRE: a single (source) required leg

   // Require producer ETH collateral too, so funding the producer below promotes
   // it to ACTIVE via opreg::processprod (the underwriter requirement stays as
   // bootstrap set it).
   BOOST_REQUIRE_EQUAL(success(), opreg_setconfig_collat(
      /*req_uw_collat=*/   fc::variants{chain_min_bond_mvo("ETH", "ETH", 1)},
      /*req_prod_collat=*/ fc::variants{chain_min_bond_mvo("ETH", "ETH", 1)}));

   // A second operator registered as a PRODUCER (NOT an underwriter). Privileged
   // opreg self-registration skips the authex-link precondition.
   const name PRODOP = "prodop.a"_n;
   create_account(PRODOP);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
      "regoperator"_n, mvo()
         ("account",         PRODOP.to_string())
         ("type",            OperatorType::OPERATOR_TYPE_PRODUCER)
         ("is_bootstrapped", false)));

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 7400;

   // Fund ETH bond: covers the source leg AND clears req_prod_collat, so
   // processprod flips PRODOP to ACTIVE. The candidate is now an ACTIVE,
   // sufficiently-bonded operator that is simply the wrong role — only op.type
   // can disqualify it.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(PRODOP, "ETH", "ETH", uint64_t{1'000'000}));
   {
      const auto op = get_operator(PRODOP);
      BOOST_REQUIRE_EQUAL("OPERATOR_TYPE_PRODUCER", op["type"].as_string());
      BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op["status"].as_string());
   }

   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      wire, wire, primary, /*target*/ 50,
      5000, ChainKind::CHAIN_KIND_WIRE, rcpt);
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   // Valid UIC self-signed by PRODOP — signature recovery passes, so only the
   // eligibility gate can reject it.
   const auto uic = make_signed_uic(PRODOP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, PRODOP, eth, "ETH", "ETH", "PRIMARY", uic));

   // rcrdcommit drops the commit at ingestion — an ACTIVE PRODUCER is the wrong role, so it is not
   // an ACTIVE underwriter. No commit_entry is created (no row growth), the uwreq stays PENDING, and
   // no lock is written.
   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   BOOST_REQUIRE_EQUAL(0u, req["commits_by"].get_array().size());
   BOOST_REQUIRE(get_lock(1).is_null());   // no lock written
} FC_LOG_AND_RETHROW() }

// A registered UNDERWRITER that has NOT cleared its activation threshold (status
// UNKNOWN) cannot have a commit recorded, even funded on the swap's leg. Requiring
// an additional unfunded collateral pair (SOLANA/SOL) keeps UWRIT_OP inactive while
// it still holds ample ETH bond — isolating the activation gate from the bond check.
BOOST_FIXTURE_TEST_CASE(swap_commit_inactive_underwriter_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   register_wire_depot();
   // Require ETH AND SOLANA collateral, so funding ETH alone no longer activates.
   BOOST_REQUIRE_EQUAL(success(), opreg_setconfig_collat(fc::variants{
      chain_min_bond_mvo("ETH",    "ETH", 1),
      chain_min_bond_mvo("SOLANA", "SOL", 1)}));

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 7500;

   // Ample ETH bond for the source leg, but SOLANA stays unfunded → meets_role_min
   // is false → UWRIT_OP never reaches ACTIVE.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000}));
   {
      const auto op = get_operator(UWRIT_OP);
      BOOST_REQUIRE_EQUAL("OPERATOR_TYPE_UNDERWRITER", op["type"].as_string());
      BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_UNKNOWN",   op["status"].as_string());
   }

   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      wire, wire, primary, /*target*/ 50,
      5000, ChainKind::CHAIN_KIND_WIRE, rcpt);
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   const auto uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", uic));

   // rcrdcommit drops the commit at ingestion — UWRIT_OP is a registered underwriter but not yet
   // ACTIVE. No commit_entry is created, the uwreq stays PENDING, and no lock is written.
   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   BOOST_REQUIRE_EQUAL(0u, req["commits_by"].get_array().size());
   BOOST_REQUIRE(get_lock(1).is_null());
} FC_LOG_AND_RETHROW() }

// Regression (r3444212155): a malformed inbound SwapRequest must NOT abort the
// consensus-tipping delivery. createuwreq logs + skips (no row, no throw) when
// the payload fails to decode — it cannot be refunded either, since the revert
// needs the undecodable actor / source_amount.
BOOST_FIXTURE_TEST_CASE(createuwreq_malformed_swaprequest_does_not_abort,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const uint64_t eth = fc::slug_name{"ETH"}.value;
   constexpr uint64_t ATT_ID = 7300;
   // A length-delimited protobuf field (tag 0x0a) claiming far more bytes than
   // are present — the decoder underruns the buffer and returns an error. (Even
   // a tolerant decoder yields random fields that name no registered chain, so
   // no row is created either way.)
   const std::vector<char> garbage{'\x0a', '\x80', '\x80', '\x80', '\x80', '\x08'};
   const std::string garbage_str(garbage.begin(), garbage.end());
   // Must NOT throw (a check() here would abort the delivery before any row
   // exists) and must NOT create a uwreq row.
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, garbage_str));
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// The consensus-envelope analogue of the case above. msgch's dispatch forwards SWAP_REQUEST bytes
// to createuwreq UNDECODED, so createuwreq's own decode guard is all that stands between
// attacker-shaped attestation bytes and a throw that would abort the consensus-tipping delivery
// (and drop the whole epoch's inbound dispatch with it). Three malformed shapes ride ONE envelope
// through the full deliver -> evalcons -> apply_consensus -> inline-createuwreq chain, ahead of a
// fully valid swap: deliver must succeed, the malformed entries must create no uwreq rows, and the
// valid entry behind them must still create exactly one.
BOOST_FIXTURE_TEST_CASE(swap_request_malformed_bytes_do_not_abort_consensus_delivery,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // ETH source outpost
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000'000));

   const auto eth       = fc::slug_name{"ETH"}.value;
   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;

   // Empty bytes: proto3 decodes zero fields into an all-defaults SwapRequest whose
   // source_chain_code (0) can never match the proven outpost -- refunded/dropped, never thrown.
   const std::string empty_bytes{};
   // A lone length-delimited field tag with its length varint missing -- the decoder underruns.
   const std::string truncated_tag{"\x0a"};
   // Deterministic junk: the leading tag varint carries wire type 6, which protobuf does not define.
   const std::string junk_bytes{"\xde\xad\xbe\xef\x42"};
   // Fully valid ETH->SOLANA swap, placed LAST so every malformed entry dispatches before it.
   const auto valid_sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      sol_chain, sol_token, primary, /*target*/ 100,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));

   // The envelope's four attestations mint sequential ids starting here (reserve registration
   // above already consumed ids for its outbound RESERVE_READY queueouts).
   const uint64_t first_att_id = next_att_id();

   // deliver() must SUCCEED -- each malformed payload is dropped inside its inline createuwreq,
   // never thrown up through the dispatch chain.
   BOOST_REQUIRE_EQUAL(success(), deliver(/*proven=*/ eth,
      encode_envelope_with_attestations(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_SWAP_REQUEST,
         {empty_bytes, truncated_tag, junk_bytes, valid_sr})));

   // The three malformed entries created no uwreq; the valid one behind them created exactly
   // one, keyed by its minted id.
   BOOST_REQUIRE(get_uwreq(first_att_id + 0).is_null());
   BOOST_REQUIRE(get_uwreq(first_att_id + 1).is_null());
   BOOST_REQUIRE(get_uwreq(first_att_id + 2).is_null());
   const auto req = get_uwreq(first_att_id + 3);
   BOOST_REQUIRE(!req.is_null());
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   BOOST_REQUIRE_EQUAL(eth, req["src_chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(100u, req["src_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

// Duplicate-id idempotency: re-delivering an attestation_id that already has a uwreq row -- the
// protocol's normal every-cron-tick re-relay -- must no-op WITHOUT throwing and WITHOUT touching
// the existing row, even when the re-delivery carries different (or malformed) bytes. The
// duplicate guard runs before the decode, so a garbage duplicate is skipped by id alone.
BOOST_FIXTURE_TEST_CASE(createuwreq_duplicate_attestation_id_is_idempotent,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, "regchain"_n, mvo()
      ("kind", ChainKind::CHAIN_KIND_SVM)("code", codename_mvo("SOLANA"))
      ("external_chain_id", 900)("name", std::string("solana-test"))("description", std::string{})));
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH",    "ETH", 1'000'000'000));
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "SOLANA", "SOL", 1'000'000'000));

   const auto eth       = fc::slug_name{"ETH"}.value;
   const auto sol_chain = fc::slug_name{"SOLANA"}.value;
   const auto sol_token = fc::slug_name{"SOL"}.value;
   const auto primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 7400;

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ 100,
      sol_chain, sol_token, primary, /*target*/ 100,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   const auto row_before = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, ATT_ID);
   BOOST_REQUIRE(!row_before.empty());

   // Same id, different payload (amounts 100 -> 250): the duplicate must be skipped, not
   // overwritten -- the row stays byte-identical to the original delivery.
   const auto sr_conflicting = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0c'),
      eth, eth, primary, /*src_amount*/ 250,
      sol_chain, sol_token, primary, /*target*/ 250,
      5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0d'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr_conflicting));
   BOOST_REQUIRE(get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, ATT_ID) == row_before);

   // Same id, malformed payload: skipped by id before the decode ever runs.
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, std::string{"\x0a"}));
   BOOST_REQUIRE(get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, ATT_ID) == row_before);
} FC_LOG_AND_RETHROW() }

// ───────────────────────────── WSA-028: signed TokenAmount ingress ─────────────────────────────
//
// OPP TokenAmount.amount is signed on the wire (int64 / vint64_t). The historical foot-gun was
// static_cast<uint64_t>(static_cast<int64_t>(amount)): a negative value such as -1 wraps to
// 18446744073709551615, an impossible "balance" that sails through zero-only guards and inflates
// collateral / reserve / settlement accounting. Every value-bearing ingress path now routes the
// amount through sysio::opp::safe::to_depot_amount, which rejects amount <= 0 AND amount >
// asset::max_amount before any unsigned use. These cases drive malformed amounts through the real
// dispatch paths and assert the depot applies NO value-bearing effect — and never throws (a throw
// inside the evalcons dispatch chain stalls consensus chain-wide). The positive controls are
// dispatch_routes_deposit_to_opreg / dispatch_routes_withdraw_request_to_opreg above.

// DEPOSIT_REQUEST: a valid +1,000,000 deposit rides one envelope alongside two malformed amounts —
// -1 (wraps to UINT64_MAX, the amount <= 0 branch) and 2^62 (== asset::max_amount + 1, the
// out-of-range branch). The valid deposit credits EXACTLY 1,000,000; neither malformed amount
// credits anything (a wrapped credit would make the final balance differ from the valid amount), and
// deliver never throws.
BOOST_FIXTURE_TEST_CASE(operator_action_negative_deposit_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t VALID_DEPOSIT = 1'000'000;
   constexpr int64_t ASSET_MAX_PLUS_ONE = int64_t{1} << 62;   // sysio::asset::max_amount + 1
   const auto eth_code = fc::slug_name{"ETH"}.value;

   auto mk = [&](int64_t amount) {
      return encode_operator_action(
         sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
         sysio::opp::types::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
         eth_code, eth_code, amount);
   };

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code,
      encode_envelope_with_attestations(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
         {mk(VALID_DEPOSIT), mk(-1), mk(ASSET_MAX_PLUS_ONE)})));

   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   auto bal = find_balance(op, "ETH", "ETH");
   BOOST_REQUIRE(!bal.is_null());
   // Exactly the valid amount — the wrapped -1 and the out-of-range 2^62 credited nothing.
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(VALID_DEPOSIT), bal["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// WITHDRAW_REQUEST: a valid deposit funds the operator, then a wrapped -1 withdraw rides the same
// envelope. The deposit credits; the negative withdraw is dropped — no row is queued (and no
// successful action log is appended). Positive control: dispatch_routes_withdraw_request_to_opreg.
BOOST_FIXTURE_TEST_CASE(operator_action_negative_withdraw_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   constexpr int64_t INITIAL_DEPOSIT = 5'000'000;
   const auto eth_code = fc::slug_name{"ETH"}.value;

   auto deposit_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
      eth_code, eth_code, INITIAL_DEPOSIT);
   auto neg_wtdw_payload = encode_operator_action(
      sysio::opp::attestations::OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST,
      sysio::opp::types::CHAIN_KIND_EVM, uwrit_op_eth_pubkey,
      eth_code, eth_code, /*amount=*/ -1);

   BOOST_REQUIRE_EQUAL(success(), deliver(/*chain_code=*/eth_code,
      encode_envelope_with_attestations(current_epoch(),
         sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION,
         {deposit_payload, neg_wtdw_payload})));

   // The deposit credited normally...
   auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE(!op.is_null());
   auto bal = find_balance(op, "ETH", "ETH");
   BOOST_REQUIRE(!bal.is_null());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(INITIAL_DEPOSIT), bal["balance"].as_uint64());
   // ...but the wrapped-negative withdraw queued nothing.
   BOOST_REQUIRE(get_wtdw(/*request_id=*/1).is_null());
} FC_LOG_AND_RETHROW() }

// SWAP_REQUEST: a wrapped -1 source_amount must REVERT (refund on the proven outpost) and create no
// uwreq — never wrap into a huge src_amount that corrupts the swap quote / reserve settlement. Mirrors
// swap_zero_quote_from_active_reserve_fails_closed; createuwreq never throws (it emits SWAP_REVERT).
BOOST_FIXTURE_TEST_CASE(swap_request_negative_source_is_reverted,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // registers ETH

   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
      "regchain"_n, mvo()
         ("kind",              ChainKind::CHAIN_KIND_SVM)
         ("code",              codename_mvo("SOLANA"))
         ("external_chain_id", 900)
         ("name",              std::string("solana-test"))
         ("description",       std::string{})));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t sol_chain = fc::slug_name{"SOLANA"}.value;
   const uint64_t sol_token = fc::slug_name{"SOL"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 6200;

   setup_wire_token_and_reserves();   // ACTIVE ETH/ETH and SOLANA/SOL reserves

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary, /*src_amount*/ -1,
      sol_chain, sol_token, primary, /*target_amount*/ 900'000'000'000ull,
      /*tolerance_bps*/ 5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
   // createuwreq never throws (it emits SWAP_REVERT and returns).
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   // Fail closed: the wrapped-negative source created no pending uwreq.
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// ── Same-token underwriter overcommit (one collateral bucket, two legs) ──────
//
// Underwriter collateral is held per (underwriter, chain_code, token_code) —
// NOT per reserve_code. A swap whose source and destination legs share one
// (chain, token) bucket but use different reserve_code values (a shape
// rcrdcommit explicitly routes) draws BOTH locks against that single balance.
// The winner check must require availability to cover the AGGREGATE of both
// legs; checking each leg independently lets a balance covering each single leg
// but not their sum win and overcommit the bucket.

// Negative: balance 150 covers each single 100-leg but not the 200 aggregate —
// the candidate must be DISQUALIFIED and the race left PENDING with no locks.
BOOST_FIXTURE_TEST_CASE(swap_same_token_legs_overcommit_is_disqualified,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();   // ETH chain + UWRIT_OP (EVM authex link)
   // Both reserves must be ACTIVE and priceable: the resolver re-quotes on the
   // live curve BEFORE the bond check (it is the quote that fixes `dst_amount`,
   // and the bond must cover what the winner will actually deliver), so an
   // unpriceable request never reaches the bond gate at all.
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID     = 8000;
   constexpr int64_t  SRC_AMOUNT = 100;
   constexpr uint64_t DST_AMOUNT = 100;

   // One (ETH, ETH) bucket holds 150 against an aggregate need of
   // `src_amount + quote(src_amount)` — just under 200, so 150 cannot cover it.
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 150));

   // Same-(chain, token) swap between two reserves on the one ETH outpost.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   SRC_AMOUNT,
      eth, eth, secondary, DST_AMOUNT,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
   bool found = false;
   for (const auto& c : req["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string()) {
         found = true;
         BOOST_REQUIRE_EQUAL("UNDERWRITE_STATUS_DISQUALIFIED", c["status"].as_string());
         BOOST_REQUIRE(c["reason"].as_string().find("aggregate required") != std::string::npos);
      }
   }
   BOOST_REQUIRE(found);
   BOOST_REQUIRE(get_lock(1).is_null());   // no locks written
} FC_LOG_AND_RETHROW() }

// Positive + existing-locks coverage: a balance that covers the aggregate
// (`src_amount + quote`) must select the underwriter and write two locks
// totaling exactly that. A subsequent same-bucket swap must then see
// availability reduced by those active locks and be disqualified.
//
// The destination lock is sized by the AMM QUOTE, not by the caller's
// `target_amount` (WNS-02) — hence the assertions derive from `expected_quote`
// rather than repeating the request's target.
BOOST_FIXTURE_TEST_CASE(swap_same_token_legs_exact_balance_wins,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();                          // ETH/ETH/PRIMARY (+ SOL)
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID = 8100;

   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 200));

   // The quote the swap will settle at, taken before any reserve moves.
   const uint64_t quote = expected_quote("ETH", "ETH", "PRIMARY", /*src_amount*/ 100,
                                         "ETH", "ETH", "SECOND", kDefaultUwritFeeBps);
   BOOST_REQUIRE(quote > 0);

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   /*src_amount*/ 100,
      eth, eth, secondary, /*target_amount*/ 100,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   // The row carries the QUOTE, never the caller's target.
   BOOST_REQUIRE_EQUAL(quote, get_uwreq(ATT_ID)["dst_amount"].as_uint64());

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

   const auto req = get_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED", req["status"].as_string());
   BOOST_REQUIRE_EQUAL(UWRIT_OP.to_string(), req["winner"].as_string());

   // Two locks, both on (ETH, ETH): the source leg at `src_amount`, the
   // destination leg at the quote — together the aggregate the bond had to cover.
   const auto l1 = get_lock(1);
   const auto l2 = get_lock(2);
   BOOST_REQUIRE(!l1.is_null());
   BOOST_REQUIRE(!l2.is_null());
   BOOST_REQUIRE_EQUAL(eth, l1["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l1["token_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l2["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l2["token_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(100u,   l1["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(quote,  l2["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(100u + quote, l1["amount"].as_uint64() + l2["amount"].as_uint64());

   // Existing active locks now reserve the whole bucket (available == 0), so a
   // fresh same-bucket swap must be disqualified. Amounts must be large enough
   // to price against the 1e12 reserves — a sub-quote-floor amount is rejected
   // earlier by the unpriceable-reserve gate, which would mask the bond check.
   constexpr uint64_t ATT_ID2 = 8101;
   const auto sr2 = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   /*src_amount*/ 100,
      eth, eth, secondary, /*dst_amount*/ 100,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID2, eth, sr2));
   const auto src_uic2 = make_signed_uic(UWRIT_OP, ATT_ID2, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID2, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic2));
   const auto dst_uic2 = make_signed_uic(UWRIT_OP, ATT_ID2, eth, eth, secondary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID2, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic2));

   const auto req2 = get_uwreq(ATT_ID2);
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req2["status"].as_string());
   bool dq = false;
   for (const auto& c : req2["commits_by"].get_array()) {
      if (c["underwriter"].as_string() == UWRIT_OP.to_string())
         dq = (c["status"].as_string() == "UNDERWRITE_STATUS_DISQUALIFIED");
   }
   BOOST_REQUIRE(dq);
} FC_LOG_AND_RETHROW() }

// WSA-028 closes the single-swap aggregate-overflow vector at ingress. SEC-15's
// uint128 winner-check guard (uwrit.cpp `need = src + dst`) was originally proven
// by driving src_amount to UINT64_MAX — reachable only because a signed source
// amount of -1 wrapped to UINT64_MAX. to_depot_amount now rejects that source
// before any uwreq exists, so a single swap can no longer form the overflow: the
// request reverts and creates no uwreq. The uint128 aggregate addition itself
// stays covered by swap_same_token_legs_overcommit_is_disqualified / _exact_balance_wins.
BOOST_FIXTURE_TEST_CASE(swap_oversized_source_reverts_at_ingress,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID = 8200;

   // Maximal availability, so the revert below is provably from the oversized
   // source at ingress — not from an insufficient-balance check downstream.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", (uint64_t{1} << 62) - 1));

   // source_amount == UINT64_MAX, encoded as -1 in the signed wire field (the
   // exact pre-WSA-028 wrap). to_depot_amount rejects it via the amount <= 0 branch.
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   static_cast<int64_t>(~uint64_t{0}),
      eth, eth, secondary, /*dst_amount*/ 1,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   // Reverts at ingress (never throws) and creates no uwreq.
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// Defence-in-depth: opreg::releaselock settles deferred slashes INLINE inside
// sysio.uwrit::chklocks at sysio.epoch::advance. If a released amount ever
// exceeds the live balance bucket, subtract_balance must NOT underflow + abort
// — that would stall epoch advancement chain-wide. releaselock clamps the
// settled amount to the live balance instead. (The aggregate winner check above
// prevents the overcommit at lock-creation time; this guards the cleanup path
// regardless.)
BOOST_FIXTURE_TEST_CASE(releaselock_clamps_overdrain_without_aborting,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   register_wire_depot();             // to-WIRE: a single (source) required leg
   setup_wire_token_and_reserves();   // ACTIVE ETH/ETH/PRIMARY source reserve w/ WIRE

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   constexpr uint64_t ATT_ID = 8300;

   // Bond 100 on (ETH, ETH); a to-WIRE swap locks the whole 100 (one source
   // lock), so slash leaves the balance intact (slashable-now == 0).
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 100));

   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth,  eth,  primary, /*src_amount*/ 100,
      wire, wire, primary, /*target*/ 50,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_WIRE, rcpt);
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED",
      get_uwreq(ATT_ID)["status"].as_string());

   // Slash: locked 100 == balance 100, so nothing is debited now; status SLASHED.
   BOOST_REQUIRE_EQUAL(success(), slash_op(UWRIT_OP, "test slash"));
   {
      const auto op = get_operator(UWRIT_OP);
      BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_SLASHED", op["status"].as_string());
      BOOST_REQUIRE_EQUAL(100u, find_balance(op, "ETH", "ETH")["balance"].as_uint64());
   }

   // Two deferred releases summing to 120 > balance 100 (distinct amounts so
   // the txns don't collide as duplicates). The first debits 70 (100 -> 30);
   // the second would underflow 30 - 50 without the clamp, which instead settles
   // only the remaining 30. Both must succeed (no abort).
   BOOST_REQUIRE_EQUAL(success(), releaselock_direct(UWRIT_OP, "ETH", "ETH", 70));
   BOOST_REQUIRE_EQUAL(success(), releaselock_direct(UWRIT_OP, "ETH", "ETH", 50));

   const auto op = get_operator(UWRIT_OP);
   BOOST_REQUIRE_EQUAL(0u, find_balance(op, "ETH", "ETH")["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// SEC-77 / WSA-165: drainfwq drains at most MAX_FWQ_DRAIN_PER_EPOCH swap-from-WIRE rows per advance,
// so a caller cannot split escrowed WIRE into enough queued rows to blow the transaction CPU deadline
// advance shares with the rest of its fan-out and stall epoch progress chain-wide. The remainder stays
// queued (escrow safe in reserve custody) and drains on the next advance. This is also the first
// end-to-end coverage of drainfwq draining a populated queue: the bounded front-read FIFO loop, and
// that a second drain resumes where the first stopped.
BOOST_FIXTURE_TEST_CASE(drainfwq_bounds_rows_per_epoch, sysio_dispatch_tester) { try {
   // Mirror of the contract-internal cap (contract headers are not host-compilable, same convention
   // as the msgch size-cap tests). Keep in sync with sysio.uwrit.hpp::MAX_FWQ_DRAIN_PER_EPOCH.
   constexpr uint32_t MAX_FWQ_DRAIN_PER_EPOCH = 32;
   constexpr uint32_t N = MAX_FWQ_DRAIN_PER_EPOCH + 8;              // 40 > one epoch's drain budget
   constexpr uint64_t DEPOT_ORIGIN_ID_BASE = 0x8000000000000000ull; // fwqueue id = base | seq

   bootstrap_for_dispatch();            // registers the ETH (EVM) outpost + epoch machinery
   setup_wire_token_and_reserves();     // sysio.token + ACTIVE public ETH/ETH/PRIMARY reserve
   register_wire_depot();               // so drainfwq's depot_chain_code() resolves
   // Floor lowered to 1: this case exercises the drain BOUND with 40 cheap distinct rows; the
   // default 5-WIRE floor and the revert fee have their own dedicated cases below.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 0)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));

   // A funded from-WIRE swap user (plain account, no ROA policy).
   create_account("swapuser"_n, config::system_account_name, /*multisig=*/false,
                  /*include_code=*/true, /*include_roa_policy=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
      "transfer"_n, mvo()("from", "sysio")("to", "swapuser")
         ("quantity", "1000.000000000 WIRE")("memo", "fund swap user")));

   // Queue N from-WIRE swaps. wire_amount varies (1e6 + i) so each is a distinct transaction AND
   // prices to a positive quote (rows take drainfwq's uwreq-emplace path, not the refund path); the
   // exact amount is otherwise irrelevant to the bound. target_amount + 100% tolerance keep every
   // row within variance so none refund.
   for (uint32_t i = 0; i < N; ++i) {
      BOOST_REQUIRE_EQUAL(success(),
         push(UWRIT_ACCOUNT, uwrit_abi, "swapuser"_n, "swapfromwire"_n, mvo()
            ("user",                 "swapuser")
            ("wire_amount",          uint64_t{1'000'000} + i)
            ("dst_chain_code",       codename_mvo("ETH"))
            ("dst_token_code",       codename_mvo("ETH"))
            ("dst_reserve_code",     codename_mvo("PRIMARY"))
            ("target_amount",        uint64_t{1'000'000})
            ("target_tolerance_bps", uint32_t{10000})
            ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_EVM)
            ("recipient_addr",       std::vector<char>(20, '\x0a'))));
   }

   auto get_fwqueue = [&](uint64_t id) -> fc::variant {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "fwqueue"_n, id);
      return data.empty() ? fc::variant() : uwrit_abi.binary_to_variant(
         "fromwire_q", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   };
   auto count_queued = [&]() {
      uint32_t n = 0;
      for (uint32_t s = 0; s < N; ++s)
         if (!get_fwqueue(DEPOT_ORIGIN_ID_BASE | s).is_null()) ++n;
      return n;
   };
   BOOST_REQUIRE_EQUAL(N, count_queued());

   // One drain processes exactly MAX_FWQ_DRAIN_PER_EPOCH rows; the remainder stays queued.
   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, EPOCH_ACCOUNT, "drainfwq"_n, mvo()));
   BOOST_REQUIRE_EQUAL(N - MAX_FWQ_DRAIN_PER_EPOCH, count_queued());

   // Cross a block boundary so the second drain is a distinct transaction — an identical action in
   // the same block is rejected as a duplicate before the contract runs, masking the guard under test.
   produce_blocks();
   // The next drain resumes where the first stopped and clears the rest.
   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, EPOCH_ACCOUNT, "drainfwq"_n, mvo()));
   BOOST_REQUIRE_EQUAL(0u, count_queued());
} FC_LOG_AND_RETHROW() }

// The swapfromwire escrow floor prices fwqueue slots in locked capital: dust rows are refunded in
// full at drain, so without the floor spam rows could hold drain slots while locking nothing. The
// floor defaults to 5 WIRE and is retunable via setconfig without an upgrade.
BOOST_FIXTURE_TEST_CASE(swapfromwire_enforces_min_amount, sysio_dispatch_tester) { try {
   // Mirror of the contract default (contract headers are not host-compilable). Keep in sync with
   // sysio.uwrit.hpp::DEFAULT_MIN_FROMWIRE_AMOUNT.
   constexpr uint64_t DEFAULT_MIN_FROMWIRE_AMOUNT = 5'000'000'000ull; // 5 WIRE @ 9 decimals
   constexpr uint64_t DEPOT_ORIGIN_ID_BASE        = 0x8000000000000000ull;

   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();   // ACTIVE public ETH/ETH/PRIMARY destination reserve

   create_account("swapuser"_n, config::system_account_name, /*multisig=*/false,
                  /*include_code=*/true, /*include_roa_policy=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
      "transfer"_n, mvo()("from", "sysio")("to", "swapuser")
         ("quantity", "100.000000000 WIRE")("memo", "fund swap user")));

   auto swap = [&](uint64_t wire_amount) {
      return push(UWRIT_ACCOUNT, uwrit_abi, "swapuser"_n, "swapfromwire"_n, mvo()
         ("user",                 "swapuser")
         ("wire_amount",          wire_amount)
         ("dst_chain_code",       codename_mvo("ETH"))
         ("dst_token_code",       codename_mvo("ETH"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        uint64_t{1'000'000})
         ("target_tolerance_bps", uint32_t{10000})
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_EVM)
         ("recipient_addr",       std::vector<char>(20, '\x0a')));
   };
   auto queued = [&](uint64_t seq) {
      return !get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "fwqueue"_n,
                            DEPOT_ORIGIN_ID_BASE | seq).empty();
   };

   // Default config (setconfig never pushed): one atom below the floor is rejected, the exact
   // floor is accepted and lands in the queue.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: wire_amount below the configured minimum"),
      swap(DEFAULT_MIN_FROMWIRE_AMOUNT - 1));
   BOOST_REQUIRE_EQUAL(success(), swap(DEFAULT_MIN_FROMWIRE_AMOUNT));
   BOOST_REQUIRE(queued(0));

   // The floor is live config: lower it and the new boundary is enforced instead.
   constexpr uint64_t LOWERED_FLOOR = 1'000'000;
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", LOWERED_FLOOR)("fromwire_revert_fee_bps", 10)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: wire_amount below the configured minimum"),
      swap(LOWERED_FLOOR - 1));
   BOOST_REQUIRE_EQUAL(success(), swap(LOWERED_FLOOR));
   BOOST_REQUIRE(queued(1));
} FC_LOG_AND_RETHROW() }

// Caller-controlled drain-time reverts forfeit the configured revert fee: the refund returns the
// escrow minus the fee, and the fee routes through the standard `route_wire_fee` path exactly
// like a settlement fee — so revert churn pays the system instead of recycling for free.
BOOST_FIXTURE_TEST_CASE(drainfwq_charges_revert_fee_on_caller_fault, sysio_dispatch_tester) { try {
   constexpr uint64_t ESCROW              = 5'000'000'000ull; // the default floor exactly
   constexpr uint32_t REVERT_FEE_BPS      = 100;              // 1%
   constexpr uint64_t FEE                 = ESCROW * REVERT_FEE_BPS / 10000ull; // 0.05 WIRE
   // A revert has no winning underwriter, so `refundwire` passes a zero underwriter share and
   // the WHOLE fee becomes the rewards pool. reserv's `fee_emissions_share_bps` is never set
   // here (the default 0), so that pool lands in the rewards bucket intact (batch operators);
   // a configured dial would divert its share to the emissions treasury.
   constexpr uint64_t REWARD_SHARE        = FEE;
   constexpr uint64_t DEPOT_ORIGIN_ID_0   = 0x8000000000000000ull;
   const auto WIRE_SYM = symbol(9, "WIRE");

   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   register_wire_depot();             // depot registered => the drain reaches the variance check
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", ESCROW)("fromwire_revert_fee_bps", REVERT_FEE_BPS)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));

   create_account("swapuser"_n, config::system_account_name, /*multisig=*/false,
                  /*include_code=*/true, /*include_roa_policy=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
      "transfer"_n, mvo()("from", "sysio")("to", "swapuser")
         ("quantity", "10.000000000 WIRE")("memo", "fund swap user")));
   const int64_t funded = get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount();

   // target_amount=1 with zero tolerance: the live quote against the seeded 1e12/1e12 pool is
   // ~5e9, so |quote - 1| > 0 == allowed and the row reverts at drain — a failure produced
   // entirely by the caller's own parameters.
   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, "swapuser"_n, "swapfromwire"_n, mvo()
         ("user",                 "swapuser")
         ("wire_amount",          ESCROW)
         ("dst_chain_code",       codename_mvo("ETH"))
         ("dst_token_code",       codename_mvo("ETH"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        uint64_t{1})
         ("target_tolerance_bps", uint32_t{0})
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_EVM)
         ("recipient_addr",       std::vector<char>(20, '\x0a'))));
   BOOST_REQUIRE_EQUAL(funded - static_cast<int64_t>(ESCROW),
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount());

   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, EPOCH_ACCOUNT, "drainfwq"_n, mvo()));

   // Row consumed; escrow minus the fee came back; the whole fee accrued into the reserv
   // rewards bucket (custody-internal — no transfer leaves reserv for a fee).
   BOOST_REQUIRE(get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "fwqueue"_n, DEPOT_ORIGIN_ID_0).empty());
   BOOST_REQUIRE_EQUAL(funded - static_cast<int64_t>(FEE),
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount());
   auto bkt_data = get_row_by_account(RESERV_ACCOUNT, RESERV_ACCOUNT, "rewardbkt"_n, "rewardbkt"_n);
   BOOST_REQUIRE(!bkt_data.empty());
   auto bkt = reserv_abi.binary_to_variant("rewards_bucket", bkt_data,
      abi_serializer::create_yield_function(abi_serializer_max_time));
   BOOST_REQUIRE_EQUAL(REWARD_SHARE, bkt["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// Reverts caused by system state changes after enqueue refund in full even with a nonzero revert
// fee configured — the caller did nothing wrong. Exercised via the cheapest system-fault branch:
// no WIRE depot chain registered at drain time.
BOOST_FIXTURE_TEST_CASE(drainfwq_full_refund_on_system_caused_revert, sysio_dispatch_tester) { try {
   constexpr uint64_t ESCROW            = 5'000'000'000ull;
   constexpr uint64_t DEPOT_ORIGIN_ID_0 = 0x8000000000000000ull;
   const auto WIRE_SYM = symbol(9, "WIRE");

   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   // register_wire_depot() deliberately NOT called: drainfwq's depot_chain_code() comes back
   // empty, which is a system-caused revert (the registry, not the caller's parameters).
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", ESCROW)("fromwire_revert_fee_bps", 100)
      ("uwreq_pending_timeout_epochs", 10)("uwreq_retention_epochs", 10)));

   create_account("swapuser"_n, config::system_account_name, /*multisig=*/false,
                  /*include_code=*/true, /*include_roa_policy=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
      "transfer"_n, mvo()("from", "sysio")("to", "swapuser")
         ("quantity", "10.000000000 WIRE")("memo", "fund swap user")));
   const int64_t funded = get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount();

   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, "swapuser"_n, "swapfromwire"_n, mvo()
         ("user",                 "swapuser")
         ("wire_amount",          ESCROW)
         ("dst_chain_code",       codename_mvo("ETH"))
         ("dst_token_code",       codename_mvo("ETH"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        uint64_t{1'000'000})
         ("target_tolerance_bps", uint32_t{10000})
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_EVM)
         ("recipient_addr",       std::vector<char>(20, '\x0a'))));

   BOOST_REQUIRE_EQUAL(success(),
      push(UWRIT_ACCOUNT, uwrit_abi, EPOCH_ACCOUNT, "drainfwq"_n, mvo()));

   // Full escrow returned — no fee — and no rewards-bucket accrual.
   BOOST_REQUIRE(get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "fwqueue"_n, DEPOT_ORIGIN_ID_0).empty());
   BOOST_REQUIRE_EQUAL(funded,
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount());
   BOOST_REQUIRE(get_row_by_account(RESERV_ACCOUNT, RESERV_ACCOUNT,
                                    "rewardbkt"_n, "rewardbkt"_n).empty());
} FC_LOG_AND_RETHROW() }

// ═════════════════════════════════════════════════════════════════════════
// SEC-129 / WSA-223 — UWREQ lifecycle: expiry is enforced by triggers.
//
// `expires_at_epoch` is passive row data; the `pruneuwreqs` sweep inlined in
// every real `sysio.epoch::advance` is the only thing that evaluates it. The
// cases below drive genuine advances (emissions gate satisfied) and assert
// the full lifecycle: PENDING deadline → EXPIRED + refund + compaction →
// retention → erased; CONFIRMED → COMPLETED (chklocks) → retention → erased;
// the per-epoch budget; and the rcrdcommit row-growth rails.
// ═════════════════════════════════════════════════════════════════════════

// A PENDING uwreq whose underwriter race never resolves survives pre-deadline
// advances untouched, is expired by the first advance at/past its deadline
// (EXPIRED + payload compaction), and is erased once retention elapses.
BOOST_FIXTURE_TEST_CASE(uwreq_pending_timeout_expires_then_retention_erases,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   // Pending timeout 2 epochs + retention 1 epoch — the shortest schedule
   // that still proves the pre-deadline advance is a no-op for the row.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 10)
      ("uwreq_pending_timeout_epochs", 2)("uwreq_retention_epochs", 1)));
   constexpr uint64_t ATT_ID = 9100;
   setup_eth_to_sol_uwreq(ATT_ID);        // PENDING, no commits, deadline = 0 + 2
   enable_epoch_advancement();

   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE_EQUAL(2u, req["expires_at_epoch"].as<uint32_t>());
      BOOST_REQUIRE(req["attestation_inbound_data"].as_string().size() > 0);
   }

   // Epoch 1 < deadline 2 — the row is inert data; the sweep leaves it alone.
   age_one_epoch();
   BOOST_REQUIRE_EQUAL(1u, current_epoch());
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING",
                       get_uwreq(ATT_ID)["status"].as_string());

   // Epoch 2 == deadline — the same advance's inline sweep expires it.
   age_one_epoch();
   BOOST_REQUIRE_EQUAL(2u, current_epoch());
   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE(!req.is_null());
      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_EXPIRED", req["status"].as_string());
      BOOST_REQUIRE(req["settled_at_ms"].as_uint64() > 0);
      // Terminal compaction: the inbound attestation copy is gone; the
      // compact audit metadata (codes, amounts, timestamps) is retained.
      BOOST_REQUIRE_EQUAL(0u, req["attestation_inbound_data"].as_string().size());
      // Retention window re-stamped: epoch 2 + 1.
      BOOST_REQUIRE_EQUAL(3u, req["expires_at_epoch"].as<uint32_t>());
   }

   // Epoch 3 — retention elapsed; the row is erased outright.
   age_one_epoch();
   BOOST_REQUIRE_EQUAL(3u, current_epoch());
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// A queued swap-from-WIRE whose race never resolves refunds the user's FULL
// escrow at expiry (expiry is not a caller-controlled revert cause — no
// revert fee) and follows the same EXPIRED → retention → erased lifecycle.
BOOST_FIXTURE_TEST_CASE(uwreq_from_wire_pending_timeout_refunds_escrow,
                        sysio_dispatch_tester) { try {
   constexpr uint64_t DEPOT_ORIGIN_ID_0 = 0x8000000000000000ull;
   constexpr uint64_t ESCROW            = 1'000'000;
   const auto WIRE_SYM = symbol(9, "WIRE");

   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   register_wire_depot();
   // Non-zero revert fee configured on purpose: the full refund below proves
   // the expiry path is fee-exempt even when a fee is configured.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 100)
      ("uwreq_pending_timeout_epochs", 1)("uwreq_retention_epochs", 1)));

   // Created + funded before the full system contract lands on `sysio`, the
   // same way every other from-WIRE case provisions its user.
   create_account("swapuser"_n, config::system_account_name, /*multisig=*/false,
                  /*include_code=*/true, /*include_roa_policy=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
      "transfer"_n, mvo()("from", "sysio")("to", "swapuser")
         ("quantity", "10.000000000 WIRE")("memo", "fund swap user")));
   const int64_t funded =
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount();

   enable_epoch_advancement();

   // target == wire_amount with 100% tolerance prices within variance against
   // the balanced 1e12/1e12 reserve, so the drain emplaces the uwreq (no
   // refund at drain) — same recipe as drainfwq_bounds_rows_per_epoch.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, "swapuser"_n,
      "swapfromwire"_n, mvo()
         ("user",                 "swapuser")
         ("wire_amount",          ESCROW)
         ("dst_chain_code",       codename_mvo("ETH"))
         ("dst_token_code",       codename_mvo("ETH"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        uint64_t{1'000'000})
         ("target_tolerance_bps", uint32_t{10000})
         ("recipient_kind",       ChainKind::CHAIN_KIND_EVM)
         ("recipient_addr",       std::vector<char>(20, '\x0a'))));
   BOOST_REQUIRE_EQUAL(funded - int64_t(ESCROW),
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount());

   // Advance #1: drainfwq (inline, post-increment) creates the PENDING uwreq
   // at epoch 1 with deadline 1 + 1 = 2.
   age_one_epoch();
   {
      const auto req = get_uwreq(DEPOT_ORIGIN_ID_0);
      BOOST_REQUIRE(!req.is_null());
      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_PENDING", req["status"].as_string());
      BOOST_REQUIRE_EQUAL(2u, req["expires_at_epoch"].as<uint32_t>());
   }

   // Advance #2 (epoch 2 == deadline): the sweep expires the row and refunds
   // the FULL escrow via reserv::refundwire.
   age_one_epoch();
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_EXPIRED",
                       get_uwreq(DEPOT_ORIGIN_ID_0)["status"].as_string());
   BOOST_REQUIRE_EQUAL(funded,
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYM, "swapuser"_n).get_amount());

   // Advance #3 (epoch 3): retention elapsed — erased.
   age_one_epoch();
   BOOST_REQUIRE(get_uwreq(DEPOT_ORIGIN_ID_0).is_null());
} FC_LOG_AND_RETHROW() }

// A settled swap's row is compacted at COMPLETED (chklocks closes the
// challenge window) and erased once retention elapses — the full happy-path
// lifecycle driven end to end by real advances. Winner selection itself
// zeroes the deadline (the lock window owns a CONFIRMED row).
BOOST_FIXTURE_TEST_CASE(uwreq_completed_row_erased_after_retention,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));
   // 60s challenge window (== one epoch) + 1-epoch retention; the pending
   // timeout stays clear of the race so only the lock machinery drives it.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 60'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 10)
      ("uwreq_pending_timeout_epochs", 5)("uwreq_retention_epochs", 1)));
   enable_epoch_advancement();

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID = 9200;

   // Same-chain double-leg winner recipe as swap_same_token_legs_exact_balance_wins.
   BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 200));
   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   /*src_amount*/ 100,
      eth, eth, secondary, /*dst_amount*/ 100,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
   BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(ATT_ID, eth, sr));
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY",
                        make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, primary)));
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "SECOND",
                        make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, secondary)));
   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED", req["status"].as_string());
      // Winner selection cleared the deadline — chklocks owns the row now.
      BOOST_REQUIRE_EQUAL(0u, req["expires_at_epoch"].as<uint32_t>());
   }

   // Advance #1 (62s elapsed > the 60s lock window): chklocks sweeps both
   // locks, flips COMPLETED, stamps retention 1 + 1 = 2, and clears the
   // remaining heavy payloads. The same advance's sweep (epoch 1 < 2) must
   // NOT touch the freshly-completed row.
   age_one_epoch();
   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE(!req.is_null());
      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_COMPLETED", req["status"].as_string());
      BOOST_REQUIRE_EQUAL(2u, req["expires_at_epoch"].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(0u, req["attestation_inbound_data"].as_string().size());
      for (const auto& c : req["commits_by"].get_array()) {
         BOOST_REQUIRE_EQUAL(0u, c["source_uic_bytes"].as_string().size());
         BOOST_REQUIRE_EQUAL(0u, c["dest_uic_bytes"].as_string().size());
      }
   }

   // Advance #2 (epoch 2): retention elapsed — erased. Steady state: the
   // table carries nothing from a fully-settled swap.
   age_one_epoch();
   BOOST_REQUIRE(get_uwreq(ATT_ID).is_null());
} FC_LOG_AND_RETHROW() }

// The per-epoch budget bounds the sweep's work: with more due rows than
// MAX_UWREQ_PRUNE_PER_EPOCH (32), one advance handles exactly 32 and the
// backlog drains the next epoch — advance's CPU stays bounded, nothing is
// lost, nothing is double-handled.
BOOST_FIXTURE_TEST_CASE(pruneuwreqs_budget_bounds_rows_per_epoch,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   // Retention kept long (100) so the second advance only expires the
   // leftover PENDING row instead of competing with 32 fresh erases.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 10)
      ("uwreq_pending_timeout_epochs", 1)("uwreq_retention_epochs", 100)));
   constexpr uint64_t FIRST_ATT_ID = 9300;
   constexpr uint32_t ROWS = 33;              // MAX_UWREQ_PRUNE_PER_EPOCH + 1
   setup_eth_to_sol_uwreq(FIRST_ATT_ID);      // registers SOLANA + reserves once
   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t sol_chain = fc::slug_name{"SOLANA"}.value;
   const uint64_t sol_token = fc::slug_name{"SOL"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   for (uint32_t i = 1; i < ROWS; ++i) {
      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary, 100, sol_chain, sol_token, primary, 100,
         5000, ChainKind::CHAIN_KIND_SVM, std::vector<char>(32, '\x0b'));
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(FIRST_ATT_ID + i, eth, sr));
      if (i % 8 == 7) produce_blocks();   // spread across blocks (per-block CPU)
   }
   produce_blocks();
   enable_epoch_advancement();

   // Advance #1 (epoch 1 == every row's deadline): exactly the budgeted 32
   // rows are expired; one stays PENDING for the next epoch.
   age_one_epoch();
   uint32_t expired = 0, pending = 0;
   for (uint32_t i = 0; i < ROWS; ++i) {
      const auto req = get_uwreq(FIRST_ATT_ID + i);
      BOOST_REQUIRE(!req.is_null());
      const auto st = req["status"].as_string();
      if (st == "UNDERWRITE_REQUEST_STATUS_EXPIRED") ++expired;
      else if (st == "UNDERWRITE_REQUEST_STATUS_PENDING") ++pending;
   }
   BOOST_REQUIRE_EQUAL(32u, expired);
   BOOST_REQUIRE_EQUAL(1u, pending);

   // Advance #2 drains the backlog: every row is now EXPIRED.
   age_one_epoch();
   expired = 0;
   for (uint32_t i = 0; i < ROWS; ++i) {
      const auto req = get_uwreq(FIRST_ATT_ID + i);
      BOOST_REQUIRE(!req.is_null());
      if (req["status"].as_string() == "UNDERWRITE_REQUEST_STATUS_EXPIRED") ++expired;
   }
   BOOST_REQUIRE_EQUAL(ROWS, expired);
} FC_LOG_AND_RETHROW() }

// ── rcrdcommit row-growth rails (SEC-129 / WSA-223) ──

// An oversized UIC payload is refused at the door: no candidate entry, no
// stored bytes, no throw. At the cap, the commit records normally.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_oversized_uic_leg_is_dropped,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9400;
   setup_eth_to_sol_uwreq(ATT_ID);
   const uint64_t eth = fc::slug_name{"ETH"}.value;

   // One byte past MAX_UIC_LEG_BYTES (2048) — dropped with no mutation.
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY",
                        std::vector<char>(2049, '\x01')));
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["commits_by"].get_array().size());

   // At the cap the commit records (bytes are stored verbatim; nothing
   // decodes them until winner selection, which this single leg never arms).
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY",
                        std::vector<char>(2048, '\x01')));
   BOOST_REQUIRE_EQUAL(1u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
} FC_LOG_AND_RETHROW() }

// The candidate roster is capped at MAX_UWREQ_CANDIDATES (32): the 33rd
// distinct ACTIVE underwriter is refused with no row growth, while an
// existing candidate still updates its entry at the cap (dedupe, not append).
BOOST_FIXTURE_TEST_CASE(rcrdcommit_candidate_cap_bounds_row,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();
   constexpr uint64_t ATT_ID = 9500;
   setup_eth_to_sol_uwreq(ATT_ID);
   const uint64_t eth = fc::slug_name{"ETH"}.value;

   // Provision 33 ACTIVE underwriters (register + meet the 1-unit ETH/ETH
   // minimum from bootstrap_for_dispatch's opreg config). UWRIT_OP stays out
   // of this roster.
   std::vector<name> uws;
   for (uint32_t i = 0; i < 33; ++i) {
      std::string s = "uwcap";
      s += static_cast<char>('a' + i / 26);
      s += static_cast<char>('a' + i % 26);
      uws.emplace_back(s);
   }
   create_accounts(uws);
   produce_blocks();
   // Blocks are produced along the way — 33 registrations + credits + 33
   // commits in one block would exhaust the per-block billable CPU.
   for (uint32_t i = 0; i < uws.size(); ++i) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",         uws[i].to_string())
            ("type",            OperatorType::OPERATOR_TYPE_UNDERWRITER)
            ("is_bootstrapped", false)));
      BOOST_REQUIRE_EQUAL(success(), depositinle_credit(uws[i], "ETH", "ETH", 1'000));
      if (i % 8 == 7) produce_blocks();
   }
   produce_blocks();

   // 32 distinct candidates record; the 33rd is refused at the cap.
   for (uint32_t i = 0; i < 32; ++i) {
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(ATT_ID, uws[i], eth, "ETH", "ETH", "PRIMARY",
                           std::vector<char>{1, 2, 3}));
      if (i % 8 == 7) produce_blocks();
   }
   BOOST_REQUIRE_EQUAL(32u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, uws[32], eth, "ETH", "ETH", "PRIMARY",
                        std::vector<char>{1, 2, 3}));
   {
      const auto req = get_uwreq(ATT_ID);
      BOOST_REQUIRE_EQUAL(32u, req["commits_by"].get_array().size());
      for (const auto& c : req["commits_by"].get_array()) {
         BOOST_REQUIRE(c["underwriter"].as_string() != uws[32].to_string());
      }
   }

   // An EXISTING candidate still updates its entry at the cap.
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, uws[0], eth, "ETH", "ETH", "PRIMARY",
                        std::vector<char>{4, 5, 6}));
   BOOST_REQUIRE_EQUAL(32u, get_uwreq(ATT_ID)["commits_by"].get_array().size());
} FC_LOG_AND_RETHROW() }

// ═══════════════════════════════════════════════════════════════════════════
//  Underwriter-fault challenge (WIRE-297) — openuwchal / voteuwchal /
//  chkuwchal / uwchalbond + the uwrit lock-hold trio
// ═══════════════════════════════════════════════════════════════════════════

/// The dispatch stack + a deployed `sysio.chalg`, a Tier-1 electorate of three, and a
/// WIRE-funded challenger. The confirmed-uwreq builder mirrors
/// `swap_same_token_legs_exact_balance_wins` exactly: two same-chain legs of LEG_AMOUNT
/// against the 1e12/1e12 cw-5000 books, so the winner holds two (ETH, ETH) locks — the source
/// leg at LEG_AMOUNT and the destination leg at the AMM quote (swaps settle on the quote, never
/// on the caller's target). Nothing here pins either amount: the bond math reads the live lock
/// rows, so the fixture tracks whatever the curve prices.
class sysio_uwchal_tester : public sysio_dispatch_tester {
public:
   static constexpr auto CHALLENGER = "challenger"_n;
   static constexpr auto VOTER1     = "voter1"_n;
   static constexpr auto VOTER2     = "voter2"_n;
   static constexpr auto VOTER3     = "voter3"_n;
   static constexpr auto VOTER4     = "voter4"_n;

   /// Ballot wire values (the uint8 `uwchal_ballot` members).
   static constexpr uint8_t BALLOT_UPHOLD         = 0;
   static constexpr uint8_t BALLOT_REJECT_REFUND  = 1;
   static constexpr uint8_t BALLOT_REJECT_FORFEIT = 2;
   /// Fault-reason wire value (`underwrite_fault_reason::SOURCE_DEPOSIT_MISSING`).
   static constexpr uint8_t REASON_DEPOSIT_MISSING = 0;

   /// Source amount (and requested target) the confirmed-uwreq builder swaps. The SOURCE leg
   /// locks exactly this; the destination leg locks the AMM quote for it, which the curve puts
   /// just under it — so the two locks together stay inside the 200 credited to the underwriter.
   static constexpr uint64_t LEG_AMOUNT = 100;

   abi_serializer chalg_abi;

   sysio_uwchal_tester() {
      // CHALG_ACCOUNT exists (base fixture creates it); it just never had code until now.
      deploy(CHALG_ACCOUNT, contracts::chalg_wasm(), contracts::chalg_abi(), chalg_abi);

      // Tier-1 voters (no roa policy — same shape as the dispute tester's electorate) and the
      // bond-posting challenger.
      for (auto v : {VOTER1, VOTER2, VOTER3, VOTER4, CHALLENGER}) {
         create_account(v, config::system_account_name, /*multisig=*/false,
                        /*include_code=*/true, /*include_roa_policy=*/false);
      }
      produce_blocks();

      // T1 electorate rows in sysio.roa (gen 0). `forcereg` works pre-emitcfg — the electorate
      // snapshot walks `nodeowners` directly, never `nodecount`. Genesis already carries ONE T1
      // owner, so the snapshot is these four + it: N = 5, Q = 3 — every quorum path below casts
      // three ballots, and the tie case splits four rejectors 2–2.
      for (auto v : {VOTER1, VOTER2, VOTER3, VOTER4}) {
         BOOST_REQUIRE_EQUAL(success(), push(ROA_ACCOUNT, roa_abi, ROA_ACCOUNT, "forcereg"_n,
            mvo()("owner", v.to_string())("tier", 1)));
      }
      produce_blocks();
   }

   // ── setup: the challenged commitment ─────────────────────────────────────

   /// Full path to a CONFIRMED uwreq with two live (ETH, ETH) locks — the source leg at
   /// LEG_AMOUNT, the destination leg at the AMM quote — byte-for-byte the
   /// `swap_same_token_legs_exact_balance_wins` recipe.
   void make_confirmed_uwreq(uint64_t att_id) {
      bootstrap_for_dispatch();
      setup_wire_token_and_reserves();
      BOOST_REQUIRE_EQUAL(success(), regreserve_active("ETH", "ETH", "SECOND"));

      // The challenger's bond funding comes from the WIRE treasury seeded above.
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, config::system_account_name,
         "transfer"_n, mvo()("from", "sysio")("to", CHALLENGER.to_string())
            ("quantity", "100.000000000 WIRE")("memo", "challenge bond funding")));

      const uint64_t eth       = fc::slug_name{"ETH"}.value;
      const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
      const uint64_t secondary = fc::slug_name{"SECOND"}.value;

      BOOST_REQUIRE_EQUAL(success(), depositinle_credit(UWRIT_OP, "ETH", "ETH", 200));

      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary,   /*src_amount*/ LEG_AMOUNT,
         eth, eth, secondary, /*dst_amount*/ LEG_AMOUNT,
         /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(att_id, eth, sr));

      const auto src_uic = make_signed_uic(UWRIT_OP, att_id, eth, eth, primary);
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(att_id, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
      const auto dst_uic = make_signed_uic(UWRIT_OP, att_id, eth, eth, secondary);
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(att_id, UWRIT_OP, eth, "ETH", "ETH", "SECOND", dst_uic));

      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_CONFIRMED",
                          get_uwreq(att_id)["status"].as_string());
   }

   /// Walk `sysio.reserv::reserves` (checksum256-keyed, so `get_row_by_id` cannot address it)
   /// and return the row matching the slug triple — the reserv tests' scan workaround. Takes the
   /// raw slug VALUES so a lock row's fields feed straight in.
   fc::variant find_reserve(uint64_t target_chain, uint64_t target_token,
                            uint64_t target_reserve) {
      const auto& db       = control->db();
      const auto  table_id = chain::compute_table_id("reserves"_n.to_uint64_t());
      const auto& kv_idx   = db.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(RESERV_ACCOUNT, table_id, std::string_view{}));
      for (; itr != kv_idx.end() && itr->code == RESERV_ACCOUNT && itr->table_id == table_id; ++itr) {
         std::vector<char> raw(itr->value.size());
         if (!raw.empty())
            std::memcpy(raw.data(), itr->value.data(), raw.size());
         try {
            auto row = reserv_abi.binary_to_variant(
               "reserve_row", raw, abi_serializer::create_yield_function(abi_serializer_max_time));
            if (row["chain_code"]["value"].as_uint64()   == target_chain &&
                row["token_code"]["value"].as_uint64()   == target_token &&
                row["reserve_code"]["value"].as_uint64() == target_reserve) {
               return row;
            }
         } catch (...) {
            // skip rows that don't decode
         }
      }
      return fc::variant();
   }

   /// What `uwchalbond`/`openuwchal` must price: the swap's GROSS PRE-FEE WIRE leg — the
   /// SOURCE leg's `token_to_wire` on its own reserve's LIVE books (Jonathan, 2026-08-11:
   /// the challenge is adjudicated on WIRE, so the stake is the WIRE amount before fees).
   /// Live books, not the registration constants — winner selection settles inline, so by
   /// challenge time `applyswap` has already moved both sides. Recomputing on the host over
   /// the same row pins the contract to the shared kernel: right reserve, right field order.
   /// Deliberately NOT the two legs summed — that was the superseded per-lock valuation.
   uint64_t expected_bond(uint64_t uwreq_id) {
      const auto req = get_uwreq(uwreq_id);
      BOOST_REQUIRE(!req.is_null());
      const auto row = find_reserve(req["src_chain_code"]["value"].as_uint64(),
                                    req["src_token_code"]["value"].as_uint64(),
                                    req["src_reserve_code"]["value"].as_uint64());
      BOOST_REQUIRE(!row.is_null());
      return sysio::opp::amm::token_to_wire(row["reserve_chain_amount"].as_uint64(),
                                            row["reserve_wire_amount"].as_uint64(),
                                            row["connector_weight_bps"].as_uint64(),
                                            req["src_amount"].as_uint64());
   }

   // ── chalg action wrappers ────────────────────────────────────────────────

   action_result openuwchal(name challenger, uint64_t uwreq_id, name underwriter,
                            uint8_t reason, const std::string& detail) {
      return push(CHALG_ACCOUNT, chalg_abi, challenger, "openuwchal"_n, mvo()
         ("challenger", challenger.to_string())("uwreq_id", uwreq_id)
         ("underwriter", underwriter.to_string())("reason", reason)("detail", detail));
   }

   action_result voteuwchal(name owner, uint64_t chal_id, uint8_t ballot) {
      return push(CHALG_ACCOUNT, chalg_abi, owner, "voteuwchal"_n, mvo()
         ("owner", owner.to_string())("chal_id", chal_id)("ballot", ballot));
   }

   action_result chkuwchal(uint64_t chal_id, name signer = CHALLENGER) {
      return push(CHALG_ACCOUNT, chalg_abi, signer, "chkuwchal"_n, mvo()("chal_id", chal_id));
   }

   /// Pull a resolved challenge's bond out of chalg custody, signed by the recipient itself.
   action_result claimbond(name account, name signer = name()) {
      return push(CHALG_ACCOUNT, chalg_abi, signer == name() ? account : signer, "claimbond"_n,
                  mvo()("account", account.to_string()));
   }

   /// Decoded return of the read-only bond quote. Seals a block first: tests re-quote the SAME
   /// (uwreq, underwriter) before and after filing, and identical bytes in one block window
   /// collide on transaction dedup.
   uint64_t uwchalbond(uint64_t uwreq_id, name underwriter) {
      produce_block();
      auto trace = base_tester::push_action(CHALG_ACCOUNT, "uwchalbond"_n, CHALG_ACCOUNT, mvo()
         ("uwreq_id", uwreq_id)("underwriter", underwriter.to_string()));
      BOOST_REQUIRE(trace && !trace->action_traces.empty());
      return fc::raw::unpack<uint64_t>(trace->action_traces[0].return_value);
   }

   // ── row / balance readers ────────────────────────────────────────────────

   fc::variant get_uwchal(uint64_t id) {
      auto data = get_row_by_id(CHALG_ACCOUNT, CHALG_ACCOUNT, "uwchals"_n, id);
      return data.empty() ? fc::variant() : chalg_abi.binary_to_variant(
         "uwchal_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// The challenge's escrowed bond, straight off the row.
   uint64_t uwchal_bond_amount(uint64_t id) {
      return get_uwchal(id)["bond_amount"].as_uint64();
   }

   /// One owner's ballot in a challenge (the vote table is scoped by chal_id) — null once
   /// resolution erased the scope.
   fc::variant get_uwchal_vote(uint64_t chal_id, name owner) {
      auto data = get_row_by_id(CHALG_ACCOUNT, name(chal_id), "uwchalvote"_n, owner.value);
      return data.empty() ? fc::variant() : chalg_abi.binary_to_variant(
         "uwchal_vote", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Unclaimed WIRE `sysio.chalg` owes `account` from resolved challenges; 0 when no row exists.
   uint64_t get_bond_credit(name account) {
      auto data = get_row_by_id(CHALG_ACCOUNT, CHALG_ACCOUNT, "bondcredits"_n, account.value);
      if (data.empty()) return 0;
      return chalg_abi.binary_to_variant(
         "bond_credit", data,
         abi_serializer::create_yield_function(abi_serializer_max_time))["amount"].as_uint64();
   }

   int64_t wire_balance(name account) {
      return get_currency_balance(TOKEN_ACCOUNT, symbol{9, "WIRE"}, account).get_amount();
   }

   /// The decoded `sysio.opreg::operators` row.
   fc::variant get_operator(name account) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, account.value);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// The operator's status string straight off `sysio.opreg::operators`.
   std::string operator_status(name account) {
      return get_operator(account)["status"].as_string();
   }

   /// Sweep expired locks as the epoch machinery would (`chklocks` accepts epoch or self auth).
   action_result chklocks() {
      return push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "chklocks"_n, mvo());
   }
};

// The read-only quote prices the live locks through their own books and answers 0 for every
// not-challengeable state — the same soft contract as sysio.reserv::swapquote.
BOOST_FIXTURE_TEST_CASE(uwchalbond_quotes_the_live_lock_value, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9100;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE_EQUAL(expected_bond(ATT_ID), uwchalbond(ATT_ID, UWRIT_OP));
   BOOST_REQUIRE_GT(expected_bond(ATT_ID), 0u);

   BOOST_REQUIRE_EQUAL(0u, uwchalbond(ATT_ID + 1, UWRIT_OP));    // no such uwreq
   BOOST_REQUIRE_EQUAL(0u, uwchalbond(ATT_ID, "batchop.a"_n));   // not the winner
} FC_LOG_AND_RETHROW() }

// Filing escrows exactly the quoted bond, stamps every lock with the challenge id, and records
// the OPEN row — and the quote answers 0 afterwards (a commitment is challengeable once).
BOOST_FIXTURE_TEST_CASE(openuwchal_escrows_bond_and_holds_locks, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9200;
   make_confirmed_uwreq(ATT_ID);

   const uint64_t bond   = uwchalbond(ATT_ID, UWRIT_OP);
   const int64_t  before = wire_balance(CHALLENGER);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "no such deposit"));

   BOOST_REQUIRE_EQUAL(before - static_cast<int64_t>(bond), wire_balance(CHALLENGER));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(bond), wire_balance(CHALG_ACCOUNT));

   const auto chal = get_uwchal(1);
   BOOST_REQUIRE(!chal.is_null());
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", chal["status"].as_string());
   BOOST_REQUIRE_EQUAL("NONE", chal["verdict"].as_string());
   BOOST_REQUIRE_EQUAL(ATT_ID, chal["uwreq_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(bond, chal["bond_amount"].as_uint64());
   // Four registered voters + the ONE genesis T1 owner (this assert breaks loudly if genesis
   // ever seeds a different count — the quorum arithmetic below depends on it).
   BOOST_REQUIRE_EQUAL(5u, chal["electorate"].get_array().size());
   BOOST_REQUIRE_EQUAL(3u, chal["quorum"].as_uint64());

   BOOST_REQUIRE_EQUAL(1u, get_lock(1)["challenge_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, get_lock(2)["challenge_id"].as_uint64());

   BOOST_REQUIRE_EQUAL(0u, uwchalbond(ATT_ID, UWRIT_OP));
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "again")
                    .find("already been challenged") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// Every way a filing can be stale or malformed is refused before any WIRE moves.
BOOST_FIXTURE_TEST_CASE(openuwchal_guards, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9300;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID + 77, UWRIT_OP, REASON_DEPOSIT_MISSING, "x")
                    .find("not found") != std::string::npos);
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, "batchop.a"_n, REASON_DEPOSIT_MISSING, "x")
                    .find("not this request's winner") != std::string::npos);
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, /*reason*/ 99, "x")
                    .find("unknown fault reason") != std::string::npos);
   // The caller-controlled detail note is retained on the audit row indefinitely (RAM billed to
   // the contract) — the byte cap refuses the filing before any escrow moves.
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING,
                            std::string(1024 + 1, 'x'))
                    .find("detail exceeds max_uwchal_detail_bytes") != std::string::npos);

   // Time-travel past the 12h window: the commitment is no longer challengeable, and the locks
   // release healthy on the next sweep. Seal the pending block FIRST — a big skip aborts pending
   // and re-applies its transactions at the jumped time, where they have expired.
   const int64_t before = wire_balance(CHALLENGER);
   produce_blocks();
   produce_block(fc::hours(13));
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "late")
                    .find("lock window has closed") != std::string::npos);
   // The whole transaction rolled back (the bond gate refuses pre-escrow here; a mid-window
   // race would instead throw in holdlocks AFTER the escrow was sent — same atomicity): no
   // WIRE moved, no challenge row survives.
   BOOST_REQUIRE_EQUAL(before, wire_balance(CHALLENGER));
   BOOST_REQUIRE(get_uwchal(1).is_null());
} FC_LOG_AND_RETHROW() }

// Ballot guards mirror votedispute's: electorate membership, one vote per owner, a real ballot
// value, and an OPEN challenge.
BOOST_FIXTURE_TEST_CASE(voteuwchal_guards, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9400;
   make_confirmed_uwreq(ATT_ID);
   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "d"));

   BOOST_REQUIRE(voteuwchal(CHALLENGER, 1, BALLOT_UPHOLD)
                    .find("not in the challenge's tier-1 electorate") != std::string::npos);
   BOOST_REQUIRE(voteuwchal(VOTER1, 1, /*ballot*/ 7).find("unknown ballot") != std::string::npos);
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_UPHOLD));
   produce_block(); // identical re-vote bytes need fresh TAPOS to reach the contract's guard
   BOOST_REQUIRE(voteuwchal(VOTER1, 1, BALLOT_UPHOLD)
                    .find("already voted") != std::string::npos);
   BOOST_REQUIRE(chkuwchal(99).find("challenge not found") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// The uphold path end-to-end: quorum -> SLASHED -> the locks sweep through releaselock's
// deferred-slash branch (collateral debited) -> uwreq COMPLETED -> bond back to the challenger.
// An OPEN challenge PINS the uwreq row against the retention sweep. The locks hold the
// collateral, but THIS row holds the evidence the challenge adjudicates against
// (`attestation_inbound_data`, `source_tx_id`, `commits_by`), and the terminal retention
// window is far shorter than the challenge window — so without the pin a challenge could
// outlive the record it exists to adjudicate. Both release branches are covered: `freelocks`
// on a REJECTED verdict here, `sweeplocks` on UPHELD below.
BOOST_FIXTURE_TEST_CASE(pruneuwreqs_skips_a_challenged_uwreq, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9600;
   make_confirmed_uwreq(ATT_ID);

   // Filing stamps the request, not just its locks.
   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "no deposit on ETH"));
   BOOST_REQUIRE_EQUAL(1u, get_uwreq(ATT_ID)["challenge_id"].as_uint64());

   // The sweep refuses it however generous the budget — the pin is not status- or
   // retention-conditional.
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT,
      "pruneuwreqs"_n, mvo()("max_rows", 100)));
   BOOST_REQUIRE(!get_uwreq(ATT_ID).is_null());

   // REJECT_FORFEIT resolves through `freelocks`, which clears both markers.
   for (auto v : {VOTER1, VOTER2, VOTER3}) {
      BOOST_REQUIRE_EQUAL(success(), voteuwchal(v, 1, BALLOT_REJECT_FORFEIT));
   }
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["challenge_id"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// The UPHELD branch releases the pin too — `sweeplocks` ERASES the locks rather than
/// clearing their markers, so it is the only place the row's own marker comes off there.
/// Without it the row would be skipped forever and never reclaimable.
BOOST_FIXTURE_TEST_CASE(uphold_releases_the_uwreq_challenge_pin, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9610;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "no deposit on ETH"));
   BOOST_REQUIRE_EQUAL(1u, get_uwreq(ATT_ID)["challenge_id"].as_uint64());

   for (auto v : {VOTER1, VOTER2, VOTER3}) {
      BOOST_REQUIRE_EQUAL(success(), voteuwchal(v, 1, BALLOT_UPHOLD));
   }
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));
   BOOST_REQUIRE_EQUAL("UPHELD", get_uwchal(1)["verdict"].as_string());
   BOOST_REQUIRE_EQUAL(0u, get_uwreq(ATT_ID)["challenge_id"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(chkuwchal_uphold_slashes_and_returns_bond, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9500;
   make_confirmed_uwreq(ATT_ID);
   const int64_t challenger_start = wire_balance(CHALLENGER);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "no deposit on ETH"));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_UPHOLD));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER2, 1, BALLOT_UPHOLD));

   // Sub-quorum (2 of Q=3): the crank resolves nothing and the operator stands.
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_OPEN", get_uwchal(1)["status"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", operator_status(UWRIT_OP));

   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER3, 1, BALLOT_UPHOLD));
   produce_block(); // the resolving crank re-pushes identical bytes — fresh TAPOS required
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));

   const auto chal = get_uwchal(1);
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_RESOLVED", chal["status"].as_string());
   BOOST_REQUIRE_EQUAL("UPHELD", chal["verdict"].as_string());

   // The operator is slashed and BOTH locks are gone — swept, not expired.
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_SLASHED", operator_status(UWRIT_OP));
   BOOST_REQUIRE(get_lock(1).is_null());
   BOOST_REQUIRE(get_lock(2).is_null());

   // Every bucket drains: `opreg::slash` debits each bucket's slashable-now portion (balance
   // minus active locks — for the challenged (ETH, ETH) bucket that is everything ABOVE the two
   // 100-unit locks), and the two sweeping releaselocks then take the deferred-slash branch for
   // the locked 200. Unlocked buckets on other chains were slashed in full at slash time.
   // (Bind the row first — ranging over `get_operator(...)[...]` would iterate a dangling
   // temporary.)
   const auto slashed_op = get_operator(UWRIT_OP);
   for (const auto& bal : slashed_op["balances"].get_array()) {
      BOOST_REQUIRE_EQUAL(0u, bal["balance"].as_uint64());
   }

   // COMPLETED flip ran through the shared tail.
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_COMPLETED",
                       get_uwreq(ATT_ID)["status"].as_string());

   // The bond is CREDITED, not pushed: resolution moves no WIRE at all, so custody is still
   // chalg's and the challenger is still down the escrow. (The crank can run under the epoch
   // tick, where a transfer would run the recipient's notification handler — see
   // hostile_bond_recipient_cannot_stall_the_epoch_tick.)
   const uint64_t bond = chal["bond_amount"].as_uint64();
   BOOST_REQUIRE_GT(bond, 0u);
   BOOST_REQUIRE_EQUAL(challenger_start - static_cast<int64_t>(bond), wire_balance(CHALLENGER));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(bond), wire_balance(CHALG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(bond, get_bond_credit(CHALLENGER));

   // The pull settles it: the bond comes home and chalg ends at zero custody.
   BOOST_REQUIRE_EQUAL(success(), claimbond(CHALLENGER));
   BOOST_REQUIRE_EQUAL(challenger_start, wire_balance(CHALLENGER));
   BOOST_REQUIRE_EQUAL(0, wire_balance(CHALG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0u, get_bond_credit(CHALLENGER));
} FC_LOG_AND_RETHROW() }

// An explicit REJECT_FORFEIT majority is the ONLY road to forfeiture: the bond lands on the
// wrongly-challenged underwriter, the holds clear, and the locks release healthy at expiry
// with the collateral untouched.
BOOST_FIXTURE_TEST_CASE(chkuwchal_reject_forfeit_pays_underwriter, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9600;
   make_confirmed_uwreq(ATT_ID);
   const uint64_t bond = uwchalbond(ATT_ID, UWRIT_OP);
   const int64_t  uw_start = wire_balance(UWRIT_OP);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "wrong"));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_REJECT_FORFEIT));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER2, 1, BALLOT_REJECT_FORFEIT));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER3, 1, BALLOT_REJECT_FORFEIT));
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));

   BOOST_REQUIRE_EQUAL("REJECTED_FORFEIT", get_uwchal(1)["verdict"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", operator_status(UWRIT_OP));

   // Forfeiture credits the underwriter; the WIRE lands when IT pulls.
   BOOST_REQUIRE_EQUAL(bond, get_bond_credit(UWRIT_OP));
   BOOST_REQUIRE_EQUAL(uw_start, wire_balance(UWRIT_OP));
   BOOST_REQUIRE_EQUAL(success(), claimbond(UWRIT_OP));
   BOOST_REQUIRE_EQUAL(uw_start + static_cast<int64_t>(bond), wire_balance(UWRIT_OP));
   BOOST_REQUIRE_EQUAL(0, wire_balance(CHALG_ACCOUNT));

   // Holds cleared; locks still present until natural expiry, then a HEALTHY release.
   BOOST_REQUIRE_EQUAL(0u, get_lock(1)["challenge_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, get_lock(2)["challenge_id"].as_uint64());
   const auto balances_before = get_operator(UWRIT_OP)["balances"];
   produce_blocks();
   produce_block(fc::hours(13));
   BOOST_REQUIRE_EQUAL(success(), chklocks());
   BOOST_REQUIRE(get_lock(1).is_null());
   // A healthy release moves NO collateral: the balances are byte-identical to the pre-expiry
   // snapshot after a rejected challenge.
   const auto balances_after = get_operator(UWRIT_OP)["balances"];
   BOOST_REQUIRE_EQUAL(fc::json::to_string(balances_before, fc::time_point::maximum()),
                       fc::json::to_string(balances_after, fc::time_point::maximum()));
} FC_LOG_AND_RETHROW() }

// A split reject quorum favours the refund on a tie — forfeiture requires a strict majority of
// the rejectors.
BOOST_FIXTURE_TEST_CASE(chkuwchal_reject_tie_favours_refund, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9700;
   make_confirmed_uwreq(ATT_ID);
   const int64_t challenger_start = wire_balance(CHALLENGER);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "honest mistake"));
   // Four rejectors split 2–2: the combined rejects clear Q=3, and the forfeit/refund tie must
   // fall to the refund — forfeiture only ever happens by a strict majority of the rejectors.
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_REJECT_REFUND));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER2, 1, BALLOT_REJECT_FORFEIT));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER3, 1, BALLOT_REJECT_REFUND));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER4, 1, BALLOT_REJECT_FORFEIT));
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));

   BOOST_REQUIRE_EQUAL("REJECTED_REFUND", get_uwchal(1)["verdict"].as_string());
   BOOST_REQUIRE_EQUAL(success(), claimbond(CHALLENGER));
   BOOST_REQUIRE_EQUAL(challenger_start, wire_balance(CHALLENGER));
} FC_LOG_AND_RETHROW() }

// The epoch tick IS the challenge's cadence: an expired-but-challenged lock is NOT released by
// the sweep; the sweep pokes the tally, the sub-quorum challenge LAPSES (bond refunded — silence
// never punishes), the freed locks release on the NEXT sweep.
BOOST_FIXTURE_TEST_CASE(chklocks_skips_held_lock_and_lapse_refunds, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9800;
   make_confirmed_uwreq(ATT_ID);
   const int64_t challenger_start = wire_balance(CHALLENGER);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "nobody voted"));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_UPHOLD)); // sub-quorum (Q=3)

   produce_blocks();             // seal pending before the jump (see openuwchal_guards)
   produce_block(fc::hours(13)); // past both the lock expiry and therefore the vote deadline

   // Past the deadline the ballot door is closed: a late quorum can never be assembled for a
   // manual chkuwchal crank in the expiry→sweep gap — the only remaining outcome is LAPSED.
   BOOST_REQUIRE(voteuwchal(VOTER2, 1, BALLOT_UPHOLD)
                    .find("the challenge window has expired") != std::string::npos);

   // First sweep: the held locks are SKIPPED (still present), the poke lapses the challenge.
   BOOST_REQUIRE_EQUAL(success(), chklocks());
   BOOST_REQUIRE(!get_lock(1).is_null());
   BOOST_REQUIRE(!get_lock(2).is_null());
   BOOST_REQUIRE_EQUAL(0u, get_lock(1)["challenge_id"].as_uint64());
   BOOST_REQUIRE_EQUAL("LAPSED", get_uwchal(1)["verdict"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", operator_status(UWRIT_OP));
   // Silence never punishes: the whole bond is credited back, claimable on demand.
   BOOST_REQUIRE_EQUAL(uwchal_bond_amount(1), get_bond_credit(CHALLENGER));
   BOOST_REQUIRE_EQUAL(success(), claimbond(CHALLENGER));
   BOOST_REQUIRE_EQUAL(challenger_start, wire_balance(CHALLENGER));

   // Second sweep: the now-unheld expired locks release healthy; uwreq completes.
   produce_block(); // identical sweep bytes — fresh TAPOS
   BOOST_REQUIRE_EQUAL(success(), chklocks());
   BOOST_REQUIRE(get_lock(1).is_null());
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_COMPLETED",
                       get_uwreq(ATT_ID)["status"].as_string());
} FC_LOG_AND_RETHROW() }

// A resolved challenge keeps only what consensus still needs: the uniqueness gate and the
// verdict. The caller-controlled detail note, the electorate snapshot, and every ballot row go —
// filing is permissionless and the bond returns on every non-forfeit outcome, so without this
// the same recycled capital could pin unbounded bytes in RAM billed to sysio.
BOOST_FIXTURE_TEST_CASE(chkuwchal_compacts_resolved_row_and_erases_ballots,
                        sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9250;
   make_confirmed_uwreq(ATT_ID);

   const std::string detail(1024, 'e');   // a full-size note, right at max_uwchal_detail_bytes
   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, detail));

   // While OPEN the row carries everything the council votes against.
   const auto open_row = get_uwchal(1);
   BOOST_REQUIRE_EQUAL(detail, open_row["detail"].as_string());
   BOOST_REQUIRE_EQUAL(5u, open_row["electorate"].get_array().size());

   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER1, 1, BALLOT_REJECT_REFUND));
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER2, 1, BALLOT_REJECT_REFUND));
   BOOST_REQUIRE(!get_uwchal_vote(1, VOTER1).is_null());
   BOOST_REQUIRE_EQUAL(success(), voteuwchal(VOTER3, 1, BALLOT_REJECT_REFUND));
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));

   // Resolved: the fixed-width record stands, both variable-length fields are empty, the ballots
   // are gone.
   const auto tomb = get_uwchal(1);
   BOOST_REQUIRE_EQUAL("DISPUTE_STATUS_RESOLVED", tomb["status"].as_string());
   BOOST_REQUIRE_EQUAL("REJECTED_REFUND", tomb["verdict"].as_string());
   BOOST_REQUIRE_EQUAL(ATT_ID, tomb["uwreq_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(3u, tomb["quorum"].as_uint64());
   BOOST_REQUIRE(tomb["detail"].as_string().empty());
   BOOST_REQUIRE_EQUAL(0u, tomb["electorate"].get_array().size());
   for (auto v : {VOTER1, VOTER2, VOTER3}) {
      BOOST_REQUIRE(get_uwchal_vote(1, v).is_null());
   }

   // The tombstone still does its consensus job — a verdict is final per commitment, even though
   // the reject freed the locks and they are live again.
   BOOST_REQUIRE_EQUAL(0u, uwchalbond(ATT_ID, UWRIT_OP));
   BOOST_REQUIRE(openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "retry")
                    .find("already been challenged") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// The payout is a pull under the recipient's own authority, and it settles exactly once.
BOOST_FIXTURE_TEST_CASE(claimbond_guards, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9350;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE(claimbond(CHALLENGER).find("no claimable bond") != std::string::npos);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "d"));
   for (auto v : {VOTER1, VOTER2, VOTER3}) {
      BOOST_REQUIRE_EQUAL(success(), voteuwchal(v, 1, BALLOT_REJECT_REFUND));
   }
   BOOST_REQUIRE_EQUAL(success(), chkuwchal(1));

   const uint64_t bond = uwchal_bond_amount(1);
   BOOST_REQUIRE_GT(bond, 0u);
   BOOST_REQUIRE_EQUAL(bond, get_bond_credit(CHALLENGER));

   // Another account cannot pull someone else's credit.
   BOOST_REQUIRE(claimbond(CHALLENGER, /*signer*/ VOTER1)
                    .find("missing authority of challenger") != std::string::npos);

   const int64_t before = wire_balance(CHALLENGER);
   BOOST_REQUIRE_EQUAL(success(), claimbond(CHALLENGER));
   BOOST_REQUIRE_EQUAL(before + static_cast<int64_t>(bond), wire_balance(CHALLENGER));

   // Erased on payout — a second pull draws nothing.
   produce_block(); // identical claim bytes need fresh TAPOS to reach the contract's guard
   BOOST_REQUIRE(claimbond(CHALLENGER).find("no claimable bond") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// A hostile bond recipient cannot hold epoch advancement hostage. `sysio.token::transfer` runs
// its recipient's code through `require_recipient(to)`, so a contract at the challenger's account
// can assert on the incoming refund. Were the payout pushed from the crank, that assert would
// abort `sysio.epoch::advance -> chklocks -> chkuwchal`, roll the challenge back to OPEN with its
// locks still held, and be retried identically by every later sweep — a permanent chain-wide
// stall bought with one permissionless filing. Crediting keeps the crank inside system-owned
// state: the challenge resolves, the locks release, and the only transaction the hostile account
// can abort is its own claim, stranding only its own funds.
BOOST_FIXTURE_TEST_CASE(hostile_bond_recipient_cannot_stall_the_epoch_tick,
                        sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9450;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE_EQUAL(success(),
      openuwchal(CHALLENGER, ATT_ID, UWRIT_OP, REASON_DEPOSIT_MISSING, "then i go hostile"));
   const uint64_t bond = uwchal_bond_amount(1);
   BOOST_REQUIRE_GT(bond, 0u);

   // The escrow is in; NOW the challenger deploys a contract that rejects every incoming
   // notification. Nothing prevents this — filing is permissionless and code can land afterwards,
   // so the payout path can never assume a bare recipient. (The fixture creates the challenger
   // without a roa policy; setcode needs RAM, so grant one first.)
   BOOST_REQUIRE(add_roa_policy(NODE_DADDY, CHALLENGER, "0.0010 SYS", "0.0010 SYS",
                                "50.0000 SYS", /*time_block*/ 0, /*network_gen*/ 0));
   set_code(CHALLENGER, contracts::util::reject_all_wasm());
   produce_blocks();               // seal pending before the jump (see openuwchal_guards)
   produce_block(fc::hours(13));   // past the lock expiry: the sweep must now resolve the challenge

   // The epoch tick's sweep runs to completion: it pokes the tally, the sub-quorum challenge
   // LAPSES, and nothing in the crank touches the hostile account.
   BOOST_REQUIRE_EQUAL(success(), chklocks());
   BOOST_REQUIRE_EQUAL("LAPSED", get_uwchal(1)["verdict"].as_string());
   BOOST_REQUIRE_EQUAL(bond, get_bond_credit(CHALLENGER));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(bond), wire_balance(CHALG_ACCOUNT));

   // The freed locks release on the next sweep and the uwreq settles — advancement never stalled.
   produce_block(); // identical sweep bytes — fresh TAPOS
   BOOST_REQUIRE_EQUAL(success(), chklocks());
   BOOST_REQUIRE(get_lock(1).is_null());
   BOOST_REQUIRE(get_lock(2).is_null());
   BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_COMPLETED",
                       get_uwreq(ATT_ID)["status"].as_string());

   // Only the hostile account's OWN claim fails, and its credit simply stays put.
   BOOST_REQUIRE(claimbond(CHALLENGER).find("rejecting all notifications") != std::string::npos);
   BOOST_REQUIRE_EQUAL(bond, get_bond_credit(CHALLENGER));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(bond), wire_balance(CHALG_ACCOUNT));
} FC_LOG_AND_RETHROW() }

// The lock-control trio is chalg's alone.
BOOST_FIXTURE_TEST_CASE(lock_hold_actions_require_chalg_auth, sysio_uwchal_tester) { try {
   constexpr uint64_t ATT_ID = 9900;
   make_confirmed_uwreq(ATT_ID);

   BOOST_REQUIRE(push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "holdlocks"_n, mvo()
      ("uwreq_id", ATT_ID)("underwriter", UWRIT_OP.to_string())("chal_id", 7))
         .find("missing authority of sysio.chalg") != std::string::npos);
   BOOST_REQUIRE(push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "freelocks"_n, mvo()
      ("uwreq_id", ATT_ID)("underwriter", UWRIT_OP.to_string()))
         .find("missing authority of sysio.chalg") != std::string::npos);
   BOOST_REQUIRE(push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "sweeplocks"_n, mvo()
      ("uwreq_id", ATT_ID)("underwriter", UWRIT_OP.to_string()))
         .find("missing authority of sysio.chalg") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

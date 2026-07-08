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

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
}

/// Encode an Envelope wrapping N attestations of the same type. Used to fit
/// multiple OPERATOR_ACTIONs into a single delivery, since the depot
/// deduplicates per-(batch_op, outpost, epoch) — a second `deliver` from
/// the same batch op in the same epoch is silently dropped.
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

   std::vector<char> out(env.ByteSizeLong());
   env.SerializeToArray(out.data(), static_cast<int>(out.size()));
   return out;
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
   action_result opreg_setconfig_collat(const fc::variants& req_uw_collat,
                                        const fc::variants& req_prod_collat = fc::variants{}) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers",          21)
         ("max_available_batch_ops",          63)
         ("max_available_underwriters",       21)
         ("terminate_prune_delay_ms",         600000)
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
   // from the same batch op in the same epoch is silently dropped. To exercise
   // both dispatch branches in one test, both attestations ride a single
   // envelope.
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

// [P1] regression (r3444213199): an oversized to-WIRE target_amount must be
// terminally REJECTED + refunded BEFORE settlement. dst_amount is the unbounded
// cross-chain SwapRequest.target_amount; left unchecked, a value near UINT64_MAX
// wraps the `dst_amount + to_wire_fee` sufficiency guard, slips into settlement,
// and aborts reserv::paywire's asset(static_cast<int64_t>(dst_amount)) inside
// evalcons — a chain-wide consensus stall. A representable-but-too-large target
// (> asset::max_amount) must also be rejected, never left stuck PENDING forever.
BOOST_FIXTURE_TEST_CASE(swap_to_wire_oversized_target_is_rejected_not_aborted,
                        sysio_dispatch_tester) { try {
   bootstrap_for_dispatch();          // ETH source outpost + UWRIT_OP (EVM link)
   register_wire_depot();             // WIRE depot => to-WIRE destination
   setup_wire_token_and_reserves();   // ACTIVE ETH/ETH/PRIMARY source reserve w/ WIRE

   const uint64_t eth     = fc::slug_name{"ETH"}.value;
   const uint64_t wire    = fc::slug_name{"WIRE"}.value;
   const uint64_t primary = fc::slug_name{"PRIMARY"}.value;
   // src_amount large enough that the 0.1% to-WIRE fee rounds to >= 1, so the
   // pre-fix `dst_amount + to_wire_fee` actually wraps for dst_amount near
   // UINT64_MAX and slips into paywire (the abort being regressed).
   constexpr int64_t SRC_AMOUNT = 1'000'000;
   BOOST_REQUIRE_EQUAL(success(),
      depositinle_credit(UWRIT_OP, "ETH", "ETH", uint64_t{1'000'000'000}));

   // A valid existing WIRE recipient so the to-WIRE path reaches the amount
   // guard (past the recipient-validity terminal check).
   const std::string rs = UWRIT_OP.to_string();
   const std::vector<char> rcpt(rs.begin(), rs.end());

   constexpr uint64_t ASSET_MAX = (uint64_t{1} << 62) - 1;
   auto run = [&](uint64_t att_id, uint64_t target) {
      const auto sr = encode_swap_request(
         ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
         eth, eth, primary, SRC_AMOUNT,
         wire, wire, primary, target,
         /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_WIRE, rcpt);
      BOOST_REQUIRE_EQUAL(success(), createuwreq_direct(att_id, eth, sr));
      const auto src_uic = make_signed_uic(UWRIT_OP, att_id, eth, eth, primary);
      // The push MUST succeed — a pre-fix wrap aborts paywire's asset() here and
      // fails the whole transaction (the production consensus stall).
      BOOST_REQUIRE_EQUAL(success(),
         rcrdcommit_direct(att_id, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
      const auto req = get_uwreq(att_id);
      BOOST_REQUIRE_EQUAL("UNDERWRITE_REQUEST_STATUS_REJECTED", req["status"].as_string());
      // Pin the rejection to the amount guard (not the variance recheck, which
      // the 100x tolerance above lets pass).
      bool reason_ok = false;
      for (const auto& c : req["commits_by"].get_array()) {
         if (c["underwriter"].as_string() == UWRIT_OP.to_string())
            reason_ok = c["reason"].as_string().find("asset") != std::string::npos;
      }
      BOOST_REQUIRE(reason_ok);
   };

   run(7001, ASSET_MAX + 1);        // target == asset::max_amount + 1 (boundary)
   run(7002, uint64_t{1} << 63);    // high bit set (negative as int64)
   run(7003, ~uint64_t{0});         // UINT64_MAX — wraps dst_amount + fee pre-fix
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

   const uint64_t eth       = fc::slug_name{"ETH"}.value;
   const uint64_t primary   = fc::slug_name{"PRIMARY"}.value;
   const uint64_t secondary = fc::slug_name{"SECOND"}.value;
   constexpr uint64_t ATT_ID     = 8000;
   constexpr int64_t  SRC_AMOUNT = 100;
   constexpr uint64_t DST_AMOUNT = 100;

   // One (ETH, ETH) bucket holds 150. The bond check runs before any
   // reserve-liquidity gate, so no reserves are needed to reach disqualification.
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

// Positive + existing-locks coverage: a balance that exactly covers the
// aggregate (200 == 100 + 100) must select the underwriter and write two locks
// totaling 200. A subsequent same-bucket swap must then see availability
// reduced by those active locks (200 - 200 = 0) and be disqualified.
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

   const auto sr = encode_swap_request(
      ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0a'),
      eth, eth, primary,   /*src_amount*/ 100,
      eth, eth, secondary, /*dst_amount*/ 100,
      /*tolerance_bps*/ 1'000'000, ChainKind::CHAIN_KIND_EVM, std::vector<char>(20, '\x0b'));
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

   // Two locks, both on (ETH, ETH), totaling 200.
   const auto l1 = get_lock(1);
   const auto l2 = get_lock(2);
   BOOST_REQUIRE(!l1.is_null());
   BOOST_REQUIRE(!l2.is_null());
   BOOST_REQUIRE_EQUAL(eth, l1["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l1["token_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l2["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(eth, l2["token_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(200u, l1["amount"].as_uint64() + l2["amount"].as_uint64());

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
      ("min_fromwire_amount", 1)("fromwire_revert_fee_bps", 0)));

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
      ("min_fromwire_amount", LOWERED_FLOOR)("fromwire_revert_fee_bps", 10)));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: wire_amount below the configured minimum"),
      swap(LOWERED_FLOOR - 1));
   BOOST_REQUIRE_EQUAL(success(), swap(LOWERED_FLOOR));
   BOOST_REQUIRE(queued(1));
} FC_LOG_AND_RETHROW() }

// Caller-controlled drain-time reverts forfeit the configured revert fee: the refund returns the
// escrow minus the fee, and the fee routes through the standard rewards/emissions split exactly
// like a settlement fee — so revert churn pays the system instead of recycling for free.
BOOST_FIXTURE_TEST_CASE(drainfwq_charges_revert_fee_on_caller_fault, sysio_dispatch_tester) { try {
   constexpr uint64_t ESCROW              = 5'000'000'000ull; // the default floor exactly
   constexpr uint32_t REVERT_FEE_BPS      = 100;              // 1%
   constexpr uint64_t FEE                 = ESCROW * REVERT_FEE_BPS / 10000ull; // 0.05 WIRE
   // Mirror of sysio.reserv.hpp::FEE_REWARD_SHARE_BPS (50% rewards / 50% emissions).
   constexpr uint64_t REWARD_SHARE        = FEE / 2;
   constexpr uint64_t DEPOT_ORIGIN_ID_0   = 0x8000000000000000ull;
   const auto WIRE_SYM = symbol(9, "WIRE");

   bootstrap_for_dispatch();
   setup_wire_token_and_reserves();
   register_wire_depot();             // depot registered => the drain reaches the variance check
   BOOST_REQUIRE_EQUAL(success(), push(UWRIT_ACCOUNT, uwrit_abi, UWRIT_ACCOUNT, "setconfig"_n, mvo()
      ("fee_bps", 10)("collateral_lock_duration_ms", 120'000u)
      ("min_fromwire_amount", ESCROW)("fromwire_revert_fee_bps", REVERT_FEE_BPS)));

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

   // Row consumed; escrow minus the fee came back; the fee split 50/50 into the reserv rewards
   // bucket (custody-internal) and the sysio emissions treasury (real transfer).
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
      ("min_fromwire_amount", ESCROW)("fromwire_revert_fee_bps", 100)));

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

BOOST_AUTO_TEST_SUITE_END()

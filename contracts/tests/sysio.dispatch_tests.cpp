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
      // CLAIM_ACCOUNT with NO roa policy (include_roa_policy=false) so it has no pre-existing
      // reslimit -- regnodeowner creates that, and would throw "Resource limit already exist"
      // otherwise. include_code=true leaves the standard <account>@sysio.code on active, which
      // exercises active_key_matches against a real (non-single-entry) authority.
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

      create_uwrit_op_eth_authex_link();

      // v6: chains are first-class registry rows.
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              ChainKind::CHAIN_KIND_EVM)
            ("code",              codename_mvo("ETH"))
            ("external_chain_id", 31337)
            ("name",              std::string("ethereum-test"))
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
                                     uint64_t outpost_id, uint64_t chain_code_v,
                                     uint64_t token_code_v, uint64_t reserve_code_v) {
      sysio::opp::attestations::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter.to_string());
      uic.mutable_uw_ext_chain_addr()->set_kind(sysio::opp::types::CHAIN_KIND_EVM);
      uic.set_uw_request_id(uwreq_id);
      uic.set_outpost_id(outpost_id);
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

   fc::variant get_uwreq(uint64_t id) {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, id);
      return data.empty() ? fc::variant() : uwrit_abi.binary_to_variant(
         "uw_request_t", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
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

      auto reg = [&](std::string_view c, std::string_view t, std::string_view r) {
         return push(RESERV_ACCOUNT, reserv_abi, RESERV_ACCOUNT, "regreserve"_n, mvo()
            ("chain_code",           codename_mvo(c))
            ("token_code",           codename_mvo(t))
            ("reserve_code",         codename_mvo(r))
            ("name",                 std::string(c))
            ("description",          std::string{})
            ("initial_chain_amount", uint64_t{1'000'000'000'000ull})
            ("initial_wire_amount",  uint64_t{1'000'000'000'000ull})
            ("connector_weight_bps", uint32_t{5000})
            ("is_private",           false)
            ("owner",                ""));
      };
      BOOST_REQUIRE_EQUAL(success(), reg("ETH",    "ETH", "PRIMARY"));
      BOOST_REQUIRE_EQUAL(success(), reg("SOLANA", "SOL", "PRIMARY"));
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
   bootstrap_for_dispatch();

   const auto eth_code = fc::slug_name{"ETH"}.value;
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

   const auto src_uic = make_signed_uic(UWRIT_OP, ATT_ID, eth, eth, eth, primary);
   BOOST_REQUIRE_EQUAL(success(),
      rcrdcommit_direct(ATT_ID, UWRIT_OP, eth, "ETH", "ETH", "PRIMARY", src_uic));
   const auto dst_uic = make_signed_uic(UWRIT_OP, ATT_ID, sol_chain, sol_chain, sol_token, primary);
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
      const auto src_uic = make_signed_uic(UWRIT_OP, att_id, eth, eth, eth, primary);
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
   const auto bad_uic = make_signed_uic(BATCHOP, ATT_ID, eth, eth, eth, primary);
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

BOOST_AUTO_TEST_SUITE_END()

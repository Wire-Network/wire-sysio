#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>
#include <boost/endian/conversion.hpp>

#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

#include <sysio/opp/types/types.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>

#include <algorithm>

namespace sysio {

using namespace chain_apis;
using namespace sysio::opp::types;
namespace opp_att = sysio::opp::attestations;
namespace eth = fc::network::ethereum;
namespace sol = fc::network::solana;

// ---------------------------------------------------------------------------
//  Swap candidate — parsed from sysio.msgch PENDING messages
// ---------------------------------------------------------------------------
struct swap_candidate {
   uint64_t    msg_id;
   uint64_t    outpost_id;
   std::string message_id_hex;      // hex of the checksum256 message_id
   ChainKind   source_chain;
   ChainKind   target_chain;
   uint32_t    target_chain_id;
   int64_t     source_amount;       // in chain-native precision
   int64_t     target_amount;
   int64_t     fee_amount;          // computed: source_amount * fee_bps / 10000
   double      fee_ratio;           // fee_amount / source_amount — for knapsack sort
   TokenKind   source_token;
   TokenKind   target_token;
   bool        verified = false;
};

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct underwriter_plugin::impl {
   // Configuration
   chain::name  underwriter_account;
   bool         enabled             = false;
   uint32_t     scan_interval_ms    = 5000;
   uint32_t     verify_timeout_ms   = 10000;
   std::string  eth_client_id;
   std::string  sol_client_id;
   std::string  eth_opp_addr;          // OPP contract address on ETH
   std::string  sol_program_id;        // OPP program ID on SOL

   // Collateral state (read from sysio.uwrit::collateral each cycle)
   std::map<int, int64_t> available_collateral; // ChainKind -> available amount

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Cron job handle
   cron_service::job_id_t            scan_job_id = 0;
   bool                              shutting_down = false;

   // Fee basis points (read from sysio.uwrit::uwconfig)
   uint32_t                          fee_bps = 10;

   // Outpost chain_kind cache: outpost_id -> ChainKind
   std::map<uint64_t, ChainKind>     outpost_chain_kinds;

   // -----------------------------------------------------------------------
   //  Table read helper (same pattern as batch_operator_plugin)
   // -----------------------------------------------------------------------

   read_only::get_table_rows_result read_table(const std::string& code,
                                                const std::string& scope,
                                                const std::string& table,
                                                uint32_t limit = 100) {
      auto ro = chain_plug->get_read_only_api(fc::microseconds(200000));
      read_only::get_table_rows_params p;
      p.json  = true;
      p.code  = chain::name(code);
      p.scope = scope;
      p.table = chain::name(table);
      p.limit = limit;
      auto deadline = fc::time_point::now() + fc::milliseconds(verify_timeout_ms);
      auto result_fn = ro.get_table_rows(p, deadline);
      auto result = result_fn();
      if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
         elog("underwriter: table read failed {}::{} — {}", code, table, (*err)->to_string());
         return {};
      }
      return std::get<read_only::get_table_rows_result>(result);
   }

   // -----------------------------------------------------------------------
   //  Main scan cycle
   // -----------------------------------------------------------------------

   void scan_cycle() {
      if (shutting_down || !enabled) return;
      try {
         do_scan_cycle();
      } FC_LOG_AND_DROP();
   }

   void do_scan_cycle() {
      // Step 1: Read fee config
      read_uw_config();

      // Step 2: Read outpost registry for chain_kind mappings
      read_outpost_registry();

      // Step 3: Read our available collateral
      read_collateral();

      // Step 4: Scan for PENDING SWAP messages
      auto candidates = scan_pending_swaps();
      if (candidates.empty()) return;

      ilog("underwriter: found {} SWAP candidates", candidates.size());

      // Step 5: Verify each on external chain
      verify_candidates(candidates);

      // Step 6: Select optimal set via greedy knapsack
      auto selected = select_optimal(candidates);
      if (selected.empty()) {
         ilog("underwriter: no candidates passed verification or collateral check");
         return;
      }

      ilog("underwriter: selected {} swaps for underwriting", selected.size());

      // Step 7: Submit intents
      for (auto& s : selected) {
         submit_intent(s);
      }

      // Step 8: Check confirmations on existing entries
      check_confirmations();
   }

   // -----------------------------------------------------------------------
   //  Config and collateral reads
   // -----------------------------------------------------------------------

   void read_uw_config() {
      auto rows = read_table("sysio.uwrit", "sysio.uwrit", "uwconfig", 1);
      if (!rows.rows.empty()) {
         auto obj = rows.rows[0].get_object();
         fee_bps = static_cast<uint32_t>(obj["fee_bps"].as_uint64());
      }
   }

   void read_outpost_registry() {
      outpost_chain_kinds.clear();
      auto rows = read_table("sysio.epoch", "sysio.epoch", "outposts", 100);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         uint64_t id = obj["id"].as_uint64();
         auto ck = static_cast<ChainKind>(obj["chain_kind"].as_uint64());
         outpost_chain_kinds[id] = ck;
      }
   }

   void read_collateral() {
      available_collateral.clear();

      // Read collateral table, filter by our underwriter account
      auto rows = read_table("sysio.uwrit", "sysio.uwrit", "collateral", 100);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         auto uw_name = obj["underwriter"].as_string();
         if (chain::name(uw_name) != underwriter_account) continue;

         int chain_kind = static_cast<int>(obj["chain_kind"].as_uint64());
         // available_amount is an asset string like "100.0000 WIRE"
         auto avail_str = obj["available_amount"].as_string();
         // Parse amount from asset string (before the space)
         auto space_pos = avail_str.find(' ');
         std::string amount_str = (space_pos != std::string::npos) ? avail_str.substr(0, space_pos) : avail_str;
         // Remove decimal point and convert to integer
         auto dot_pos = amount_str.find('.');
         int64_t amount = 0;
         if (dot_pos != std::string::npos) {
            std::string whole = amount_str.substr(0, dot_pos);
            std::string frac = amount_str.substr(dot_pos + 1);
            uint32_t precision = static_cast<uint32_t>(frac.size());
            amount = std::stoll(whole);
            for (uint32_t i = 0; i < precision; ++i) amount *= 10;
            amount += std::stoll(frac);
         } else {
            amount = std::stoll(amount_str);
         }

         available_collateral[chain_kind] += amount;
      }

      for (auto& [ck, amt] : available_collateral) {
         ilog("underwriter: available collateral chain_kind={} amount={}", ck, amt);
      }
   }

   // -----------------------------------------------------------------------
   //  Scan PENDING messages for SWAP attestations
   // -----------------------------------------------------------------------

   std::vector<swap_candidate> scan_pending_swaps() {
      std::vector<swap_candidate> candidates;

      // Read messages table — filter by status=PENDING, attestation_type=SWAP
      auto rows = read_table("sysio.msgch", "sysio.msgch", "messages", 100);

      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         auto status = static_cast<MessageStatus>(obj["status"].as_uint64());
         auto attest = static_cast<AttestationType>(obj["attestation_type"].as_uint64());

         if (status != MESSAGE_STATUS_PENDING || attest != ATTESTATION_TYPE_SWAP) continue;

         swap_candidate sc;
         sc.msg_id       = obj["id"].as_uint64();
         sc.outpost_id   = obj["outpost_id"].as_uint64();
         sc.message_id_hex = obj["message_id"].as_string();

         // Parse raw_payload to extract swap details via protobuf
         auto payload_hex = obj["raw_payload"].as_string();
         if (!parse_swap_payload(payload_hex, sc)) continue;

         // Compute fee
         sc.fee_amount = (sc.source_amount * fee_bps) / 10000;
         sc.fee_ratio  = (sc.source_amount > 0)
                           ? static_cast<double>(sc.fee_amount) / sc.source_amount
                           : 0.0;

         candidates.push_back(std::move(sc));
      }

      return candidates;
   }

   bool parse_swap_payload(const std::string& payload_hex, swap_candidate& sc) {
      // Decode hex string to raw bytes
      auto bytes = fc::from_hex(payload_hex);
      if (bytes.empty()) {
         elog("underwriter: empty payload for msg {}", sc.msg_id);
         return false;
      }

      // Parse the protobuf Swap message
      opp_att::Swap_ swap;
      if (!swap.ParseFromString(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
         elog("underwriter: protobuf parse failed for msg {}", sc.msg_id);
         return false;
      }

      // Extract source_amount
      if (swap.has_source_amount()) {
         sc.source_amount = swap.source_amount().amount();
         sc.source_token  = static_cast<TokenKind>(swap.source_amount().kind());
      } else {
         elog("underwriter: swap msg {} missing source_amount", sc.msg_id);
         return false;
      }

      // Extract target_chain
      if (swap.has_target_chain()) {
         sc.target_chain    = swap.target_chain().kind();
         sc.target_chain_id = swap.target_chain().id();
      } else {
         elog("underwriter: swap msg {} missing target_chain", sc.msg_id);
         return false;
      }

      // Extract target_token
      sc.target_token = swap.target_token();

      // Determine source chain from the outpost registry
      auto it = outpost_chain_kinds.find(sc.outpost_id);
      if (it == outpost_chain_kinds.end()) {
         elog("underwriter: unknown outpost {} for msg {}", sc.outpost_id, sc.msg_id);
         return false;
      }
      sc.source_chain = it->second;

      // target_amount defaults to source_amount (1:1 bridging)
      sc.target_amount = sc.source_amount;

      ilog("underwriter: parsed swap msg={} source_chain={} target_chain={} amount={}",
           sc.msg_id, static_cast<int>(sc.source_chain), static_cast<int>(sc.target_chain), sc.source_amount);

      return true;
   }

   // -----------------------------------------------------------------------
   //  External chain verification
   // -----------------------------------------------------------------------

   void verify_candidates(std::vector<swap_candidate>& candidates) {
      for (auto& sc : candidates) {
         sc.verified = verify_on_external_chain(sc);
      }
   }

   bool verify_on_external_chain(const swap_candidate& sc) {
      if (sc.source_chain == CHAIN_KIND_ETHEREUM) {
         return verify_eth_deposit(sc);
      } else if (sc.source_chain == CHAIN_KIND_SOLANA) {
         return verify_sol_deposit(sc);
      }
      elog("underwriter: unsupported source chain {} for msg {}", static_cast<int>(sc.source_chain), sc.msg_id);
      return false;
   }

   bool verify_eth_deposit(const swap_candidate& sc) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found", eth_client_id);
         return false;
      }

      if (eth_opp_addr.empty()) {
         elog("underwriter: ETH OPP contract address not configured");
         return false;
      }

      try {
         // Get ABI definitions for event decoding
         auto& abis = eth_plug->get_abi_files();
         std::vector<eth::abi::contract> all_abis;
         for (auto& [path, contracts] : abis) {
            for (auto& c : contracts) all_abis.push_back(c);
         }

         // Query OPPMessageEvent logs from the OPP contract
         auto events = entry->client->get_events(
            eth_opp_addr,
            {"OPPMessageEvent"},
            all_abis,
            eth::block_tag_t{std::string("earliest")},
            eth::block_tag_t{std::string(eth::block_tag_latest)});

         // Compute the expected message_id hash to match against
         auto expected_hash = sc.message_id_hex;

         for (auto& evt : events) {
            if (evt.event_name != "OPPMessageEvent" || evt.data.empty()) continue;

            // Hash the event data and compare to the swap's message_id
            auto event_hash = fc::sha256::hash(
               reinterpret_cast<const char*>(evt.data.data()),
               evt.data.size()).str();

            if (event_hash == expected_hash) {
               ilog("underwriter: ETH deposit verified for msg {} in tx {}",
                    sc.msg_id, evt.transaction_hash);
               return true;
            }
         }

         elog("underwriter: ETH deposit not found for msg {} (checked {} events)",
              sc.msg_id, events.size());
      } catch (const fc::exception& e) {
         elog("underwriter: ETH verification failed for msg {} — {}", sc.msg_id, e.to_string());
      } catch (const std::exception& e) {
         elog("underwriter: ETH verification failed for msg {} — {}", sc.msg_id, e.what());
      }
      return false;
   }

   bool verify_sol_deposit(const swap_candidate& sc) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: SOL client '{}' not found", sol_client_id);
         return false;
      }

      if (sol_program_id.empty()) {
         elog("underwriter: SOL program ID not configured");
         return false;
      }

      try {
         // Query recent transaction signatures for the OPP program
         auto sigs_result = entry->client->execute(
            "getSignaturesForAddress",
            fc::variants{
               fc::variant(sol_program_id),
               fc::variant(fc::mutable_variant_object()("limit", 50))
            });

         if (!sigs_result.is_array()) {
            elog("underwriter: SOL getSignaturesForAddress returned non-array for msg {}", sc.msg_id);
            return false;
         }

         auto expected_hash = sc.message_id_hex;

         for (auto& sig_entry : sigs_result.get_array()) {
            if (!sig_entry.is_object()) continue;
            auto sig = sig_entry.get_object()["signature"].as_string();

            auto tx_result = entry->client->execute(
               "getTransaction",
               fc::variants{
                  fc::variant(sig),
                  fc::variant(fc::mutable_variant_object()
                     ("encoding", "json")
                     ("maxSupportedTransactionVersion", 0))
               });

            if (!tx_result.is_object()) continue;
            auto& meta = tx_result.get_object()["meta"];
            if (!meta.is_object()) continue;

            auto& log_messages = meta.get_object()["logMessages"];
            if (!log_messages.is_array()) continue;

            for (auto& log : log_messages.get_array()) {
               auto log_str = log.as_string();
               auto pos = log_str.find("Program data: ");
               if (pos == std::string::npos) continue;

               auto b64_data = log_str.substr(pos + 14);
               auto decoded = fc::base64_decode(b64_data);

               // Hash the decoded program data and compare to expected message_id
               auto data_hash = fc::sha256::hash(decoded.data(), decoded.size()).str();
               if (data_hash == expected_hash) {
                  ilog("underwriter: SOL deposit verified for msg {} in tx {}",
                       sc.msg_id, sig);
                  return true;
               }
            }
         }

         elog("underwriter: SOL deposit not found for msg {}", sc.msg_id);
      } catch (const fc::exception& e) {
         elog("underwriter: SOL verification failed for msg {} — {}", sc.msg_id, e.to_string());
      } catch (const std::exception& e) {
         elog("underwriter: SOL verification failed for msg {} — {}", sc.msg_id, e.what());
      }
      return false;
   }

   // -----------------------------------------------------------------------
   //  Greedy knapsack selection
   // -----------------------------------------------------------------------

   std::vector<swap_candidate> select_optimal(std::vector<swap_candidate>& candidates) {
      // Filter to verified only
      std::vector<swap_candidate*> verified;
      for (auto& sc : candidates) {
         if (sc.verified) verified.push_back(&sc);
      }

      // Sort by fee-to-collateral ratio (highest first) — greedy knapsack
      std::sort(verified.begin(), verified.end(),
                [](const swap_candidate* a, const swap_candidate* b) {
                   return a->fee_ratio > b->fee_ratio;
                });

      // Track remaining collateral per chain
      auto remaining = available_collateral;

      std::vector<swap_candidate> selected;
      for (auto* sc : verified) {
         // Check if we have enough collateral on the source chain
         auto it = remaining.find(static_cast<int>(sc->source_chain));
         if (it == remaining.end() || it->second < sc->source_amount) {
            ilog("underwriter: skipping msg {} — insufficient collateral on chain {}",
                 sc->msg_id, static_cast<int>(sc->source_chain));
            continue;
         }

         // Reserve the collateral
         it->second -= sc->source_amount;
         selected.push_back(*sc);

         ilog("underwriter: selected msg {} (amount={}, fee={}, remaining={})",
              sc->msg_id, sc->source_amount, sc->fee_amount, it->second);
      }

      return selected;
   }

   // -----------------------------------------------------------------------
   //  Intent submission with real cryptographic signatures
   // -----------------------------------------------------------------------

   fc::crypto::signature_provider_ptr get_signature_provider_for_chain(ChainKind chain_kind) {
      if (chain_kind == CHAIN_KIND_ETHEREUM) {
         auto entry = eth_plug->get_client(eth_client_id);
         if (entry && entry->signature_provider) return entry->signature_provider;
      } else if (chain_kind == CHAIN_KIND_SOLANA) {
         auto entry = sol_plug->get_client(sol_client_id);
         if (entry && entry->signature_provider) return entry->signature_provider;
      }
      return nullptr;
   }

   void submit_intent(const swap_candidate& sc) {
      ilog("underwriter: submitting intent for msg {}", sc.msg_id);

      // Build the UnderwriteIntent protobuf for signing
      opp_att::UnderwriteIntent intent;

      // Set underwriter address based on source chain
      auto source_sig_provider = get_signature_provider_for_chain(sc.source_chain);
      auto target_sig_provider = get_signature_provider_for_chain(sc.target_chain);

      if (!source_sig_provider) {
         elog("underwriter: no signature provider for source chain {} on msg {}",
              static_cast<int>(sc.source_chain), sc.msg_id);
         return;
      }
      if (!target_sig_provider) {
         elog("underwriter: no signature provider for target chain {} on msg {}",
              static_cast<int>(sc.target_chain), sc.msg_id);
         return;
      }

      // Set wire_account
      auto* wire_acct = intent.mutable_wire_account();
      wire_acct->set_name(underwriter_account.to_string());

      // Set original_message_id from hex
      auto msg_id_bytes = fc::from_hex(sc.message_id_hex);
      intent.set_original_message_id(
         std::string(reinterpret_cast<const char*>(msg_id_bytes.data()), msg_id_bytes.size()));

      // Set source_amount
      auto* src_amt = intent.mutable_source_amount();
      src_amt->set_kind(sc.source_token);
      src_amt->set_amount(sc.source_amount);

      // Set target_amount
      auto* tgt_amt = intent.mutable_target_amount();
      tgt_amt->set_kind(sc.target_token);
      tgt_amt->set_amount(sc.target_amount);

      // Set target_chain
      auto* tgt_chain = intent.mutable_target_chain();
      tgt_chain->set_kind(sc.target_chain);
      tgt_chain->set_id(sc.target_chain_id);

      // Set unlock_timestamp (current time + 24 hours)
      auto now = fc::time_point::now();
      auto unlock = now + fc::hours(24);
      intent.set_unlock_timestamp(
         static_cast<uint64_t>(unlock.sec_since_epoch()));

      // Serialize the intent for signing
      std::string intent_bytes;
      intent.SerializeToString(&intent_bytes);

      // Produce the digest of the serialized intent
      auto digest = fc::sha256::hash(intent_bytes.data(), intent_bytes.size());

      // Sign with source chain key
      auto source_sig = source_sig_provider->sign(digest);
      auto source_sig_hex = source_sig.to_string({}, true);

      // Sign with target chain key
      auto target_sig = target_sig_provider->sign(digest);
      auto target_sig_hex = target_sig.to_string({}, true);

      push_action("sysio.uwrit", "submituw", underwriter_account,
                  fc::mutable_variant_object()
                     ("underwriter", underwriter_account.to_string())
                     ("msg_id", sc.msg_id)
                     ("source_sig", source_sig_hex)
                     ("target_sig", target_sig_hex));
   }

   // -----------------------------------------------------------------------
   //  Confirmation monitoring
   // -----------------------------------------------------------------------

   void check_confirmations() {
      // Read uwledger for our entries
      auto rows = read_table("sysio.uwrit", "sysio.uwrit", "uwledger", 100);

      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         auto uw_name = obj["underwriter"].as_string();
         if (chain::name(uw_name) != underwriter_account) continue;

         auto status = static_cast<UnderwriteStatus>(obj["status"].as_uint64());
         uint64_t entry_id = obj["id"].as_uint64();

         if (status == UNDERWRITE_STATUS_INTENT_CONFIRMED) {
            // Both outposts confirmed — trigger fee distribution
            ilog("underwriter: entry {} confirmed, distributing fees", entry_id);
            push_action("sysio.uwrit", "distfee", underwriter_account,
                        fc::mutable_variant_object()("uw_entry_id", entry_id));
         } else if (status == UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            // Still waiting for confirmation — check unlock time
            auto unlock_str = obj["unlock_time"].as_string();
            ilog("underwriter: entry {} awaiting confirmation (unlock={})", entry_id, unlock_str);
         }
      }
   }

   // -----------------------------------------------------------------------
   //  Action push helper (same pattern as batch_operator_plugin)
   // -----------------------------------------------------------------------

   void push_action(const std::string& contract,
                    const std::string& action_name,
                    chain::name auth_account,
                    const fc::variant_object& data) {
      fc::mutable_variant_object action_obj;
      action_obj("account", contract);
      action_obj("name", action_name);
      action_obj("authorization", fc::variants{
         fc::variant(fc::mutable_variant_object()
            ("actor", auth_account.to_string())
            ("permission", "active"))
      });
      action_obj("data", data);

      auto& chain = chain_plug->chain();
      auto head_id = chain.head().id();
      auto ref_block_num = boost::endian::endian_reverse(head_id._hash[0]);
      auto ref_block_prefix = head_id._hash[1];
      auto expiration = chain.head().block_time() + fc::seconds(30);

      fc::mutable_variant_object trx_obj;
      trx_obj("expiration", expiration);
      trx_obj("ref_block_num", ref_block_num & 0xffff);
      trx_obj("ref_block_prefix", ref_block_prefix);
      trx_obj("actions", fc::variants{fc::variant(std::move(action_obj))});

      auto rw = chain_plug->get_read_write_api(fc::microseconds(verify_timeout_ms * 1000));
      auto params = fc::variant(std::move(trx_obj)).get_object();

      std::promise<void> done;
      auto future = done.get_future();

      rw.push_transaction(
         params,
         [&done, &contract, &action_name](const auto& result) {
            if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
               elog("underwriter: push {}::{} failed — {}", contract, action_name, (*err)->to_string());
            } else {
               ilog("underwriter: pushed {}::{} ok", contract, action_name);
            }
            done.set_value();
         });

      if (future.wait_for(std::chrono::milliseconds(verify_timeout_ms)) == std::future_status::timeout) {
         elog("underwriter: push {}::{} timed out", contract, action_name);
      }
   }
};

// ---------------------------------------------------------------------------
//  Plugin lifecycle
// ---------------------------------------------------------------------------
underwriter_plugin::underwriter_plugin()
   : _impl(std::make_unique<impl>()) {}

underwriter_plugin::~underwriter_plugin() = default;

void underwriter_plugin::set_program_options(options_description& cli,
                                              options_description& cfg) {
   auto opts = cfg.add_options();
   opts("underwriter-account", bpo::value<std::string>(),
        "WIRE account name for this underwriter");
   opts("underwriter-scan-interval-ms", bpo::value<uint32_t>()->default_value(5000),
        "How often to scan for pending messages (ms)");
   opts("underwriter-verify-timeout-ms", bpo::value<uint32_t>()->default_value(10000),
        "Timeout for external chain verification (ms)");
   opts("underwriter-enabled", bpo::value<bool>()->default_value(false),
        "Enable underwriter functionality");
   opts("underwriter-eth-client-id", bpo::value<std::string>()->default_value("eth-default"),
        "Ethereum outpost client ID");
   opts("underwriter-sol-client-id", bpo::value<std::string>()->default_value("sol-default"),
        "Solana outpost client ID");
   opts("underwriter-eth-opp-addr", bpo::value<std::string>(),
        "OPP contract address on Ethereum (hex)");
   opts("underwriter-sol-program-id", bpo::value<std::string>(),
        "OPP program ID on Solana (base58)");
}

void underwriter_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("underwriter-account"))
      _impl->underwriter_account = chain::name(options["underwriter-account"].as<std::string>());
   _impl->scan_interval_ms  = options["underwriter-scan-interval-ms"].as<uint32_t>();
   _impl->verify_timeout_ms = options["underwriter-verify-timeout-ms"].as<uint32_t>();
   _impl->enabled           = options["underwriter-enabled"].as<bool>();
   _impl->eth_client_id     = options["underwriter-eth-client-id"].as<std::string>();
   _impl->sol_client_id     = options["underwriter-sol-client-id"].as<std::string>();
   if (options.count("underwriter-eth-opp-addr"))
      _impl->eth_opp_addr = options["underwriter-eth-opp-addr"].as<std::string>();
   if (options.count("underwriter-sol-program-id"))
      _impl->sol_program_id = options["underwriter-sol-program-id"].as<std::string>();

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug  = &app().get_plugin<cron_plugin>();
   _impl->eth_plug   = &app().get_plugin<outpost_ethereum_client_plugin>();
   _impl->sol_plug   = &app().get_plugin<outpost_solana_client_plugin>();
}

void underwriter_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("underwriter_plugin: disabled, skipping startup");
      return;
   }

   ilog("underwriter_plugin: starting for account {}", _impl->underwriter_account.to_string());

   // Schedule scanning via cron_plugin
   auto& cron = app().get_plugin<cron_plugin>();
   cron_service::job_schedule sched;
   sched.milliseconds = {cron_service::job_schedule::step_value{_impl->scan_interval_ms}};

   cron_service::job_metadata_t meta;
   meta.label = "underwriter_scan";
   meta.one_at_a_time = true;

   _impl->scan_job_id = cron.add_job(
      sched,
      [this]() { _impl->scan_cycle(); },
      meta
   );

   ilog("underwriter_plugin: scheduled scan (id={}, interval={}ms)",
        _impl->scan_job_id, _impl->scan_interval_ms);
}

void underwriter_plugin::plugin_shutdown() {
   _impl->shutting_down = true;

   if (_impl->scan_job_id != 0) {
      auto& cron = app().get_plugin<cron_plugin>();
      cron.cancel_job(_impl->scan_job_id);
   }

   ilog("underwriter_plugin: shutdown complete");
}

} // namespace sysio

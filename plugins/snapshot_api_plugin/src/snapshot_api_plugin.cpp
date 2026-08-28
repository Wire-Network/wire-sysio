#include <sysio/snapshot_api_plugin/snapshot_api_plugin.hpp>
#include <sysio/snapshot_api_plugin/snapshot_catalog.hpp>
#include <sysio/http_plugin/common.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/snapshot.hpp>

#include <fc/io/json.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/crypto/blake3.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <system_error>

namespace sysio {

using namespace sysio;
using namespace sysio::chain;

/// Diagnostic returned when the catalog has no attested snapshot eligible for scheduled bootstrap.
constexpr auto no_servable_scheduled_snapshots_message = "No attested scheduled snapshots available";

/// Diagnostic returned when scheduled-snapshot discovery cannot complete reliably.
constexpr auto snapshot_discovery_unavailable_message = "Snapshot discovery temporarily unavailable";

/// HTTP status returned for a successful scheduled-snapshot discovery.
constexpr unsigned int snapshot_discovery_success_status = 200;

/// HTTP status returned when no servable scheduled snapshot exists.
constexpr unsigned int snapshot_discovery_not_found_status = 404;

/// HTTP status returned when scheduled-snapshot discovery cannot complete reliably.
constexpr unsigned int snapshot_discovery_unavailable_status = 503;

/// Request and ABI-decoder budget for one steady-state attestation-record lookup.
constexpr auto snapshot_attestation_query_timeout =
   snapshot_attestation_minimum_table_read_timeout;

/// Log label for a scheduled catalog entry whose attestation record cannot be inspected.
constexpr auto snapshot_attestation_query_log_prefix = "snapshot_api_plugin";

struct snapshot_entry {
   block_num_type         block_num = 0;
   chain::block_id_type   block_id;
   fc::time_point         block_time;
   fc::crypto::blake3     root_hash;
   std::filesystem::path  file_path;
   uint64_t               file_size = 0;
};

struct snapshot_metadata {
   block_num_type       block_num = 0;
   chain::block_id_type block_id;
   fc::time_point       block_time;
   fc::crypto::blake3   root_hash;
};

struct by_block_params {
   block_num_type block_num = 0;
};

struct download_params {
   block_num_type block_num = 0;
};

/** Return whether a catalog entry still names a regular snapshot file. */
bool is_snapshot_file_available(const snapshot_entry& entry) {
   std::error_code error;
   return std::filesystem::is_regular_file(entry.file_path, error);
}

} // namespace sysio

FC_REFLECT(sysio::snapshot_metadata, (block_num)(block_id)(block_time)(root_hash))
FC_REFLECT(sysio::by_block_params, (block_num))
FC_REFLECT(sysio::download_params, (block_num))

namespace sysio {

class snapshot_api_plugin_impl {
public:
   /** Bind catalog eligibility checks to the local chain state. */
   explicit snapshot_api_plugin_impl(chain_plugin& chain_plugin)
      : chain_plugin_(chain_plugin) {}

   mutable std::mutex                          catalog_mtx_;
   std::map<block_num_type, snapshot_entry>    catalog_; // block_num -> entry

   std::filesystem::path                       snapshots_dir_;

   /// Chain access used only through a valid main-thread or executor read window.
   chain_plugin&                               chain_plugin_;

   /** Return whether irreversible local state attests the exact snapshot tuple. */
   bool is_attested_snapshot(const snapshot_entry& entry) const {
      const auto expected_block_num = entry.block_num;
      const auto expected_block_id = entry.block_id;
      const auto expected_root_hash = entry.root_hash;

      static const std::atomic<bool> not_shutting_down{false};
      const auto result = retry_snapshot_attestation_table_read([&]() {
         auto params = make_snapshot_attestation_record_query(expected_block_num);
         params.filter = [this, expected_block_num, expected_block_id, expected_root_hash](
                            const fc::variant& row) {
            const auto& chain = chain_plugin_.chain();
            if (!chain.fork_db_has_root()) {
               return false;
            }
            return snapshot_api::is_servable_snapshot_attestation(
               row, expected_block_num, expected_block_id, expected_root_hash,
               chain.fork_db_root().block_num());
         };
         return chain_plugin_.read_table_rows_checked(
            std::move(params), snapshot_attestation_query_timeout,
            snapshot_attestation_query_timeout, snapshot_attestation_query_log_prefix,
            not_shutting_down);
      });
      return result && !result->rows.empty();
   }

   /** Return whether an explicit snapshot download passes file and scheduled-serving policy. */
   bool is_servable_snapshot(const snapshot_entry& entry) const {
      return is_snapshot_file_available(entry)
             && snapshot_api::is_snapshot_servable(
                entry.block_num, entry,
                [this](const snapshot_entry& snapshot) { return is_attested_snapshot(snapshot); });
   }

   void scan_snapshots_dir() {
      namespace fs = std::filesystem;
      if (!fs::is_directory(snapshots_dir_)) {
         wlog("Snapshot directory {} does not exist", snapshots_dir_.string());
         return;
      }

      std::lock_guard lock(catalog_mtx_);

      // Scan for snapshot-*.bin files
      std::regex snap_re("^snapshot-.*\\.bin$");
      for (const auto& entry : fs::directory_iterator(snapshots_dir_)) {
         if (!entry.is_regular_file())
            continue;
         auto fname = entry.path().filename().string();
         if (!std::regex_match(fname, snap_re))
            continue;
         // Skip pending/incomplete snapshots
         if (fname.starts_with("."))
            continue;

         try {
            threaded_snapshot_reader reader(entry.path());
            reader.load_index(); // fast - reads footer only

            // Extract metadata via snapshot_info
            reader.return_to_header();
            auto info = snapshot_info(reader);

            auto block_num = info["head_block_id"].as<block_id_type>();
            auto head_num = block_header::num_from_id(block_num);

            snapshot_entry se;
            se.block_num  = head_num;
            se.block_id   = block_num;
            se.block_time = info["head_block_time"].as<block_timestamp_type>();
            se.root_hash  = reader.get_root_hash();
            se.file_path  = entry.path();
            se.file_size  = entry.file_size();

            catalog_[se.block_num] = std::move(se);

            ilog("Catalogued snapshot: block #{} at {}", head_num, entry.path().string());
         } catch (const fc::exception& e) {
            elog("Failed to catalog snapshot {}: {}", entry.path().string(), e.to_detail_string());
         } catch (const std::exception& e) {
            elog("Failed to catalog snapshot {}: {}", entry.path().string(), e.what());
         }
      }
   }

   void on_snapshot_finalized(const snapshot_scheduler::snapshot_information& si) {
      snapshot_entry se;
      se.block_num  = si.head_block_num;
      se.block_id   = si.head_block_id;
      se.block_time = si.head_block_time;
      se.root_hash  = si.root_hash;
      se.file_path  = si.snapshot_name;
      try {
         se.file_size = std::filesystem::file_size(se.file_path);
      } catch (...) {
         se.file_size = 0;
      }

      std::unique_lock lock(catalog_mtx_);
      catalog_[se.block_num] = std::move(se);
      lock.unlock();
      ilog("Added snapshot to catalog: block #{}", si.head_block_num);
   }

   /**
    * Return the newest locally available snapshot with an exact irreversible attestation.
    *
    * This method is called from the HTTP thread pool. Each reverse attestation page is therefore
    * posted once to the read executor rather than nesting a blocking read inside a read-only task.
    * The configured HTTP response budget bounds the complete multi-page scan.
    */
   snapshot_api::snapshot_discovery_result<snapshot_entry>
   get_latest(fc::microseconds max_response_time) const {
      std::shared_ptr<const std::map<block_num_type, snapshot_entry>> catalog_snapshot;
      {
         std::lock_guard lock(catalog_mtx_);
         catalog_snapshot = std::make_shared<const std::map<block_num_type, snapshot_entry>>(catalog_);
      }

      const auto discovery_deadline = max_response_time == fc::microseconds::maximum()
                                         ? fc::time_point::maximum()
                                         : fc::time_point::now().safe_add(max_response_time);
      static const std::atomic<bool> not_shutting_down{false};
      const auto is_available = [](const snapshot_entry& snapshot) {
         return is_snapshot_file_available(snapshot);
      };
      const auto is_servable_attestation = [this](const auto& catalog, const fc::variant& row) {
         const auto& chain = chain_plugin_.chain();
         return chain.fork_db_has_root()
                && snapshot_api::is_servable_catalog_snapshot_attestation(
                   catalog, row, chain.fork_db_root().block_num(),
                   [](const snapshot_entry& snapshot) {
                      return is_snapshot_file_available(snapshot);
                   });
      };
      const auto read_page = [this](const auto& params) {
         return retry_snapshot_attestation_table_read([&]() {
            return chain_plugin_.read_table_rows_checked(
               params, snapshot_attestation_query_timeout,
               snapshot_attestation_query_timeout, snapshot_attestation_query_log_prefix,
               not_shutting_down);
         });
      };
      const auto deadline_reached = [discovery_deadline]() {
         return fc::time_point::now() >= discovery_deadline;
      };
      return snapshot_api::discover_latest_servable_scheduled_snapshot(
         *catalog_snapshot, is_available, is_servable_attestation, read_page,
         deadline_reached);
   }

   /** Return an explicit catalog entry without applying the separate download policy. */
   std::optional<snapshot_entry> get_by_block(block_num_type block_num) const {
      std::lock_guard lock(catalog_mtx_);
      const auto entry = catalog_.find(block_num);
      if (entry == catalog_.end()) {
         return std::nullopt;
      }
      return std::optional{entry->second};
   }
};

void snapshot_api_plugin::plugin_initialize(const variables_map& vm) {
   try {
      const auto& _http_plugin = app().get_plugin<http_plugin>();
      if (!_http_plugin.is_on_loopback(api_category::snapshot_ro)) {
         ilog("snapshot_api_plugin: snapshot_ro API exposed on non-loopback address (public snapshot serving enabled)");
      }
   } FC_LOG_AND_RETHROW()
}

void snapshot_api_plugin::plugin_startup() {
   ilog("starting snapshot_api_plugin");

   auto impl = std::make_shared<snapshot_api_plugin_impl>(
      app().get_plugin<chain_plugin>());

   auto& producer = app().get_plugin<producer_plugin>();
   impl->snapshots_dir_ = producer.get_snapshots_dir();

   // Scan existing snapshots
   impl->scan_snapshots_dir();

   // Register callback for new snapshots
   producer.add_snapshot_finalized_callback(
      [impl](const snapshot_scheduler::snapshot_information& si) {
         impl->on_snapshot_finalized(si);
      });

   auto& http = app().get_plugin<http_plugin>();
   const auto snapshot_discovery_max_response_time = http.get_max_response_time();

   // /v1/snapshot/latest - discover on the HTTP worker so chain reads can use the read executor
   http.add_raw_handler("/v1/snapshot/latest", api_category::snapshot_ro,
      [impl, snapshot_discovery_max_response_time](
         sysio::detail::abstract_conn_ptr conn, string&&, string&& body) {
         try {
            parse_params<std::string, http_params_types::no_params>(body);
            const auto result = impl->get_latest(snapshot_discovery_max_response_time);
            if (result.status == snapshot_api::snapshot_discovery_status::not_found) {
               conn->send_response(fc::json::to_string(
                                      fc::mutable_variant_object()
                                      ("message", no_servable_scheduled_snapshots_message),
                                      fc::time_point::maximum()),
                                   snapshot_discovery_not_found_status);
               return;
            }
            if (result.status == snapshot_api::snapshot_discovery_status::unavailable) {
               conn->send_response(fc::json::to_string(
                                      fc::mutable_variant_object()
                                      ("message", snapshot_discovery_unavailable_message),
                                      fc::time_point::maximum()),
                                   snapshot_discovery_unavailable_status);
               return;
            }
            const auto& entry = *result.snapshot;
            snapshot_metadata meta{entry.block_num, entry.block_id, entry.block_time, entry.root_hash};
            conn->send_response(
               fc::json::to_string(fc::variant(meta), fc::time_point::maximum()),
               snapshot_discovery_success_status);
         } catch (...) {
            conn->handle_exception();
         }
      });

   // /v1/snapshot/by_block - return metadata of snapshot at specific block
   http.add_api({
      {std::string("/v1/snapshot/by_block"),
       api_category::snapshot_ro,
       [impl](string&&, string&& body, url_response_callback&& cb) {
          try {
             auto params = parse_params<by_block_params, http_params_types::params_required>(body);
             auto entry = impl->get_by_block(params.block_num);
             if (!entry) {
                cb(404, fc::variant(fc::mutable_variant_object()("message", "No snapshot found for block " + std::to_string(params.block_num))));
                return;
             }
             snapshot_metadata meta{entry->block_num, entry->block_id, entry->block_time, entry->root_hash};
             cb(200, fc::variant(meta));
          } catch (...) {
             http_plugin::handle_exception("snapshot", "by_block", body, cb);
          }
       }},
   }, appbase::exec_queue::read_only, appbase::priority::medium_low);

   // /v1/snapshot/download - serve snapshot file
   http.add_raw_handler("/v1/snapshot/download", api_category::snapshot_ro,
      [impl](sysio::detail::abstract_conn_ptr conn, string&&, string&& body) {
         try {
            auto params = parse_params<download_params, http_params_types::params_required>(body);
            auto entry = impl->get_by_block(params.block_num);
            if (!entry || !impl->is_servable_snapshot(*entry)) {
               conn->send_response(fc::json::to_string(
                                      fc::mutable_variant_object()
                                      ("code", 404)
                                      ("message", "No snapshot found for block " + std::to_string(params.block_num)),
                                      fc::time_point::maximum()),
                                   404);
               return;
            }

            // Parse Range header if present
            std::optional<std::pair<uint64_t, uint64_t>> byte_range;
            auto range_header = conn->get_request_header("Range");
            if (!range_header.empty()) {
               // Parse "bytes=START-END" format
               std::regex range_re("bytes=(\\d+)-(\\d*)");
               std::smatch match;
               if (std::regex_match(range_header, match, range_re)) {
                  uint64_t start = std::stoull(match[1].str());
                  uint64_t end = match[2].str().empty() ? entry->file_size - 1 : std::stoull(match[2].str());
                  byte_range = std::make_pair(start, end);
               }
            }

            conn->send_file_response(entry->file_path, 200, "application/octet-stream", byte_range);
         } catch (...) {
            conn->handle_exception();
         }
      });

   ilog("snapshot_api_plugin: {} snapshot(s) catalogued", impl->catalog_.size());
}

} // namespace sysio

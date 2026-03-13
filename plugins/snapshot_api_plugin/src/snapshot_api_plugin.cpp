#include <sysio/snapshot_api_plugin/snapshot_api_plugin.hpp>
#include <sysio/http_plugin/common.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/snapshot.hpp>

#include <fc/io/json.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/crypto/blake3.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <regex>

namespace sysio {

using namespace sysio;
using namespace sysio::chain;

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

} // namespace sysio

FC_REFLECT(sysio::snapshot_metadata, (block_num)(block_id)(block_time)(root_hash))
FC_REFLECT(sysio::by_block_params, (block_num))
FC_REFLECT(sysio::download_params, (block_num))

namespace sysio {

class snapshot_api_plugin_impl {
public:
   mutable std::mutex                          catalog_mtx_;
   std::map<block_num_type, snapshot_entry>    catalog_; // block_num -> entry

   std::filesystem::path                       snapshots_dir_;

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

   std::optional<snapshot_entry> get_latest() const {
      std::lock_guard lock(catalog_mtx_);
      if (catalog_.empty())
         return std::nullopt;
      return std::optional{catalog_.rbegin()->second}; // highest block_num
   }

   std::optional<snapshot_entry> get_by_block(block_num_type block_num) const {
      std::lock_guard lock(catalog_mtx_);
      auto it = catalog_.find(block_num);
      if (it == catalog_.end())
         return std::nullopt;
      return std::optional{it->second};
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

   auto impl = std::make_shared<snapshot_api_plugin_impl>();

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

   // /v1/snapshot/latest - return metadata of latest snapshot
   http.add_api({
      {std::string("/v1/snapshot/latest"),
       api_category::snapshot_ro,
       [impl](string&&, string&& body, url_response_callback&& cb) {
          try {
             parse_params<std::string, http_params_types::no_params>(body);
             auto entry = impl->get_latest();
             if (!entry) {
                cb(404, fc::variant(fc::mutable_variant_object()("message", "No snapshots available")));
                return;
             }
             snapshot_metadata meta{entry->block_num, entry->block_id, entry->block_time, entry->root_hash};
             cb(200, fc::variant(meta));
          } catch (...) {
             http_plugin::handle_exception("snapshot", "latest", body, cb);
          }
       }},
   }, appbase::exec_queue::read_only, appbase::priority::medium_low);

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
            if (!entry) {
               conn->send_response(fc::json::to_string(
                                      fc::mutable_variant_object()
                                      ("code", 404)
                                      ("message", "No snapshot found for block " + std::to_string(params.block_num)),
                                      fc::time_point::maximum()),
                                   404);
               return;
            }

            if (!std::filesystem::exists(entry->file_path)) {
               conn->send_response(fc::json::to_string(
                                      fc::mutable_variant_object()
                                      ("code", 404)
                                      ("message", "Snapshot file no longer exists on disk"),
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

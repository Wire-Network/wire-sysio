#include <sysio/trace_api/store_provider.hpp>
#include <sysio/trace_api/logging.hpp>
#include <sysio/chain/config.hpp>

#include <fc/io/raw.hpp>
#include <fc/variant_object.hpp>
#include <fc/log/logger_config.hpp>

namespace {
      static constexpr uint32_t _current_version = 1;
      static constexpr const char* _trace_prefix = "trace_";
      static constexpr const char* _trace_index_prefix = "trace_index_";
      static constexpr const char* _trace_trx_id_prefix = "trace_trx_id_";
      static constexpr const char* _trace_trx_id_index_prefix = "trace_trx_idx_";
      static constexpr const char* _trace_blk_idx_prefix = "trace_blk_idx_";
      static constexpr const char* _trace_recv_bloom_prefix = "trace_recv_bloom_";
      static constexpr const char* _trace_ext = ".log";
      static constexpr const char* _compressed_trace_ext = ".clog";
      // Sized for the longest possible filename across every prefix and
      // extension known to this file.  Adding a longer prefix above
      // automatically grows the buffer; no manual recount needed.
      static constexpr size_t _max_prefix_length = std::max({
         std::char_traits<char>::length(_trace_prefix),
         std::char_traits<char>::length(_trace_index_prefix),
         std::char_traits<char>::length(_trace_trx_id_prefix),
         std::char_traits<char>::length(_trace_trx_id_index_prefix),
         std::char_traits<char>::length(_trace_blk_idx_prefix),
         std::char_traits<char>::length(_trace_recv_bloom_prefix),
      });
      static constexpr size_t _max_ext_length = std::max(
         std::char_traits<char>::length(_trace_ext),
         std::char_traits<char>::length(_compressed_trace_ext)
      );
      // prefix + 10-digit start + '-' + 10-digit end + ext + '\0'
      static constexpr int _max_filename_size = _max_prefix_length + 10 + 1 + 10 + _max_ext_length + 1;

      std::string make_filename(const char* slice_prefix, const char* slice_ext, uint32_t slice_number, uint32_t slice_width) {
         char filename[_max_filename_size] = {};
         const uint32_t slice_start = slice_number * slice_width;
         const int size_written = snprintf(filename, _max_filename_size, "%s%010u-%010u%s", slice_prefix, slice_start, (slice_start + slice_width), slice_ext);
         // assert that _max_filename_size is correct
         if ( size_written >= _max_filename_size ) {
            const std::string max_size_str = std::to_string(_max_filename_size - 1); // dropping null character from size
            const std::string size_written_str = std::to_string(size_written);
            throw std::runtime_error("Could not write the complete filename.  Anticipated the max filename characters to be: " +
               max_size_str + " or less, but wrote: " + size_written_str + " characters.  This is likely because the file "
               "format was changed and the code was not updated accordingly. Filename created: " + filename);
         }

         return std::string(filename);
      }
}

namespace sysio::trace_api {
      store_provider::store_provider(const std::filesystem::path& slice_dir, uint32_t stride_width, std::optional<uint32_t> minimum_irreversible_history_blocks,
                                  std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks, size_t compression_seek_point_stride)
   : _slice_directory(slice_dir, stride_width, minimum_irreversible_history_blocks, minimum_uncompressed_irreversible_history_blocks, compression_seek_point_stride)
   , _abi_log(slice_dir / "abi_log.log") {
      // Both watermarks (best_known_lib, last_recorded_block) are seeded from disk by the
      // slice_directory constructor above, so the reversible window is known here.
      rebuild_reversible_abis();
   }

   template<typename BlockTrace>
   void store_provider::append(const BlockTrace& bt) {
      fc::cfile trace;
      fc::cfile index;
      const uint32_t slice_number = _slice_directory.slice_number(bt.number);

      _slice_directory.find_or_create_slice_pair(slice_number, open_state::write, trace, index);
      // storing as static_variant to allow adding other data types to the trace file in the future
      const uint64_t offset = append_store(data_log_entry { bt }, trace);

      auto be = metadata_log_entry { block_entry_v0 { .id = bt.id, .number = bt.number, .offset = offset }};
      append_store(be, index);

      // Record in the block-offset sidecar for O(1) get_block lookups.  Fork re-writes
      // overwrite the slot.  write_block_offset never throws (the sidecar is advisory):
      // on failure the metadata log remains the source of truth and get_block falls
      // back to the linear scan.
      _slice_directory.write_block_offset(bt.number, offset);

      _slice_directory.note_recorded_block(bt.number);
   }

   template void store_provider::append<block_trace_v0>(const block_trace_v0& bt);

   void store_provider::append_lib(uint32_t lib) {
      fc::cfile index, trx_id;
      const uint32_t slice_number = _slice_directory.slice_number(lib);
      _slice_directory.find_or_create_index_slice(slice_number, open_state::write, index);
      auto le = metadata_log_entry { lib_entry_v0 { .lib = lib }};
      append_store(le, index);
      _slice_directory.find_or_create_trx_id_slice(slice_number, open_state::write, trx_id);
      append_store(le, trx_id);
      // Blocks at or below lib can no longer fork out: persist their ABI records to the
      // on-disk abi log, the same finality boundary the other trace files key off.
      _abi_log.flush_irreversible(lib);
      _slice_directory.set_lib(lib);
   }

   void store_provider::append_trx_ids(block_trxs_entry tt){
      fc::cfile trx_id_file;
      const uint32_t slice_number = _slice_directory.slice_number(tt.block_num);
      _slice_directory.find_or_create_trx_id_slice(slice_number, open_state::write, trx_id_file);
      auto entry = metadata_log_entry { std::move(tt) };
      append_store(entry, trx_id_file);
   }

   bloom_reader store_provider::get_bloom(uint32_t slice_number) const {
      const auto path = _slice_directory.bloom_slice_path(slice_number);
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) return bloom_reader{};
      return bloom_reader{path};
   }

   get_block_t store_provider::get_block(uint32_t block_height, const yield_function& yield) {
      // Fast path: O(1) random-access lookup of the trace offset via the block-offset sidecar.
      std::optional<uint64_t> trace_offset = _slice_directory.lookup_block_offset(block_height);

      if (!trace_offset) {
         // Fallback: scan the metadata log.  Covers slices without a sidecar (e.g. corruption,
         // missing sidecar file) or the rare case where a block exists in the metadata log but
         // the sidecar write was interrupted.
         scan_metadata_log_from(block_height, 0, [&block_height, &trace_offset](const metadata_log_entry& e) -> bool {
            if (std::holds_alternative<block_entry_v0>(e)) {
               const auto& block = std::get<block_entry_v0>(e);
               if (block.number == block_height) {
                  trace_offset = block.offset;
               }
            }
            return true;
         }, yield);
      }

      if (!trace_offset) {
         return get_block_t{};
      }

      const bool irreversible = block_height <= _slice_directory.best_known_lib();

      std::optional<data_log_entry> entry = read_data_log(block_height, *trace_offset);
      if (!entry) {
         return get_block_t{};
      }
      return std::make_tuple( entry.value(), irreversible );
   }

   get_block_n store_provider::get_trx_block_number(const chain::transaction_id_type& trx_id, const yield_function& yield) {
      // Fast path: probe the per-slice hash index for each slice newest-to-oldest.
      // The index covers only irreversible slices; reversible blocks fall through to
      // the linear scan below.
      std::set<uint32_t> trx_block_nums;

      _slice_directory.for_each_trx_id_slice([&](fc::cfile& trx_id_file) -> bool {
         yield();

         // Derive the slice number from the file path and try the index first.
         // If the filename can't be parsed (slice_number_from_path returns nullopt),
         // fall through to the linear scan below.
         const std::optional<uint32_t> slice_number =
            _slice_directory.slice_number_from_path(trx_id_file.get_file_path());
         if (slice_number) {
            if (auto reader = _slice_directory.find_trx_id_index_slice(*slice_number)) {
               if (auto block_num = reader->lookup(trx_id)) {
                  // Confirm the hit with a full trx_id match.  A naked 64-bit
                  // prefix collision (extremely rare with natural sha256 ids,
                  // but findable in ~2^32 GPU work adversarially) would otherwise
                  // return the wrong block_num and the downstream get_block
                  // scan would 404 the real trx.  On confirm failure, reset the
                  // file to offset 0 and fall through to the linear scan below.
                  bool confirmed = false;
                  if (const std::optional<bool> fast = confirm_trx_in_block(*block_num, trx_id)) {
                     // O(1) path: one block-trace read via the block-offset sidecar instead of
                     // parsing the trx_id log up to the candidate block.  A `false` here can also
                     // mean the trx has ids-only coverage (recorded in block_trxs_entry but its
                     // trace was never cached); the collision fall-through below still finds those
                     // through the trx_id-log scan, so the fast path never loses results.
                     confirmed = *fast;
                  } else {
                     // Block-offset sidecar or trace data couldn't resolve the candidate block:
                     // confirm by scanning this slice's trx_id log for the block's entry.
                     metadata_log_entry entry;
                     auto ds = trx_id_file.create_datastream();
                     const uint64_t end = file_size(trx_id_file.get_file_path());
                     while (trx_id_file.tellp() < end) {
                        yield();
                        fc::raw::unpack(ds, entry);
                        if (!std::holds_alternative<block_trxs_entry>(entry)) continue;
                        const auto& te = std::get<block_trxs_entry>(entry);
                        if (te.block_num != *block_num) continue;
                        for (const auto& id : te.ids) {
                           if (id == trx_id) { confirmed = true; break; }
                        }
                        if (confirmed) break;
                     }
                  }
                  if (confirmed) {
                     trx_block_nums.insert(*block_num);
                     return false; // found in an irreversible slice; stop
                  }
                  // Prefix collision: reset the file so the linear scan starts
                  // from offset 0 -- the target trx may still be in a DIFFERENT
                  // block whose trxs entry we passed over during confirmation.
                  trx_id_file.seek(0);
                  // fall through to linear scan
               } else {
                  return true; // not in this indexed slice; continue to next
               }
            }
         }

         // No index for this slice (reversible window or index not yet built):
         // fall back to linear scan.
         metadata_log_entry entry;
         auto ds = trx_id_file.create_datastream();
         const uint64_t end = file_size(trx_id_file.get_file_path());
         uint64_t offset = trx_id_file.tellp();
         while (offset < end) {
            yield();
            fc::raw::unpack(ds, entry);
            if (std::holds_alternative<block_trxs_entry>(entry)) {
               const auto& trxs_entry = std::get<block_trxs_entry>(entry);
               bool found_in_block = false;
               for (auto i = 0U; i < trxs_entry.ids.size(); ++i) {
                  if (trxs_entry.ids[i] == trx_id) {
                     trx_block_nums.insert(trxs_entry.block_num);
                     found_in_block = true;
                     break;
                  }
               }
               // block can be seen again when a fork happens, if not in the new block remove it from blocks that have the trx
               if (!found_in_block)
                  trx_block_nums.erase(trxs_entry.block_num);
            } else if (std::holds_alternative<lib_entry_v0>(entry)) {
               auto lib = std::get<lib_entry_v0>(entry).lib;
               if (!trx_block_nums.empty() && lib >= *(--trx_block_nums.end())) {
                  return false; // *(--trx_block_nums.end()) is the block with highest block number which is final
               }
            } else {
               FC_ASSERT( false, "unpacked data should be a block_trxs_entry or a lib_entry_v0" );
            }
            offset = trx_id_file.tellp();
         }
         // if empty() keep searching
         // if not empty() then we have found the trx and since traversing in reverse order this should be the latest
         return trx_block_nums.empty();
      });

      if (!trx_block_nums.empty())
         return *(--trx_block_nums.end());

      return {};
   }

   std::optional<bool> store_provider::confirm_trx_in_block(uint32_t block_num, const chain::transaction_id_type& trx_id) {
      // Resolve the block's canonical trace via the block-offset sidecar (fork re-writes overwrite the
      // slot, so the offset always names the post-fork-resolution copy) and compare full transaction ids.
      std::optional<bool> result;
      try {
         const std::optional<uint64_t> offset = _slice_directory.lookup_block_offset(block_num);
         if (!offset)
            return result;
         std::optional<data_log_entry> entry = read_data_log(block_num, *offset);
         if (!entry)
            return result;
         result = std::visit([&trx_id](const auto& bt) {
            for (const auto& trx : bt.transactions) {
               if (trx.id == trx_id)
                  return true;
            }
            return false;
         }, *entry);
      } catch (...) {
         // Unreadable trace data: stay disengaged so the caller falls back to the trx_id-log scan.
         result.reset();
      }
      return result;
   }

   slice_directory::slice_directory(const std::filesystem::path& slice_dir, uint32_t width, std::optional<uint32_t> minimum_irreversible_history_blocks, std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks, size_t compression_seek_point_stride)
   : _slice_dir(slice_dir)
   , _width(width)
   , _minimum_irreversible_history_blocks(minimum_irreversible_history_blocks)
   , _minimum_uncompressed_irreversible_history_blocks(minimum_uncompressed_irreversible_history_blocks)
   , _compression_seek_point_stride(compression_seek_point_stride)
   , _best_known_lib(0) {
      if (!exists(_slice_dir)) {
         std::filesystem::create_directories(slice_dir);
      }
      seed_watermarks_from_disk();
   }

   bool slice_directory::find_or_create_index_slice(uint32_t slice_number, open_state state, fc::cfile& index_file) const {
      const bool found = find_index_slice(slice_number, state, index_file);
      if( !found ) {
         create_new_index_slice_file(index_file);
      }
      return found;
   }

   bool slice_directory::find_index_slice(uint32_t slice_number, open_state state, fc::cfile& index_file, bool open_file) const {
      const bool found = find_slice(_trace_index_prefix, slice_number, index_file, open_file);
      if( !found || !open_file ) {
         return found;
      }

      validate_existing_index_slice_file(index_file, state);
      return true;
   }

   void slice_directory::create_new_index_slice_file(fc::cfile& index_file) const {
      index_file.open(fc::cfile::create_or_update_rw_mode);
      index_header h { .version = _current_version };
      append_store(h, index_file);
   }

   void slice_directory::validate_existing_index_slice_file(fc::cfile& index_file, open_state state) const {
      const auto header = extract_store<index_header>(index_file);
      if (header.version != _current_version) {
         throw old_slice_version("Old slice file with version: " + std::to_string(header.version) +
                                 " is in directory, only supporting version: " + std::to_string(_current_version));
      }

      if( state == open_state::write ) {
         index_file.seek_end(0);
      }
   }

   bool slice_directory::find_or_create_trace_slice(uint32_t slice_number, open_state state, fc::cfile& trace_file) const {
      const bool found = find_trace_slice(slice_number, state, trace_file);

      if( !found ) {
         trace_file.open(fc::cfile::create_or_update_rw_mode);
      }

      return found;
   }

   bool slice_directory::find_trace_slice(uint32_t slice_number, open_state state, fc::cfile& trace_file, bool open_file) const {
      const bool found = find_slice(_trace_prefix, slice_number, trace_file, open_file);

      if( !found || !open_file ) {
         return found;
      }

      if( state == open_state::write ) {
         trace_file.seek_end(0);
      }
      else {
         trace_file.seek(0); // ensure we are at the start of the file
      }
      return true;
   }

   std::filesystem::path slice_directory::bloom_slice_path(uint32_t slice_number) const {
      // Mirrors the filename convention of the other sidecars: <prefix><first>-<last><ext>.  Callers write through
      // bloom_builder::finalize_and_write (temp + rename) and read through bloom_reader, neither of which uses
      // fc::cfile, so no open-helper is needed here.
      return _slice_dir / make_filename(_trace_recv_bloom_prefix, _trace_ext, slice_number, _width);
   }

   std::optional<compressed_file> slice_directory::find_compressed_trace_slice(uint32_t slice_number, bool open_file ) const {
      auto filename = make_filename(_trace_prefix, _compressed_trace_ext, slice_number, _width);
      const auto slice_path = _slice_dir / filename;
      const bool file_exists = exists(slice_path);

      if (file_exists) {
         auto result = compressed_file(slice_path);
         if (open_file) {
            result.open();
         }

         return std::move(result);
      } else {
         return {};
      }
   }

   bool slice_directory::find_slice(const char* slice_prefix, uint32_t slice_number, fc::cfile& slice_file, bool open_file) const {
      auto filename = make_filename(slice_prefix, _trace_ext, slice_number, _width);
      const auto slice_path = _slice_dir / filename;
      slice_file.set_file_path(slice_path);

      const bool file_exists = exists(slice_path);
      if( !file_exists || !open_file ) {
         return file_exists;
      }

      slice_file.open(fc::cfile::create_or_update_rw_mode);
      // TODO: this is a temporary fix until fc::cfile handles it internally.  OSX and Linux differ on the read offset
      // when opening in "ab+" mode
      slice_file.seek(0);
      return true;
   }


   void slice_directory::find_or_create_slice_pair(uint32_t slice_number, open_state state, fc::cfile& trace, fc::cfile& index) {
      const bool trace_found = find_or_create_trace_slice(slice_number, state, trace);
      const bool index_found = find_or_create_index_slice(slice_number, state, index);
      if (trace_found != index_found) {
         const std::string trace_status = trace_found ? "existing" : "new";
         const std::string index_status = index_found ? "existing" : "new";
         fc_elog(_log, "Trace file is {}, but it's metadata file is {}. This means the files are not consistent.", trace_status, index_status);
      }
   }

   bool slice_directory::find_or_create_trx_id_slice(uint32_t slice_number, open_state state, fc::cfile& trx_id_file) const {
       const bool found = find_trx_id_slice(slice_number, state, trx_id_file);
       if( !found ) {
           trx_id_file.open(fc::cfile::create_or_update_rw_mode);
       }
       return found;
   }

   bool slice_directory::find_trx_id_slice(uint32_t slice_number, open_state state, fc::cfile& trx_id_file, bool open_file) const {
      const bool found = find_slice(_trace_trx_id_prefix, slice_number, trx_id_file, open_file);
      if( !found || !open_file ) {
         return found;
      }
      if( state == open_state::write ) {
          trx_id_file.seek_end(0);
      }
      return true;
   }

   void slice_directory::for_each_trx_id_slice(std::function<bool(fc::cfile&)> callback) const {
      namespace fs = std::filesystem;
      std::vector<fs::directory_entry> trx_id_files;
      for (const auto& entry : fs::directory_iterator(_slice_dir)) {
         if (entry.is_regular_file()) {
            // Prefix + extension match (not substring) so stray files - editor backups, a .tmp left by a
            // crashed rename - are never picked up and fed to the scan's FC_ASSERT.
            const std::string name = entry.path().filename().string();
            if (name.rfind(_trace_trx_id_prefix, 0) == 0 && entry.path().extension() == _trace_ext) {
               trx_id_files.push_back(entry);
            }
         }
      }
      // the trace_trx_id_ files naturally sort via their file names, e.g. trace_trx_id_0211960000-0211970000.log
      // std::filesystem::path is lexicographically compared
      std::sort(trx_id_files.begin(), trx_id_files.end(),
                [&](const fs::directory_entry& a, const fs::directory_entry& b) {
                   return a.path() > b.path();
                });
      fc::cfile slice_file;
      for (const auto& entry : trx_id_files) {
         std::error_code ec;
         if (!entry.exists(ec))
            continue;
         slice_file.set_file_path(entry.path());
         try {
            slice_file.open(fc::cfile::read_only_mode);
         } catch (const std::exception& e) {
            // The maintenance thread can prune the file between the existence check and the open;
            // skip it rather than surface a transient 500 to the query path.
            fc_wlog(_log, "trace_api: cannot open trx id slice '{}': {}", entry.path().string(), e.what());
            continue;
         }
         slice_file.seek(0);
         if (!callback(slice_file))
            return;
         slice_file.close();
      }
   }

   namespace {
      // Parse the zero-padded slice start block out of "<prefix><start>-<end><ext>" and convert it to a
      // slice number.  Returns nullopt when the filename does not parse (callers should fall back to a
      // slower lookup path rather than skipping data silently).
      std::optional<uint32_t> parse_slice_number(const std::filesystem::path& path, const char* prefix, uint32_t width) {
         const std::string name = path.filename().string();
         const size_t prefix_len = std::char_traits<char>::length(prefix);
         try {
            const uint32_t start_block = static_cast<uint32_t>(std::stoul(name.substr(prefix_len, 10)));
            return start_block / width;
         } catch (...) {
            fc_wlog(_log, "trace_api: cannot parse slice start-block from filename '{}'", name);
            return std::nullopt;
         }
      }

      // Collect and sort all trace_index_*.log paths; ascending=true gives lowest slice first.
      std::vector<std::filesystem::path> collect_index_paths(const std::filesystem::path& slice_dir, bool ascending) {
         namespace fs = std::filesystem;
         std::vector<fs::path> paths;
         for (const auto& entry : fs::directory_iterator(slice_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(_trace_index_prefix, 0) == 0 && entry.path().extension() == _trace_ext)
               paths.push_back(entry.path());
         }
         if (ascending) {
            std::sort(paths.begin(), paths.end());
         } else {
            std::sort(paths.begin(), paths.end(), std::greater<>());
         }
         return paths;
      }

      // Everything a full read of one index slice can tell us.  Fields stay disengaged when the slice
      // holds no entry of that kind (e.g. a freshly rolled slice with blocks but no lib entry yet).
      struct index_slice_scan_result {
         std::optional<uint32_t> lo_block;
         std::optional<uint32_t> hi_block;
         std::optional<uint32_t> max_lib;
      };

      // Scan a single index slice file.  Returns nullopt when the file is unreadable, has a wrong
      // version, or is malformed mid-record.
      std::optional<index_slice_scan_result> scan_index_slice(const std::filesystem::path& path, uint32_t current_version) {
         try {
            fc::cfile index;
            index.set_file_path(path);
            index.open(fc::cfile::read_only_mode);
            index.seek(0);
            const auto header = extract_store<slice_directory::index_header>(index);
            if (header.version != current_version)
               return std::nullopt;
            const uint64_t end = file_size(path);
            index_slice_scan_result r;
            while (index.tellp() < end) {
               const auto e = extract_store<metadata_log_entry>(index);
               if (std::holds_alternative<block_entry_v0>(e)) {
                  const auto num = std::get<block_entry_v0>(e).number;
                  if (!r.lo_block || num < *r.lo_block) r.lo_block = num;
                  if (!r.hi_block || num > *r.hi_block) r.hi_block = num;
               } else if (std::holds_alternative<lib_entry_v0>(e)) {
                  const auto lib = std::get<lib_entry_v0>(e).lib;
                  if (!r.max_lib || lib > *r.max_lib) r.max_lib = lib;
               }
            }
            return r;
         } catch (const std::exception& e) {
            // malformed or partially written slice
            fc_wlog(_log, "trace_api: cannot scan index slice '{}': {}", path.string(), e.what());
         } catch (...) {
            fc_wlog(_log, "trace_api: cannot scan index slice '{}'", path.string());
         }
         return std::nullopt;
      }
   } // anonymous namespace

   std::optional<std::pair<uint32_t,uint32_t>> slice_directory::first_and_last_recorded_blocks() const {
      // Named local with the exact return type so the compiler can NRVO it directly
      // into the caller's slot.  Single directory scan: collect ascending paths,
      // walk from both ends.  Returning both bounds from one call guarantees callers
      // see a consistent view -- either both values are present or neither is.
      std::optional<std::pair<uint32_t,uint32_t>> result;

      const auto paths = collect_index_paths(_slice_dir, /*ascending=*/true);
      std::optional<uint32_t> first_block;
      for (const auto& path : paths) {
         if (const auto r = scan_index_slice(path, _current_version); r && r->lo_block) {
            first_block = r->lo_block;
            break;
         }
      }
      if (!first_block)
         return result;

      // first_block was found, so at least one slice has block entries; the reverse
      // pass is guaranteed to find at least one (worst case, the same slice).
      uint32_t last_block = *first_block;
      for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
         if (const auto r = scan_index_slice(*it, _current_version); r && r->hi_block) {
            last_block = *r->hi_block;
            break;
         }
      }

      result.emplace(*first_block, last_block);
      return result;
   }

   void slice_directory::seed_watermarks_from_disk() {
      // Recover the previous run's watermarks from the persisted index slices so a freshly restarted node
      // answers queries consistently before the first chain signal arrives:
      //   - _best_known_lib: blocks at or below the recorded LIB keep reporting "irreversible" across a
      //     restart.  Safe to trust: finality is absolute, so a lib_entry_v0 once written can never regress
      //     (even a snapshot restore to an earlier head re-applies blocks below that recorded LIB).
      //   - _last_recorded_block: the get_actions / get_token_transfers envelope clamps its reported scan
      //     end to this watermark, so pagination over existing data works before any new block is appended.
      // Scan newest slice first: block numbers and LIB are monotonic across slices, so the first slice
      // containing an entry of each kind yields the global maximum for that kind.  Runs in the constructor,
      // before the maintenance thread or any signal handler exists, so unsynchronized writes are safe.
      std::optional<uint32_t> max_block;
      std::optional<uint32_t> max_lib;
      for (const auto& path : collect_index_paths(_slice_dir, /*ascending=*/false)) {
         if (max_block && max_lib)
            break;
         if (const auto r = scan_index_slice(path, _current_version)) {
            if (!max_block && r->hi_block)
               max_block = r->hi_block;
            if (!max_lib && r->max_lib)
               max_lib = r->max_lib;
         }
      }
      if (max_block)
         _last_recorded_block.store(*max_block, std::memory_order_relaxed);
      if (max_lib)
         _best_known_lib = *max_lib;
   }

   void slice_directory::note_recorded_block(uint32_t block_number) {
      // Single writer (the extraction thread); readers are HTTP threads.  Monotonic max keeps the watermark
      // at the head-most block ever appended, which is exactly the "scannable through" bound that the query
      // envelope reports.
      if (block_number > _last_recorded_block.load(std::memory_order_relaxed))
         _last_recorded_block.store(block_number, std::memory_order_relaxed);
   }

   uint32_t slice_directory::last_recorded_block() const {
      return _last_recorded_block.load(std::memory_order_relaxed);
   }

   std::optional<std::pair<uint32_t,uint32_t>> slice_directory::find_index_slice_gap() const {
      // Filename-only contiguity check: collect_index_paths returns zero-padded names whose
      // lexicographic order equals numeric order, so the parsed slice numbers arrive ascending.
      // A present-but-unreadable slice is the continuity scan's concern, not this one's - this
      // catches the "operator deleted or partially copied middle slices" hole that the
      // first/last range check cannot see.
      std::optional<std::pair<uint32_t,uint32_t>> result;
      std::vector<uint32_t> slice_numbers;
      for (const auto& path : collect_index_paths(_slice_dir, /*ascending=*/true)) {
         if (const auto n = parse_slice_number(path, _trace_index_prefix, _width))
            slice_numbers.push_back(*n);
      }
      for (size_t i = 1; i < slice_numbers.size(); ++i) {
         if (slice_numbers[i] > slice_numbers[i - 1] + 1) {
            // Both bounds fit in uint32_t: the neighbouring slice files exist, so their start
            // blocks (slice_number * width) are representable by construction.
            const uint32_t first_missing = static_cast<uint32_t>((uint64_t{slice_numbers[i - 1]} + 1) * _width);
            const uint32_t last_missing  = static_cast<uint32_t>(uint64_t{slice_numbers[i]} * _width - 1);
            result.emplace(first_missing, last_missing);
            return result;
         }
      }
      return result;
   }

   std::optional<std::pair<uint32_t,uint32_t>> store_provider::first_and_last_recorded_blocks() const {
      return _slice_directory.first_and_last_recorded_blocks();
   }

   void store_provider::append_abi(uint32_t block_num, chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
      _abi_log.append_reversible(block_num, account, global_seq, std::move(abi_bytes));
   }

   void store_provider::rollback_abis(uint32_t block_num) {
      _abi_log.rollback_reversible(block_num);
   }

   void store_provider::rebuild_reversible_abis() {
      const uint32_t lib  = _slice_directory.best_known_lib();
      const uint32_t last = _slice_directory.last_recorded_block();
      // last == 0 means nothing was ever recorded; otherwise the reversible window is
      // (lib, last].  On a healthy Savanna node this is a handful of blocks at most.
      if (last == 0 || last <= lib)
         return;

      uint32_t rebuilt = 0;
      for (uint32_t block_num = lib + 1; block_num <= last; ++block_num) {
         try {
            get_block_t bt = get_block(block_num);
            if (!bt)
               continue; // hole in the reversible window (e.g. fork overwrite gap) - nothing to rebuild
            const auto& block = std::get<block_trace_v0>(std::get<0>(*bt));
            for (const auto& trx : block.transactions) {
               for (const auto& at : trx.actions) {
                  // Same detection as chain_extraction's live capture: a setabi action on the
                  // system account.  The recorded action trace carries the exact
                  // global_sequence and the raw action payload (target account + ABI bytes).
                  if (at.account != chain::config::system_account_name || at.action != setabi_action_name)
                     continue;
                  try {
                     auto [target, abi_bytes] = unpack_setabi_data(at.data);
                     _abi_log.append_reversible(block_num, target, at.global_sequence,
                                                std::vector<char>(abi_bytes.begin(), abi_bytes.end()));
                     ++rebuilt;
                  } catch (const std::exception& e) {
                     fc_wlog(_log, "trace_api: failed to unpack recorded setabi at global_seq {} while rebuilding "
                                   "reversible ABIs: {}", at.global_sequence, e.what());
                  }
               }
            }
         } catch (const std::exception& e) {
            // Advisory: a block that cannot be read only degrades decoding of its (reversible)
            // setabi targets to raw hex; it must not block startup.
            fc_wlog(_log, "trace_api: failed to read block {} while rebuilding reversible ABIs: {}",
                    block_num, e.what());
         }
      }
      if (rebuilt > 0)
         fc_ilog(_log, "trace_api: rebuilt {} reversible ABI record(s) from recorded traces for blocks ({}, {}]",
                 rebuilt, lib, last);
   }

   std::optional<uint64_t> store_provider::lookup_abi_seq(chain::name account, uint64_t global_seq) const {
      return _abi_log.lookup_seq(account, global_seq);
   }

   std::optional<std::vector<char>> store_provider::fetch_abi(chain::name account, uint64_t effective_global_seq) const {
      return _abi_log.fetch(account, effective_global_seq);
   }

   bool store_provider::has_abi_entry(chain::name account) const {
      return _abi_log.has_entry(account);
   }

   std::optional<uint32_t> slice_directory::slice_number_from_path(const std::filesystem::path& trx_id_path) const {
      // Filename format: trace_trx_id_XXXXXXXXXX-YYYYYYYYYY.log
      // Parse the start block number (XXXXXXXXXX) and divide by _width.
      return parse_slice_number(trx_id_path, _trace_trx_id_prefix, _width);
   }

   std::optional<trx_id_index_reader> slice_directory::find_trx_id_index_slice(uint32_t slice_number) const {
      auto filename = make_filename(_trace_trx_id_index_prefix, _trace_ext, slice_number, _width);
      const auto path = _slice_dir / filename;
      if (!std::filesystem::exists(path))
         return std::nullopt;
      trx_id_index_reader reader(path);
      if (!reader.valid())
         return std::nullopt;
      return reader;
   }

   void slice_directory::build_trx_id_index(uint32_t slice_number, const log_handler& log) {
      auto idx_filename = make_filename(_trace_trx_id_index_prefix, _trace_ext, slice_number, _width);
      const auto idx_path = _slice_dir / idx_filename;
      if (std::filesystem::exists(idx_path))
         return; // already built

      fc::cfile trx_id_file;
      if (!find_trx_id_slice(slice_number, open_state::read, trx_id_file))
         return; // no source data

      log(std::string("Building trx_id index for slice: ") + std::to_string(slice_number));

      // Dedup pass: the trx_id log can hold MULTIPLE block_trxs_entry records
      // with the same block_num when the chain forks at that height (each
      // accepted block, including forked-out ones, writes one entry).  The
      // last entry for each block_num reflects the canonical post-fork-
      // resolution state, matching the semantics of the linear-scan path in
      // get_trx_block_number which uses trx_block_nums.erase to drop forked-
      // out entries.  Build a canonical map first, then feed it to the
      // writer (which itself does last-write-wins per prefix).
      std::map<uint32_t /*block_num*/, std::vector<chain::transaction_id_type>> canonical;
      const uint64_t end = file_size(trx_id_file.get_file_path());
      while (trx_id_file.tellp() < end) {
         metadata_log_entry entry;
         auto ds = trx_id_file.create_datastream();
         fc::raw::unpack(ds, entry);
         if (std::holds_alternative<block_trxs_entry>(entry)) {
            const auto& te = std::get<block_trxs_entry>(entry);
            canonical[te.block_num] = te.ids; // last entry per block_num wins
         }
      }

      trx_id_index_writer writer;
      for (const auto& [block_num, ids] : canonical) {
         for (const auto& id : ids)
            writer.add(id, block_num);
      }

      // Write to a temp path and atomically rename so concurrent readers never
      // see a partially written index file.
      const auto tmp_path = idx_path.parent_path() / (idx_path.filename().string() + ".tmp");
      writer.write(tmp_path);
      std::filesystem::rename(tmp_path, idx_path);
      log(std::string("Built trx_id index for slice: ") + std::to_string(slice_number) +
          " (" + std::to_string(writer.entry_count()) + " entries)");
   }

   void slice_directory::build_recv_bloom(uint32_t slice_number, const log_handler& log) {
      const auto bloom_path = bloom_slice_path(slice_number);
      if (std::filesystem::exists(bloom_path))
         return; // already built

      // Locate the slice's trace data log (trace_<range>.log).  run_maintenance_tasks orders bloom building before
      // compression so a freshly-irreversible slice still has its uncompressed .log.  If only a compressed .clog
      // exists (e.g. upgrading a node that predates the bloom) or the file is missing, skip; the query path treats
      // a missing sidecar as "scan this slice".  Don't decompress-then-scan - compressed slices are aged and rarely
      // queried.  Look up the path without opening so we can check size before committing to an open.
      fc::cfile trace;
      const bool dont_open_file = false;
      if (!find_trace_slice(slice_number, open_state::read, trace, dont_open_file)) {
         log(std::string("trace_api: skipping receiver bloom for slice ") + std::to_string(slice_number) +
             " (no uncompressed trace data; already compressed or never written)");
         return;
      }
      // Empty trace file => no actions to bloom.  Production slices always have on-block traces so this only fires
      // in tests that pre-create slice files; keeping it guards the maintenance path from writing a zero-entry
      // sidecar that'd just clutter the directory.
      const auto trace_path = trace.get_file_path();
      std::error_code ec;
      const uint64_t trace_size = std::filesystem::file_size(trace_path, ec);
      if (ec || trace_size == 0) return;

      log(std::string("Building receiver bloom for slice: ") + std::to_string(slice_number));

      trace.open(fc::cfile::read_only_mode);

      bloom_builder builder;
      bool processed_any_block = false;
      bool parsed_clean        = true;
      try {
         // Stream through the data log record-by-record.  Fork re-writes leave stale block_trace_v0 records in the
         // file (the blk_offset sidecar only points to the canonical one), so the bloom will contain a superset of
         // the canonical receivers.  That's fine: bloom allows false positives; a receiver present only in a forked-
         // out copy just probes as present and the query scan finds no canonical match for it.
         while (trace.tellp() < trace_size) {
            data_log_entry entry;
            auto ds = trace.create_datastream();
            fc::raw::unpack(ds, entry);
            std::visit([&builder](const auto& bt) { builder.add_block(bt); }, entry);
            processed_any_block = true;
         }
      } catch (const std::exception& e) {
         parsed_clean = false;
         fc_wlog(_log, "trace_api: receiver bloom build for slice {} aborted; data log unparseable near offset {}: {}",
                 slice_number, trace.tellp(), e.what());
      } catch (...) {
         parsed_clean = false;
         fc_wlog(_log, "trace_api: receiver bloom build for slice {} aborted; data log unparseable near offset {}",
                 slice_number, trace.tellp());
      }

      if (!parsed_clean || !processed_any_block) {
         // Unparseable input - either no records decoded at all, or the scan died partway (e.g. a torn record left
         // mid-file by a crash, with post-restart re-applied blocks appended after it).  A bloom built from a
         // PARTIAL scan would return authoritative negative probes for receivers that only appear after the torn
         // record, silently dropping their actions from get_actions responses.  Never write a sidecar from a
         // partial or empty scan - the query path treats a missing sidecar as "scan this slice", which is the
         // correct behavior for unreadable input.
         return;
      }

      try {
         builder.finalize_and_write(bloom_path);
      } FC_LOG_AND_DROP();

      log(std::string("Built receiver bloom for slice: ") + std::to_string(slice_number) +
          " (" + std::to_string(builder.receiver_count()) + " receivers, " +
          std::to_string(builder.recv_action_count()) + " (receiver, action) pairs)");
   }

   void slice_directory::set_lib(uint32_t lib) {
      {
         std::scoped_lock lock(_maintenance_mtx);
         _best_known_lib = lib;
      }
      _maintenance_condition.notify_one();
   }

   uint32_t slice_directory::best_known_lib() const {
      std::scoped_lock lock(_maintenance_mtx);
      return _best_known_lib;
   }

   bool slice_directory::open_or_create_blk_offset_slice(uint32_t slice_number, fc::cfile& blk_idx) const {
      const bool found = find_slice(_trace_blk_idx_prefix, slice_number, blk_idx, /*open_file=*/false);
      if (found) {
         // Existing file: open for random-access read/write ("rb+").
         blk_idx.open(fc::cfile::update_rw_mode);
         blk_offset_index_header header{};
         try {
            blk_idx.seek(0);
            blk_idx.read(reinterpret_cast<char*>(&header), sizeof(header));
         } catch (...) {
            blk_idx.close();
            return false;
         }
         if (header.magic != blk_offset_index_header::magic_value ||
             header.version != blk_offset_index_header::current_version ||
             header.width != _width) {
            blk_idx.close();
            return false;
         }
         return true;
      }

      // New file: create with truncate+read/write ("wb+") so subsequent seek()+write()
      // behave as random-access (append mode "ab+" would ignore the seek on writes).
      blk_idx.open(fc::cfile::truncate_rw_mode);
      blk_offset_index_header header{};
      header.width = _width;
      blk_idx.seek(0);
      blk_idx.write(reinterpret_cast<const char*>(&header), sizeof(header));
      // Pre-allocate by writing the final byte.  Linux fills the gap sparsely.
      const uint64_t final_byte = sizeof(blk_offset_index_header) + uint64_t{_width} * sizeof(uint64_t) - 1;
      const char zero = 0;
      blk_idx.seek(final_byte);
      blk_idx.write(&zero, 1);
      blk_idx.flush();
      return true;
   }

   void slice_directory::write_block_offset(uint32_t block_height, uint64_t trace_offset) const {
      // The sidecar is advisory: lookup_block_offset misses fall back to the metadata-log scan, so a filesystem
      // error here must degrade to that fallback rather than propagate.  An exception escaping this method would
      // reach the extraction except_handler, which shuts the node down - a response reserved for failures on the
      // trace/index files themselves, where a gap is unrecoverable.
      const uint32_t slice_number = this->slice_number(block_height);
      try {
         fc::cfile blk_idx;
         if (!open_or_create_blk_offset_slice(slice_number, blk_idx)) {
            // Existing sidecar is unusable (wrong magic/version/width).  Remove and recreate;
            // readers fall back to the metadata-log scan in the meantime.
            std::error_code ec;
            std::filesystem::remove(blk_idx.get_file_path(), ec);
            if (ec || !open_or_create_blk_offset_slice(slice_number, blk_idx)) {
               return;
            }
         }
         const uint32_t slot = block_height % _width;
         const uint64_t slot_pos = sizeof(blk_offset_index_header) + uint64_t{slot} * sizeof(uint64_t);
         const uint64_t encoded = trace_offset + 1; // 0 reserved as "empty"
         blk_idx.seek(slot_pos);
         blk_idx.write(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
         blk_idx.flush();
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: failed to record block-offset sidecar entry for block {} (slice {}): {}; "
                       "get_block falls back to the metadata-log scan for this block",
                 block_height, slice_number, e.what());
      } catch (...) {
         fc_wlog(_log, "trace_api: failed to record block-offset sidecar entry for block {} (slice {}); "
                       "get_block falls back to the metadata-log scan for this block",
                 block_height, slice_number);
      }
   }

   std::optional<uint64_t> slice_directory::lookup_block_offset(uint32_t block_height) const {
      const uint32_t slice_number = this->slice_number(block_height);
      fc::cfile blk_idx;
      const bool found = find_slice(_trace_blk_idx_prefix, slice_number, blk_idx, /*open_file=*/false);
      if (!found) return std::nullopt;

      try {
         blk_idx.open(fc::cfile::read_only_mode);
         blk_offset_index_header header{};
         blk_idx.seek(0);
         blk_idx.read(reinterpret_cast<char*>(&header), sizeof(header));
         if (header.magic != blk_offset_index_header::magic_value ||
             header.version != blk_offset_index_header::current_version ||
             header.width != _width) {
            return std::nullopt;
         }
         const uint32_t slot = block_height % _width;
         const uint64_t slot_pos = sizeof(blk_offset_index_header) + uint64_t{slot} * sizeof(uint64_t);
         uint64_t encoded = 0;
         blk_idx.seek(slot_pos);
         blk_idx.read(reinterpret_cast<char*>(&encoded), sizeof(encoded));
         if (encoded == 0) return std::nullopt;
         return encoded - 1;
      } catch (...) {
         return std::nullopt;
      }
   }

   void slice_directory::start_maintenance_thread(log_handler log) {
      _maintenance_thread = std::thread([this, log=std::move(log)](){
         fc::set_thread_name( "trace-mx" );
         uint32_t last_lib = 0;

         while(true) {
            std::unique_lock<std::mutex> lock(_maintenance_mtx);
            while ( last_lib >= _best_known_lib && !_maintenance_shutdown ) {
               _maintenance_condition.wait(lock);
            }

            if (_maintenance_shutdown) {
               break;
            }

            uint32_t best_known_lib = _best_known_lib;
            lock.unlock();

            log(std::string("Waking up to handle lib: ") + std::to_string(best_known_lib));

            if (last_lib < best_known_lib) {
               try {
                  run_maintenance_tasks(best_known_lib, log);
                  last_lib = best_known_lib;
               } FC_LOG_AND_DROP();
            }
         }
      });
   }

   void slice_directory::stop_maintenance_thread() {
      {
         std::scoped_lock lock(_maintenance_mtx);
         _maintenance_shutdown = true;
      }
      _maintenance_condition.notify_one();
      _maintenance_thread.join();
   }

   template<typename F>
   void slice_directory::process_irreversible_slice_range(uint32_t lib, uint32_t min_irreversible, std::optional<uint32_t>& lower_bound_slice, F&& f) {
      const uint32_t lib_slice_number = slice_number( lib );
      if (lib_slice_number < 1 || (lower_bound_slice && *lower_bound_slice >= lib_slice_number - 1))
         return;

      const int64_t upper_bound_block_number = static_cast<int64_t>(lib) - static_cast<int64_t>(min_irreversible) - _width;
      if (upper_bound_block_number >= 0) {
         uint32_t upper_bound_slice_num = slice_number(static_cast<uint32_t>(upper_bound_block_number));
         while (!lower_bound_slice || *lower_bound_slice < upper_bound_slice_num) {
            const uint32_t slice_to_process = lower_bound_slice ? *lower_bound_slice + 1 : 0;
            f(slice_to_process);
            lower_bound_slice = slice_to_process;
         }
      }
   }

   void slice_directory::run_maintenance_tasks(uint32_t lib, const log_handler& log) {
      // Retention pruning runs FIRST so slices already past the retention window are deleted before the
      // index/bloom passes below spend I/O building sidecars for them.  On a node catching up after long
      // downtime (or with retention much smaller than existing history) that range can be thousands of
      // slices - scanning, hashing, and writing sidecars for data that the same wakeup then deletes.
      if (_minimum_irreversible_history_blocks) {
         process_irreversible_slice_range(lib, *_minimum_irreversible_history_blocks, _last_cleaned_up_slice, [this, &log](uint32_t slice_to_clean){
            fc::cfile trace;
            fc::cfile index;
            fc::cfile trx_id;

            log(std::string("Attempting Prune of slice: ") + std::to_string(slice_to_clean));

            // cleanup index first to reduce the likelihood of reader finding index, but not finding trace
            const bool dont_open_file = false;
            const bool index_found = find_index_slice(slice_to_clean, open_state::read, index, dont_open_file);
            if (index_found) {
               log(std::string("Removing: ") + index.get_file_path().generic_string());
               std::filesystem::remove(index.get_file_path());
            }
            const bool trace_found = find_trace_slice(slice_to_clean, open_state::read, trace, dont_open_file);
            if (trace_found) {
               log(std::string("Removing: ") + trace.get_file_path().generic_string());
               std::filesystem::remove(trace.get_file_path());
            }
            const bool trx_id_found = find_trx_id_slice(slice_to_clean, open_state::read, trx_id, dont_open_file);
            if (trx_id_found) {
               log(std::string("Removing: ") + trx_id.get_file_path().generic_string());
               std::filesystem::remove(trx_id.get_file_path());
            }
            auto idx_filename = make_filename(_trace_trx_id_index_prefix, _trace_ext, slice_to_clean, _width);
            const auto idx_path = _slice_dir / idx_filename;
            if (std::filesystem::exists(idx_path)) {
               log(std::string("Removing: ") + idx_path.generic_string());
               std::filesystem::remove(idx_path);
            }

            auto blk_idx_filename = make_filename(_trace_blk_idx_prefix, _trace_ext, slice_to_clean, _width);
            const auto blk_idx_path = _slice_dir / blk_idx_filename;
            if (std::filesystem::exists(blk_idx_path)) {
               log(std::string("Removing: ") + blk_idx_path.generic_string());
               std::filesystem::remove(blk_idx_path);
            }

            const auto bloom_path = bloom_slice_path(slice_to_clean);
            if (std::filesystem::exists(bloom_path)) {
               log(std::string("Removing: ") + bloom_path.generic_string());
               std::filesystem::remove(bloom_path);
            }

            auto ctrace = find_compressed_trace_slice(slice_to_clean, dont_open_file);
            if (ctrace) {
               log(std::string("Removing: ") + ctrace->get_file_path().generic_string());
               std::filesystem::remove(ctrace->get_file_path());
            }
         });
      }

      // Advance the index/bloom watermarks past anything pruning just deleted (or had already deleted on a
      // prior run) so the build passes below skip the pruned range outright instead of probing each slice.
      const auto skip_pruned_range = [this](std::optional<uint32_t>& watermark) {
         if (_last_cleaned_up_slice && (!watermark || *watermark < *_last_cleaned_up_slice))
            watermark = _last_cleaned_up_slice;
      };
      skip_pruned_range(_last_indexed_slice);
      skip_pruned_range(_last_bloomed_slice);

      // Build trx_id indexes for all newly irreversible slices (min_irreversible=0:
      // index as soon as a slice's block range is fully below LIB).
      process_irreversible_slice_range(lib, 0, _last_indexed_slice, [this, &log](uint32_t slice_to_index){
         try {
            build_trx_id_index(slice_to_index, log);
         } FC_LOG_AND_DROP();
      });

      // Build receiver bloom sidecars on the same schedule as trx_id indexes - any slice fully past LIB has its data
      // final, so forks can't corrupt the sidecar after it's written.  Ordering before compression keeps the source
      // .log available for the stream-scan.
      process_irreversible_slice_range(lib, 0, _last_bloomed_slice, [this, &log](uint32_t slice_to_bloom){
         try {
            build_recv_bloom(slice_to_bloom, log);
         } FC_LOG_AND_DROP();
      });

      // Only process compression if its configured AND there is a range of irreversible blocks which would not also
      // be deleted
      if (_minimum_uncompressed_irreversible_history_blocks &&
          (!_minimum_irreversible_history_blocks || *_minimum_uncompressed_irreversible_history_blocks < *_minimum_irreversible_history_blocks) )
      {
         process_irreversible_slice_range(lib, *_minimum_uncompressed_irreversible_history_blocks, _last_compressed_slice, [this, &log](uint32_t slice_to_compress){
            fc::cfile trace;
            const bool dont_open_file = false;
            const bool trace_found = find_trace_slice(slice_to_compress, open_state::read, trace, dont_open_file);

            log(std::string("Attempting compression of slice: ") + std::to_string(slice_to_compress));

            if (trace_found) {
               auto compressed_path = trace.get_file_path();
               compressed_path.replace_extension(_compressed_trace_ext);

               log(std::string("Compressing: ") + trace.get_file_path().generic_string());
               compressed_file::process(trace.get_file_path(), compressed_path.generic_string(), _compression_seek_point_stride);

               // after compression is complete, delete the old uncompressed file
               log(std::string("Removing: ") + trace.get_file_path().generic_string());
               std::filesystem::remove(trace.get_file_path());
            }
         });
      }
   }
}

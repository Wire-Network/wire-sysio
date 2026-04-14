#include <sysio/trace_api/store_provider.hpp>

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
      static constexpr const char* _trace_ext = ".log";
      static constexpr const char* _compressed_trace_ext = ".clog";
      // longest prefix is "trace_trx_idx_" or "trace_blk_idx_" (14), then 10+1+10 digits, then ".clog" extension, then null
      static constexpr int _max_filename_size = std::char_traits<char>::length(_trace_trx_id_index_prefix) + 10 + 1 + 10 + std::char_traits<char>::length(_compressed_trace_ext) + 1;

      std::string make_filename(const char* slice_prefix, const char* slice_ext, uint32_t slice_number, uint32_t slice_width) {
         char filename[_max_filename_size] = {};
         const uint32_t slice_start = slice_number * slice_width;
         const int size_written = snprintf(filename, _max_filename_size, "%s%010d-%010d%s", slice_prefix, slice_start, (slice_start + slice_width), slice_ext);
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
   , _abi_store_path(slice_dir / "abi_store.log") {
      if (std::filesystem::exists(_abi_store_path)) {
         _abi_writer.load(_abi_store_path);
         _abi_reader.store(std::make_shared<abi_store_reader>(_abi_store_path));
      }
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
      // overwrite the slot; if this step throws the metadata log remains the source of
      // truth and get_block falls back to the linear scan.
      _slice_directory.write_block_offset(bt.number, offset);
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
      _slice_directory.set_lib(lib);
   }

   void store_provider::append_trx_ids(block_trxs_entry tt){
      fc::cfile trx_id_file;
      const uint32_t slice_number = _slice_directory.slice_number(tt.block_num);
      _slice_directory.find_or_create_trx_id_slice(slice_number, open_state::write, trx_id_file);
      auto entry = metadata_log_entry { std::move(tt) };
      append_store(entry, trx_id_file);
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
         const uint32_t slice_number = _slice_directory.slice_number_from_path(trx_id_file.get_file_path());
         if (auto reader = _slice_directory.find_trx_id_index_slice(slice_number)) {
            if (auto block_num = reader->lookup(trx_id)) {
               trx_block_nums.insert(*block_num);
               return false; // found in an irreversible slice; stop
            }
            return true; // not in this indexed slice; continue to next
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
               if (!found_in_block)
                  trx_block_nums.erase(trxs_entry.block_num);
            } else if (std::holds_alternative<lib_entry_v0>(entry)) {
               auto lib = std::get<lib_entry_v0>(entry).lib;
               if (!trx_block_nums.empty() && lib >= *(--trx_block_nums.end())) {
                  return false;
               }
            } else {
               FC_ASSERT(false, "unpacked data should be a block_trxs_entry or a lib_entry_v0");
            }
            offset = trx_id_file.tellp();
         }
         return trx_block_nums.empty();
      });

      if (!trx_block_nums.empty())
         return *(--trx_block_nums.end());

      return {};
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
         elog("Trace file is {}, but it's metadata file is {}. This means the files are not consistent.", trace_status, index_status);
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
            if (entry.path().filename().string().find(_trace_trx_id_prefix) != std::string::npos) {
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
         slice_file.open("rb");
         slice_file.seek(0);
         if (!callback(slice_file))
            return;
         slice_file.close();
      }
   }

   namespace {
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
         if (ascending)
            std::sort(paths.begin(), paths.end());
         else
            std::sort(paths.begin(), paths.end(), std::greater<>());
         return paths;
      }

      // Scan a single index slice file and return the min and max block numbers found.
      std::optional<std::pair<uint32_t,uint32_t>> scan_index_slice(const std::filesystem::path& path, uint32_t current_version) {
         try {
            fc::cfile index;
            index.set_file_path(path);
            index.open("rb");
            index.seek(0);
            const auto header = extract_store<slice_directory::index_header>(index);
            if (header.version != current_version)
               return std::nullopt;
            const uint64_t end = file_size(path);
            std::optional<uint32_t> lo, hi;
            while (index.tellp() < end) {
               const auto e = extract_store<metadata_log_entry>(index);
               if (std::holds_alternative<block_entry_v0>(e)) {
                  const auto num = std::get<block_entry_v0>(e).number;
                  if (!lo || num < *lo) lo = num;
                  if (!hi || num > *hi) hi = num;
               }
            }
            if (lo && hi)
               return std::make_pair(*lo, *hi);
         } catch (...) {
            // malformed or partially written slice
         }
         return std::nullopt;
      }
   } // anonymous namespace

   std::optional<uint32_t> slice_directory::first_recorded_block() const {
      for (const auto& path : collect_index_paths(_slice_dir, /*ascending=*/true)) {
         if (const auto r = scan_index_slice(path, _current_version))
            return r->first;
      }
      return std::nullopt;
   }

   std::optional<uint32_t> slice_directory::last_recorded_block() const {
      for (const auto& path : collect_index_paths(_slice_dir, /*ascending=*/false)) {
         if (const auto r = scan_index_slice(path, _current_version))
            return r->second;
      }
      return std::nullopt;
   }

   std::optional<uint32_t> store_provider::first_recorded_block() const {
      return _slice_directory.first_recorded_block();
   }

   std::optional<uint32_t> store_provider::last_recorded_block() const {
      return _slice_directory.last_recorded_block();
   }

   void store_provider::append_abi(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
      std::lock_guard lock(_abi_write_mutex);
      _abi_writer.add(account, global_seq, std::move(abi_bytes));
      _abi_writer.write(_abi_store_path);
      _abi_reader.store(std::make_shared<abi_store_reader>(_abi_store_path));
   }

   std::optional<std::vector<char>> store_provider::lookup_abi(chain::name account, uint64_t global_seq) const {
      auto reader = _abi_reader.load();
      if (!reader) return std::nullopt;
      return reader->lookup(account, global_seq);
   }

   uint32_t slice_directory::slice_number_from_path(const std::filesystem::path& trx_id_path) const {
      // Filename format: trace_trx_id_XXXXXXXXXX-YYYYYYYYYY.log
      // Parse the start block number (XXXXXXXXXX) and divide by _width.
      const auto name = trx_id_path.filename().string();
      const auto prefix_len = std::char_traits<char>::length(_trace_trx_id_prefix);
      const uint32_t start_block = static_cast<uint32_t>(std::stoul(name.substr(prefix_len, 10)));
      return start_block / _width;
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

      trx_id_index_writer writer;
      const uint64_t end = file_size(trx_id_file.get_file_path());
      while (trx_id_file.tellp() < end) {
         metadata_log_entry entry;
         auto ds = trx_id_file.create_datastream();
         fc::raw::unpack(ds, entry);
         if (std::holds_alternative<block_trxs_entry>(entry)) {
            const auto& te = std::get<block_trxs_entry>(entry);
            for (const auto& id : te.ids)
               writer.add(id, te.block_num);
         }
      }

      // Write to a temp path and atomically rename so concurrent readers never
      // see a partially written index file.
      const auto tmp_path = idx_path.parent_path() / (idx_path.filename().string() + ".tmp");
      writer.write(tmp_path);
      std::filesystem::rename(tmp_path, idx_path);
      log(std::string("Built trx_id index for slice: ") + std::to_string(slice_number) +
          " (" + std::to_string(writer.entry_count()) + " entries)");
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
      const uint32_t slice_number = this->slice_number(block_height);
      fc::cfile blk_idx;
      if (!open_or_create_blk_offset_slice(slice_number, blk_idx)) {
         // Existing sidecar is unusable (wrong magic/version/width).  Remove and recreate;
         // readers fall back to the metadata-log scan in the meantime.
         std::filesystem::remove(blk_idx.get_file_path());
         if (!open_or_create_blk_offset_slice(slice_number, blk_idx)) {
            return;
         }
      }
      const uint32_t slot = block_height % _width;
      const uint64_t slot_pos = sizeof(blk_offset_index_header) + uint64_t{slot} * sizeof(uint64_t);
      const uint64_t encoded = trace_offset + 1; // 0 reserved as "empty"
      blk_idx.seek(slot_pos);
      blk_idx.write(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
      blk_idx.flush();
   }

   std::optional<uint64_t> slice_directory::lookup_block_offset(uint32_t block_height) const {
      const uint32_t slice_number = this->slice_number(block_height);
      fc::cfile blk_idx;
      const bool found = find_slice(_trace_blk_idx_prefix, slice_number, blk_idx, /*open_file=*/false);
      if (!found) return std::nullopt;

      try {
         blk_idx.open(fc::cfile::update_rw_mode);
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
      // Build trx_id indexes for all newly irreversible slices (min_irreversible=0:
      // index as soon as a slice's block range is fully below LIB).
      process_irreversible_slice_range(lib, 0, _last_indexed_slice, [this, &log](uint32_t slice_to_index){
         try {
            build_trx_id_index(slice_to_index, log);
         } FC_LOG_AND_DROP();
      });

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

            auto ctrace = find_compressed_trace_slice(slice_to_clean, dont_open_file);
            if (ctrace) {
               log(std::string("Removing: ") + ctrace->get_file_path().generic_string());
               std::filesystem::remove(ctrace->get_file_path());
            }
         });
      }

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

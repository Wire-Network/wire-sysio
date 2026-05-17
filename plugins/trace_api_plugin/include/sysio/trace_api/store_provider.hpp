#pragma once

#include <ios>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fc/io/cfile.hpp>
#include <fc/variant.hpp>
#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/bloom_sidecar.hpp>
#include <sysio/trace_api/common.hpp>
#include <sysio/trace_api/compressed_file.hpp>
#include <sysio/trace_api/data_log.hpp>
#include <sysio/trace_api/metadata_log.hpp>
#include <sysio/trace_api/trx_id_index.hpp>

namespace sysio::trace_api {

   class path_does_not_exist : public std::runtime_error {
   public:
      explicit path_does_not_exist(const char* what_arg)
         :std::runtime_error(what_arg)
      {}
      explicit path_does_not_exist(const std::string& what_arg)
         :std::runtime_error(what_arg)
      {}
   };

   class old_slice_version : public std::runtime_error {
   public:
      explicit old_slice_version(const char* what_arg)
         :std::runtime_error(what_arg)
      {}
      explicit old_slice_version(const std::string& what_arg)
         :std::runtime_error(what_arg)
      {}
   };

   class incompatible_slice_files : public std::runtime_error {
   public:
      explicit incompatible_slice_files(const char* what_arg)
         :std::runtime_error(what_arg)
      {}
      explicit incompatible_slice_files(const std::string& what_arg)
         :std::runtime_error(what_arg)
      {}
   };

   class malformed_slice_file : public std::runtime_error {
   public:
      explicit malformed_slice_file(const char* what_arg)
         :std::runtime_error(what_arg)
      {}
      explicit malformed_slice_file(const std::string& what_arg)
         :std::runtime_error(what_arg)
      {}
   };

   /**
    * append an entry to the store
    *
    * @param entry : the entry to append
    * @param file : the file to append entry to
    * @return the offset in the file where that entry is written
    */
   template<typename DataEntry, typename File>
   static uint64_t append_store(const DataEntry &entry, File &file) {
      auto data = fc::raw::pack(entry);
      const auto offset = file.tellp();
      file.write(data.data(), data.size());
      file.flush();
      return offset;
   }

   /**
    * extract an entry from the data log
    *
    * @param file : the file to extract entry from
    * @return the extracted data log
    */
   template<typename DataEntry, typename File>
   static DataEntry extract_store( File& file ) {
      DataEntry entry;
      auto ds = file.create_datastream();
      fc::raw::unpack(ds, entry);
      return entry;
   }


   class store_provider;

   // On-disk format: trace_blk_idx_<range>.log
   //
   // Layout: blk_offset_index_header (16 bytes) followed by a flat array of
   // width uint64_t slots.  Slot (block_num - slice_base) holds offset+1,
   // where offset is the position in trace_<range>.log of that block's trace
   // data.  Slot value 0 means "not present"; this distinguishes a missing
   // block from a block stored at offset 0 (the first block in a slice).
   //
   // The file is pre-allocated sparse at creation, so any slot write is a
   // single 8-byte in-place update.  Forks naturally overwrite the slot.
   //
   // Native-endian, x86_64 Linux only (same convention as other slice files).
   struct blk_offset_index_header {
      // Stored little-endian on disk so a hex dump of the first 4 bytes reads "BLIX".
      static constexpr uint32_t magic_value     = 0x58494C42; // bytes on disk: 'B','L','I','X'
      static constexpr uint32_t current_version = 1;

      uint32_t magic    = magic_value;
      uint32_t version  = current_version;
      uint32_t width    = 0; // slice width (block count per slice)
      uint32_t reserved = 0;
   };
   static_assert(sizeof(blk_offset_index_header) == 16);

   /**
    * Provides access to the slice directory.  It is only intended to be used by store_provider
    * and unit tests.
    */
   class slice_directory {
   public:
      struct index_header {
         uint32_t version = 0;
      };

      enum class open_state { read /*read from front to back*/, write /*write to end of file*/ };
      slice_directory(const std::filesystem::path& slice_dir, uint32_t width, std::optional<uint32_t> minimum_irreversible_history_blocks,
                      std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks, size_t compression_seek_point_stride);

      /**
       * Return the slice number that would include the passed in block_height
       *
       * @param block_height : height of the requested data
       * @return the slice number for the block_height
       */
      uint32_t slice_number(uint32_t block_height) const {
         return block_height / _width;
      }

      /**
       * Slice stride (blocks per slice) as configured at construction.
       */
      uint32_t width() const noexcept { return _width; }

      /**
       * Filesystem path for a slice's receiver bloom sidecar.  The file is only read/written via the bloom_builder
       * and bloom_reader helpers; no fc::cfile overload is provided because the sidecar is written once at slice
       * close (temp + rename) and only mmap-style read by the query path.
       */
      std::filesystem::path bloom_slice_path(uint32_t slice_number) const;

      /**
       * Find or create the index file associated with the indicated slice_number
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param index_file : the cfile that will be set to the appropriate slice filename
       *                     and opened to that file
       * @return the true if file was found (i.e. already existed)
       */
      bool find_or_create_index_slice(uint32_t slice_number, open_state state, fc::cfile& index_file) const;

      /**
       * Find the index file associated with the indicated slice_number
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param index_file : the cfile that will be set to the appropriate slice filename (always)
       *                     and opened to that file (if it was found)
       * @param open_file : indicate if the file should be opened (if found) or not
       * @return the true if file was found (i.e. already existed), if not found index_file
       *         is set to the appropriate file, but not open
       */
      bool find_index_slice(uint32_t slice_number, open_state state, fc::cfile& index_file, bool open_file = true) const;

      /**
       * Find or create the trace file associated with the indicated slice_number
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param trace_file : the cfile that will be set to the appropriate slice filename
       *                     and opened to that file
       * @return the true if file was found (i.e. already existed)
       */
      bool find_or_create_trace_slice(uint32_t slice_number, open_state state, fc::cfile& trace_file) const;

      /**
       * Find the trace file associated with the indicated slice_number
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param trace_file : the cfile that will be set to the appropriate slice filename (always)
       *                     and opened to that file (if it was found)
       * @param open_file : indicate if the file should be opened (if found) or not
       * @return the true if file was found (i.e. already existed), if not found index_file
       *         is set to the appropriate file, but not open
       */
      bool find_trace_slice(uint32_t slice_number, open_state state, fc::cfile& trace_file, bool open_file = true) const;

      /**
       * Find the read-only compressed trace file associated with the indicated slice_number
       *
       * @param slice_number : slice number of the requested slice file
       * @param open_file : indicate if the file should be opened (if found) or not
       * @return if file was found (i.e. already existed) returns an optional containing a compressed_file which is
       *         open (or not) depending on the `open_file` paraneter,
       *         Otherwise, the returned optional is empty
       */
      std::optional<compressed_file> find_compressed_trace_slice(uint32_t slice_number, bool open_file = true) const;

      /**
       * Find or create a trace and index file pair
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param trace : the cfile that will be set to the appropriate slice filename and
       *                opened to that file
       */
      void find_or_create_slice_pair(uint32_t slice_number, open_state state, fc::cfile& trace, fc::cfile& index);

      /**
       * Find or create a trx id file that contains all the transaction ids and associated block numbers
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param trx_id_file : the cfile
       * @return true if file was found (i.e. already existed)
       */
      bool find_or_create_trx_id_slice(uint32_t slice_number, open_state state, fc::cfile& trx_id_file) const;

      /**
       * Find the trx id file
       *
       * @param slice_number : slice number of the requested slice file
       * @param state : indicate if the file is going to be written to (appended) or read
       * @param trx_id_file : the cfile
       * @param open_file : indicate if the file should be opened (if found) or not
       * @return true if file was found (i.e. already existed), if not found trx_id_file
       *         is set to the appropriate file, but not opened
       */
      bool find_trx_id_slice(uint32_t slice_number, open_state state, fc::cfile& trx_id_file, bool open_file = true) const;

      /**
       * Traverses the trx id slice files in reverse order of block num, latest block num stride first
       *
       * Call callback with each already opened at beginning trx id slice file
       * @param callback return false to stop iteration
       */
      void for_each_trx_id_slice(std::function<bool(fc::cfile&)> callback) const;

      /**
       * Derive the slice number from a trx_id slice file path.
       * Parses the block-range start from the filename.  Returns nullopt if
       * the filename does not parse (callers should fall back to a slower
       * lookup path rather than skipping the file silently).
       */
      std::optional<uint32_t> slice_number_from_path(const std::filesystem::path& trx_id_path) const;

      /**
       * Find the trx_id index file for a given slice number (or return nullopt if not present).
       */
      std::optional<trx_id_index_reader> find_trx_id_index_slice(uint32_t slice_number) const;

      /**
       * Build the trx_id index for a given slice from its trx_id log file.
       * No-op if the index already exists for that slice.
       */
      void build_trx_id_index(uint32_t slice_number, const log_handler& log);

      /**
       * Build the per-slice receiver bloom sidecar from the slice's trace data log.  Called on slices that are fully
       * past LIB so the source data is final (no fork can reach back into an already-built sidecar).  No-op if the
       * sidecar already exists or the slice has no uncompressed trace data.
       */
      void build_recv_bloom(uint32_t slice_number, const log_handler& log);

      /**
       * Return {first, last} block numbers recorded across all index slice files, or nullopt
       * if no data exists.  Used at startup to detect gaps between existing trace data and the
       * current chain head.  Atomic in the sense that both values come from a single directory
       * scan, so callers don't need to guard against seeing `first` but not `last`.
       */
      std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const;

      /**
       * Record the offset of a block's trace data in trace_<range>.log, via the block-offset
       * sidecar trace_blk_idx_<range>.log.  Creates the sidecar on first write to a new slice.
       * Writes to an existing slot naturally overwrite it (fork re-writes).
       */
      void write_block_offset(uint32_t block_height, uint64_t trace_offset) const;

      /**
       * O(1) lookup of the trace-log offset for a block via the block-offset sidecar.
       * Returns nullopt when the sidecar is missing, the slot is empty, or the file is invalid.
       * Callers should fall back to scanning the metadata log in that case.
       */
      std::optional<uint64_t> lookup_block_offset(uint32_t block_height) const;

      /**
       * Current best-known LIB as reported by append_lib.  Thread-safe; used by readers to
       * determine whether a given block is irreversible without scanning the metadata log.
       */
      uint32_t best_known_lib() const;

      /**
       * set the LIB for maintenance
       * @param lib
       */
      void set_lib(uint32_t lib);

      /**
       * Start a thread which does background maintenance
       */
      void start_maintenance_thread( log_handler log );

      /**
       * Stop and join the thread doing background maintenance
       */
      void stop_maintenance_thread();

      /**
       * Cleans up all slices that are no longer needed to maintain the minimum number of blocks past lib
       * Compresses up all slices that can be compressed
       *
       * @param lib : block number of the current lib
       */
      void run_maintenance_tasks(uint32_t lib, const log_handler& log);

   private:
      // returns true if slice is found, slice_file will always be set to the appropriate path for
      // the slice_prefix and slice_number, but will only be opened if found
      bool find_slice(const char* slice_prefix, uint32_t slice_number, fc::cfile& slice_file, bool open_file) const;

      // take an index file that is initialized to a file and open it and write its header
      void create_new_index_slice_file(fc::cfile& index_file) const;

      // take an open index slice file and verify its header is valid and prepare the file to be appended to (or read from)
      void validate_existing_index_slice_file(fc::cfile& index_file, open_state state) const;

      // Open the block-offset sidecar for a slice; creates and pre-allocates if missing.
      // Validates the header on open.  Returns false + leaves blk_idx at the sidecar path
      // (unopened) if the existing file has a wrong magic/version/width.
      bool open_or_create_blk_offset_slice(uint32_t slice_number, fc::cfile& blk_idx) const;

      // helper for methods that process irreversible slice files
      template<typename F>
      void process_irreversible_slice_range(uint32_t lib, uint32_t upper_bound_block, std::optional<uint32_t>& lower_bound_slice, F&& f);

      const std::filesystem::path _slice_dir;
      const uint32_t _width;
      const std::optional<uint32_t> _minimum_irreversible_history_blocks;
      std::optional<uint32_t> _last_cleaned_up_slice;
      const std::optional<uint32_t> _minimum_uncompressed_irreversible_history_blocks;
      std::optional<uint32_t> _last_compressed_slice;
      std::optional<uint32_t> _last_indexed_slice;
      std::optional<uint32_t> _last_bloomed_slice;
      const size_t _compression_seek_point_stride;

      mutable std::mutex _maintenance_mtx;
      std::condition_variable _maintenance_condition;
      std::thread _maintenance_thread;
      bool _maintenance_shutdown{false};
      uint32_t _best_known_lib{0};
   };

   /**
    * Provides read and write access to block trace data.
    */
   class store_provider {
   public:
      using open_state = slice_directory::open_state;

      store_provider(const std::filesystem::path& slice_dir, uint32_t stride_width, std::optional<uint32_t> minimum_irreversible_history_blocks,
            std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks, size_t compression_seek_point_stride);

      template<typename BlockTrace>
      void append(const BlockTrace& bt);
      void append_lib(uint32_t lib);
      void append_trx_ids(block_trxs_entry tt);

      /**
       * Slice stride used for all sidecars.  Exposed on the provider so callers (e.g. request_handler's block-range
       * scan) can partition queries by slice without having to reach into slice_directory.
       */
      uint32_t slice_stride() const noexcept { return _slice_directory.width(); }

      /**
       * Slice number containing the given block.
       */
      uint32_t slice_number(uint32_t block_height) const noexcept { return _slice_directory.slice_number(block_height); }

      /**
       * Open the per-slice bloom sidecar for a given slice number.  Returns a bloom_reader whose valid() is false
       * when the sidecar is missing, truncated, wrong-version, or CRC-corrupt - in which case the caller MUST fall
       * back to a full scan of the slice (an invalid reader returns true from may_contain_*, honoring the fail-safe
       * invariant).  A positive probe is not authoritative (standard bloom semantics); only a negative probe on a
       * valid reader permits skipping.
       */
      bloom_reader get_bloom(uint32_t slice_number) const;

      /**
       * Record an ABI version for an account at a given global_sequence.
       * global_seq == 0 means "captured lazily; exact seq unknown".
       * Thread-safe; may be called from the extraction thread.
       */
      void append_abi(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes);

      /**
       * Return the ABI in effect for account at global_seq (the ABI with the
       * largest recorded global_seq <= the query), or nullopt if none is found.
       * The returned pair is {effective_global_seq, abi_bytes} where
       * effective_global_seq is the recorded setabi's global_seq (0 for the
       * lazy-capture sentinel).  Decoders use effective_global_seq as a stable
       * cache key so actions that share an ABI version all hit the same entry.
       * Thread-safe; may be called from the HTTP thread.
       */
      std::optional<abi_log::lookup_result> lookup_abi(chain::name account, uint64_t global_seq) const;

      /**
       * Return true if any ABI record exists for the account.  Used by extraction
       * to decide whether to trigger a lazy ABI fetch on first encounter.
       * Thread-safe.
       */
      bool has_abi_entry(chain::name account) const;

      /**
       * Read the trace for a given block
       * @param block_height : the height of the data being read
       * @return empty optional if the data cannot be read OTHERWISE
       *         optional containing a 2-tuple of the block_trace and a flag indicating irreversibility
       */
      get_block_t get_block(uint32_t block_height, const yield_function& yield= {});

      get_block_n get_trx_block_number(const chain::transaction_id_type& trx_id, const yield_function& yield= {});

      /**
       * Return {first, last} block numbers recorded across all index slice files, or nullopt
       * if the slice directory is empty.  Used at startup to verify continuity between existing
       * trace data and the current chain head.
       */
      std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const;

      void start_maintenance_thread( log_handler log ) {
         _slice_directory.start_maintenance_thread( std::move(log) );
      }
      void stop_maintenance_thread() {
         _slice_directory.stop_maintenance_thread();
      }

      protected:
      /**
       * Read the metadata log font-to-back starting at an offset passing each entry to a provided functor/lambda
       *
       * @tparam Fn : type of the functor/lambda
       * @param block_height : height of the requested data
       * @param offset : initial offset to read from
       * @param fn : the functor/lambda
       * @return the highest offset read during this scan
       */
      template<typename Fn>
      uint64_t scan_metadata_log_from( uint32_t block_height, uint64_t offset, Fn&& fn, const yield_function& yield ) {
         // ignoring offset
         offset = 0;
         fc::cfile index;
         const uint32_t slice_number = _slice_directory.slice_number(block_height);
         const bool found = _slice_directory.find_index_slice(slice_number, open_state::read, index);
         if( !found ) {
            return 0;
         }
         const uint64_t end = file_size(index.get_file_path());
         offset = index.tellp();
         uint64_t last_read_offset = offset;
         while (offset < end) {
            yield();
            const auto metadata = extract_store<metadata_log_entry>(index);
            if(! fn(metadata)) {
               break;
            }
            last_read_offset = offset;
            offset = index.tellp();
         }
         return last_read_offset;
      }

      /**
       * Read from the data log
       * @param block_height : the block_height of the data being read
       * @param offset : the offset in the datalog to read
       * @return empty optional if the data log does not exist, data otherwise
       * @throws std::exception : when the data is not the correct type or if the log is corrupt in some way
       *
       */
      std::optional<data_log_entry> read_data_log( uint32_t block_height, uint64_t offset ) {
         const uint32_t slice_number = _slice_directory.slice_number(block_height);

         fc::cfile trace;
         if( !_slice_directory.find_trace_slice(slice_number, open_state::read, trace) ) {
            // attempt to read a compressed trace if one exists
            std::optional<compressed_file> ctrace = _slice_directory.find_compressed_trace_slice(slice_number);
            if (ctrace) {
               ctrace->seek(offset);
               return extract_store<data_log_entry>(*ctrace);
            }

            const std::string offset_str = boost::lexical_cast<std::string>(offset);
            const std::string bh_str = boost::lexical_cast<std::string>(block_height);
            throw malformed_slice_file("Requested offset: " + offset_str + " to retrieve block number: " + bh_str + " but this trace file is new, so there are no traces present.");
         }
         const uint64_t end = file_size(trace.get_file_path());
         if( offset >= end ) {
            const std::string offset_str = boost::lexical_cast<std::string>(offset);
            const std::string bh_str = boost::lexical_cast<std::string>(block_height);
            const std::string end_str = boost::lexical_cast<std::string>(end);
            throw malformed_slice_file("Requested offset: " + offset_str + " to retrieve block number: " + bh_str + " but this trace file only goes to offset: " + end_str);
         }
         trace.seek(offset);
         return extract_store<data_log_entry>(trace);
      }

      /**
       * Initialize a new index slice with a valid header
       * @param index : index file to open and add header to
       *
       */
      void initialize_new_index_slice_file(fc::cfile& index);

      /**
       * Ensure an existing index slice has a valid header
       * @param index : index file to open and read header from
       * @param state : indicate if the file is going to be written to (appended) or read
       *
       */
      void validate_existing_index_slice_file(fc::cfile& index, open_state state);

      slice_directory _slice_directory;

   private:
      // ABI sidecar: one global append-only log in the slice directory.
      // abi_log serialises its own writes and allows concurrent lookups.
      abi_log _abi_log;
   };

}

FC_REFLECT(sysio::trace_api::slice_directory::index_header, (version))
FC_REFLECT(sysio::trace_api::blk_offset_index_header, (magic)(version)(width)(reserved))

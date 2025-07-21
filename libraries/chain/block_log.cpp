#include <sysio/chain/block_header_state.hpp>
#include <sysio/chain/block_log.hpp>
#include <sysio/chain/block_log_config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/log_catalog.hpp>
#include <sysio/chain/log_data_base.hpp>
#include <sysio/chain/log_index.hpp>
#include <fc/bitutil.hpp>
#include <fc/io/raw.hpp>
#include <mutex>
#include <string>

#if defined(__BYTE_ORDER__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
#endif

namespace sysio { namespace chain {

   enum versions {
      initial_version = 1,                  ///< complete block log from genesis
      block_x_start_version = 2,            ///< adds optional partial block log, cannot be used for replay without snapshot
                                            ///< this is in the form of an first_block_num that is written immediately after the version
      genesis_state_or_chain_id_version = 3 ///< improvement on version 2 to not require the genesis state be provided when not starting from block 1
   };

   // though the content may be different, the way it is written out will remain identical
   namespace common_protocol {
      constexpr uint32_t min_supported_version = initial_version;
      constexpr uint32_t max_supported_version = genesis_state_or_chain_id_version;
      uint32_t default_initial_version = max_supported_version;
   }

   template<typename StoredType>
   constexpr uint32_t block_log<StoredType>::min_supported_version = common_protocol::min_supported_version;
   template<typename StoredType>
   constexpr uint32_t block_log<StoredType>::max_supported_version = common_protocol::max_supported_version;

   template<typename StoredType>
   inline std::string filename_prefix();
   template<>
   inline std::string filename_prefix<signed_block>() { return std::string("blocks"); }
   template<>
   inline std::string filename_prefix<block_header_state>() { return std::string("block_state"); }

   template<typename StoredType>
   inline std::string log_filename();
   template<>
   inline std::string log_filename<signed_block>() { return std::string("blocks.log"); }
   template<>
   inline std::string log_filename<block_header_state>() { return std::string("block_state.log"); }

   template<typename StoredType>
   inline std::string index_filename();
   template<>
   inline std::string index_filename<signed_block>() { return std::string("blocks.index"); }
   template<>
   inline std::string index_filename<block_header_state>() { return std::string("block_state.index"); }

   template<typename StoredType>
   inline bool needed_for_replay();
   template<>
   inline bool needed_for_replay<signed_block>() { return true; }
   template<>
   inline bool needed_for_replay<block_header_state>() { return false; }

   namespace detail {
      constexpr uint32_t pruned_version_flag = 1 << 31;
  
      inline block_id_type retrieve_id(const signed_block& b) { return b.calculate_id(); }
      inline block_id_type retrieve_id(const block_header_state& b) { return b.id; }

      inline uint32_t retrieve_block_num(const signed_block& b) { return b.block_num(); }
      inline uint32_t retrieve_block_num(const block_header_state& b) { return b.block_num; }

      inline signed_block_header retrieve_header(const signed_block& b) { return b; }
      inline signed_block_header retrieve_header(const block_header_state& b) { return b.header; }
      
      static const uint64_t npos = std::numeric_limits<uint64_t>::max();
//      static const uint64_t npos = REMOVE::get_npos();
  }

   // copy up to n bytes from src to dest
   inline void copy_file_content(fc::cfile& src, fc::cfile& dest, uint64_t n) {
      // calculate the number of bytes remaining in the src file can be copied
      auto current_pos = src.tellp();
      src.seek_end(0);
      auto end_pos = src.tellp();
      src.seek(current_pos);
      uint64_t remaining = end_pos - current_pos;

      // copy up to 4M bytes each iteration until done
      remaining = std::min<uint64_t>(n, remaining);
      std::vector<char> buf(4 * 1024 * 1024);
      while (remaining > 0) {
         uint64_t len = std::min<uint64_t>(remaining, buf.size());
         src.read(buf.data(), len);
         dest.write(buf.data(), len);
         remaining -= len;
      }
   }

   struct block_log_preamble {

      uint32_t                                   ver             = 0;
      uint32_t                                   first_block_num = 0;
      std::variant<genesis_state, chain_id_type> chain_context;

      uint32_t version() const { return ver & ~detail::pruned_version_flag; }
      bool     is_currently_pruned() const { return ver & detail::pruned_version_flag; }

      chain_id_type chain_id() const {
         return std::visit(overloaded{ [](const chain_id_type& id) { return id; },
                                       [](const genesis_state& state) { return state.compute_chain_id(); } },
                           chain_context);
      }

      constexpr static int nbytes_with_chain_id = // the bytes count when the preamble contains chain_id
            sizeof(uint32_t) + sizeof(first_block_num) + sizeof(chain_id_type) + sizeof(detail::npos);

      static bool is_supported_version(uint32_t version);
      static bool contains_genesis_state(uint32_t version, uint32_t first_block_num);
      static bool contains_chain_id(uint32_t version, uint32_t first_block_num);
      template <typename Stream>
      void read_from(Stream& ds, const std::filesystem::path& log_path) {
         ds.read((char*)&ver, sizeof(ver));
         SYS_ASSERT(version() > 0, block_log_exception, "Block log was not setup properly");
         SYS_ASSERT(is_supported_version(version()), block_log_unsupported_version,
                    "Unsupported version of block log. Block log version is ${version} while code supports version(s) "
                    "[${min},${max}], log file: ${log}",
                    ("version", version())("min", common_protocol::min_supported_version)(
                          "max", common_protocol::max_supported_version)("log", log_path));

         first_block_num = 1;
         if (version() != initial_version) {
            ds.read(reinterpret_cast<char*>(&first_block_num), sizeof(first_block_num));
         }

         if (contains_genesis_state(version(), first_block_num)) {
            chain_context.emplace<genesis_state>();
            fc::raw::unpack(ds, std::get<genesis_state>(chain_context));
         } else if (contains_chain_id(version(), first_block_num)) {
            chain_context = chain_id_type::empty_chain_id();
            ds >> std::get<chain_id_type>(chain_context);
         } else {
            SYS_THROW(block_log_exception,
                      "Block log is not supported. version: ${ver} and first_block_num: ${fbn} does not contain "
                      "a genesis_state nor a chain_id.",
                      ("ver", version())("fbn", first_block_num));
         }

         if (version() != initial_version) {
            const auto                           expected_totem = detail::npos;
            std::decay_t<decltype(detail::npos)> actual_totem;
            ds.read((char*)&actual_totem, sizeof(actual_totem));

            SYS_ASSERT(actual_totem == expected_totem, block_log_exception,
                       "Expected separator between log header and block storage was not found( expected: ${e}, actual: "
                       "${a} )",
                       ("e", fc::to_hex((char*)&expected_totem, sizeof(expected_totem)))(
                             "a", fc::to_hex((char*)&actual_totem, sizeof(actual_totem))));
         }
      }

      template <typename Stream>
      void write_exclude_version(Stream& ds) const {
         // write the rest of header without the leading version field
         if (version() != initial_version) {
            ds.write(reinterpret_cast<const char*>(&first_block_num), sizeof(first_block_num));

            std::visit(overloaded{ [&ds](const chain_id_type& id) { ds << id; },
                                   [&ds](const genesis_state& state) {
                                      auto data = fc::raw::pack(state);
                                      ds.write(data.data(), data.size());
                                   } },
                       chain_context);

            auto totem = detail::npos;
            ds.write(reinterpret_cast<const char*>(&totem), sizeof(totem));
         } else {
            const auto& state = std::get<genesis_state>(chain_context);
            auto        data  = fc::raw::pack(state);
            ds.write(data.data(), data.size());
         }
      }

      template <typename Stream>
      void write_to(Stream& ds) const {
         ds.write(reinterpret_cast<const char*>(&ver), sizeof(ver));
         write_exclude_version(ds);
      }

      void write_to(fc::datastream<fc::cfile>& ds) const {
         uint32_t local_ver = 0;
         ds.write(reinterpret_cast<const char*>(&local_ver), sizeof(local_ver));
         write_exclude_version(ds);
         ds.flush();
         ds.seek(0);
         ds.write(reinterpret_cast<const char*>(&ver), sizeof(ver));
         ds.flush();
      }
   };

   namespace {

      class index_writer {
       public:
         index_writer(const std::filesystem::path& block_index_name, uint32_t blocks_expected, bool create = true) {
            index_file.set_file_path(block_index_name);
            auto mode = create ? fc::cfile::truncate_rw_mode : fc::cfile::update_rw_mode;
            index_file.open(mode);
            index_file.seek(sizeof(uint64_t) * (blocks_expected - 1));
         }
         void write(uint64_t pos) {
            index_file.write((const char*)&pos, sizeof(pos));
            if (index_file.tellp() >= 2 * sizeof(uint64_t))
               index_file.skip(-2 * sizeof(uint64_t));
         }

         void close() { index_file.close(); }

       private:
         fc::cfile index_file;
      };

      struct bad_block_exception {
         std::exception_ptr inner;
      };

      template <typename Stream, typename StoredType>
      std::shared_ptr<StoredType> read_block(Stream& ds, uint32_t expect_block_num = 0) {
         auto block = std::make_shared<StoredType>();
         fc::raw::unpack(ds, *block);
         if (expect_block_num != 0) {
            SYS_ASSERT(!!block && detail::retrieve_block_num(*block) == expect_block_num, block_log_exception,
                       "Wrong block was read from the log.");
         }

         return block;
      }

      template<typename Stream>
      signed_block_header retrieve_block_header(Stream& ds, uint32_t expect_block_num, signed_block* n) {
         // just extract the header to skip having to read the whole block, since we just need the header 
         signed_block_header sbh;
         fc::raw::unpack(ds, sbh);

         SYS_ASSERT(sbh.block_num() == expect_block_num, block_log_exception,
                    "Wrong block header was read from block log.",
                    ("returned", sbh.block_num())("expected", expect_block_num));
         return sbh;
      }


      template<typename Stream>
      signed_block_header retrieve_block_header(Stream& ds, uint32_t expect_block_num, block_header_state* n) {
         block_header_state bhs;
         fc::raw::unpack(ds, bhs);

         SYS_ASSERT(bhs.block_num == expect_block_num, block_log_exception,
                    "Wrong block header was read from block log.",
                    ("returned", bhs.block_num)("expected", expect_block_num));
         return bhs.header;
      }

      template <typename Stream, typename StoredType>
      signed_block_header read_block_header(Stream& ds, uint32_t expect_block_num) {
         return retrieve_block_header<Stream>(ds, expect_block_num, (StoredType*)nullptr);
      }

      /// Provide the read only view of the log file
      class block_log_data : public chain::log_data_base<block_log_data> {
         block_log_preamble preamble;
         uint64_t           first_block_pos = 0;
         std::size_t        size_ = 0;

       public:
         block_log_data() = default;
         block_log_data(const std::filesystem::path& path) { open(path); }
         uint64_t first_block_position() const { return first_block_pos; }

         const block_log_preamble& get_preamble() const { return preamble; }

         void open(const std::filesystem::path& path) {
            if (file.is_open())
               file.close();
            file.set_file_path(path);
            file.open("rb");
            preamble.read_from(file, file.get_file_path());
            first_block_pos = file.tellp();
            file.seek_end(0);
            size_ = file.tellp();
         }

         uint64_t size() const { return size_; }

         uint32_t      version() { return preamble.version(); }
         uint32_t      first_block_num() const { return preamble.first_block_num; }
         uint32_t      number_of_blocks();
         chain_id_type chain_id() { return preamble.chain_id(); }
         bool          is_currently_pruned() const { return preamble.is_currently_pruned(); }
         uint64_t      end_of_block_position() const { return is_currently_pruned() ? size() - sizeof(uint32_t) : size(); }

         std::optional<genesis_state> get_genesis_state() {
            return std::visit(
                  overloaded{ [](const chain_id_type&) { return std::optional<genesis_state>{}; },
                              [](const genesis_state& state) { return std::optional<genesis_state>{ state }; } },
                  preamble.chain_context);
         }

         uint32_t block_num_at(uint64_t position) {
            // to derive blknum_offset==14 see block_header.hpp and note on disk struct is packed
            //   block_timestamp_type timestamp;                  //bytes 0:3
            //   account_name         producer;                   //bytes 4:11
            //   uint16_t             confirmed;                  //bytes 12:13
            //   block_id_type        previous;                   //bytes 14:45, low 4 bytes is big endian block number
            //   of previous block

            SYS_ASSERT(position <= size(), block_log_exception, "Invalid block position ${position}",
                       ("position", position));

            int      blknum_offset  = 14;
            uint32_t prev_block_num = read_data_at<uint32_t>(file, position + blknum_offset);
            return fc::endian_reverse_u32(prev_block_num) + 1;
         }

         auto& ro_stream_at(uint64_t pos) {
            file.seek(pos);
            return file;
         }

         uint64_t remaining() const { return size() - file.tellp(); }
         /**
          *  Validate a block log entry WITHOUT deserializing the entire block data.
          **/
         void light_validate_block_entry_at(uint64_t pos, uint32_t expected_block_num) {
            const uint32_t actual_block_num = block_num_at(pos);

            SYS_ASSERT(actual_block_num == expected_block_num, block_log_exception,
                       "At position ${pos} expected to find block number ${exp_bnum} but found ${act_bnum}",
                       ("pos", pos)("exp_bnum", expected_block_num)("act_bnum", actual_block_num));
         }

         /**
          *  Validate a block log entry by deserializing the entire block data.
          *
          *  @returns The tuple of block number and block id in the entry
          **/
         template<typename StoredType>
         std::tuple<uint32_t, block_id_type> full_validate_block_entry(uint32_t             previous_block_num,
                                                                       const block_id_type& previous_block_id,
                                                                       StoredType&          entry) {
            uint64_t pos = file.tellp();

            try {
               fc::raw::unpack(file, entry);
            } catch (...) { throw bad_block_exception{ std::current_exception() }; }

            auto id        = detail::retrieve_id(entry);
            auto block_num = block_header::num_from_id(id);

            if (block_num != previous_block_num + 1) {
               elog("Block ${num} (${id}) skips blocks. Previous block in log is block ${prev_num} (${previous})",
                    ("num", block_num)("id", id)("prev_num", previous_block_num)("previous", previous_block_id));
            }

            const block_header& header = detail::retrieve_header(entry);

            if (!previous_block_id.empty() && previous_block_id != header.previous) {
               elog("Block ${num} (${id}) does not link back to previous block. "
                    "Expected previous: ${expected}. Actual previous: ${actual}.",
                    ("num", block_num)("id", id)("expected", previous_block_id)("actual", header.previous));
            }

            uint64_t tmp_pos = std::numeric_limits<uint64_t>::max();

            if (remaining() >= sizeof(tmp_pos)) {
               file.read(reinterpret_cast<char*>(&tmp_pos), sizeof(tmp_pos));
            }

            SYS_ASSERT(pos == tmp_pos, block_log_exception,
                       "the block position for block ${num} at the end of a block entry is incorrect",
                       ("num", block_num));
            return std::make_tuple(block_num, id);
         }

         template<typename StoredType>
         std::tuple<uint64_t, uint32_t, std::string>
         full_validate_blocks(uint32_t last_block_num, const std::filesystem::path& blocks_dir, fc::time_point now);

         void construct_index(const std::filesystem::path& index_file_path);
      };

      using block_log_index = sysio::chain::log_index<block_log_exception>;

      /// Provide the read only view for both log and index files
      template<typename StoredType>
      struct block_log_bundle {
         using stored_type = StoredType;

         std::filesystem::path        block_file_name, index_file_name; // full pathname for log and index filenames
         block_log_data  log_data;
         block_log_index log_index;

         block_log_bundle(std::filesystem::path block_file, std::filesystem::path index_file, bool validate_indx)
             : block_file_name(std::move(block_file)), index_file_name(std::move(index_file)) {

            log_data.open(block_file_name);
            log_index.open(index_file_name);

            SYS_ASSERT(!log_data.get_preamble().is_currently_pruned(), block_log_unsupported_version,
                       "Block log is currently in pruned format, it must be vacuumed before doing this operation");

            if (validate_indx)
               validate_index();
         }

         explicit block_log_bundle(const std::filesystem::path& block_dir, bool validate_index=true)
             : block_log_bundle(block_dir / log_filename<stored_type>(), block_dir / index_filename<stored_type>(), validate_index) {}

         // throws if not valid
         void validate_index() {
            uint32_t log_num_blocks   = log_data.num_blocks();
            uint32_t index_num_blocks = log_index.num_blocks();

            SYS_ASSERT(
                  log_num_blocks == index_num_blocks, block_log_exception,
                  "${block_file_name} says it has ${log_num_blocks} blocks which disagrees with ${index_num_blocks} "
                  "indicated by ${index_file_name}",
                  ("block_file_name", block_file_name)("log_num_blocks", log_num_blocks)(
                        "index_num_blocks", index_num_blocks)("index_file_name", index_file_name));
         }
      };

      /// Used to traverse the block position (i.e. the last 8 bytes in each block log entry) of the log file
      class reverse_block_position_iterator {
         fc::datastream<fc::cfile>& file;
         uint64_t                   first_block_pos;
         uint64_t                   end_of_block_pos;
         uint64_t                   current_position;

         uint64_t get_value() {
            SYS_ASSERT(
                  current_position > first_block_pos && current_position <= end_of_block_pos, block_log_exception,
                  "Block log file formatting is incorrect, it contains a block position value: ${pos}, which is not "
                  "in the range of (${begin_pos},${last_pos})",
                  ("pos", current_position)("begin_pos", first_block_pos)("last_pos", end_of_block_pos));

            uint64_t value;
            file.seek(current_position - sizeof(uint64_t));
            fc::raw::unpack(file, value);
            return value;
         }

       public:
         reverse_block_position_iterator(fc::datastream<fc::cfile>& data, uint64_t first_block_pos,
                                         uint64_t end_of_block_pos)
             : file(data), first_block_pos(first_block_pos), end_of_block_pos(end_of_block_pos),
               current_position(end_of_block_pos) {}

         uint64_t get_value_then_advance() {
            current_position = get_value();
            return current_position;
         }

         uint64_t add_value_then_advance(uint64_t offset) {
            current_position = get_value() + offset;
            file.skip(-sizeof(uint64_t));
            fc::raw::pack(file, current_position);
            return current_position;
         }

         bool done() const { return current_position <= first_block_pos; }
      };

      void adjust_block_positions(index_writer& index, fc::datastream<fc::cfile>& block_file,
                                  uint64_t first_block_position, int64_t offset) {

         block_file.seek_end(0);
         // walk along the block position of each block entry and add its value by offset
         auto iter = reverse_block_position_iterator{ block_file, first_block_position, block_file.tellp() };
         while (!iter.done()) { index.write(iter.add_value_then_advance(offset)); }
      }

      uint32_t block_log_data::number_of_blocks() {
         const uint32_t num_blocks =
               first_block_position() == end_of_block_position() ? 0 : last_block_num() - first_block_num() + 1;
         return num_blocks;
      }

      void block_log_data::construct_index(const std::filesystem::path& index_file_path) {
         std::string index_file_name = index_file_path.generic_string();
         ilog("Will write new index file ${file}", ("file", index_file_name));

         const uint32_t num_blocks = number_of_blocks();

         ilog("block log version= ${version}, number of blocks ${n}", ("version", this->version())("n", num_blocks));

         if (num_blocks == 0) {
            return;
         }

         ilog("first block= ${first}         last block= ${last}",
              ("first", this->first_block_num())("last", (this->last_block_num())));

         index_writer index(index_file_path, num_blocks);
         uint32_t     blocks_remaining = this->num_blocks();

         for (auto iter = reverse_block_position_iterator{ file, first_block_position(), end_of_block_position() };
              !iter.done() && blocks_remaining > 0; --blocks_remaining) {
            auto pos = iter.get_value_then_advance();
            index.write(pos);
            if ((blocks_remaining & 0xfffff) == 0)
               ilog("blocks remaining to index: ${blocks_left}      position in log file: ${pos}",
                    ("blocks_left", blocks_remaining)("pos", pos));
         }
      }

   } // namespace

   struct block_log_verifier {
      chain_id_type chain_id = chain_id_type::empty_chain_id();

      void verify(block_log_data& log, const std::filesystem::path& log_path) {
         if (chain_id.empty()) {
            chain_id = log.chain_id();
         } else {
            SYS_ASSERT(chain_id == log.chain_id(), block_log_exception,
                       "block log file ${path} has a different chain id", ("path", log_path));
         }
      }
   };
   template<typename StoredType>
   using block_log_catalog = sysio::chain::log_catalog<StoredType, block_log_data, block_log_index, block_log_verifier>;

   namespace detail {

      static bool is_pruned_log_and_mask_version(uint32_t& version) {
         bool ret = version & pruned_version_flag;
         version &= ~pruned_version_flag;
         return ret;
      }

      template<typename StoredType>
      struct block_log_impl {
         using stored_type = StoredType;
         using stored_type_ptr = std::shared_ptr<stored_type>;

         std::mutex       mtx;
         struct stored_type_with_id {
            stored_type_ptr ptr;
            block_id_type id;
         };
         std::optional<stored_type_with_id> head;

         virtual ~block_log_impl() = default;

         virtual bool     is_initialized() const                                              = 0;
         virtual uint32_t first_block_num()                                                   = 0;
         virtual void     append(const stored_type_ptr& b, const block_id_type& id,
                                 const std::vector<char>& packed_block)                       = 0;
         virtual uint64_t get_block_pos(uint32_t block_num)                                   = 0;
         virtual void     reset(const genesis_state& gs, const stored_type_ptr& first_block) = 0;
         virtual void     reset(const chain_id_type& chain_id, uint32_t first_block_num)      = 0;
         virtual void     flush()                                                             = 0;

         virtual stored_type_ptr                    read_block_by_num(uint32_t block_num)        = 0;
         virtual std::optional<signed_block_header> read_block_header_by_num(uint32_t block_num) = 0;

         virtual uint32_t version() const = 0;

         virtual stored_type_ptr read_head() = 0;
         void                    update_head(const stored_type_ptr& b, const std::optional<block_id_type>& id = {}) {
            if (b)
               head = { b, id ? *id : detail::retrieve_id(*b) };
            else
               head = {};
         }
      }; // block_log_impl

      /// Would remove pre-existing log and index, never write blocks into disk.
      template<typename StoredType>
      struct empty_block_log final : block_log_impl<StoredType> {
         using stored_type = StoredType;
         using stored_type_ptr = std::shared_ptr<stored_type>;

         uint32_t first_block_number = std::numeric_limits<uint32_t>::max();

         explicit empty_block_log(const std::filesystem::path& log_dir) {
            std::filesystem::remove(log_dir / log_filename<stored_type>());
            std::filesystem::remove(log_dir / index_filename<stored_type>());
         }

         bool     is_initialized() const final { return true; }
         uint32_t first_block_num() final { return this->head ? retrieve_block_num(*this->head->ptr) : first_block_number; }
         void append(const stored_type_ptr& b, const block_id_type& id, const std::vector<char>& packed_block) final {
            block_log_impl<stored_type>::update_head(b, id);
         }

         uint64_t get_block_pos(uint32_t block_num) final { return detail::npos; }
         void reset(const genesis_state& gs, const stored_type_ptr& first_block) final { block_log_impl<stored_type>::update_head(first_block); }
         void reset(const chain_id_type& chain_id, uint32_t first_block_num) final { first_block_number = first_block_num; }
         void flush() final {}

         stored_type_ptr read_block_by_num(uint32_t block_num) final { return {}; };
         std::optional<signed_block_header> read_block_header_by_num(uint32_t block_num) final { return {}; };

         uint32_t        version() const final { return 0; }
         stored_type_ptr read_head() final { return {}; };
      };

      template<typename StoredType>
      struct basic_block_log : block_log_impl<StoredType> {
         using stored_type = StoredType;
         using stored_type_ptr = std::shared_ptr<stored_type>;

         fc::datastream<fc::cfile> block_file;
         fc::datastream<fc::cfile> index_file;
         block_log_preamble        preamble;
         bool                      genesis_written_to_block_log = false;

         basic_block_log() = default;

         explicit basic_block_log(std::filesystem::path log_dir) { open(log_dir); }

         static void ensure_file_exists(fc::cfile& f) {
            if (std::filesystem::exists(f.get_file_path()))
               return;
            f.open(fc::cfile::create_or_update_rw_mode);
            f.close();
         }

         virtual void transform_block_log() {
            // convert from  pruned block log to non-pruned if necessary
            if (preamble.is_currently_pruned()) {
               block_file.open(fc::cfile::update_rw_mode);
               this->update_head(this->read_head());
               if (this->head) {
                  index_file.open(fc::cfile::update_rw_mode);
                  vacuum(first_block_num_from_pruned_log(), preamble.first_block_num);
               } else {
                  std::filesystem::resize_file(index_file.get_file_path(), 0);
               }
               preamble.ver = preamble.version();
            }
         }

         bool     is_initialized() const override { return genesis_written_to_block_log; }
         uint32_t first_block_num() override { return preamble.first_block_num; }
         uint32_t index_first_block_num() const { return preamble.first_block_num; }

         virtual uint32_t         working_block_file_first_block_num() { return preamble.first_block_num; }
         virtual void             post_append(uint64_t pos) {}
         virtual stored_type_ptr retry_read_block_by_num(uint32_t block_num) { return {}; }
         virtual std::optional<signed_block_header> retry_read_block_header_by_num(uint32_t block_num) { return {}; }

         void append(const stored_type_ptr& b, const block_id_type& id,
                     const std::vector<char>& packed_block) override {
            try {
               SYS_ASSERT(genesis_written_to_block_log, block_log_append_fail,
                          "Cannot append to ${desc} log until the genesis is first written", ("desc", filename_prefix<StoredType>()));

               block_file.seek_end(0);
               index_file.seek_end(0);
               // if pruned log, rewind over count trailer if any block is already present
               if (preamble.is_currently_pruned() && this->head)
                  block_file.skip(-sizeof(uint32_t));
               uint64_t pos = block_file.tellp();

               SYS_ASSERT(index_file.tellp() == sizeof(uint64_t) * (retrieve_block_num(*b) - preamble.first_block_num),
                          block_log_append_fail, "Append to ${desc}'s index file occuring at wrong position.",
                          ("desc", filename_prefix<StoredType>())
                          ("position", (uint64_t)index_file.tellp())(
                                "expected", (retrieve_block_num(*b) - preamble.first_block_num) * sizeof(uint64_t)));
               block_file.write(packed_block.data(), packed_block.size());
               block_file.write((char*)&pos, sizeof(pos));
               index_file.write((char*)&pos, sizeof(pos));
               index_file.flush();
               this->update_head(b, id);

               post_append(pos);
               block_file.flush();
            }
            FC_LOG_AND_RETHROW()
         }

         uint64_t get_block_pos(uint32_t block_num) final {
            if (!(this->head && block_num <= block_header::num_from_id(this->head->id) &&
                  block_num >= working_block_file_first_block_num()))
               return detail::npos;
            index_file.seek(sizeof(uint64_t) * (block_num - index_first_block_num()));
            uint64_t pos;
            index_file.read((char*)&pos, sizeof(pos));
            return pos;
         }

         stored_type_ptr read_block_by_num(uint32_t block_num) final {
            try {
               uint64_t pos = get_block_pos(block_num);
               if (pos != detail::npos) {
                  block_file.seek(pos);
                  return read_block<decltype(block_file), stored_type>(block_file,
                                                                       block_num);
               }
               return retry_read_block_by_num(block_num);
            }
            FC_LOG_AND_RETHROW()
         }

         std::optional<signed_block_header> read_block_header_by_num(uint32_t block_num) final {
            try {
               uint64_t pos = get_block_pos(block_num);
               if (pos != detail::npos) {
                  block_file.seek(pos);
                  return read_block_header<decltype(block_file), stored_type>(block_file, block_num);
               }
               return retry_read_block_header_by_num(block_num);
            }
            FC_LOG_AND_RETHROW()
         }

         void open(const std::filesystem::path& data_dir) {

            if (!std::filesystem::is_directory(data_dir))
               std::filesystem::create_directories(data_dir);

            this->block_file.set_file_path(data_dir / log_filename<stored_type>());
            this->index_file.set_file_path(data_dir / index_filename<stored_type>());

            /* On startup of the block log, there are several states the log file and the index file can be
             * in relation to each other.
             *
             *                          Block Log
             *                     Exists       Is New
             *                 +------------+------------+
             *          Exists |    Check   |   Delete   |
             *   Index         |    Head    |    Index   |
             *    File         +------------+------------+
             *          Is New |   Replay   |     Do     |
             *                 |    Log     |   Nothing  |
             *                 +------------+------------+
             *
             * Checking the heads of the files has several conditions as well.
             *  - If they are the same, do nothing.
             *  - If the index file head is not in the log file, delete the index and replay.
             *  - If the index file head is in the log, but not up to date, replay from index head.
             */
            ensure_file_exists(block_file);
            ensure_file_exists(index_file);
            auto log_size   = std::filesystem::file_size(this->block_file.get_file_path());
            auto index_size = std::filesystem::file_size(this->index_file.get_file_path());
            ilog("${desc} Log file_size: ${size}", ("desc", filename_prefix<StoredType>())("size", log_size));

            if (log_size) {
               block_log_data log_data(block_file.get_file_path());
               preamble = log_data.get_preamble();
               // genesis state is not going to be useful afterwards, just convert it to chain id to save space
               preamble.chain_context = preamble.chain_id();

               genesis_written_to_block_log = true; // Assume it was constructed properly.

               uint32_t number_of_blocks = log_data.number_of_blocks();
               ilog("Log has ${n} blocks", ("n", number_of_blocks));

               SYS_ASSERT(index_size || number_of_blocks == 0, block_log_exception,
                          "${index_file} file is empty, please use sys-util to fix the problem.",
                          ("index_file", index_file.get_file_path().string()));
               SYS_ASSERT(index_size % sizeof(uint64_t) == 0, block_log_exception,
                          "${index_file} file is invalid, please use sys-util to reconstruct the index.",
                          ("index_file", index_file.get_file_path().string()));

               if (index_size) {
                  block_log_index index(index_file.get_file_path());
                  auto last_block_pos = log_data.last_block_position();
                  auto last_index_pos = index.back();

                  SYS_ASSERT(last_block_pos == last_index_pos, block_log_exception,
                             "The last block position from ${block_file} is at ${block_pos} "
                             "which does not match the last block postion ${index_pos} from ${index_file}, please use "
                             "sys-util to fix the inconsistency.",
                             ("block_pos", last_block_pos)("index_pos", last_index_pos)
                             ("block_file", block_file.get_file_path().string())
                             ("index_file", index_file.get_file_path().string()));
               }
               log_data.close();

               transform_block_log();

            } else if (index_size) {
               ilog("Log file is empty while the index file is nonempty, discard the index file");
               std::filesystem::resize_file(index_file.get_file_path(), 0);
            }

            if (!block_file.is_open())
               block_file.open(fc::cfile::update_rw_mode);
            if (!index_file.is_open())
               index_file.open(fc::cfile::update_rw_mode);
            if (log_size && !this->head)
               this->update_head(this->read_head());
         }

         uint64_t first_block_num_from_pruned_log() {
            uint32_t num_blocks;
            this->block_file.seek_end(-sizeof(uint32_t));
            fc::raw::unpack(this->block_file, num_blocks);
            return retrieve_block_num(*this->head->ptr) - num_blocks + 1;
         }

         void reset(uint32_t first_bnum, std::variant<genesis_state, chain_id_type>&& chain_context, uint32_t version) {

            block_file.open(fc::cfile::truncate_rw_mode);
            preamble.ver             = version | (preamble.ver & pruned_version_flag);
            preamble.first_block_num = first_bnum;
            preamble.chain_context   = std::move(chain_context);
            preamble.write_to(block_file);

            // genesis state is not going to be useful afterwards, just convert it to chain id to save space
            preamble.chain_context = preamble.chain_id();

            genesis_written_to_block_log = true;
            static_assert(common_protocol::max_supported_version > 0, "a version number of zero is not supported");

            index_file.open(fc::cfile::truncate_rw_mode);
            index_file.flush();
         }

         void reset(const genesis_state& gs, const stored_type_ptr& first_block) override {
            this->reset(1, gs, common_protocol::default_initial_version);
            this->append(first_block, retrieve_id(*first_block), fc::raw::pack(*first_block));
         }

         void reset(const chain_id_type& chain_id, uint32_t first_block_num) override {
            // if this block_log type is required for replay (like the traditional block log)
            // then don't allow just writing the chain_id, since that will force this instance
            // to restart with a snapshot, rather than being able to reconstuct it via this
            // block log
            SYS_ASSERT(
                  first_block_num > 1 || !needed_for_replay<StoredType>(), block_log_exception,
                  "Block log version ${ver} needs to be created with a genesis state if starting from block number 1.");

            this->reset(first_block_num, chain_id, common_protocol::max_supported_version);
            this->head.reset();
         }

         void flush() final {
            block_file.flush();
            index_file.flush();
         }

         stored_type_ptr read_head() final {
            auto pos = read_head_position();
            if (pos != detail::npos) {
               block_file.seek(pos);
               return read_block<decltype(block_file), StoredType>(block_file, 0);
            } else {
               return {};
            }
         }

         uint64_t read_head_position() {
            uint64_t pos;

            // Check that the file is not empty
            this->block_file.seek_end(0);
            if (this->block_file.tellp() <= sizeof(pos))
               return detail::npos;

            // figure out if this is a pruned log or not. we can't just look at the configuration since
            //  read_head() is called early on, and this isn't hot enough to warrant a member bool to track it
            this->block_file.seek(0);
            uint32_t current_version;
            fc::raw::unpack(this->block_file, current_version);
            const bool is_currently_pruned = detail::is_pruned_log_and_mask_version(current_version);

            this->block_file.seek_end(0);
            int64_t skip_count = -sizeof(pos);

            if (is_currently_pruned) {
               skip_count += -sizeof(uint32_t); // skip the trailer containing block count
            }
            this->block_file.skip(skip_count);
            fc::raw::unpack(this->block_file, pos);

            return pos;
         }

         void vacuum(uint64_t first_block_num, uint64_t index_first_block_num) {
            // go ahead and write a new valid header now. if the vacuum fails midway, at least this means maybe the
            //  block recovery can get through some blocks.
            size_t copy_to_pos = convert_existing_header_to_vacuumed(first_block_num);

            preamble.ver = common_protocol::max_supported_version;

            // if there is no head block though, bail now, otherwise first_block_num won't actually be available
            //  and it'll mess this all up. Be sure to still remove the 4 byte trailer though.
            if (!block_log_impl<stored_type>::head) {
               block_file.flush();
               std::filesystem::resize_file(block_file.get_file_path(),
                                            std::filesystem::file_size(block_file.get_file_path()) - sizeof(uint32_t));
               return;
            }

            size_t copy_from_pos = get_block_pos(first_block_num);
            block_file.seek_end(-sizeof(uint32_t));
            size_t         copy_sz           = block_file.tellp() - copy_from_pos;
            const uint32_t num_blocks_in_log = chain::block_header::num_from_id(block_log_impl<stored_type>::head->id) - first_block_num + 1;

            const size_t offset_bytes  = copy_from_pos - copy_to_pos;
            const size_t offset_blocks = first_block_num - index_first_block_num;

            std::vector<char> buff;
            buff.resize(4 * 1024 * 1024);

            auto tick = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
            while (copy_sz) {
               const size_t copy_this_round = std::min(buff.size(), copy_sz);
               block_file.seek(copy_from_pos);
               block_file.read(buff.data(), copy_this_round);
               block_file.punch_hole(copy_to_pos, copy_from_pos + copy_this_round);
               block_file.seek(copy_to_pos);
               block_file.write(buff.data(), copy_this_round);

               copy_from_pos += copy_this_round;
               copy_to_pos += copy_this_round;
               copy_sz -= copy_this_round;

               const auto tock = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
               if (tick < tock - std::chrono::seconds(5)) {
                  ilog("Vacuuming pruned block log, ${b} bytes remaining", ("b", copy_sz));
                  tick = tock;
               }
            }
            block_file.flush();
            std::filesystem::resize_file(block_file.get_file_path(), block_file.tellp());

            index_file.flush();
            {
               boost::interprocess::mapped_region index_mapped(index_file, boost::interprocess::read_write);
               uint64_t*                          index_ptr = (uint64_t*)index_mapped.get_address();

               for (uint32_t new_block_num = 0; new_block_num < num_blocks_in_log; ++new_block_num) {
                  const uint64_t new_pos   = index_ptr[new_block_num + offset_blocks] - offset_bytes;
                  index_ptr[new_block_num] = new_pos;

                  if (new_block_num + 1 != num_blocks_in_log)
                     block_file.seek(index_ptr[new_block_num + offset_blocks + 1] - offset_bytes - sizeof(uint64_t));
                  else
                     block_file.seek_end(-sizeof(uint64_t));
                  block_file.write((char*)&new_pos, sizeof(new_pos));
               }
            }
            std::filesystem::resize_file(index_file.get_file_path(), num_blocks_in_log * sizeof(uint64_t));

            preamble.first_block_num = first_block_num;
         }

         size_t convert_existing_header_to_vacuumed(uint32_t first_block_num) {
            uint32_t   old_version;
            uint32_t   old_first_block_num;
            const auto totem = detail::npos;

            block_file.seek(0);
            fc::raw::unpack(block_file, old_version);
            fc::raw::unpack(block_file, old_first_block_num);
            SYS_ASSERT(is_pruned_log_and_mask_version(old_version), block_log_exception,
                       "Trying to vacuumed a non-pruned block log");

            if (block_log_preamble::contains_genesis_state(old_version, old_first_block_num)) {
               // we'll always write a v3 log, but need to possibly mutate the genesis_state to a chainid should we have
               // pruned a log starting with a genesis_state
               genesis_state gs;
               auto          ds = block_file.create_datastream();
               fc::raw::unpack(ds, gs);

               block_file.seek(0);
               fc::raw::pack(block_file, common_protocol::max_supported_version);
               fc::raw::pack(block_file, first_block_num);
               if (first_block_num == 1) {
                  SYS_ASSERT(old_first_block_num == 1, block_log_exception, "expected an old first blocknum of 1");
                  fc::raw::pack(block_file, gs);
               } else
                  fc::raw::pack(block_file, gs.compute_chain_id());
               fc::raw::pack(block_file, totem);
            } else {
               // read in the existing chainid, to parrot back out
               fc::sha256 chainid;
               fc::raw::unpack(block_file, chainid);

               block_file.seek(0);
               fc::raw::pack(block_file, common_protocol::max_supported_version);
               fc::raw::pack(block_file, first_block_num);
               fc::raw::pack(block_file, chainid);
               fc::raw::pack(block_file, totem);
            }

            return block_file.tellp();
         }

         static void write_incomplete_block_data(const std::filesystem::path& blocks_dir, fc::time_point now, uint32_t block_num,
                                                 fc::cfile& strm) {
            auto      tail_path = blocks_dir / filename_prefix<stored_type>().append("-bad-tail-").append(now.to_iso_string()).append(".log");
            fc::cfile tail;
            tail.set_file_path(tail_path);
            tail.open(fc::cfile::create_or_update_rw_mode);
            copy_file_content(strm, tail);

            ilog("Data at tail end of block log which should contain the (incomplete) serialization of block ${num} "
                 "has been written out to '${tail_path}'.",
                 ("num", block_num + 1)("tail_path", tail_path));
         }

         bool recover_from_incomplete_block_head(block_log_data& log_data, block_log_index& index) {
            const uint64_t pos = index.back();
            if (log_data.size() <= pos) {
               // index refers to an invalid position, we cannot recover from it
               return false;
            }

            const uint32_t expected_block_num = log_data.first_block_num() + index.num_blocks() - 1;
            auto&          ds                 = log_data.ro_stream_at(pos);

            try {
               StoredType entry;
               fc::raw::unpack(ds, entry);
               if (entry.block_num() != expected_block_num) {
                  return false;
               }
               uint64_t tmp_pos = std::numeric_limits<uint64_t>::max();
               ds.read(reinterpret_cast<char*>(&tmp_pos), sizeof(tmp_pos));
               if (tmp_pos != pos)
                  return false;

               const auto trimmed_block_file_size = ds.tellp();

               write_incomplete_block_data(block_file.get_file_path().parent_path(), fc::time_point::now(),
                                           expected_block_num + 1, ds);
               std::filesystem::resize_file(block_file.get_file_path(), trimmed_block_file_size);
               return true;
            } catch (...) { return false; }
         }

         uint32_t version() const final { return preamble.version(); }
      };

      template<typename StoredType>
      struct partitioned_block_log final : basic_block_log<StoredType> {
         using stored_type = StoredType;
         using stored_type_ptr = std::shared_ptr<stored_type>;

         block_log_catalog<StoredType> catalog;
         const size_t      stride;

         partitioned_block_log(const std::filesystem::path& log_dir, const partitioned_blocklog_config& config) : basic_block_log<StoredType>{}, stride(config.stride) {
            catalog.open(log_dir, config.retained_dir, config.archive_dir, filename_prefix<stored_type>().c_str());
            catalog.max_retained_files = config.max_retained_files;

            basic_block_log<StoredType>::open(log_dir);
            const auto log_size = std::filesystem::file_size(this->block_file.get_file_path());

            if (log_size == 0 && !catalog.empty()) {
               basic_block_log<stored_type>::reset(catalog.verifier.chain_id, catalog.last_block_num() + 1);
               this->update_head(this->read_block_by_num(catalog.last_block_num()));
            } else {
               SYS_ASSERT(catalog.verifier.chain_id.empty() || catalog.verifier.chain_id == this->preamble.chain_id(),
                          block_log_exception, "block log file ${path} has a different chain id",
                          ("path", this->block_file.get_file_path()));
            }
         }

         void split_log() {
            fc::datastream<fc::cfile> new_block_file;
            fc::datastream<fc::cfile> new_index_file;

            std::filesystem::path block_file_path = this->block_file.get_file_path();
            std::filesystem::path index_file_path = this->index_file.get_file_path();

            std::filesystem::path tmp_block_file_path = block_file_path;
            tmp_block_file_path.replace_extension("log.tmp");
            std::filesystem::path tmp_index_file_path = index_file_path;
            tmp_index_file_path.replace_extension("index.tmp");

            new_block_file.set_file_path(tmp_block_file_path);
            new_index_file.set_file_path(tmp_index_file_path);

            try {
               new_block_file.open(fc::cfile::truncate_rw_mode);
               new_index_file.open(fc::cfile::truncate_rw_mode);

            } catch (...) {
               wlog("Unable to open new block or index file for writing during block log spliting, "
                    "continue writing to existing block log file\n");
               return;
            }

            this->block_file.close();
            this->index_file.close();

            catalog.add(this->preamble.first_block_num, retrieve_block_num(*this->head->ptr), this->block_file.get_file_path().parent_path(),
                        filename_prefix<stored_type>().c_str());

            using std::swap;
            swap(new_block_file, this->block_file);
            swap(new_index_file, this->index_file);

            std::filesystem::rename(tmp_block_file_path, block_file_path);
            std::filesystem::rename(tmp_index_file_path, index_file_path);

            this->block_file.set_file_path(block_file_path);
            this->index_file.set_file_path(index_file_path);

            this->preamble.ver             = common_protocol::max_supported_version;
            this->preamble.chain_context   = this->preamble.chain_id();
            this->preamble.first_block_num = retrieve_block_num(*this->head->ptr) + 1;
            this->preamble.write_to(this->block_file);
         }

         uint32_t first_block_num() final {
            if (!catalog.empty())
               return catalog.collection.begin()->first;
            return this->preamble.first_block_num;
         }

         void post_append(uint64_t pos) final {
            if (retrieve_block_num(*this->head->ptr) % stride == 0) {
               split_log();
            }
         }

         stored_type_ptr retry_read_block_by_num(uint32_t block_num) final {
            auto ds = catalog.ro_stream_for_block(block_num);
            if (ds)
               return read_block<decltype(*ds), stored_type>(*ds, block_num);
            return {};
         }

         std::optional<signed_block_header> retry_read_block_header_by_num(uint32_t block_num) final {
            auto ds = catalog.ro_stream_for_block(block_num);
            if (ds)
               return read_block_header<decltype(*ds), stored_type>(*ds, block_num);
            return {};
         }

         void reset(const chain_id_type& chain_id, uint32_t first_block_num) final {

            SYS_ASSERT(catalog.verifier.chain_id.empty() || chain_id == catalog.verifier.chain_id, block_log_exception,
                       "Trying to reset to the chain to a different chain id");
            basic_block_log<stored_type>::reset(chain_id, first_block_num);
         }
      };

      template<typename StoredType>
      struct punch_hole_block_log final : basic_block_log<StoredType> {
         using stored_type = StoredType;
         using stored_type_ptr = std::shared_ptr<stored_type>;

         uint32_t              first_block_number = 0; // the first number available to read
         prune_blocklog_config prune_config;

         punch_hole_block_log(const std::filesystem::path& data_dir, const prune_blocklog_config& prune_conf)
             : prune_config(prune_conf) {
            SYS_ASSERT(__builtin_popcount(prune_config.prune_threshold) == 1, block_log_exception,
                       "block log prune threshold must be power of 2");
            // switch this over to the mask that will be used
            prune_config.prune_threshold = ~(prune_config.prune_threshold - 1);
            basic_block_log<StoredType>::open(data_dir);
            if (this->head)
               first_block_number = basic_block_log<stored_type>::first_block_num_from_pruned_log();
            else if (this->preamble.first_block_num)
               first_block_number = this->preamble.first_block_num;
            else
               first_block_number = 1;
            this->preamble.ver |= pruned_version_flag;
         }

         ~punch_hole_block_log() final {
            basic_block_log<stored_type>::flush();
            try_exit_vacuum();
         }

         uint32_t first_block_num() final { return first_block_number; }
         uint32_t working_block_file_first_block_num() final { return first_block_number; }

         void transform_block_log() final {
            // convert from  non-pruned block log to pruned if necessary
            if (!this->preamble.is_currently_pruned()) {
               this->block_file.open(fc::cfile::update_rw_mode);
               this->update_head(this->read_head());
               first_block_number = this->preamble.first_block_num;
               // need to convert non-pruned log to pruned log. prune any blocks to start with
               uint32_t num_blocks_in_log = this->prune(fc::log_level::info);

               // update version
               this->block_file.seek(0);
               fc::raw::pack(this->block_file, this->preamble.version() | pruned_version_flag);

               // and write out the trailing block count
               this->block_file.seek_end(0);
               fc::raw::pack(this->block_file, num_blocks_in_log);
               this->block_file.flush();
            }
         }

         // close() is called all over the place. Let's make this an explict call to ensure it only is called when
         //  we really want: when someone is destroying the blog instance
         void try_exit_vacuum() {
            // for a pruned log that has at least one block, see if we should vacuum it
            if (prune_config.vacuum_on_close) {
               if (!this->head) {
                  // disregard vacuum_on_close size if there isn't even a block and just do it silently anyways
                  basic_block_log<StoredType>::vacuum(first_block_number, this->preamble.first_block_num);
                  first_block_number = this->preamble.first_block_num;
               } else {
                  const size_t first_data_pos = this->get_block_pos(first_block_number);
                  this->block_file.seek_end(-sizeof(uint32_t));
                  const size_t last_data_pos = this->block_file.tellp();
                  if (last_data_pos - first_data_pos < prune_config.vacuum_on_close) {
                     ilog("Vacuuming pruned block log");
                     basic_block_log<StoredType>::vacuum(first_block_number, this->preamble.first_block_num);
                     first_block_number = this->preamble.first_block_num;
                  }
               }
            }
         }

         void post_append(uint64_t pos) final {
            uint32_t       num_blocks_in_log;
            const uint64_t end = this->block_file.tellp();
            if ((pos & prune_config.prune_threshold) != (end & prune_config.prune_threshold))
               num_blocks_in_log = prune(fc::log_level::debug);
            else
               num_blocks_in_log = chain::block_header::num_from_id(this->head->id) - first_block_number + 1;
            fc::raw::pack(this->block_file, num_blocks_in_log);
         }

         void reset(const genesis_state& gs, const stored_type_ptr& first_block) final {
            basic_block_log<stored_type>::reset(gs, first_block);
            first_block_number = 1;
         }

         void reset(const chain_id_type& chain_id, uint32_t first_block_num) final {
            basic_block_log<stored_type>::reset(chain_id, first_block_num);
            this->block_file.seek_end(0);
            fc::raw::pack(this->block_file, (uint32_t)0);
            this->block_file.flush();
            this->first_block_number = first_block_num;
         }

         // returns number of blocks after pruned
         uint32_t prune(const fc::log_level& loglevel) {
            if (!this->head)
               return 0;
            const uint32_t head_num = chain::block_header::num_from_id(this->head->id);
            if (head_num - first_block_number < prune_config.prune_blocks)
               return head_num - first_block_number + 1;

            const uint32_t prune_to_num = head_num - prune_config.prune_blocks + 1;

            static_assert(common_protocol::max_supported_version == 3,
                          "Code was written to support version 3 format, need to update this code for latest format.");
            const genesis_state gs;
            const size_t        max_header_size_v1 = sizeof(uint32_t) + fc::raw::pack_size(gs) + sizeof(uint64_t);
            const size_t        max_header_size_v23 =
                  sizeof(uint32_t) + sizeof(uint32_t) + sizeof(chain_id_type) + sizeof(uint64_t);
            const auto max_header_size = std::max(max_header_size_v1, max_header_size_v23);

            this->block_file.punch_hole(max_header_size, this->get_block_pos(prune_to_num));

            first_block_number = prune_to_num;
            this->block_file.flush();

            if (auto l = fc::logger::get(); l.is_enabled(loglevel))
               l.log(fc::log_message(fc::log_context(loglevel, __FILE__, __LINE__, __func__),
                                     "${l} pruned to blocks ${b}-${e}",
                                     fc::mutable_variant_object()("l", log_filename<stored_type>())
                                                                 ("b", first_block_number)
                                                                 ("e", head_num)));
            return prune_config.prune_blocks;
         }
      };

   } // namespace detail

   template<typename StoredType>
   block_log<StoredType>::block_log(const std::filesystem::path& data_dir, const block_log_config& config)
       : my(std::visit(overloaded{ [&data_dir](const basic_blocklog_config& conf) -> detail::block_log_impl<StoredType>* {
                                     return new detail::basic_block_log<StoredType>(data_dir);
                                  },
                                   [&data_dir](const empty_blocklog_config&) -> detail::block_log_impl<StoredType>* {
                                      return new detail::empty_block_log<StoredType>(data_dir);
                                   },
                                   [&data_dir](const partitioned_blocklog_config& conf) -> detail::block_log_impl<StoredType>* {
                                      return new detail::partitioned_block_log<StoredType>(data_dir, conf);
                                   },
                                   [&data_dir](const prune_blocklog_config& conf) -> detail::block_log_impl<StoredType>* {
                                      return new detail::punch_hole_block_log<StoredType>(data_dir, conf);
                                   } },
                       config)) {}

   template<typename StoredType>
   block_log<StoredType>::block_log(block_log&& other) noexcept { my = std::move(other.my); }

   template<typename StoredType>
   block_log<StoredType>::~block_log() = default;

   template<typename StoredType>
   void     block_log<StoredType>::set_initial_version(uint32_t ver) { common_protocol::default_initial_version = ver; }
   template<typename StoredType>
   std::optional<uint32_t> block_log<StoredType>::version() const {
      std::lock_guard g(my->mtx);
      if (my->is_initialized())
         return { my->version() };

      return {};
   }

   template<typename StoredType>
   void block_log<StoredType>::append(const stored_type_ptr& b, const block_id_type& id) {
      std::vector<char> packed_block = fc::raw::pack(*b);
      std::lock_guard g(my->mtx);
      my->append(b, id, packed_block);
   }

   template<typename StoredType>
   void block_log<StoredType>::append(const stored_type_ptr& b, const block_id_type& id, const std::vector<char>& packed_block) {
      std::lock_guard g(my->mtx);
      my->append(b, id, packed_block);
   }

   template<typename StoredType>
   void block_log<StoredType>::flush() {
      std::lock_guard g(my->mtx);
      my->flush();
   }

   template<typename StoredType>
   void block_log<StoredType>::reset(const genesis_state& gs, const stored_type_ptr& first_block) {
      // At startup, OK to be called in no log mode from controller.cpp
      std::lock_guard g(my->mtx);
      my->reset(gs, first_block);
   }

   template<typename StoredType>
   void block_log<StoredType>::reset(const chain_id_type& chain_id, uint32_t first_block_num) {
      std::lock_guard g(my->mtx);
      my->reset(chain_id, first_block_num);
   }

   template<typename StoredType>
   block_log<StoredType>::stored_type_ptr block_log<StoredType>::read_block_by_num(uint32_t block_num) const {
      std::lock_guard g(my->mtx);
      return my->read_block_by_num(block_num);
   }

   template<typename StoredType>
   std::optional<signed_block_header> block_log<StoredType>::read_block_header_by_num(uint32_t block_num) const {
      std::lock_guard g(my->mtx);
      return my->read_block_header_by_num(block_num);
   }

   template<typename StoredType>
   block_id_type block_log<StoredType>::read_block_id_by_num(uint32_t block_num) const {
      // read_block_header_by_num acquires mutex
      auto bh = read_block_header_by_num(block_num);
      if (bh) { return bh->calculate_id(); }
      return {};
   }

   template<typename StoredType>
   uint64_t block_log<StoredType>::get_block_pos(uint32_t block_num) const {
      std::lock_guard g(my->mtx);
      return my->get_block_pos(block_num);
   }

   template<typename StoredType>
   block_log<StoredType>::stored_type_ptr block_log<StoredType>::read_head() const {
      std::lock_guard g(my->mtx);
      return my->read_head();
   }

   template<typename StoredType>
   block_log<StoredType>::stored_type_ptr block_log<StoredType>::head() const {
      std::lock_guard g(my->mtx);
      return my->head ? my->head->ptr : stored_type_ptr{};
   }

   template<typename StoredType>
   std::optional<block_id_type> block_log<StoredType>::head_id() const {
      std::lock_guard g(my->mtx);
      return my->head ? my->head->id : std::optional<block_id_type>{};
   }

   template<typename StoredType>
   uint32_t block_log<StoredType>::first_block_num() const {
      std::lock_guard g(my->mtx);
      return my->first_block_num();
   }

   // static
   template<typename StoredType>
   void block_log<StoredType>::construct_index(const std::filesystem::path& block_file_name, const std::filesystem::path& index_file_name) {

      ilog("Will read existing log file ${file}", ("file", block_file_name));
      ilog("Will write new index file ${file}", ("file", index_file_name));

      block_log_data log_data;
      log_data.open(block_file_name);
      log_data.construct_index(index_file_name);
   }

   template<typename StoredType>
   std::tuple<uint64_t, uint32_t, std::string>
   block_log_data::full_validate_blocks(uint32_t last_block_num, const std::filesystem::path& blocks_dir, fc::time_point now) {
      uint64_t      pos       = first_block_position();
      uint32_t      block_num = first_block_num() - 1;
      std::string   error_msg;
      block_id_type block_id;

      file.seek(pos);
      SYS_ASSERT(!is_currently_pruned(), block_log_exception, "pruned block log cannot be repaired");
      try {
         try {
            StoredType entry;
            while (remaining() > 0 && block_num < last_block_num) {
               std::tie(block_num, block_id) = full_validate_block_entry<StoredType>(block_num, block_id, entry);
               if (block_num % 1000 == 0)
                  ilog("Verified block ${num}", ("num", block_num));
               pos = file.tellp();
            }
         } catch (const bad_block_exception& e) {
            detail::basic_block_log<StoredType>::write_incomplete_block_data(blocks_dir, now, block_num, ro_stream_at(pos));
            std::rethrow_exception(e.inner);
         }
      // clang-format off
      } catch (const std::exception& e) {
         error_msg = e.what();
      } catch (...) {
         error_msg = "unrecognized exception";
      }
      // clang-format on

      return { pos, block_num, error_msg };
   }

   // static
   template<typename StoredType>
   std::filesystem::path block_log<StoredType>::repair_log(const std::filesystem::path& data_dir, uint32_t truncate_at_block,
                                  const char* reversible_block_dir_name) {
      ilog("Recovering Block Log...");
      SYS_ASSERT(std::filesystem::is_directory(data_dir) && std::filesystem::is_regular_file(data_dir / log_filename<StoredType>()), block_log_not_found,
                 "Block log not found in '${blocks_dir}'", ("blocks_dir", data_dir));

      if (truncate_at_block == 0)
         truncate_at_block = UINT32_MAX;

      auto now        = fc::time_point::now();
      auto blocks_dir = std::filesystem::canonical(
            data_dir); // canonical always returns an absolute path that has no symbolic link, dot, or dot-dot elements
      auto blocks_dir_name = blocks_dir.filename();
      auto backup_dir      = blocks_dir.parent_path() / blocks_dir_name.generic_string().append("-").append(now.to_iso_string());

      SYS_ASSERT(!std::filesystem::exists(backup_dir), block_log_backup_dir_exist,
                 "Cannot move existing blocks directory to already existing directory '${new_blocks_dir}'",
                 ("new_blocks_dir", backup_dir));

      std::filesystem::create_directories(backup_dir);
      std::filesystem::rename(blocks_dir / log_filename<stored_type>(), backup_dir / log_filename<stored_type>());
      if (std::filesystem::exists(blocks_dir / index_filename<stored_type>())) {
         std::filesystem::rename(blocks_dir / index_filename<stored_type>(), backup_dir / index_filename<stored_type>());
      }
      if (strlen(reversible_block_dir_name) && std::filesystem::is_directory(blocks_dir / reversible_block_dir_name)) {
         std::filesystem::rename(blocks_dir / reversible_block_dir_name, backup_dir / reversible_block_dir_name);
      }
      ilog("Moved existing blocks directory to backup location: '${new_blocks_dir}'", ("new_blocks_dir", backup_dir));

      const auto block_log_path  = blocks_dir / log_filename<stored_type>();
      const auto block_index_path = blocks_dir / index_filename<stored_type>();
      const auto block_file_name = block_log_path.generic_string();
      const auto block_index_file_name = block_index_path.generic_string();

      ilog("Reconstructing '${new_block_log}' from backed up log", ("new_block_log", block_file_name));

      block_log_data log_data;
      log_data.open(backup_dir / log_filename<stored_type>().c_str());

      auto [pos, block_num, error_msg] = log_data.full_validate_blocks<StoredType>(truncate_at_block, blocks_dir, now);

      {
         fc::cfile new_block_file;
         new_block_file.set_file_path( block_log_path );
         new_block_file.open( fc::cfile::create_or_update_rw_mode );
         copy_file_content( log_data.ro_stream_at( 0 ), new_block_file, pos );
         new_block_file.close();
      }
      construct_index(block_log_path, block_index_path);

      if (error_msg.size()) {
         ilog("Recovered only up to block number ${num}. "
              "The block ${next_num} could not be deserialized from the block log due to error:\n${error_msg}",
              ("num", block_num)("next_num", block_num + 1)("error_msg", error_msg));
      } else if (block_num == truncate_at_block && pos < log_data.size()) {
         ilog("Stopped recovery of block log early at specified block number: ${stop}.", ("stop", truncate_at_block));
      } else {
         ilog("Existing block log was undamaged. Recovered all irreversible blocks up to block number ${num}.",
              ("num", block_num));
      }
      return backup_dir;
   }

   // static
   template<typename StoredType>
   std::optional<std::variant<genesis_state, chain_id_type>>
                                           block_log<StoredType>::extract_chain_context(const std::filesystem::path& block_dir,
                                                                            const std::filesystem::path& retained_dir) {
      std::filesystem::path first_block_file;
      if (!retained_dir.empty() && std::filesystem::exists(retained_dir)) {
         for_each_file_in_dir_matches(retained_dir, std::string("(") + filename_prefix<stored_type>() + "-1-\\d+\\.log)",
                                      [&](std::filesystem::path log_path) {
                                          first_block_file = std::move(log_path);
                                      });
      }

      if (first_block_file.empty() && std::filesystem::exists(block_dir / log_filename<stored_type>())) {
         first_block_file = block_dir / log_filename<stored_type>();
      }

      if (!first_block_file.empty()) {
         block_log_data log_data;
         log_data.open(first_block_file);
         return log_data.get_preamble().chain_context;
      }

      if (!retained_dir.empty() && std::filesystem::exists(retained_dir)) {
         const std::regex        my_filter(R"(" + filename_prefix<signed_block>() + R"-\d+-\d+\.log)");
         std::smatch             what;
         std::filesystem::directory_iterator end_itr; // Default ctor yields past-the-end
         for (std::filesystem::directory_iterator p(retained_dir); p != end_itr; ++p) {
            // Skip if not a file
            if (!std::filesystem::is_regular_file(p->status()))
               continue;
            // skip if it does not match the pattern
            std::string file = p->path().filename().string();
            if (!std::regex_match(file, what, my_filter))
               continue;
            block_log_data log_data;
            log_data.open(p->path());
            return log_data.chain_id();
         }
      }
      return {};
   }

   // static
   template<typename StoredType>
   std::optional<genesis_state> block_log<StoredType>::extract_genesis_state(const std::filesystem::path& block_dir,
                                                                 const std::filesystem::path& retained_dir) {
      auto context = extract_chain_context(block_dir, retained_dir);
      if (!context || std::holds_alternative<chain_id_type>(*context))
         return {};
      return std::get<genesis_state>(*context);
   }

   // static
   template<typename StoredType>
   std::optional<chain_id_type> block_log<StoredType>::extract_chain_id(const std::filesystem::path& block_dir,
                                                            const std::filesystem::path& retained_dir) {
      auto context = extract_chain_context(block_dir, retained_dir);
      if (!context)
         return {};
      return std::visit(overloaded{
         [](const chain_id_type& id){ return id; },
         [](const genesis_state& gs){ return gs.compute_chain_id(); }
          } , *context);
   }

   // static
   bool block_log_preamble::contains_genesis_state(uint32_t version, uint32_t first_block_num) {
      return version < genesis_state_or_chain_id_version || first_block_num == 1;
   }

   // static
   template<typename StoredType>
   bool block_log<StoredType>::contains_genesis_state(uint32_t version, uint32_t first_block_num) {
      return block_log_preamble::contains_genesis_state(version, first_block_num);
   }

   // static
   bool block_log_preamble::contains_chain_id(uint32_t version, uint32_t first_block_num) {
      return version >= genesis_state_or_chain_id_version && first_block_num > 1;
   }

   // static
   template<typename StoredType>
   bool block_log<StoredType>::contains_chain_id(uint32_t version, uint32_t first_block_num) {
      return block_log_preamble::contains_chain_id(version, first_block_num);
   }

   // static
   bool block_log_preamble::is_supported_version(uint32_t version) {
      return std::clamp(version, common_protocol::min_supported_version, common_protocol::max_supported_version) == version;
   }

   // static
   template<typename StoredType>
   bool block_log<StoredType>::is_pruned_log(const std::filesystem::path& data_dir) {
      uint32_t version = 0;
      try {
         fc::cfile log_file;
         log_file.set_file_path(data_dir / log_filename<stored_type>());
         log_file.open("rb");
         fc::raw::unpack(log_file, version);
      } catch (...) { return false; }
      return detail::is_pruned_log_and_mask_version(version);
   }

   template<typename StoredType>
   void extract_blocklog_i(block_log_bundle<StoredType>& log_bundle, const std::filesystem::path& new_block_filename, const std::filesystem::path& new_index_filename,
                           uint32_t first_block_num, uint32_t num_blocks) {

      auto position_for_block = [&log_bundle](uint64_t block_num) {
         uint64_t block_order = block_num - log_bundle.log_data.first_block_num();
         if (block_order < static_cast<uint64_t>(log_bundle.log_index.num_blocks()))
            return log_bundle.log_index.nth_block_position(block_order);
         return log_bundle.log_data.size();
      };

      first_block_num = std::max(first_block_num, log_bundle.log_data.first_block_num());
      num_blocks      = std::min(num_blocks, log_bundle.log_data.num_blocks());

      const auto     num_blocks_to_skip   = first_block_num - log_bundle.log_data.first_block_num();
      const uint64_t first_kept_block_pos = position_for_block(first_block_num);
      const uint64_t nbytes_to_trim =
            num_blocks_to_skip == 0 ? 0 : first_kept_block_pos - block_log_preamble::nbytes_with_chain_id;
      const uint64_t last_block_num      = first_block_num + num_blocks;
      const uint64_t last_block_pos      = position_for_block(last_block_num);
      const auto     new_block_file_size = last_block_pos - nbytes_to_trim;

      fc::datastream<fc::cfile> new_block_file;
      new_block_file.set_file_path(new_block_filename.generic_string());
      new_block_file.open(fc::cfile::truncate_rw_mode);

      if (num_blocks_to_skip == 0) {
         copy_file_content(log_bundle.log_data.ro_stream_at(0), new_block_file, new_block_file_size);
         fc::cfile new_index_file;
         new_index_file.set_file_path(new_index_filename.generic_string());
         new_index_file.open(fc::cfile::truncate_rw_mode);
         const uint64_t index_file_size = num_blocks * sizeof(uint64_t);
         log_bundle.log_index.copy_to(new_index_file, index_file_size);
         return;
      }

      block_log_preamble preamble;
      preamble.ver             = block_log<StoredType>::max_supported_version;
      preamble.first_block_num = first_block_num;
      preamble.chain_context   = log_bundle.log_data.chain_id();
      preamble.write_to(new_block_file);
      new_block_file.seek_end(0);
      copy_file_content(log_bundle.log_data.ro_stream_at(first_kept_block_pos), new_block_file,
                        last_block_pos - first_kept_block_pos);

      index_writer index(new_index_filename, num_blocks);
      adjust_block_positions(index, new_block_file, block_log_preamble::nbytes_with_chain_id, -nbytes_to_trim);
   }

   // static
   template<typename StoredType>
   bool block_log<StoredType>::trim_blocklog_front(const std::filesystem::path& block_dir, const std::filesystem::path& temp_dir,
                                       uint32_t truncate_at_block) {
      SYS_ASSERT(block_dir != temp_dir, block_log_exception, "block_dir and temp_dir need to be different directories");

      ilog("In directory ${dir} will trim all blocks before block ${n} from log and index file.",
           ("dir", block_dir)("n", truncate_at_block));

      block_log_bundle<StoredType> log_bundle(block_dir);

      if (truncate_at_block <= log_bundle.log_data.first_block_num()) {
         dlog("There are no blocks before block ${n} so do nothing.", ("n", truncate_at_block));
         return false;
      }
      if (truncate_at_block > log_bundle.log_data.last_block_num()) {
         dlog("All blocks are before block ${n} so do nothing (trim front would delete entire log file).",
              ("n", truncate_at_block));
         return false;
      }

      // ****** create the new block log file and write out the header for the file
      std::filesystem::create_directories(temp_dir);
      std::filesystem::path new_block_filename = temp_dir / log_filename<stored_type>();
      std::filesystem::path new_index_filename = temp_dir / index_filename<stored_type>();

      extract_blocklog_i<StoredType>(log_bundle, new_block_filename, new_index_filename, truncate_at_block,
                                     log_bundle.log_data.last_block_num() - truncate_at_block + 1);

      std::filesystem::path old_log = temp_dir / filename_prefix<stored_type>().append("_old.log");
      rename(log_bundle.block_file_name, old_log);
      rename(new_block_filename, log_bundle.block_file_name);
      std::filesystem::path old_ind = temp_dir / filename_prefix<stored_type>().append("_old.index");
      rename(log_bundle.index_file_name, old_ind);
      rename(new_index_filename, log_bundle.index_file_name);

      return true;
   }

   // static
   template<typename StoredType>
   int block_log<StoredType>::trim_blocklog_end(const std::filesystem::path& block_dir, uint32_t n) { // n is last block to keep (remove later blocks)

      block_log_bundle<StoredType> log_bundle(block_dir);

      ilog("In directory ${block_dir} will trim all blocks after block ${n} from ${block_file} and ${index_file}",
           ("block_dir", block_dir)("n", n)("block_file", log_bundle.block_file_name)(
                 "index_file", log_bundle.index_file_name));

      if (n < log_bundle.log_data.first_block_num()) {
         dlog("All blocks are after block ${n} so do nothing (trim_end would delete entire log)", ("n", n));
         return 1;
      }
      if (n > log_bundle.log_data.last_block_num()) {
         dlog("There are no blocks after block ${n} so do nothing", ("n", n));
         return 2;
      }
      if (n == log_bundle.log_data.last_block_num())
         return 0;

      const auto to_trim_block_index    = n + 1 - log_bundle.log_data.first_block_num();
      const auto to_trim_block_position = log_bundle.log_index.nth_block_position(to_trim_block_index);
      const auto index_file_size        = to_trim_block_index * sizeof(uint64_t);

      std::filesystem::resize_file(log_bundle.block_file_name, to_trim_block_position);
      std::filesystem::resize_file(log_bundle.index_file_name, index_file_size);
      ilog("index has been trimmed to ${index_file_size} bytes", ("index_file_size", index_file_size));
      return 0;
   }

   // static
   template<typename StoredType>
   void block_log<StoredType>::smoke_test(const std::filesystem::path& block_dir, uint32_t interval) {

      block_log_bundle<StoredType> log_bundle(block_dir, false);

      ilog("block log version= ${version}",("version", log_bundle.log_data.version()));
      ilog("first block= ${first}",("first", log_bundle.log_data.first_block_num()));
      ilog("last block= ${last}",("last", log_bundle.log_data.last_block_num()));

      log_bundle.validate_index();

      ilog("log and index agree on number of blocks");

      if (interval == 0) {
         interval = std::max((log_bundle.log_index.num_blocks() + 7) >> 3, 1U);
      }
      uint32_t expected_block_num = log_bundle.log_data.first_block_num();

      for (uint32_t pos = 0; pos < log_bundle.log_index.num_blocks(); pos += interval, expected_block_num += interval) {
         log_bundle.log_data.light_validate_block_entry_at(log_bundle.log_index.nth_block_position(pos),
                                                           expected_block_num);
      }
   }

   template<typename StoredType>
   std::pair<std::filesystem::path, std::filesystem::path> blocklog_files(const std::filesystem::path& dir, uint32_t start_block_num, uint32_t num_blocks) {
      const int bufsize = 64;
      char      buf[bufsize];
      snprintf(buf, bufsize, "%s-%u-%u.log", filename_prefix<StoredType>().c_str(), start_block_num, start_block_num + num_blocks - 1);
      std::filesystem::path new_block_filename = dir / buf;
      std::filesystem::path new_index_filename(new_block_filename);
      new_index_filename.replace_extension(".index");
      return std::make_pair(new_block_filename, new_index_filename);
   }

   // static
   template<typename StoredType>
   void block_log<StoredType>::extract_block_range(const std::filesystem::path& block_dir, const std::filesystem::path& dest_dir,
                                       block_num_type start_block_num, block_num_type last_block_num) {


      block_log_bundle<StoredType> log_bundle(block_dir);

      SYS_ASSERT(start_block_num >= log_bundle.log_data.first_block_num(), block_log_exception,
                 "The first available block is block ${first_block}.",
                 ("first_block", log_bundle.log_data.first_block_num()));

      if (!std::filesystem::exists(dest_dir))
         std::filesystem::create_directories(dest_dir);

      uint32_t num_blocks = last_block_num - start_block_num + 1;

      auto [new_block_filename, new_index_filename] = blocklog_files<StoredType>(dest_dir, start_block_num, num_blocks);

      extract_blocklog_i<StoredType>(log_bundle, new_block_filename, new_index_filename, start_block_num, num_blocks);
   }

   // static
   template<typename StoredType>
   void block_log<StoredType>::split_blocklog(const std::filesystem::path& block_dir, const std::filesystem::path& dest_dir, uint32_t stride) {

      block_log_bundle<StoredType> log_bundle(block_dir);
      const uint32_t   first_block_num = log_bundle.log_data.first_block_num();
      const uint32_t   last_block_num  = log_bundle.log_data.last_block_num();

      if (!std::filesystem::exists(dest_dir))
         std::filesystem::create_directories(dest_dir);

      for (uint32_t i = (first_block_num - 1) / stride; i < (last_block_num + stride - 1) / stride; ++i) {
         uint32_t start_block_num = std::max(i * stride + 1, first_block_num);
         uint32_t num_blocks      = std::min((i + 1) * stride, last_block_num) - start_block_num + 1;

         auto [new_block_filename, new_index_filename] = blocklog_files<StoredType>(dest_dir, start_block_num, num_blocks);

         extract_blocklog_i<StoredType>(log_bundle, new_block_filename, new_index_filename, start_block_num, num_blocks);
      }
   }

   inline std::filesystem::path operator+(const std::filesystem::path& left, const std::filesystem::path& right) { return std::filesystem::path(left) += right; }

   template<typename StoredType>
   void move_blocklog_files(const std::filesystem::path& src_dir, const std::filesystem::path& dest_dir, uint32_t start_block,
                            uint32_t end_block) {
      auto [new_log_filename, new_index_filename] = blocklog_files<StoredType>(dest_dir, start_block, end_block - start_block + 1);
      std::filesystem::rename(src_dir / (log_filename<StoredType>()), new_log_filename);
      std::filesystem::rename(src_dir / (index_filename<StoredType>()), new_index_filename);
   }

   inline uint32_t get_blocklog_version(const std::filesystem::path& blocklog_file) {
      uint32_t  version;
      fc::cfile f;
      f.set_file_path(blocklog_file.generic_string());
      f.open("r");
      f.read((char*)&version, sizeof(uint32_t));
      return version;
   }

   // static
   template<typename StoredType>
   void block_log<StoredType>::merge_blocklogs(const std::filesystem::path& blocks_dir, const std::filesystem::path& dest_dir) {
      block_log_catalog<StoredType> catalog;

      catalog.open(std::filesystem::path(""), blocks_dir, std::filesystem::path(""), filename_prefix<StoredType>().c_str(), R"(-\d+-\d+\.log)");
      if (catalog.collection.size() <= 1) {
         wlog("There's no more than one blocklog files in ${blocks_dir}, skip merge.", ("blocks_dir", blocks_dir));
         return;
      }

      if (!std::filesystem::exists(dest_dir))
         std::filesystem::create_directories(dest_dir);

      fc::temp_directory    temp_dir;
      std::filesystem::path temp_path   = temp_dir.path();
      uint32_t              start_block = 0, end_block = 0;

      std::filesystem::path     temp_block_log   = temp_path / log_filename<StoredType>();
      std::filesystem::path     temp_block_index = temp_path / index_filename<StoredType>();
      fc::datastream<fc::cfile> file;
      file.set_file_path(temp_block_log);

      for (auto const& [first_block_num, val] : catalog.collection) {
         if (std::filesystem::exists(temp_block_log)) {
            if (first_block_num == end_block + 1) {
               block_log_data log_data;
               log_data.open(val.filename_base + ".log");
               if (!file.is_open())
                  file.open(fc::cfile::update_rw_mode);
               file.seek_end(0);
               auto orig_log_size = file.tellp();
               copy_file_content(log_data.ro_stream_at(log_data.first_block_position()), file);
               file.flush();
               end_block = val.last_block_num;
               index_writer index(temp_block_index, end_block - start_block + 1, false);
               adjust_block_positions(index, file, orig_log_size, orig_log_size - log_data.first_block_position());
               file.flush();
               continue;

            } else
               wlog("${file}.log cannot be merged with previous block file because of the discontinuity of blocks, "
                    "skip merging.",
                    ("file", val.filename_base));
            // there is a version or block number gap between the stride files
            move_blocklog_files<StoredType>(temp_path, dest_dir, start_block, end_block);
         }

         std::filesystem::copy(val.filename_base + ".log", temp_block_log);
         std::filesystem::copy(val.filename_base + ".index", temp_block_index);
         start_block = first_block_num;
         end_block   = val.last_block_num;
      }

      if (file.is_open())
         file.close();

      if (std::filesystem::exists(temp_block_log)) {
         move_blocklog_files<StoredType>(temp_path, dest_dir, start_block, end_block);
      }
   }

   // Force instantiation of the two supported block_log implementations
   template class block_log<block_header_state>;
   template class block_log<signed_block>;

}} // namespace sysio::chain

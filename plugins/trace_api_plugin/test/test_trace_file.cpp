#include <boost/test/unit_test.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>
#include <sysio/trace_api/test_common.hpp>
#include <sysio/trace_api/store_provider.hpp>
#include <sysio/chain/config.hpp>
#include <fc/crypto/elliptic_ed.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;
using open_state = slice_directory::open_state;

namespace {
   struct test_fixture {

      std::vector<action_trace_v0> actions {
         {
            .global_sequence = 1,
            .receiver        = "receiver"_n,
            .account         = "contract"_n,
            .action          = "action"_n,
            .authorization   = {{ "alice"_n, "active"_n }},
            .data            = { 0x01, 0x01, 0x01, 0x01 },
            .return_value    = { 0x05, 0x05, 0x05, 0x05 }
         },
         {
            .global_sequence = 0,
            .receiver        = "receiver"_n,
            .account         = "contract"_n,
            .action          = "action"_n,
            .authorization   = {{ "alice"_n, "active"_n }},
            .data            = { 0x00, 0x00, 0x00, 0x00 },
            .return_value    = { 0x04, 0x04, 0x04, 0x04 }
         },
         {
            .global_sequence = 2,
            .receiver        = "receiver"_n,
            .account         = "contract"_n,
            .action          = "action"_n,
            .authorization   = {{ "alice"_n, "active"_n }},
            .data            = { 0x02, 0x02, 0x02, 0x02 },
            .return_value    = { 0x06, 0x06, 0x06, 0x06 }
         }
      };

      transaction_trace_v0 transaction_trace {
         "0000000000000000000000000000000000000000000000000000000000000001"_h,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 }
      };

      block_trace_v0 block_trace1 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            transaction_trace
         }
      };

      block_trace_v0 block_trace2 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         5,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            transaction_trace
         }
      };

      const block_trace_v0 bt1 {
         .id                = "0000000000000000000000000000000000000000000000000000000000000001"_h,
         .number            = 1,
         .previous_id       = "0000000000000000000000000000000000000000000000000000000000000003"_h,
         .timestamp         = chain::block_timestamp_type(1),
         .producer          = "bp.one"_n,
         .transaction_mroot = "0000000000000000000000000000000000000000000000000000000000000000"_h,
         .finality_mroot    = "0000000000000000000000000000000000000000000000000000000000000000"_h,
         .transactions = {
            transaction_trace_v0 {
               .id              = "0000000000000000000000000000000000000000000000000000000000000001"_h,
               .actions = {
                  {
                     .global_sequence = 0,
                     .receiver        = "sysio.token"_n,
                     .account         = "sysio.token"_n,
                     .action          = "transfer"_n,
                     .authorization   = {{ "alice"_n, "active"_n }},
                     .data            = make_transfer_data( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" )
                  },
                  {
                     .global_sequence = 1,
                     .receiver        = "alice"_n,
                     .account         = "sysio.token"_n,
                     .action          = "transfer"_n,
                     .authorization   = {{ "alice"_n, "active"_n }},
                     .data            = make_transfer_data( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" )
                  },
                  {
                     .global_sequence = 2,
                     .receiver        = "bob"_n,
                     .account         = "sysio.token"_n,
                     .action          = "transfer"_n,
                     .authorization   = {{ "alice"_n, "active"_n }},
                     .data            = make_transfer_data( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" )
                  }
               },
               .cpu_usage_us    = 10,
               .net_usage_words = 5,
               .signatures      = {chain::signature_type()},
               .trx_header      = chain::transaction_header{chain::time_point_sec(), 1, 0, 100, 50, 0}
            }
         }
      };

      const block_trace_v0 bt2 {
         "0000000000000000000000000000000000000000000000000000000000000002"_h,
         5,
         "0000000000000000000000000000000000000000000000000000000000000005"_h,
         chain::block_timestamp_type(2),
         "bp.two"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         {
            {
               "f000000000000000000000000000000000000000000000000000000000000004"_h,
               {},
               10,
               5,
               std::vector<chain::signature_type>{chain::signature_type()},
               chain::transaction_header{chain::time_point_sec(), 1, 0, 100, 50, 0}
            }
         }
      };

      const metadata_log_entry be1 { block_entry_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h, 5, 0
      } };
      const metadata_log_entry le1 { lib_entry_v0 { 4 } };
      const metadata_log_entry be2 { block_entry_v0 {
         "b000000000000000000000000000000000000000000000000000000000000002"_h, 7, 0
      } };
      const metadata_log_entry le2 { lib_entry_v0 { 5 } };

      bool create_non_empty_trace_slice( slice_directory& sd, uint32_t slice_number, fc::cfile& file) {
         const uint8_t bad_which = 0x7F;
         if (!sd.find_or_create_trace_slice(slice_number, open_state::write, file)) {
            file.write(reinterpret_cast<const char*>(&bad_which), sizeof(uint8_t));
            file.close();
            return sd.find_or_create_trace_slice(slice_number, open_state::read, file);
         }
         return false;
      }
   };

   struct test_store_provider : public store_provider {
      test_store_provider(const std::filesystem::path& slice_dir, uint32_t width, std::optional<uint32_t> minimum_irreversible_history_blocks = std::optional<uint32_t>(), std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks = std::optional<uint32_t>(), size_t compression_seek_point_stride = 0)
         : store_provider(slice_dir, width, minimum_irreversible_history_blocks, minimum_uncompressed_irreversible_history_blocks, compression_seek_point_stride) {
      }
      using store_provider::scan_metadata_log_from;
      using store_provider::read_data_log;
      using store_provider::_slice_directory;
   };

   class vslice_datastream;

   struct vslice {
      enum mode { read_mode, write_mode};
      vslice(mode m = write_mode) : _mode(m) {}
      unsigned long tellp() const {
         return _pos;
      }

      void seek( unsigned long loc ) {
         if (_mode == read_mode) {
            if (loc > _buffer.size()) {
               throw std::ios_base::failure( "read vslice unable to seek to: " + std::to_string(loc) + ", end is at: " + std::to_string(_buffer.size()));
            }
         }
         _pos = loc;
      }

      void seek_end( long loc ) {
         _pos = _buffer.size();
      }

      void read( char* d, size_t n ) {
         if( _pos + n > _buffer.size() ) {
            throw std::ios_base::failure( "vslice unable to read " + std::to_string( n ) + " bytes; only can read " + std::to_string( _buffer.size() - _pos ) );
         }
         std::memcpy( d, _buffer.data() + _pos, n);
         _pos += n;
      }

      void write( const char* d, size_t n ) {
         if (_mode == read_mode) {
            throw std::ios_base::failure( "read vslice should not have write called" );
         }
         if (_buffer.size() < _pos + n) {
            _buffer.resize(_pos + n);
         }
         std::memcpy( _buffer.data() + _pos, d, n);
         _pos += n;
      }

      void flush() {
         _flush = true;
      }

      void sync() {
         _sync = true;
      }

      vslice_datastream create_datastream();

      std::vector<char> _buffer;
      mode _mode = write_mode;
      unsigned long _pos = 0lu;
      bool _flush = false;
      bool _sync = false;
   };

   class vslice_datastream {
   public:
      explicit vslice_datastream( vslice& vs ) : _vs(vs) {}

      void skip( size_t s ) {
         std::vector<char> d( s );
         read( &d[0], s );
      }

      bool read( char* d, size_t s ) {
         _vs.read( d, s );
         return true;
      }

      bool get( unsigned char& c ) { return get( *(char*)&c ); }

      bool get( char& c ) { return read(&c, 1); }

   private:
      vslice& _vs;
   };

   inline vslice_datastream vslice::create_datastream() {
      return vslice_datastream(*this);
   }

   // vslice_datastream support for Ed25519 shim types
   [[maybe_unused]] inline vslice_datastream&
   operator>>( vslice_datastream& ds, ::fc::crypto::ed::public_key_shim& pk ) {
      ds.read(reinterpret_cast<char*>(pk._data.data()), crypto_sign_PUBLICKEYBYTES);
      return ds;
   }

   [[maybe_unused]] inline vslice_datastream&
   operator>>( vslice_datastream& ds, ::fc::crypto::ed::private_key_shim& sk ) {
      ds.read(reinterpret_cast<char*>(sk._data.data()), crypto_sign_SECRETKEYBYTES);
      return ds;
   }

   inline vslice_datastream&
   operator>>( vslice_datastream& ds, ::fc::crypto::ed::signature_shim& sig ) {
      ds.read(reinterpret_cast<char*>(sig._data.data()), crypto_sign_BYTES);
      uint8_t pad = 0; // consume padding byte
      ds.read(reinterpret_cast<char*>(&pad), 1);
      return ds;
   }
}

BOOST_AUTO_TEST_SUITE(slice_tests)
   BOOST_FIXTURE_TEST_CASE(write_data_trace, test_fixture)
   {
      vslice vs;
      const auto offset = append_store(bt1, vs );
      BOOST_REQUIRE_EQUAL(offset,0u);

      const auto offset2 = append_store(bt2, vs );
      BOOST_REQUIRE(offset < offset2);

      vs._pos = offset;
      const auto bt_returned = extract_store<block_trace_v0>( vs );
      BOOST_REQUIRE(bt_returned == bt1);

      vs._pos = offset2;
      const auto bt_returned2 = extract_store<block_trace_v0>( vs );
      BOOST_REQUIRE(bt_returned2 == bt2);
   }

   BOOST_FIXTURE_TEST_CASE(write_metadata_trace, test_fixture)
   {
      vslice vs;
      const auto offset = append_store( be1, vs );
      auto next_offset = vs._pos;
      BOOST_REQUIRE(offset < next_offset);
      const auto offset2 = append_store( le1, vs );
      BOOST_REQUIRE(next_offset <= offset2);
      BOOST_REQUIRE(offset2 < vs._pos);
      next_offset = vs._pos;
      const auto offset3 = append_store( be2, vs );
      BOOST_REQUIRE(next_offset <= offset3);
      BOOST_REQUIRE(offset3 < vs._pos);
      next_offset = vs._pos;
      const auto offset4 = append_store( le2, vs );
      BOOST_REQUIRE(next_offset <= offset4);
      BOOST_REQUIRE(offset4 < vs._pos);

      vs._pos = offset;
      const auto be_returned1 = extract_store<metadata_log_entry>( vs );
      BOOST_REQUIRE(std::holds_alternative<block_entry_v0>(be_returned1));
      const auto real_be_returned1 = std::get<block_entry_v0>(be_returned1);
      const auto real_be1 = std::get<block_entry_v0>(be1);
      BOOST_REQUIRE(real_be_returned1 == real_be1);

      vs._pos = offset2;
      const auto le_returned1 = extract_store<metadata_log_entry>( vs );
      BOOST_REQUIRE(std::holds_alternative<lib_entry_v0>(le_returned1));
      const auto real_le_returned1 = std::get<lib_entry_v0>(le_returned1);
      const auto real_le1 = std::get<lib_entry_v0>(le1);
      BOOST_REQUIRE(real_le_returned1 == real_le1);

      vs._pos = offset3;
      const auto be_returned2 = extract_store<metadata_log_entry>( vs );
      BOOST_REQUIRE(std::holds_alternative<block_entry_v0>(be_returned2));
      const auto real_be_returned2 = std::get<block_entry_v0>(be_returned2);
      const auto real_be2 = std::get<block_entry_v0>(be2);
      BOOST_REQUIRE(real_be_returned2 == real_be2);

      vs._pos = offset4;
      const auto le_returned2 = extract_store<metadata_log_entry>( vs );
      BOOST_REQUIRE(std::holds_alternative<lib_entry_v0>(le_returned2));
      const auto real_le_returned2 = std::get<lib_entry_v0>(le_returned2);
      const auto real_le2 = std::get<lib_entry_v0>(le2);
      BOOST_REQUIRE(real_le_returned2 == real_le2);
   }

   BOOST_FIXTURE_TEST_CASE(slice_number, test_fixture)
   {
      fc::temp_directory tempdir;
      slice_directory sd(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      BOOST_REQUIRE_EQUAL(sd.slice_number(99), 0u);
      BOOST_REQUIRE_EQUAL(sd.slice_number(100), 1u);
      BOOST_REQUIRE_EQUAL(sd.slice_number(1599), 15u);
      slice_directory sd2(tempdir.path(), 0x10, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      BOOST_REQUIRE_EQUAL(sd2.slice_number(0xf), 0u);
      BOOST_REQUIRE_EQUAL(sd2.slice_number(0x100), 0x10u);
      BOOST_REQUIRE_EQUAL(sd2.slice_number(0x233), 0x23u);
   }

   BOOST_FIXTURE_TEST_CASE(slice_file, test_fixture)
   {
      fc::temp_directory tempdir;
      slice_directory sd(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      fc::cfile slice;

      // create trace slices
      for (uint i = 0; i < 9; ++i) {
         bool found = sd.find_or_create_trace_slice(i, open_state::write, slice);
         BOOST_REQUIRE(!found);
         std::filesystem::path fp = slice.get_file_path();
         BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
         const std::string expected_filename = "trace_0000000" + std::to_string(i) + "00-0000000" + std::to_string(i+1) + "00.log";
         BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
         BOOST_REQUIRE(slice.is_open());
         BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), 0u);
         BOOST_REQUIRE_EQUAL(slice.tellp(), 0u);
         slice.close();
      }

      // create trace index slices
      for (uint i = 0; i < 9; ++i) {
         bool found = sd.find_or_create_index_slice(i, open_state::write, slice);
         BOOST_REQUIRE(!found);
         std::filesystem::path fp = slice.get_file_path();
         BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
         const std::string expected_filename = "trace_index_0000000" + std::to_string(i) + "00-0000000" + std::to_string(i+1) + "00.log";
         BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
         BOOST_REQUIRE(slice.is_open());
         slice_directory::index_header h;
         const auto data = fc::raw::pack(h);
         BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), data.size());
         BOOST_REQUIRE_EQUAL(slice.tellp(), data.size());
         slice.close();
      }

      // reopen trace slice for append
      bool found = sd.find_or_create_trace_slice(0, open_state::write, slice);
      BOOST_REQUIRE(found);
      std::filesystem::path fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
      std::string expected_filename = "trace_0000000000-0000000100.log";
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), 0u);
      BOOST_REQUIRE_EQUAL(slice.tellp(), 0u);
      uint64_t offset = append_store(bt1, slice);
      BOOST_REQUIRE_EQUAL(offset, 0u);
      auto data = fc::raw::pack(bt1);
      BOOST_REQUIRE(slice.tellp() > 0u);
      BOOST_REQUIRE_EQUAL(data.size(), slice.tellp());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), slice.tellp());
      uint64_t trace_file_size = std::filesystem::file_size(fp);
      slice.close();

      // open same file for read
      found = sd.find_or_create_trace_slice(0, open_state::read, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), trace_file_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), 0u);
      slice.close();

      // open same file for append again
      found = sd.find_or_create_trace_slice(0, open_state::write, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), trace_file_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), trace_file_size);
      slice.close();

      // reopen trace index slice for append
      found = sd.find_or_create_index_slice(1, open_state::write, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
      expected_filename = "trace_index_0000000100-0000000200.log";
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      slice_directory::index_header h;
      data = fc::raw::pack(h);
      const uint64_t header_size = data.size();
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), header_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), header_size);
      offset = append_store(be1, slice);
      BOOST_REQUIRE_EQUAL(offset, header_size);
      data = fc::raw::pack(be1);
      const auto be1_size = data.size();
      BOOST_REQUIRE_EQUAL(header_size + be1_size, slice.tellp());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), slice.tellp());
      slice.close();

      found = sd.find_or_create_index_slice(1, open_state::read, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), header_size + be1_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), header_size);
      slice.close();

      found = sd.find_or_create_index_slice(1, open_state::write, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), header_size + be1_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), header_size + be1_size);
      offset = append_store(le1, slice);
      BOOST_REQUIRE_EQUAL(offset, header_size + be1_size);
      data = fc::raw::pack(le1);
      const auto le1_size = data.size();
      BOOST_REQUIRE_EQUAL(header_size + be1_size + le1_size, slice.tellp());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), slice.tellp());
      slice.close();

      found = sd.find_or_create_index_slice(1, open_state::read, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), header_size + be1_size + le1_size);
      BOOST_REQUIRE_EQUAL(slice.tellp(), header_size);
      slice.close();
   }

   BOOST_FIXTURE_TEST_CASE(slice_file_find_test, test_fixture)
   {
      fc::temp_directory tempdir;
      slice_directory sd(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      fc::cfile slice;

      // create trace slice
      bool found = sd.find_or_create_trace_slice(1, open_state::write, slice);
      BOOST_REQUIRE(!found);
      std::filesystem::path fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
      const std::string expected_filename = "trace_0000000100-0000000200.log";
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), 0u);
      BOOST_REQUIRE_EQUAL(slice.tellp(), 0u);
      slice.close();

      // find trace slice (and open)
      found = sd.find_trace_slice(1, open_state::write, slice);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), 0u);
      slice.close();

      // find trace slice (and don't open)
      found = sd.find_trace_slice(1, open_state::write, slice, false);
      BOOST_REQUIRE(found);
      fp = slice.get_file_path();
      BOOST_REQUIRE_EQUAL(fp.parent_path().generic_string(), tempdir.path().generic_string());
      BOOST_REQUIRE_EQUAL(fp.filename().generic_string(), expected_filename);
      BOOST_REQUIRE(!slice.is_open());
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(fp), 0u);
      slice.close();
   }

   void verify_directory_contents(const std::filesystem::path& tempdir, std::set<std::filesystem::path> expected_files) {
      std::set<std::filesystem::path> unexpected_files;
      for (std::filesystem::directory_iterator itr(tempdir); itr != std::filesystem::directory_iterator(); ++itr) {
         const auto filename = itr->path().filename();
         if (expected_files.erase(filename) < 1) {
            unexpected_files.insert(filename);
         }
      }
      if (expected_files.size() + unexpected_files.size() == 0u)
         return;

      std::string msg;
      if (expected_files.size()) {
         msg += " Expected the following files to be present, but were not:";
      }
      bool comma = false;
      for(auto file : expected_files) {
         if (comma)
            msg += ",";
         msg += " " + file.generic_string();
      }
      if (unexpected_files.size()) {
         msg += " Did not expect the following files to be present, but they were:";
      }
      for(auto file : expected_files) {
         if (comma)
            msg += ",";
         msg += " " + file.generic_string();
      }
      BOOST_FAIL(msg);
   }

   BOOST_FIXTURE_TEST_CASE(slice_dir_cleanup_height_less_than_width, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      const uint32_t min_saved_blocks = 5;
      slice_directory sd(tempdir.path(), width, std::optional<uint32_t>(min_saved_blocks), std::optional<uint32_t>(), 0);
      fc::cfile file;

      // verify it cleans up when there is just an index file, just a trace file, or when both are there
      // verify it cleans up all slices that need to be cleaned
      std::set<std::filesystem::path> files;
      BOOST_REQUIRE(!sd.find_or_create_index_slice(0, open_state::read, file));
      files.insert(file.get_file_path().filename());
      verify_directory_contents(tempdir.path(), files);
      BOOST_REQUIRE(!sd.find_or_create_trace_slice(0, open_state::read, file));
      files.insert(file.get_file_path().filename());
      BOOST_REQUIRE(!sd.find_or_create_index_slice(1, open_state::read, file));
      files.insert(file.get_file_path().filename());
      BOOST_REQUIRE(!sd.find_or_create_trace_slice(2, open_state::read, file));
      files.insert(file.get_file_path().filename());
      BOOST_REQUIRE(!sd.find_or_create_index_slice(3, open_state::read, file));
      files.insert(file.get_file_path().filename());
      BOOST_REQUIRE(!sd.find_or_create_index_slice(4, open_state::read, file));
      const auto index4 = file.get_file_path().filename();
      files.insert(index4);
      BOOST_REQUIRE(!sd.find_or_create_trace_slice(4, open_state::read, file));
      const auto trace4 = file.get_file_path().filename();
      files.insert(trace4);
      BOOST_REQUIRE(!sd.find_or_create_index_slice(5, open_state::read, file));
      const auto index5 = file.get_file_path().filename();
      files.insert(index5);
      BOOST_REQUIRE(!sd.find_or_create_trace_slice(6, open_state::read, file));
      const auto trace6 = file.get_file_path().filename();
      files.insert(trace6);
      verify_directory_contents(tempdir.path(), files);

      // verify that the current_slice and the previous are maintained as long as lib - min_saved_blocks is part of previous slice
      uint32_t current_slice = 6;
      uint32_t lib = current_slice * width;
      sd.run_maintenance_tasks(lib, {});
      std::set<std::filesystem::path> files2;
      files2.insert(index5);
      files2.insert(trace6);
      verify_directory_contents(tempdir.path(), files2);

      // saved blocks still in previous slice
      lib += min_saved_blocks - 1;  // current_slice * width + min_saved_blocks - 1
      sd.run_maintenance_tasks(lib, {});
      verify_directory_contents(tempdir.path(), files2);

      // now all saved blocks in current slice
      lib += 1; // current_slice * width + min_saved_blocks
      sd.run_maintenance_tasks(lib, {});
      std::set<std::filesystem::path> files3;
      files3.insert(trace6);
      verify_directory_contents(tempdir.path(), files3);

      // moving lib into next slice, so 1 saved blocks still in 6th slice
      lib += width - 1;
      sd.run_maintenance_tasks(lib, {});
      verify_directory_contents(tempdir.path(), files3);

      // moved last saved block out of 6th slice, so 6th slice is cleaned up
      lib += 1;
      sd.run_maintenance_tasks(lib, {});
      verify_directory_contents(tempdir.path(), std::set<std::filesystem::path>());
   }

   BOOST_FIXTURE_TEST_CASE(slice_dir_compress, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      const uint32_t min_uncompressed_blocks = 5;
      slice_directory sd(tempdir.path(), width, std::optional<uint32_t>(), std::optional<uint32_t>(min_uncompressed_blocks), 8);
      fc::cfile file;

      using file_vector_t = std::vector<std::tuple<std::filesystem::path, std::filesystem::path, std::filesystem::path>>;
      file_vector_t file_paths;
      for (int i = 0; i < 7 ; i++) {
         BOOST_REQUIRE(!sd.find_or_create_index_slice(i, open_state::read, file));
         auto index_name = file.get_file_path().filename();
         BOOST_REQUIRE(create_non_empty_trace_slice(sd, i, file));
         auto trace_name = file.get_file_path().filename();
         auto compressed_trace_name = trace_name;
         compressed_trace_name.replace_extension(".clog");
         file_paths.emplace_back(index_name, trace_name, compressed_trace_name);
      }

      // initial set is only indices and uncompressed traces
      std::set<std::filesystem::path> files;
      for (const auto& e: file_paths) {
         files.insert(std::get<0>(e));
         files.insert(std::get<1>(e));
      }
      verify_directory_contents(tempdir.path(), files);

      // verify no change up to the last block before a slice becomes compressible
      sd.run_maintenance_tasks(14, {});
      verify_directory_contents(tempdir.path(), files);

      for (std::size_t reps = 0; reps < file_paths.size(); reps++) {
         //  leading edge,
         //  compresses one slice
         files.erase(std::get<1>(file_paths.at(reps)));
         files.insert(std::get<2>(file_paths.at(reps)));

         sd.run_maintenance_tasks(15 + (reps * width), {});
         verify_directory_contents(tempdir.path(), files);

         // trailing edge, no change
         sd.run_maintenance_tasks(24 + (reps * width), {});
         verify_directory_contents(tempdir.path(), files);
      }

      // make sure the test is correct and and no uncompressed files remain
      for (const auto& e: file_paths) {
         BOOST_REQUIRE_EQUAL(files.count(std::get<0>(e)), 1u);
         BOOST_REQUIRE_EQUAL(files.count(std::get<1>(e)), 0u);
         BOOST_REQUIRE_EQUAL(files.count(std::get<2>(e)), 1u);
      }
   }

   BOOST_FIXTURE_TEST_CASE(slice_dir_compress_and_delete, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      const uint32_t min_uncompressed_blocks = 5;
      const uint32_t min_saved_blocks = min_uncompressed_blocks + width;
      slice_directory sd(tempdir.path(), width, std::optional<uint32_t>(min_saved_blocks), std::optional<uint32_t>(min_uncompressed_blocks), 8);
      fc::cfile file;

      using file_vector_t = std::vector<std::tuple<std::filesystem::path, std::filesystem::path, std::filesystem::path>>;
      file_vector_t file_paths;
      for (int i = 0; i < 7 ; i++) {
         BOOST_REQUIRE(!sd.find_or_create_index_slice(i, open_state::read, file));
         auto index_name = file.get_file_path().filename();
         BOOST_REQUIRE(create_non_empty_trace_slice(sd, i, file));
         auto trace_name = file.get_file_path().filename();
         auto compressed_trace_name = trace_name;
         compressed_trace_name.replace_extension(".clog");
         file_paths.emplace_back(index_name, trace_name, compressed_trace_name);
      }

      // initial set is only indices and uncompressed traces
      std::set<std::filesystem::path> files;
      for (const auto& e: file_paths) {
         files.insert(std::get<0>(e));
         files.insert(std::get<1>(e));
      }
      verify_directory_contents(tempdir.path(), files);

      // verify no change up to the last block before a slice becomes compressible
      sd.run_maintenance_tasks(14, {});
      verify_directory_contents(tempdir.path(), files);

      for (std::size_t reps = 0; reps < file_paths.size() + 1; reps++) {
         //  leading edge,
         //  compresses one slice IF its not past the end of our test,
         if (reps < file_paths.size()) {
            files.erase(std::get<1>(file_paths.at(reps)));
            files.insert(std::get<2>(file_paths.at(reps)));
         }

         // removes one IF its not the first
         if (reps > 0) {
            files.erase(std::get<0>(file_paths.at(reps-1)));
            files.erase(std::get<2>(file_paths.at(reps-1)));
         }
         sd.run_maintenance_tasks(15 + (reps * width), {});
         verify_directory_contents(tempdir.path(), files);

         // trailing edge, no change
         sd.run_maintenance_tasks(24 + (reps * width), {});
         verify_directory_contents(tempdir.path(), files);
      }

      // make sure the test is correct and ran through the permutations
      BOOST_REQUIRE_EQUAL(files.size(), 0u);
   }

   BOOST_FIXTURE_TEST_CASE(store_provider_write_read, test_fixture)
   {
      fc::temp_directory tempdir;
      test_store_provider sp(tempdir.path(), 100);
      sp.append(block_trace1);
      sp.append_lib(54);
      sp.append(block_trace2);
      const uint32_t bt_bn = block_trace1.number;
      bool found_block = false;
      bool lib_seen = false;
      const uint64_t first_offset = sp.scan_metadata_log_from(9, 0, [&](const metadata_log_entry& e) -> bool {
         if (std::holds_alternative<block_entry_v0>(e)) {
            const auto& block = std::get<block_entry_v0>(e);
            if (block.number == bt_bn) {
               BOOST_REQUIRE(!found_block);
               found_block = true;
            }
         } else if (std::holds_alternative<lib_entry_v0>(e)) {
            auto best_lib = std::get<lib_entry_v0>(e);
            BOOST_REQUIRE(!lib_seen);
            BOOST_REQUIRE_EQUAL(best_lib.lib, 54u);
            lib_seen = true;
            return false;
         }
         return true;
      }, []() {});
      BOOST_REQUIRE(found_block);
      BOOST_REQUIRE(lib_seen);

      std::vector<uint32_t> block_nums;
      std::vector<uint64_t> block_offsets;
      lib_seen = false;
      uint64_t offset = sp.scan_metadata_log_from(9, 0, [&](const metadata_log_entry& e) -> bool {
         if (std::holds_alternative<block_entry_v0>(e)) {
            const auto& block = std::get<block_entry_v0>(e);
            block_nums.push_back(block.number);
            block_offsets.push_back(block.offset);
         } else if (std::holds_alternative<lib_entry_v0>(e)) {
            auto best_lib = std::get<lib_entry_v0>(e);
            BOOST_REQUIRE(!lib_seen);
            BOOST_REQUIRE_EQUAL(best_lib.lib, 54u);
            lib_seen = true;
         }
         return true;
      }, []() {});
      BOOST_REQUIRE(lib_seen);
      BOOST_REQUIRE_EQUAL(block_nums.size(), 2u);
      BOOST_REQUIRE_EQUAL(block_nums[0], block_trace1.number);
      BOOST_REQUIRE_EQUAL(block_nums[1], block_trace2.number);
      BOOST_REQUIRE_EQUAL(block_offsets.size(), 2u);
      BOOST_REQUIRE(block_offsets[0] < block_offsets[1]);
      BOOST_REQUIRE(first_offset < offset);

      std::optional<data_log_entry> bt_data = sp.read_data_log(block_nums[0], block_offsets[0]);
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(*bt_data), block_trace1);

      bt_data = sp.read_data_log(block_nums[1], block_offsets[1]);
      BOOST_REQUIRE(bt_data);
      auto v = data_log_entry(*bt_data);
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(v), block_trace2);

      block_nums.clear();
      block_offsets.clear();
      lib_seen = false;
      int counter = 0;
      try {
         offset = sp.scan_metadata_log_from(9, 0, [&](const metadata_log_entry& e) -> bool {
            if (std::holds_alternative<block_entry_v0>(e)) {
               const auto& block = std::get<block_entry_v0>(e);
               block_nums.push_back(block.number);
               block_offsets.push_back(block.offset);
            } else if (std::holds_alternative<lib_entry_v0>(e)) {
               auto best_lib = std::get<lib_entry_v0>(e);
               BOOST_REQUIRE(!lib_seen);
               BOOST_REQUIRE_EQUAL(best_lib.lib, 54u);
               lib_seen = true;
            }
            return true;
         }, [&counter]() {
            if( ++counter == 3 ) {
               throw yield_exception("");
            }
         });
         BOOST_FAIL("Should not have completed scan");
      } catch (const yield_exception& ex) {
      }
      BOOST_REQUIRE(lib_seen);
      BOOST_REQUIRE_EQUAL(block_nums.size(), 1u);
      BOOST_REQUIRE_EQUAL(block_nums[0], block_trace1.number);
      BOOST_REQUIRE_EQUAL(block_offsets.size(), 1u);
      BOOST_REQUIRE(first_offset < offset);
   }

   BOOST_FIXTURE_TEST_CASE(test_get_block, test_fixture)
   {
      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      sp.append(block_trace1);
      sp.append_lib(1);
      sp.append(block_trace2);

      // get_block uses the trace_blk_idx_<range>.log sidecar for O(1) lookup and does
      // not iterate, so the yield callback is not invoked on the fast path.
      get_block_t block1 = sp.get_block(1, [](){ BOOST_FAIL("yield must not be called on sidecar fast path"); });
      BOOST_REQUIRE(block1);
      BOOST_REQUIRE(std::get<1>(*block1));
      const auto block1_bt = std::get<0>(*block1);
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(block1_bt), block_trace1);

      get_block_t block2 = sp.get_block(5, [](){ BOOST_FAIL("yield must not be called on sidecar fast path"); });
      BOOST_REQUIRE(block2);
      BOOST_REQUIRE(!std::get<1>(*block2));
      const auto block2_bt = std::get<0>(*block2);
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(block2_bt), block_trace2);

      // Missing block: sidecar slot is empty, we fall back to scanning the metadata log.
      get_block_t block_missing = sp.get_block(2);
      BOOST_REQUIRE(!block_missing);
   }

   // Sidecar must be removed when its slice is cleaned up.
   BOOST_FIXTURE_TEST_CASE(test_blk_offset_sidecar_cleanup, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 100;
      slice_directory sd(tempdir.path(), width, /*min_irr=*/0u, std::optional<uint32_t>(), 0);

      // Create sidecar + matching trace/index for slice 0 so cleanup has something to do.
      sd.write_block_offset(1, 0);
      fc::cfile f;
      sd.find_or_create_trace_slice(0, open_state::read, f);
      sd.find_or_create_index_slice(0, open_state::read, f);

      const auto sidecar = tempdir.path() / "trace_blk_idx_0000000000-0000000100.log";
      BOOST_REQUIRE(std::filesystem::exists(sidecar));

      // Advance LIB several slices past 0 so slice 0 is rotated out.
      sd.run_maintenance_tasks(width * 5, [](auto&&){});

      BOOST_REQUIRE(!std::filesystem::exists(sidecar));
   }

   // Fork re-writes the block to a new offset.  The sidecar slot must be overwritten so
   // get_block returns the latest (fork-resolved) copy, matching the scan-based "last wins".
   BOOST_FIXTURE_TEST_CASE(test_blk_offset_fork_rewrite, test_fixture)
   {
      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      // Different block bodies but same block number.  Simulates a fork re-applying block 1.
      block_trace_v0 forked = block_trace1;
      forked.producer = "bp.two"_n;

      sp.append(block_trace1);
      sp.append(forked);

      auto result = sp.get_block(1);
      BOOST_REQUIRE(result);
      const auto bt = std::get<block_trace_v0>(std::get<0>(*result));
      BOOST_REQUIRE_EQUAL(bt, forked);
   }

// Verify basics of get_trx_block_number()
   BOOST_FIXTURE_TEST_CASE(test_get_trx_block_number_basic, test_fixture)
   {
      chain::transaction_id_type trx_id1 = "0000000000000000000000000000000000000000000000000000000000000001"_h;
      chain::transaction_id_type trx_id2 = "0000000000000000000000000000000000000000000000000000000000000002"_h;
      uint32_t block_num1 = 1;
      uint32_t block_num2 = 2;

      transaction_trace_v0 trx_trace1 {
         trx_id1,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 }
      };

      transaction_trace_v0 trx_trace2 {
         trx_id2,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 }
      };

      // block 1 includes trx_trace1
      block_trace_v0 block_trace_1 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         block_num1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "test"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            trx_trace1
         }
      };

      // block 2 includes trx_trace2
      block_trace_v0 block_trace_2 {
         "b000000000000000000000000000000000000000000000000000000000000003"_h,
         block_num2,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "test"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            trx_trace2
         }
      };

      block_trxs_entry block_trxs_entry1 {
         .ids       = {trx_id1},
         .block_num = block_num1
      };

      block_trxs_entry block_trxs_entry2 {
         .ids       = {trx_id2},
         .block_num = block_num2
      };

      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      // on_accepted_block of block 1
      sp.append(block_trace_1);
      sp.append_trx_ids(block_trxs_entry1);

      // block 1 is reversible and get_trx_block_number should find trx_id1 in block 1
      get_block_n block_num = sp.get_trx_block_number(trx_id1, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num1);

      // block 1 becomes final
      sp.append_lib(block_num1);

      // get_trx_block_number should find trx_id1 in block 1
      block_num = sp.get_trx_block_number(trx_id1, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num1);

      // on_accepted_block of block 2
      sp.append(block_trace_2);
      sp.append_trx_ids(block_trxs_entry2);

      // get_trx_block_number should find both trx_id1 and trx_id2
      block_num = sp.get_trx_block_number(trx_id1, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num1);
      block_num = sp.get_trx_block_number(trx_id2, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num2);

      // block 2 becomes final
      sp.append_lib(block_num2);

      // get_trx_block_number should still find both trx_id1 and trx_id2
      block_num = sp.get_trx_block_number(trx_id1, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num1);
      block_num = sp.get_trx_block_number(trx_id2, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, block_num2);
   }

// This test verifies the bug reported by https://github.com/AntelopeIO/spring/issues/942
// is fixed. The bug was if the block containing a transaction forked out,
// get_trx_block_number() always returned the latest block whose block number was
// the same as the initial block's, but this latest block might not include the
// transaction anymore.
   BOOST_FIXTURE_TEST_CASE(test_get_trx_block_number_forked, test_fixture)
   {
      chain::transaction_id_type target_trx_id = "0000000000000000000000000000000000000000000000000000000000000001"_h;
      uint32_t initial_block_num = 1;
      uint32_t final_block_num   = 3;

      transaction_trace_v0 trx_trace1 {
         target_trx_id,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 }
      };

      transaction_trace_v0 trx_trace2 {
         "0000000000000000000000000000000000000000000000000000000000000002"_h,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 }
      };

      // Initial block including trx_trace1
      block_trace_v0 initial_block_trace {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         initial_block_num,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "test"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            trx_trace1
         }
      };

      // Initial block is forked. The original trx_trace1 is forked out and
      // replaced by trx_trace2.
      block_trace_v0 forked_block_trace = initial_block_trace;
      forked_block_trace.transactions = std::vector<transaction_trace_v0> { trx_trace2 };

      // Final block including original trx_trace1
      block_trace_v0 final_block_trace {
         "b000000000000000000000000000000000000000000000000000000000000003"_h,
         final_block_num,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "test"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            trx_trace1
         }
      };

      block_trxs_entry initial_block_trxs_entry {
         .ids       = {target_trx_id},
         .block_num = initial_block_num
      };

      block_trxs_entry forked_block_trxs_entry {
         .ids       = {trx_trace2.id},
         .block_num = initial_block_num
      };

      block_trxs_entry final_block_trxs_entry {
         .ids       = {target_trx_id},
         .block_num = final_block_num
      };

      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      // on_accepted_block of the initial block
      sp.append(initial_block_trace);                  // block 1
      sp.append_trx_ids(initial_block_trxs_entry);     // block 1

      // target trx is in the first block (is still reversible)
      get_block_n block_num = sp.get_trx_block_number(target_trx_id, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, initial_block_num);

      // initial block forks out
      sp.append(forked_block_trace);                   // block 1
      sp.append_trx_ids(forked_block_trxs_entry);      // block 1

      // forked block becomes final
      sp.append_lib(initial_block_num);                // block 1

      // target trx is forked out. block 1 does not include
      // target_trx (trx_trace1) but trx_trace2;
      // therefore no block is found for target_trx_id.
      block_num = sp.get_trx_block_number(target_trx_id, {});
      BOOST_REQUIRE(!block_num);

      // on_accepted_block of the final block
      sp.append(final_block_trace);                    // block 3
      sp.append_trx_ids(final_block_trxs_entry);       // block 3

      // final block becomes final
      sp.append_lib(final_block_num);                  // block 3

      block_num = sp.get_trx_block_number(target_trx_id, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, final_block_num); // target trx is in final block
   }

// This test verifies the bug reported by https://github.com/AntelopeIO/spring/issues/1693
// is fixed. The issue was that when starting from a snapshot not all stride files were
// available. When it couldn't find the first stride file it reported trx not found.
   BOOST_FIXTURE_TEST_CASE(test_get_trx_when_missing_strides, test_fixture)
   {
      chain::transaction_id_type target_trx_id = "0000000000000000000000000000000000000000000000000000000000000001"_h;
      uint32_t trx_block_num = 10;

      std::vector<block_trace_v0> empty_blocks;
      for (uint32_t i = 1; i < trx_block_num; ++i) {
         empty_blocks.push_back({
               "b000000000000000000000000000000000000000000000000000000000000001"_h,
               i, // block_num
               "0000000000000000000000000000000000000000000000000000000000000000"_h,
               chain::block_timestamp_type(0),
               "test"_n,
               "0000000000000000000000000000000000000000000000000000000000000000"_h,
               "0000000000000000000000000000000000000000000000000000000000000000"_h,
               std::vector<transaction_trace_v0>{
               }
            });
      }

      transaction_trace_v0 trx_trace1 {
         target_trx_id,
         actions,
         10,
         5,
         {chain::signature_type()},
         {chain::time_point_sec(), 1, 0, 100, 50, 0}
      };

      // Block including trx_trace1
      block_trace_v0 block_trace {
         "b000000000000000000000000000000000000000000000000000000000000010"_h,
         trx_block_num,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "test"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            trx_trace1
         }
      };

      block_trxs_entry initial_block_trxs_entry {
         .ids       = {target_trx_id},
         .block_num = trx_block_num
      };

      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 5, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      // store some empty blocks which will create a stride
      for (const auto& bt : empty_blocks) {
         sp.append(bt);
         sp.append_trx_ids({.block_num = bt.number});
      }

      // on_accepted_block of the initial block
      sp.append(block_trace);                          // block 10
      sp.append_trx_ids(initial_block_trxs_entry);     // block 10

      get_block_n block_num = sp.get_trx_block_number(target_trx_id, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, trx_block_num); // target trx is in final block

      // remove first stride file and verify we can still find trx
      BOOST_REQUIRE(std::filesystem::exists(tempdir.path() / "trace_trx_id_0000000000-0000000005.log"));
      std::filesystem::remove(tempdir.path() / "trace_trx_id_0000000000-0000000005.log");

      // verify we can still find the trx
      block_num = sp.get_trx_block_number(target_trx_id, {});
      BOOST_REQUIRE(block_num);
      BOOST_REQUIRE_EQUAL(*block_num, trx_block_num); // target trx is in final block
   }

   // build_trx_id_index must apply the same fork-resolution logic as the linear
   // scan in get_trx_block_number: when the trx_id log holds multiple
   // block_trxs_entry records for the same block_num (one per accepted block at
   // that height, including forked-out ones), only the LAST entry per block_num
   // reflects the canonical post-fork state.  Trxs that appeared in an earlier
   // entry for that block_num but not the last one were forked out and must NOT
   // appear in the index.  Trxs that moved to a different block in the canonical
   // fork must resolve to the new block_num.
   BOOST_FIXTURE_TEST_CASE(test_build_trx_id_index_dedups_forked_trxs, test_fixture)
   {
      // Distinct first-8-byte prefixes so each trx maps to a unique bucket
      // (the writer's prefix64 = first 8 bytes of the trx_id).
      const chain::transaction_id_type trx_a =
         "a1a2a3a4a5a6a7a8000000000000000000000000000000000000000000000001"_h;
      const chain::transaction_id_type trx_b =
         "b1b2b3b4b5b6b7b8000000000000000000000000000000000000000000000002"_h;
      const chain::transaction_id_type trx_c =
         "c1c2c3c4c5c6c7c8000000000000000000000000000000000000000000000003"_h;

      fc::temp_directory tempdir;
      const uint32_t width = 100;
      test_store_provider sp(tempdir.path(), width);

      // First accepted block at height 1: contains trx_a + trx_b.
      sp.append_trx_ids(block_trxs_entry{ .ids = {trx_a, trx_b}, .block_num = 1 });
      // Block 1 forks out and is replaced by a different block at height 1:
      // canonical block 1 contains only trx_c (trx_a moved, trx_b removed).
      sp.append_trx_ids(block_trxs_entry{ .ids = {trx_c},        .block_num = 1 });
      // trx_a re-appears at block 2 in the canonical chain.
      sp.append_trx_ids(block_trxs_entry{ .ids = {trx_a},        .block_num = 2 });

      // Build the index for slice 0 directly (bypass the maintenance thread).
      sp._slice_directory.build_trx_id_index(0, [](const std::string&){});

      // Open the resulting on-disk index and verify lookups.
      auto reader = sp._slice_directory.find_trx_id_index_slice(0);
      BOOST_REQUIRE(reader.has_value());
      BOOST_REQUIRE(reader->valid());

      // trx_a moved to block 2 in the canonical chain -> lookup returns 2,
      // NOT 1 (the forked-out occurrence).
      auto a = reader->lookup(trx_a);
      BOOST_REQUIRE(a.has_value());
      BOOST_CHECK_EQUAL(*a, 2u);

      // trx_b was removed entirely (only present in the forked-out block 1).
      // Its bucket must be empty.
      auto b = reader->lookup(trx_b);
      BOOST_CHECK(!b.has_value());

      // trx_c is in canonical block 1.
      auto c = reader->lookup(trx_c);
      BOOST_REQUIRE(c.has_value());
      BOOST_CHECK_EQUAL(*c, 1u);
   }

   // Receiver bloom sidecar is built by run_maintenance_tasks at slice irreversibility, not during append.  Before
   // LIB crosses the slice, the sidecar must be absent (queries fall back to scan); once LIB advances past the slice,
   // a maintenance pass produces a valid sidecar whose probes hit every receiver actually present in the slice and
   // miss for receivers that were never appended.  This exercises the full on-LIB build path including the data log
   // stream-scan and the atomic sidecar write.
   BOOST_FIXTURE_TEST_CASE(slice_dir_recv_bloom_build_on_lib, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      // No compression, no deletion - keep the bloom build path focused.
      test_store_provider sp(tempdir.path(), width);

      // Build two block_trace_v0s in slice 0 (block numbers 1 and 2), each with one transaction whose actions touch
      // a distinct, known set of receivers.
      auto make_bt = [](uint32_t num, chain::checksum256_type id, std::vector<chain::name> receivers) {
         block_trace_v0 bt;
         bt.id = id;
         bt.number = num;
         transaction_trace_v0 trx;
         trx.id = id;
         trx.block_num = num;
         uint64_t seq = uint64_t{num} * 100;
         for (auto r : receivers) {
            action_trace_v0 a{};
            a.global_sequence = seq++;
            a.receiver        = r;
            a.account         = "sysio.token"_n;
            a.action          = "transfer"_n;
            trx.actions.push_back(std::move(a));
         }
         bt.transactions.push_back(std::move(trx));
         return bt;
      };

      auto id1 = "b000000000000000000000000000000000000000000000000000000000000001"_h;
      auto id2 = "b000000000000000000000000000000000000000000000000000000000000002"_h;
      sp.append(make_bt(1, id1, { "alice"_n, "bob"_n }));
      sp.append(make_bt(2, id2, { "charlie"_n }));

      const auto bloom_path = sp._slice_directory.bloom_slice_path(0);

      // Before LIB, no sidecar should exist - the append path must not have built anything on the fly.
      BOOST_CHECK(!std::filesystem::exists(bloom_path));

      // Advance LIB so slice 0 (blocks 0..9) is past LIB.  run_maintenance_tasks processes irreversible slices with
      // min_irreversible=0, so a LIB inside slice 1 (block >= 10) makes slice 0 eligible.
      sp._slice_directory.run_maintenance_tasks(/*lib=*/15, [](const std::string&){});

      BOOST_REQUIRE(std::filesystem::exists(bloom_path));

      bloom_reader r(bloom_path);
      BOOST_REQUIRE(r.valid());
      BOOST_CHECK(r.may_contain_receiver("alice"_n));
      BOOST_CHECK(r.may_contain_receiver("bob"_n));
      BOOST_CHECK(r.may_contain_receiver("charlie"_n));
      BOOST_CHECK(r.may_contain_recv_action("alice"_n,   "transfer"_n));
      BOOST_CHECK(r.may_contain_recv_action("charlie"_n, "transfer"_n));
      // A receiver that was never appended should probe as absent (allowing for the 1% FPR on the small-capacity
      // filter - try several unrelated names and tolerate at most one spurious hit).
      std::size_t false_positives = 0;
      for (auto n : { "never1"_n, "never2"_n, "never3"_n, "never4"_n, "never5"_n }) {
         if (r.may_contain_receiver(n)) ++false_positives;
      }
      BOOST_CHECK_LE(false_positives, 1u);

      // Re-running maintenance is idempotent: the bloom path still exists and the file wasn't clobbered.
      sp._slice_directory.run_maintenance_tasks(/*lib=*/15, [](const std::string&){});
      BOOST_REQUIRE(std::filesystem::exists(bloom_path));
   }

   // Fork behavior inside a single slice.  The extraction path re-applies forked blocks by calling append() again
   // with a new block_trace_v0 at the same block number.  The data log ends up with BOTH the forked-out trace and
   // the canonical trace: the blk_offset sidecar points only to the canonical offset, but the pre-fork record is
   // still physically present in the file.  Because the bloom is built by streaming the entire data log (not by
   // walking blk_offset), it naturally includes receivers from forked-out blocks too.  That is safe: a bloom may
   // have false positives (a probe "hits" for a receiver that isn't in the canonical chain) but must never have
   // false negatives (a probe "misses" for a receiver that IS in the canonical chain).  The test asserts both halves
   // of the invariant - canonical receivers probe as present, AND a forked-out receiver also probes as present
   // (harmless false positive), AND a never-appended receiver does not.
   BOOST_FIXTURE_TEST_CASE(slice_dir_recv_bloom_fork_in_slice, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      test_store_provider sp(tempdir.path(), width);

      auto make_bt = [](uint32_t num, chain::checksum256_type id, chain::name receiver) {
         block_trace_v0 bt;
         bt.id = id;
         bt.number = num;
         transaction_trace_v0 trx;
         trx.id = id;
         trx.block_num = num;
         action_trace_v0 a{};
         a.global_sequence = uint64_t{num} * 100;
         a.receiver        = receiver;
         a.account         = "sysio.token"_n;
         a.action          = "transfer"_n;
         trx.actions.push_back(std::move(a));
         bt.transactions.push_back(std::move(trx));
         return bt;
      };

      // Initial chain: block 1 with alice, block 2 with bob.
      sp.append(make_bt(1, "b000000000000000000000000000000000000000000000000000000000000001"_h, "alice"_n));
      sp.append(make_bt(2, "b000000000000000000000000000000000000000000000000000000000000002"_h, "bob"_n));

      // Fork: chain switches to a different branch.  Block 2 gets replayed with a different trace containing eve.
      // Controller fires accepted_block again with the new block_trace; store_provider::append writes the new trace
      // to the data log (appending, not overwriting in place) and updates the blk_offset sidecar to point at the new
      // offset.  The stale "bob" record still occupies its original position in the trace file.
      sp.append(make_bt(2, "b0000000000000000000000000000000000000000000000000000000000000b2"_h, "eve"_n));

      // Advance LIB past slice 0 so maintenance builds the bloom.
      sp._slice_directory.run_maintenance_tasks(/*lib=*/15, [](const std::string&){});
      const auto bloom_path = sp._slice_directory.bloom_slice_path(0);
      BOOST_REQUIRE(std::filesystem::exists(bloom_path));
      bloom_reader r(bloom_path);
      BOOST_REQUIRE(r.valid());

      // Canonical receivers must probe as present - this is the correctness invariant.
      BOOST_CHECK(r.may_contain_receiver("alice"_n));
      BOOST_CHECK(r.may_contain_receiver("eve"_n));
      // Forked-out receiver also probes as present because the stream-scan includes its stale record.  This is a
      // benign false positive; the query scan will then visit that slice and find no canonical match for "bob".
      BOOST_CHECK(r.may_contain_receiver("bob"_n));
      // Sanity: a receiver that was never in any branch at any time should still miss (modulo FPR).
      std::size_t false_positives = 0;
      for (auto n : { "never1"_n, "never2"_n, "never3"_n, "never4"_n, "never5"_n }) {
         if (r.may_contain_receiver(n)) ++false_positives;
      }
      BOOST_CHECK_LE(false_positives, 1u);
   }

   // Fork that crosses a slice boundary.  The scenario that motivated moving the bloom write to LIB (rather than
   // doing it at slice roll-over during append): the tail of slice K is replayed after the head of slice K+1 is
   // already in flight.  Under the earlier roll-over-based design the back-and-forth would have overwritten slice
   // K's bloom with an incomplete one built only from the replayed blocks.  Under the LIB-based design the sidecar
   // isn't written until the slice is fully irreversible, so forks can't reach back into an already-written sidecar.
   BOOST_FIXTURE_TEST_CASE(slice_dir_recv_bloom_cross_slice_fork, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      test_store_provider sp(tempdir.path(), width);

      auto make_bt = [](uint32_t num, chain::checksum256_type id, chain::name receiver) {
         block_trace_v0 bt;
         bt.id = id;
         bt.number = num;
         transaction_trace_v0 trx;
         trx.id = id;
         trx.block_num = num;
         action_trace_v0 a{};
         a.global_sequence = uint64_t{num} * 100;
         a.receiver        = receiver;
         a.account         = "sysio.token"_n;
         a.action          = "transfer"_n;
         trx.actions.push_back(std::move(a));
         bt.transactions.push_back(std::move(trx));
         return bt;
      };

      // Normal forward progress through slice 0: blocks 1..9 each with a distinct receiver.  These will all end up
      // in slice 0's bloom if LIB crosses cleanly.
      for (uint32_t n = 1; n <= 9; ++n) {
         chain::name r(0x4000'0000'0000'0000ull | n);  // synthesize distinct names
         chain::checksum256_type id;
         std::memcpy(id.data(), &n, sizeof(n));
         sp.append(make_bt(n, id, r));
      }
      // Block 10 lands in slice 1.
      sp.append(make_bt(10, "b00000000000000000000000000000000000000000000000000000000000000a"_h, "frank"_n));

      // Simulate a fork that replays the last block of slice 0 with a different trace, then replays slice 1's first
      // block.  This is exactly the cross-slice rollback pattern that broke the earlier design.
      sp.append(make_bt(9,  "b0000000000000000000000000000000000000000000000000000000000000f9"_h, "grace"_n));
      sp.append(make_bt(10, "b00000000000000000000000000000000000000000000000000000000000000b"_h, "harry"_n));

      // Neither slice 0 nor slice 1 has been built yet: no LIB has crossed them.
      BOOST_CHECK(!std::filesystem::exists(sp._slice_directory.bloom_slice_path(0)));
      BOOST_CHECK(!std::filesystem::exists(sp._slice_directory.bloom_slice_path(1)));

      // Advance LIB past slice 0 but still within slice 1.  Slice 0 should now be bloomed; slice 1 should still be
      // absent (it's still in flight, potentially subject to further forks).
      sp._slice_directory.run_maintenance_tasks(/*lib=*/12, [](const std::string&){});
      BOOST_REQUIRE(std::filesystem::exists(sp._slice_directory.bloom_slice_path(0)));
      BOOST_CHECK (!std::filesystem::exists(sp._slice_directory.bloom_slice_path(1)));

      // Slice 0's bloom must contain every receiver that was ever recorded in it - canonical and forked-out alike.
      // The key invariant: a query for "grace" (canonical tail of slice 0) MUST hit the bloom.  Under the pre-fix
      // design this was the receiver that could get lost.
      bloom_reader r0(sp._slice_directory.bloom_slice_path(0));
      BOOST_REQUIRE(r0.valid());
      for (uint32_t n = 1; n <= 8; ++n) {
         chain::name expected(0x4000'0000'0000'0000ull | n);
         BOOST_TEST_INFO("canonical slice-0 receiver " << expected.to_string());
         BOOST_CHECK(r0.may_contain_receiver(expected));
      }
      BOOST_CHECK(r0.may_contain_receiver("grace"_n));  // canonical post-fork tail of slice 0
      // Forked-out block 9 receiver (the 0x4000... name for n=9): also present because stream-scan includes it.
      {
         chain::name pre_fork_9(0x4000'0000'0000'0000ull | 9);
         BOOST_CHECK(r0.may_contain_receiver(pre_fork_9));
      }

      // Advance LIB past slice 1 and rebuild.  Slice 1 must now be bloomed and must contain harry (canonical) - bob
      // frank was the forked-out block 10, which the stream-scan still finds.
      sp._slice_directory.run_maintenance_tasks(/*lib=*/25, [](const std::string&){});
      BOOST_REQUIRE(std::filesystem::exists(sp._slice_directory.bloom_slice_path(1)));
      bloom_reader r1(sp._slice_directory.bloom_slice_path(1));
      BOOST_REQUIRE(r1.valid());
      BOOST_CHECK(r1.may_contain_receiver("harry"_n));   // canonical slice-1
      BOOST_CHECK(r1.may_contain_receiver("frank"_n));   // forked-out slice-1 (harmless false positive)
   }

   // A data log with an unparseable record in the MIDDLE (torn write from a crash, with re-applied
   // blocks appended after it) must not produce a bloom sidecar at all.  A bloom built from the
   // partial prefix would return authoritative negative probes for receivers recorded after the torn
   // record - silently dropping their actions from get_actions.  No sidecar -> the query path scans
   // the slice, which still works because it reads via per-block offsets, not a sequential stream.
   BOOST_FIXTURE_TEST_CASE(slice_dir_recv_bloom_skipped_on_corrupt_data_log, test_fixture)
   {
      fc::temp_directory tempdir;
      const uint32_t width = 10;
      test_store_provider sp(tempdir.path(), width);

      auto make_bt = [](uint32_t num, chain::checksum256_type id, chain::name receiver) {
         block_trace_v0 bt;
         bt.id = id;
         bt.number = num;
         transaction_trace_v0 trx;
         trx.id = id;
         trx.block_num = num;
         action_trace_v0 a{};
         a.global_sequence = uint64_t{num} * 100;
         a.receiver        = receiver;
         a.account         = "sysio.token"_n;
         a.action          = "transfer"_n;
         trx.actions.push_back(std::move(a));
         bt.transactions.push_back(std::move(trx));
         return bt;
      };

      sp.append(make_bt(1, "b000000000000000000000000000000000000000000000000000000000000001"_h, "alice"_n));

      // Torn record: raw garbage at EOF, exactly what an interrupted append leaves behind.  0xff
      // bytes guarantee the variant-index unpack fails deterministically.
      {
         fc::cfile trace;
         BOOST_REQUIRE(sp._slice_directory.find_trace_slice(0, open_state::write, trace));
         const char garbage[8] = { '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff' };
         trace.write(garbage, sizeof(garbage));
         trace.flush();
      }

      // Post-restart re-applied block lands AFTER the garbage; its blk_offset slot points past it,
      // so offset-based reads still work even though the stream is desynced at the torn record.
      sp.append(make_bt(2, "b000000000000000000000000000000000000000000000000000000000000002"_h, "charlie"_n));
      get_block_t b2 = sp.get_block(2);
      BOOST_REQUIRE(b2);

      // Maintenance must NOT write a bloom for this slice.
      sp._slice_directory.run_maintenance_tasks(/*lib=*/15, [](const std::string&){});
      BOOST_CHECK(!std::filesystem::exists(sp._slice_directory.bloom_slice_path(0)));

      // And it stays that way on subsequent runs (no flapping).
      sp._slice_directory.run_maintenance_tasks(/*lib=*/16, [](const std::string&){});
      BOOST_CHECK(!std::filesystem::exists(sp._slice_directory.bloom_slice_path(0)));
   }

   // The block-offset sidecar is advisory: a filesystem failure writing it must neither abort the
   // append (which would shut the node down through the extraction except_handler) nor break
   // get_block, which falls back to the metadata-log scan.
   BOOST_FIXTURE_TEST_CASE(test_blk_offset_write_failure_is_advisory, test_fixture)
   {
      fc::temp_directory tempdir;

      // Block the sidecar path with a non-empty DIRECTORY: opening it as a file fails, and it can't
      // be removed-and-recreated by the recovery path either.
      const auto sidecar = tempdir.path() / "trace_blk_idx_0000000000-0000000100.log";
      std::filesystem::create_directories(sidecar);
      { std::ofstream(sidecar / "occupied") << "x"; }

      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      BOOST_CHECK_NO_THROW(sp.append(block_trace1));

      // Sidecar unusable -> lookup misses -> metadata-log scan fallback still serves the block.
      get_block_t b1 = sp.get_block(1);
      BOOST_REQUIRE(b1);
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(std::get<0>(*b1)), block_trace1);
   }

   // Restart recovery: a fresh store_provider over an existing slice dir must pick up the previous
   // run's LIB and last-recorded-block watermarks from the persisted index slices, so irreversible
   // blocks do not flip back to "pending" and the query envelope's recorded-end clamp keeps working
   // before any new chain signal arrives.
   BOOST_FIXTURE_TEST_CASE(test_watermarks_seed_from_disk_on_reopen, test_fixture)
   {
      fc::temp_directory tempdir;
      {
         store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
         BOOST_CHECK_EQUAL(sp.last_recorded_block(), 0u);
         sp.append(block_trace1);   // block 1
         sp.append_lib(1);
         sp.append(block_trace2);   // block 5
         BOOST_CHECK_EQUAL(sp.last_recorded_block(), 5u);
      }

      store_provider sp2(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      BOOST_CHECK_EQUAL(sp2.last_recorded_block(), 5u);

      // Block 1 was at/below the recorded LIB -> still reported irreversible after the restart.
      get_block_t b1 = sp2.get_block(1);
      BOOST_REQUIRE(b1);
      BOOST_CHECK(std::get<1>(*b1));
      // Block 5 was beyond LIB -> still pending.
      get_block_t b5 = sp2.get_block(5);
      BOOST_REQUIRE(b5);
      BOOST_CHECK(!std::get<1>(*b5));
   }

   // Build a single-transaction block trace whose only action is a recorded sysio::setabi - a
   // realistic reversible block whose ABI capture is also mirrored to the abi_log journal.
   block_trace_v0 make_setabi_block_trace(uint32_t number, chain::name target, uint64_t global_seq,
                                          const std::vector<char>& abi) {
      chain::bytes abi_bytes(abi.begin(), abi.end());
      chain::bytes data;
      fc::datastream<size_t> ps;
      fc::raw::pack(ps, target, abi_bytes);
      data.resize(ps.tellp());
      fc::datastream<char*> ds(data.data(), data.size());
      fc::raw::pack(ds, target, abi_bytes);

      return block_trace_v0 {
         .id           = fc::sha256::hash(std::to_string(number)),
         .number       = number,
         .previous_id  = fc::sha256::hash(std::to_string(number - 1)),
         .timestamp    = chain::block_timestamp_type(number),
         .producer     = "bp.one"_n,
         .transactions = {
            transaction_trace_v0 {
               .id      = fc::sha256::hash("setabi-trx-" + std::to_string(number)),
               .actions = {
                  {
                     .global_sequence = global_seq,
                     .receiver        = chain::config::system_account_name,
                     .account         = chain::config::system_account_name,
                     .action          = setabi_action_name,
                     .authorization   = {{ target, "active"_n }},
                     .data            = data
                  }
               }
            }
         }
      };
   }

   // append_lib must flush reversible ABI records at/below LIB to the on-disk abi log: they
   // survive a restart even though the reversible overlay is memory-only (and the startup
   // rebuild has nothing to do here - the whole window is at/below LIB after the flush).
   BOOST_FIXTURE_TEST_CASE(test_abi_flush_at_lib_survives_restart, test_fixture)
   {
      fc::temp_directory tempdir;
      const auto abi = std::vector<char>{'a', 'b', 'i'};
      {
         store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
         sp.append(block_trace1);   // block 1
         sp.append_abi(1, "acct"_n, 100, abi);
         BOOST_REQUIRE(sp.lookup_abi_seq("acct"_n, 100).has_value());
         sp.append_lib(1);          // block 1 final -> record flushed to disk
      }

      store_provider sp2(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      auto seq = sp2.lookup_abi_seq("acct"_n, 150);
      BOOST_REQUIRE(seq.has_value());
      BOOST_CHECK_EQUAL(*seq, 100u);
      auto fetched = sp2.fetch_abi("acct"_n, 100);
      BOOST_REQUIRE(fetched.has_value());
      BOOST_CHECK(*fetched == abi);
      BOOST_CHECK(sp2.has_abi_entry("acct"_n));
   }

   // Reversible (above-LIB) ABI records must not reach the main file, but a restart must not lose
   // them either.  The abi_log restores its overlay from its durable journal sidecar - including
   // the lazy global_seq-0 record, which is read from chain state and leaves no action trace, so a
   // trace scan could never recover it.  Here "x" is lazily captured (seq 0 = its pre-setabi ABI)
   // and also has a setabi at seq 500 in reversible block 5; both survive the restart, and a
   // pre-setabi global_seq still resolves to the lazy bytes (the case the old design lost).
   BOOST_FIXTURE_TEST_CASE(test_reversible_abi_restored_from_journal_on_restart, test_fixture)
   {
      fc::temp_directory tempdir;
      const auto v0 = std::vector<char>{'v', '0'};
      const auto v1 = std::vector<char>{'v', '1'};
      {
         store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
         sp.append(block_trace1);                                  // block 1
         sp.append_lib(1);                                         // LIB = 1
         sp.append(make_setabi_block_trace(5, "x"_n, 500, v1));    // block 5, reversible
         sp.append_abi(5, "x"_n, 0,   v0);                         // lazy capture: x's pre-setabi ABI
         sp.append_abi(5, "x"_n, 500, v1);                         // setabi capture at its global_seq
         BOOST_REQUIRE(sp.lookup_abi_seq("x"_n, 500).has_value());
      }

      // Restart: both records come back from the journal (the live signals for blocks 2..5 do not
      // re-fire on a clean restart, and the lazy seq-0 record has no trace to scan).
      store_provider sp2(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      // Pre-setabi action (global_seq < 500) resolves to the lazy v0 - lost by the old rebuild.
      auto pre = sp2.lookup_abi_seq("x"_n, 100);
      BOOST_REQUIRE(pre.has_value());
      BOOST_CHECK_EQUAL(*pre, 0u);
      auto pre_blob = sp2.fetch_abi("x"_n, 0);
      BOOST_REQUIRE(pre_blob.has_value());
      BOOST_CHECK(*pre_blob == v0);

      // Post-setabi action resolves to v1.
      auto post = sp2.lookup_abi_seq("x"_n, 600);
      BOOST_REQUIRE(post.has_value());
      BOOST_CHECK_EQUAL(*post, 500u);
      auto post_blob = sp2.fetch_abi("x"_n, 500);
      BOOST_REQUIRE(post_blob.has_value());
      BOOST_CHECK(*post_blob == v1);

      // LIB advances past block 5 -> both records flush to disk; a further restart serves them from
      // the file alone (the journal compacts to empty).
      sp2.append_lib(5);
      store_provider sp3(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);
      auto final_pre  = sp3.lookup_abi_seq("x"_n, 100);
      auto final_post = sp3.lookup_abi_seq("x"_n, 600);
      BOOST_REQUIRE(final_pre.has_value());
      BOOST_REQUIRE(final_post.has_value());
      BOOST_CHECK_EQUAL(*final_pre, 0u);
      BOOST_CHECK_EQUAL(*final_post, 500u);
   }

   // rollback_abis discards reversible records for blocks at/above the given height (fork
   // replacement), and a subsequent re-commit for the replacing block resolves instead.
   BOOST_FIXTURE_TEST_CASE(test_abi_rollback_discards_reversible_records, test_fixture)
   {
      fc::temp_directory tempdir;
      store_provider sp(tempdir.path(), 100, std::optional<uint32_t>(), std::optional<uint32_t>(), 0);

      sp.append_abi(5, "x"_n, 500, std::vector<char>{'a'});
      BOOST_REQUIRE(sp.lookup_abi_seq("x"_n, 500).has_value());

      sp.rollback_abis(5);   // fork switch: a new block 5 replaces the old one
      BOOST_CHECK(!sp.lookup_abi_seq("x"_n, 500));
      BOOST_CHECK(!sp.has_abi_entry("x"_n));

      sp.append_abi(5, "x"_n, 501, std::vector<char>{'b'});
      auto seq = sp.lookup_abi_seq("x"_n, 600);
      BOOST_REQUIRE(seq.has_value());
      BOOST_CHECK_EQUAL(*seq, 501u);
   }

BOOST_AUTO_TEST_SUITE_END()

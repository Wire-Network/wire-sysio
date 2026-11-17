#include "temp_directory.hpp"

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <chainbase/environment.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <new>
#include <optional>
#include <utility>

namespace {

constexpr char compiler_character = 'x';
constexpr auto database_filename = "shared_memory.bin";
constexpr uint64_t database_size = 8 * 1024 * 1024;
constexpr auto compiler_field = "compiler";
constexpr uint64_t unsupported_header_id = chainbase::header_id ^ 1;
constexpr uint32_t other_boost_version = BOOST_VERSION + 1;

/// Produce the native bytes used by existing databases, including their original field offsets.
std::array<char, chainbase::header_size> header_bytes(bool dirty = false) {
   chainbase::db_header header;
   header.dirty = dirty;
   std::array<char, chainbase::header_size> bytes{};
   std::memcpy(bytes.data(), &header, sizeof(header));
   return bytes;
}

/// Original native header shape, retained only to verify compatibility with existing database bytes.
struct legacy_db_header {
   uint64_t id = chainbase::header_id;
   bool dirty = false;
   chainbase::bip::offset_ptr<char> small_size_allocator;
   chainbase::environment dbenviron;
};

constexpr size_t allocator_fixture_extent = 32;

/// Keep pointees on both sides of a header to test signed-distance wrapping and complete-image relocation.
template <typename Header>
struct allocator_image {
   std::array<char, allocator_fixture_extent> before{};
   Header header;
   std::array<char, allocator_fixture_extent> after{};
};

static_assert(std::is_trivially_copyable_v<chainbase::db_header>);
static_assert(std::is_trivially_copy_constructible_v<chainbase::db_header>);
static_assert(sizeof(chainbase::db_header) == sizeof(legacy_db_header));
static_assert(alignof(chainbase::db_header) == alignof(legacy_db_header));
static_assert(offsetof(chainbase::db_header, id) == offsetof(legacy_db_header, id));
static_assert(offsetof(chainbase::db_header, dirty) == offsetof(legacy_db_header, dirty));
static_assert(offsetof(chainbase::db_header, small_size_allocator_storage) ==
              offsetof(legacy_db_header, small_size_allocator));
static_assert(offsetof(chainbase::db_header, dbenviron) == offsetof(legacy_db_header, dbenviron));

/// Create a complete mapped database for original dirty/environment acceptance tests.
void create_database(const std::filesystem::path& directory) {
   chainbase::pinnable_mapped_file database(directory, true, database_size, false,
                                            chainbase::pinnable_mapped_file::mapped);
}

/// Modify a native field in an owned fixture, retaining all other database contents.
template <typename T>
void write_header_field(const std::filesystem::path& path, size_t offset, const T& value) {
   std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
   file.seekp(offset);
   file.write(reinterpret_cast<const char*>(&value), sizeof(value));
   BOOST_REQUIRE(file.good());
}

} // namespace

BOOST_AUTO_TEST_SUITE(database_header_tests)

/// A null stored allocator offset is returned unchanged by the production allocator lookup.
BOOST_AUTO_TEST_CASE(empty_native_allocator_slot) {
   alignas(chainbase::db_header) std::byte storage[chainbase::header_size];
   new (storage) chainbase::db_header;
   BOOST_CHECK(!chainbase::pinnable_mapped_file::get_small_size_allocator(storage + sizeof(storage)));
}

/// Keep every compiler byte identical to the original bounded strncpy initialization.
BOOST_AUTO_TEST_CASE(compiler_encoding_matches_legacy) {
   const chainbase::environment value;
   std::array<char, std::tuple_size_v<decltype(value.compiler)>> expected{};
   std::strncpy(expected.data(), __VERSION__, expected.size() - 1);
   BOOST_CHECK_EQUAL_COLLECTIONS(value.compiler.begin(), value.compiler.end(), expected.begin(), expected.end());
}

/// Preserve Boost's existing null/relative encodings and keep copied database images self-relative.
BOOST_AUTO_TEST_CASE(allocator_storage_matches_legacy_and_relocates) {
   allocator_image<legacy_db_header> legacy;
   allocator_image<chainbase::db_header> current;
   BOOST_CHECK(!current.header.small_size_allocator_address());
   BOOST_CHECK_EQUAL(current.header.small_size_allocator_storage, legacy.header.small_size_allocator.get_offset());
   const std::array legacy_targets{legacy.before.data(), legacy.after.data(),
                                   reinterpret_cast<char*>(&legacy.header.small_size_allocator)};
   const std::array current_targets{current.before.data(), current.after.data(),
                                    reinterpret_cast<char*>(&current.header.small_size_allocator_storage)};
   for (size_t index{}; index < current_targets.size(); ++index) {
      legacy.header.small_size_allocator = legacy_targets[index];
      current.header.set_small_size_allocator_address(current_targets[index]);
      BOOST_CHECK_EQUAL(current.header.small_size_allocator_storage, legacy.header.small_size_allocator.get_offset());
      const auto stored = current.header.small_size_allocator_storage;
      BOOST_CHECK_EQUAL(std::as_const(current.header).small_size_allocator_address(), current_targets[index]);
      BOOST_CHECK_EQUAL(current.header.small_size_allocator_storage, stored);
      auto relocated = current;
      const std::array relocated_targets{relocated.before.data(), relocated.after.data(),
                                         reinterpret_cast<char*>(&relocated.header.small_size_allocator_storage)};
      BOOST_CHECK_EQUAL(relocated.header.small_size_allocator_address(), relocated_targets[index]);
      std::memcpy(&relocated.header, &legacy.header, sizeof(legacy.header));
      BOOST_CHECK_EQUAL(relocated.header.small_size_allocator_address(), relocated_targets[index]);
   }
}

/// The original stream read rejects missing paths and incomplete native header payloads.
BOOST_AUTO_TEST_CASE(opener_rejects_short_payload) {
   temp_directory directory;
   const auto path = directory.path() / database_filename;
   const auto open_read_only = [&] {
      return chainbase::pinnable_mapped_file(directory.path(), false, database_size, false,
                                             chainbase::pinnable_mapped_file::mapped);
   };
   BOOST_CHECK_EXCEPTION(open_read_only(), std::system_error, [](const std::system_error& error) {
      return error.code() == chainbase::make_error_code(chainbase::not_found);
   });
   const auto bytes = header_bytes();
   for (const size_t size : {sizeof(chainbase::db_header) - 1, sizeof(chainbase::db_header), chainbase::header_size - 1,
                             chainbase::header_size, chainbase::minimum_database_file_size - 1}) {
      {
         std::ofstream file(path, std::ios::binary | std::ios::trunc);
         file.write(bytes.data(), std::min(size, bytes.size()));
         file.close();
         std::filesystem::resize_file(path, size);
      }
      BOOST_CHECK_EXCEPTION(open_read_only(), std::system_error, [](const std::system_error& error) {
         return error.code() == chainbase::make_error_code(chainbase::bad_header);
      });
   }
   std::filesystem::remove(path);
   std::filesystem::create_directory(path);
   BOOST_CHECK_EXCEPTION(open_read_only(), std::system_error, [](const std::system_error& error) {
      return error.code() == chainbase::make_error_code(chainbase::bad_header);
   });
}

/// The shared production reader rejects truncated prefixes and accepts the exact manager-storage boundary.
BOOST_AUTO_TEST_CASE(reader_checks_complete_database_prefix) {
   temp_directory directory;
   create_database(directory.path());
   const std::filesystem::path path = directory.path() / database_filename;
   for (const size_t size :
        {chainbase::minimum_database_file_size, chainbase::minimum_database_file_size - 1, chainbase::header_size,
         chainbase::header_size - 1, sizeof(chainbase::db_header), sizeof(chainbase::db_header) - 1}) {
      std::filesystem::resize_file(path, size);
      const std::optional<chainbase::db_header> header = chainbase::read_db_header(path);
      BOOST_CHECK_EQUAL(header.has_value(), size >= chainbase::minimum_database_file_size);
      if (header) {
         BOOST_CHECK_EQUAL(header->id, chainbase::header_id);
         BOOST_CHECK(!header->dirty);
         BOOST_CHECK(header->dbenviron == chainbase::environment());
      }
   }
   std::filesystem::remove(path);
   BOOST_CHECK(!chainbase::read_db_header(path));
   std::filesystem::create_directory(path);
   BOOST_CHECK(!chainbase::read_db_header(path));
}

/// Reject complete manager headers whose recorded segment extends beyond the file, without growing rejected files.
BOOST_AUTO_TEST_CASE(opener_rejects_truncated_segment) {
   temp_directory directory;
   create_database(directory.path());
   const std::filesystem::path path = directory.path() / database_filename;
   constexpr std::array<size_t, 4> truncated_sizes{chainbase::minimum_database_file_size,
                                                   chainbase::minimum_database_file_size + 1, database_size / 2,
                                                   database_size - 1};
   for (const size_t size : truncated_sizes) {
      std::filesystem::resize_file(path, size);
      for (const chainbase::pinnable_mapped_file::map_mode mode :
           {chainbase::pinnable_mapped_file::mapped, chainbase::pinnable_mapped_file::mapped_private,
            chainbase::pinnable_mapped_file::heap}) {
         for (const bool writable : {false, true}) {
            BOOST_CHECK_EXCEPTION(
               chainbase::pinnable_mapped_file(directory.path(), writable, database_size, false, mode),
               std::system_error, [](const std::system_error& error) {
                  return error.code() == chainbase::make_error_code(chainbase::bad_header);
               });
            BOOST_CHECK_EQUAL(std::filesystem::file_size(path), size);
         }
      }
   }
}

/// A recorded segment may fit within a larger file; the extent check must allow trailing bytes.
BOOST_AUTO_TEST_CASE(opener_accepts_trailing_file_bytes) {
   temp_directory directory;
   create_database(directory.path());
   const std::filesystem::path path = directory.path() / database_filename;
   constexpr size_t padded_size = database_size + 1;
   std::filesystem::resize_file(path, padded_size);
   for (const bool writable : {false, true}) {
      BOOST_CHECK_NO_THROW(chainbase::pinnable_mapped_file(directory.path(), writable, database_size, false,
                                                           chainbase::pinnable_mapped_file::mapped));
      BOOST_CHECK_EQUAL(std::filesystem::file_size(path), padded_size);
   }
}

/// The original magic check rejects an unsupported id in an otherwise valid native representation.
BOOST_AUTO_TEST_CASE(opener_rejects_unsupported_magic) {
   temp_directory directory;
   create_database(directory.path());
   write_header_field(directory.path() / database_filename, offsetof(chainbase::db_header, id), unsupported_header_id);
   BOOST_CHECK_EXCEPTION(chainbase::pinnable_mapped_file(directory.path(), false, database_size, false,
                                                         chainbase::pinnable_mapped_file::mapped),
                         std::system_error, [](const std::system_error& error) {
                            return error.code() == chainbase::make_error_code(chainbase::incorrect_db_version);
                         });
}

/// Keep original clean/dirty and allow_dirty behavior for valid native boolean bytes.
BOOST_AUTO_TEST_CASE(opener_preserves_dirty_handling) {
   temp_directory directory;
   create_database(directory.path());
   for (const bool dirty : {false, true}) {
      write_header_field(directory.path() / database_filename, offsetof(chainbase::db_header, dirty), dirty);
      for (const bool allow_dirty : {false, true}) {
         const auto open = [&] {
            return chainbase::pinnable_mapped_file(directory.path(), false, database_size, allow_dirty,
                                                   chainbase::pinnable_mapped_file::mapped);
         };
         if (dirty && !allow_dirty) {
            BOOST_CHECK_EXCEPTION(open(), std::system_error, [](const std::system_error& error) {
               return error.code() == chainbase::make_error_code(chainbase::dirty);
            });
         } else {
            BOOST_CHECK_NO_THROW(open());
         }
      }
   }
}

/// Preserve the original environment mismatch rejection without adding representation validation.
BOOST_AUTO_TEST_CASE(opener_rejects_environment_mismatch) {
   temp_directory directory;
   create_database(directory.path());
   constexpr auto offset = offsetof(chainbase::db_header, dbenviron) + offsetof(chainbase::environment, boost_version);
   write_header_field(directory.path() / database_filename, offset, other_boost_version);
   BOOST_CHECK_EXCEPTION(chainbase::pinnable_mapped_file(directory.path(), false, database_size, false,
                                                         chainbase::pinnable_mapped_file::mapped),
                         std::system_error, [](const std::system_error& error) {
                            return error.code() == chainbase::make_error_code(chainbase::incompatible);
                         });
}

/// Native compiler-name formatting remains bounded without requiring an extra header check.
BOOST_AUTO_TEST_CASE(bounded_compiler_name_formatting) {
   chainbase::environment environment;
   environment.compiler.fill(compiler_character);
   fc::variant value;
   fc::to_variant(environment, value);
   BOOST_CHECK_EQUAL(value[compiler_field].as_string(), std::string(environment.compiler.size(), compiler_character));
}

BOOST_AUTO_TEST_SUITE_END()

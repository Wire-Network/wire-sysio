#pragma once
#include <chainbase/pinnable_mapped_file.hpp>
#include <iomanip>
#include <fc/variant.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <optional>
#include <string_view>
#include <type_traits>

consteval uint64_t operator""_u64str(const char* s, const size_t size) {
   assert(size == 8);

   uint64_t result = 0;
   for(size_t i = 0; i < 8; ++i)
      result |= static_cast<uint64_t>(s[i]) << (i * 8);

   return result;
}

namespace chainbase {

constexpr size_t header_size = 1024;
// `CHAINB01` reflects changes since `EOSIODB3`.
// 'CHAINB02' adds the small size allocator
//
// Spring 1.0 is compatible with `CHAINB01`.
// Wire 6.0 is compatible with `CHAINB02`.
// ------------------------------------------
constexpr uint64_t header_id = 0x3230424e49414843ULL; //"CHAINB02" little endian

struct environment  {
   /// Preserve the native bounded compiler-name encoding while permitting constant initialization.
   constexpr environment() {
      constexpr std::string_view compiler_version = __VERSION__;
      std::copy_n(compiler_version.begin(), std::min(compiler_version.size(), compiler.size() - 1), compiler.begin());
   }

   enum os_t : unsigned char {
      OS_LINUX,
      OS_MACOS,
      OS_WINDOWS,
      OS_OTHER
   };
   enum arch_t : unsigned char {
      ARCH_X86_64,
      ARCH_ARM,
      ARCH_RISCV,
      ARCH_OTHER
   };

   bool debug =
#ifndef NDEBUG
      true;
#else
      false;
#endif
   os_t os =
#if defined(__linux__)
      OS_LINUX;
#elif defined(__APPLE__)
      OS_MACOS;
#elif defined(_WIN32)
      OS_WINDOWS;
#else
      OS_OTHER;
#endif
   arch_t arch =
#if defined(__x86_64__)
      ARCH_X86_64;
#elif defined(__aarch64__)
      ARCH_ARM;
#elif defined(__riscv__)
      ARCH_RISCV;
#else
      ARCH_OTHER;
#endif

   uint32_t boost_version = BOOST_VERSION;
   std::array<char,256> compiler = {};

   friend bool operator==(const environment&, const environment&) = default;

   template <typename DataStream>
   friend DataStream& operator<<(DataStream& ds, const environment& e) {
      fc::raw::pack(ds, e.debug);
      fc::raw::pack(ds, (uint8_t)e.os);
      fc::raw::pack(ds, (uint8_t)e.arch);
      fc::raw::pack(ds, e.boost_version);
      ds.skip(512); //the old 'reserved' field
      fc::raw::pack(ds, e.compiler);
      return ds;
   }

   template <typename DataStream>
   friend DataStream& operator>>(DataStream& ds, environment& e) {
      fc::raw::unpack(ds, e.debug);
      unsigned char tmp;
      fc::raw::unpack(ds, tmp);
      e.os = (os_t)tmp;
      fc::raw::unpack(ds, tmp);
      e.arch = (arch_t)tmp;
      fc::raw::unpack(ds, e.boost_version);
      ds.skip(512); //the old 'reserved' field
      fc::raw::unpack(ds, e.compiler);
      return ds;
   }
};

/// Native header with trivially copyable allocator storage, preserving the persisted relative-offset representation.
struct db_header  {
   using allocator_pointer = bip::offset_ptr<char>;
   using allocator_offset = allocator_pointer::offset_type;

   /// Boost offset_ptr reserves the relative offset one for its null representation.
   static constexpr allocator_offset null_allocator_offset = 1;

   uint64_t id = header_id;
   bool dirty = false;
   alignas(allocator_pointer) allocator_offset small_size_allocator_storage = null_allocator_offset;
   environment dbenviron;

   /**
    * Decode the allocator relative to this mapped header without modifying storage.
    * The allocator and header must share the original mapping or a relocated complete database image.
    */
   char* small_size_allocator_address() const noexcept {
      if (small_size_allocator_storage == null_allocator_offset)
         return nullptr;
      return reinterpret_cast<char*>(reinterpret_cast<allocator_offset>(&small_size_allocator_storage) +
                                     small_size_allocator_storage);
   }

   /// Store a non-null pointee relative to this field, retaining offset_ptr's existing native representation.
   void set_small_size_allocator_address(char* pointer) noexcept {
      assert(pointer);
      small_size_allocator_storage = reinterpret_cast<allocator_offset>(pointer) -
                                     reinterpret_cast<allocator_offset>(&small_size_allocator_storage);
      assert(small_size_allocator_storage != null_allocator_offset);
   }
};

static_assert(sizeof(db_header::allocator_offset) == sizeof(db_header::allocator_pointer));
static_assert(std::is_trivially_copyable_v<db_header>);

constexpr size_t header_dirty_bit_offset = offsetof(db_header, dirty);
static_assert(std::is_standard_layout_v<db_header>);
/// Guard the one-byte dirty-flag stores into the mapped header.
static_assert(sizeof(bool) == sizeof(uint8_t));
//older implementations had a packed db_header; double check that non-packed db_header struct still has this in the right spot. Reconsider post-CHAINB01
static_assert(header_dirty_bit_offset == 8, "DB dirty byte is expected to be at offset 8");

//really what we want to check here is the pack_size() of a db_header, but that isn't constexpr. Reconsider post-CHAINB01
static_assert(sizeof(db_header) <= header_size, "DB header struct too large");

/// Minimum file extent required before accessing the native segment manager after the reserved header prefix.
inline constexpr size_t minimum_database_file_size = header_size + sizeof(segment_manager);

/**
 * Read a native header from a file containing the reserved prefix and segment-manager storage.
 * Return nullopt for read or file-size failures; callers retain their magic, dirty, and environment checks.
 */
std::optional<db_header> read_db_header(const std::filesystem::path& path);

}

FC_REFLECT_ENUM(chainbase::environment::os_t, (OS_LINUX)(OS_MACOS)(OS_WINDOWS)(OS_OTHER))
FC_REFLECT_ENUM(chainbase::environment::arch_t, (ARCH_X86_64)(ARCH_ARM)(ARCH_RISCV)(ARCH_OTHER))

FC_REFLECT(chainbase::db_header, (id)(dirty)(dbenviron));

namespace fc {
   void to_variant(const chainbase::environment& var, variant& vo);
}
#pragma once

#include <fc/io/raw.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace sysio::chain::sysvmoc {

/// Native persistent cache header, retaining the packed SYSVMOC2 file layout.
struct code_cache_header {
   /**
    * Cache magic for the current intrinsic table and generated-code layout.
    * Bump this together with current_codegen_version whenever intrinsic ordinals or the GS-region layout change:
    * persisted machine code embeds ordinal-derived dispatch offsets and cannot be reused across those changes.
    */
   static constexpr uint64_t magic = 0x32434f4d56535953ULL; // "SYSVMOC2" little endian

   uint64_t id = magic;
   bool dirty = false;
   uintptr_t serialized_descriptor_index = 0;
} __attribute__((packed));

static_assert(std::is_trivially_copyable_v<code_cache_header>);
static_assert(sizeof(bool) == sizeof(uint8_t));

/**
 * Decode a native header after checking its extent and boolean representation.
 * Only the flag byte uses the checked boolean decoder; the complete header retains its native representation.
 * The caller remains responsible for checking cache magic and rejecting an otherwise valid dirty cache.
 */
inline code_cache_header read_code_cache_header(std::span<const char> bytes) {
   FC_ASSERT(bytes.size() >= sizeof(code_cache_header));
   bool dirty = false;
   fc::datastream<const char*> flag_stream(bytes.data() + offsetof(code_cache_header, dirty), sizeof(uint8_t));
   fc::raw::unpack(flag_stream, dirty);
   code_cache_header result;
   std::memcpy(&result, bytes.data(), sizeof(result));
   result.dirty = dirty;
   return result;
}

} // namespace sysio::chain::sysvmoc

#include <boost/test/unit_test.hpp>

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
#include "IR/Module.h"
#include "WASM/WASM.h"
#include "Inline/Serialization.h"
#endif

#include <cstdint>
#include <vector>

/**
 * Regression tests for wasm-jit's binary deserializer as used by the sys-vm-oc compile child
 * (sysvmoc::run_compile). Legal-but-degenerate wasm modules must deserialize without undefined
 * behavior: a zero-length UserSection payload used to reach memcpy with a null destination
 * (std::vector::data() of an empty vector) through Inline/Serialization.h serializeBytes().
 * memcpy with a null pointer is UB regardless of size, and under the ubsan CI build
 * (-fno-sanitize-recover=all) it aborts the compile child; the death then surfaced as
 * compilation_result_unknownfailure from the compile monitor.
 *
 * These run the deserializer in-process so the regression is caught directly, in every build
 * and sanitizer flavor, not only via a dead compile child under ubsan.
 */
BOOST_AUTO_TEST_SUITE(wasm_jit_serialization_tests)

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
namespace {
   /// 8-byte wasm binary preamble: magic "\0asm" + version 1
   constexpr uint8_t wasm_preamble[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

   /// Deserialize raw wasm bytes the same way the sys-vm-oc compile child does
   /// (compile_trampoline.cpp run_compile), including its scoped_skip_checks setting.
   IR::Module deserialize_as_compile_child(const std::vector<uint8_t>& wasm) {
      IR::Module module;
      Serialization::MemoryInputStream stream(wasm.data(), wasm.size());
      WASM::scoped_skip_checks no_check;
      WASM::serialize(stream, module);
      return module;
   }
}
#endif

// A trailing custom section whose payload is empty after the section name -- the exact shape
// sysvmoc_interrupt_tests/oc_interrupt_preserves_dedup_record appends to vary a code hash, and
// legal per the wasm spec. Must parse cleanly with an empty data vector.
BOOST_AUTO_TEST_CASE( empty_user_section_payload ) {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   std::vector<uint8_t> wasm( std::begin(wasm_preamble), std::end(wasm_preamble) );
   // custom section: id 0, section size 2, name length 1, name "A", zero payload bytes
   const uint8_t custom_section[] = { 0x00, 0x02, 0x01, 'A' };
   wasm.insert( wasm.end(), std::begin(custom_section), std::end(custom_section) );

   IR::Module module = deserialize_as_compile_child( wasm );
   BOOST_REQUIRE_EQUAL( module.userSections.size(), 1u );
   BOOST_TEST( module.userSections[0].name == "A" );
   BOOST_TEST( module.userSections[0].data.empty() );
#endif
}

// Degenerate further: the name is empty too (section payload is just the zero name length).
// std::string::data() is guaranteed non-null even when empty, but the section data vector is
// not -- both must be tolerated. Runs with default limit checking enabled to cover the
// check_limits == true branch as well.
BOOST_AUTO_TEST_CASE( empty_user_section_name_and_payload ) {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   std::vector<uint8_t> wasm( std::begin(wasm_preamble), std::end(wasm_preamble) );
   // custom section: id 0, section size 1, name length 0, no name, zero payload bytes
   const uint8_t custom_section[] = { 0x00, 0x01, 0x00 };
   wasm.insert( wasm.end(), std::begin(custom_section), std::end(custom_section) );

   IR::Module module;
   Serialization::MemoryInputStream stream( wasm.data(), wasm.size() );
   WASM::serialize( stream, module );
   BOOST_REQUIRE_EQUAL( module.userSections.size(), 1u );
   BOOST_TEST( module.userSections[0].name.empty() );
   BOOST_TEST( module.userSections[0].data.empty() );
#endif
}

// A custom section truncated to zero total bytes cannot even hold its name length; it must be
// rejected with a serialization exception rather than crash.
BOOST_AUTO_TEST_CASE( truncated_user_section_throws ) {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   std::vector<uint8_t> wasm( std::begin(wasm_preamble), std::end(wasm_preamble) );
   // custom section: id 0, section size 0 -- no room for the name length varint
   const uint8_t custom_section[] = { 0x00, 0x00 };
   wasm.insert( wasm.end(), std::begin(custom_section), std::end(custom_section) );

   BOOST_CHECK_THROW( deserialize_as_compile_child( wasm ), Serialization::FatalSerializationException );
#endif
}

BOOST_AUTO_TEST_SUITE_END()

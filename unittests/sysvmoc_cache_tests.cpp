#include <sysio/testing/tester.hpp>
#include <test_contracts.hpp>
#include <boost/test/unit_test.hpp>
#include <fstream>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
namespace {
/// Historical LLVM 18 on-disk header location and magic, independent of the new version.
constexpr std::streamoff legacy_header_offset = 512;
constexpr uint64_t llvm18_cache_magic = 0x33434f4d56535953ULL;
constexpr auto cache_filename = "code_cache.bin";
constexpr auto test_account = "cachetest"_n;
constexpr auto test_action = "doit"_n;

/// Read the persisted cache format marker, checking I/O rather than assuming a cache exists.
uint64_t read_cache_magic(const std::filesystem::path& file) {
   std::ifstream stream(file, std::ios::binary);
   uint64_t magic = 0;
   stream.seekg(legacy_header_offset);
   stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
   BOOST_REQUIRE(stream.good());
   return magic;
}
}

BOOST_AUTO_TEST_SUITE(sysvmoc_cache_tests)

/// A populated old cache is replaced on startup, then both cold and warm execution work.
BOOST_AUTO_TEST_CASE(llvm18_cache_recompiled_after_restart) { try {
   fc::temp_directory directory;
   tester chain(directory, [](controller::config& cfg) {
      cfg.wasm_runtime = wasm_interface::vm_type::sys_vm_oc;
   }, true);
   chain.create_accounts({test_account});
   chain.set_code(test_account, test_contracts::payloadless_wasm());
   chain.set_abi(test_account, test_contracts::payloadless_abi());
   chain.push_action(test_account, test_action, test_account, fc::mutable_variant_object());
   chain.produce_block();
   const auto cache = chain.get_config().state_dir / cache_filename;
   chain.close();
   const auto current_magic = read_cache_magic(cache);
   BOOST_REQUIRE_NE(current_magic, llvm18_cache_magic);

   // Simulate the persisted format produced before the compiler upgrade.
   {
      std::fstream stream(cache, std::ios::binary | std::ios::in | std::ios::out);
      stream.seekp(legacy_header_offset);
      stream.write(reinterpret_cast<const char*>(&llvm18_cache_magic), sizeof(llvm18_cache_magic));
      BOOST_REQUIRE(stream.good());
   }
   BOOST_REQUIRE_EQUAL(read_cache_magic(cache), llvm18_cache_magic);
   chain.open();
   BOOST_CHECK_EQUAL(read_cache_magic(cache), current_magic);
   chain.produce_block();
   BOOST_CHECK_NO_THROW(chain.push_action(test_account, test_action, test_account, fc::mutable_variant_object()));
   chain.produce_block();
   chain.close();

   // The new cache remains readable across a second restart.
   chain.open();
   BOOST_CHECK_EQUAL(read_cache_magic(cache), current_magic);
   chain.produce_block();
   BOOST_CHECK_NO_THROW(chain.push_action(test_account, test_action, test_account, fc::mutable_variant_object()));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
#endif

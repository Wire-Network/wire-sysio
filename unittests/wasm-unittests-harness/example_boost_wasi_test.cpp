// Example Boost.Test unit test meant to be compiled for WASI (wasm32-wasi)
// Build (outside this project, using a WASI-capable toolchain), e.g.:
//   clang++ --target=wasm32-wasi -O2 -nostartfiles -Wl,--export=__heap_base -Wl,--export=__data_end \
//     -Wl,--import-memory -Wl,--export-table -Wl,--no-gc-sections \
//     -o example_boost_wasi_test.wasm example_boost_wasi_test.cpp
// The above flags vary by runtime; simplest with wasmtime is usually:
//   clang++ --target=wasm32-wasi -O2 -o example_boost_wasi_test.wasm example_boost_wasi_test.cpp
//
// Run with harness:
//   wasm-unittests-harness example_boost_wasi_test.wasm
//
// This test uses Boost.Test's header-only runner which provides main().

#define BOOST_TEST_MODULE ExampleWasiBoostTests
// #include <boost/test/included/unit_test.hpp>

#include <cstdio>
#include <ostream>
#include <string>
#include <sysio.system/non_wasm_types.hpp>
#include <sysio.depot/sysio.opp.hpp>
#include <sysio/asset.hpp>
#include <sysio/datastream.hpp>
// #include <sys/stat.h>
#include <iostream>
#include <sysio.depot/sysio.opp.hpp>
#include <vector>
// BOOST_AUTO_TEST_SUITE(example_suite)
//
// BOOST_AUTO_TEST_CASE(simple_arithmetic) {
//   BOOST_TEST(1 + 1 == 2);
// }
//
// BOOST_AUTO_TEST_CASE(can_write_and_read_file_from_root_dir) {
//   // The harness preopens "/" for read/write. Create a temporary file.
//   const char* path = "/tmp_wasi_boost_test.txt";
//   {
//     std::ofstream out(path);
//     BOOST_REQUIRE(out.good());
//     out << "hello wasi";
//   }
//   std::ifstream in(path);
//   BOOST_REQUIRE(in.good());
//   std::string s; in >> s;
//   BOOST_TEST(s == "hello"); // first token
// }
//
// BOOST_AUTO_TEST_SUITE_END()

extern "C" int main(void) {
   using namespace sysio;
   using namespace sysio::opp;
   // The harness preopens "/" for read/write. Create a temporary file.
   constexpr std::string_view test_str{"hello wasi"};
   std::string                path{"/tmp/_wasi_boost_test.txt"};
   // sysio::print("Test file path: ", path);
   std::cout << "Test file path: " << test_str << std::endl;

   std::vector<char> buffer(std::size_t(1024));
   message_unknown   mb1{message_type_balance_sheet};
   META_DATASTREAM ds(buffer.data(), buffer.size());
   ds << mb1;
   // FILE* f = fopen(path.c_str(), "wb");
   // if (!f) {
   //    std::cerr << "Failed to open file for writing" << std::endl;
   //    return 1;
   // }
   //
   // std::cout << "Writing" << std::endl;
   // {
   //
   //    fwrite(test_str.data(), 1, test_str.length(), f);
   //    fclose(f);
   // }
   //
   // std::cout << "Stating" << std::endl;
   // std::size_t f_size{0};
   // {
   //    struct stat st;
   //    if (stat(path.c_str(), &st) != 0) {
   //       std::cerr << "Stating Failed" << std::endl;
   //       return 1;
   //    }
   //    f_size = st.st_size;
   // }
   //
   // std::cout << "Reading" << std::endl;
   // {
   //    f = fopen(path.c_str(), "rb");
   //    if (!f) {
   //       std::cerr << "Failed to open file for reading" << std::endl;
   //       return 1;
   //    }
   //
   //    std::vector<char> buffer(f_size);
   //    size_t            bytes = fread(buffer.data(), 1, f_size, f);
   //    fclose(f);
   //    if (bytes != f_size) {
   //       std::cerr << "Failed to read all bytes. Read=" << bytes << ",expected=" << f_size << std::endl;
   //       return 1;
   //    }
   //    std::string s(buffer.begin(), buffer.end());
   //    std::cout << "Read: " << s << std::endl;
   //    if (s != test_str) {
   //       std::cerr << "Read data does not match expected" << std::endl;
   //       return 1;
   //    }
   // }

   return 0;
};

#include <format>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include <boost/dll.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/spawn.hpp>
#include <fc/io/json.hpp>
#include <fc-test/crypto_utils.hpp>

namespace fc::test {

constexpr auto wire_build_root_file = ".wire-build-root";

namespace {
namespace bp       = boost::process;
namespace bfs = boost::filesystem;
auto exe_path      = boost::dll::program_location().parent_path();
}
bfs::path get_build_root_path() {
   static std::mutex mutex;
   static std::optional<bfs::path> build_root_path;
   std::lock_guard lock(mutex);

   if (build_root_path.has_value()) {
      return build_root_path.value();
   }

   bfs::path current_path = exe_path;
   while (current_path != current_path.root_path()) {
      if (bfs::exists(current_path / wire_build_root_file)) {
         build_root_path = current_path;
         return current_path;
      }
      current_path = current_path.parent_path();
   }
   FC_THROW_EXCEPTION(fc::exception, "Unable to find build directory");
}

bfs::path get_source_root_path() {
   static std::mutex mutex;
   static std::optional<bfs::path> source_root_path;
   std::lock_guard lock(mutex);

   if (source_root_path.has_value()) {
      return source_root_path.value();
   }

   auto current_path = get_build_root_path();
   while (current_path != current_path.root_path()) {
      if (bfs::is_directory(current_path / ".git")) {
         source_root_path = current_path;
         return current_path;
      }
      current_path = current_path.parent_path();
   }
   FC_THROW_EXCEPTION(fc::exception, "Unable to find source root directory");
}

bfs::path get_test_fixtures_path() {
   return get_source_root_path() / "tests" / "fixtures";
}


}
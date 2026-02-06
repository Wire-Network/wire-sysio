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
}

bfs::path get_exe_path() {
   static std::mutex mutex;
   static std::optional<bfs::path> exe_path;
   std::scoped_lock lock(mutex);

   if (exe_path.has_value()) {
      return exe_path.value();
   }

   auto p  = boost::dll::program_location();
   FC_ASSERT(!p.string().empty(), "exe_path is empty");
   exe_path = p;
   return exe_path.value();
}

bfs::path get_build_root_path() {
   static std::mutex mutex;
   static std::optional<bfs::path> build_root_path;
   std::scoped_lock lock(mutex);

   if (build_root_path.has_value()) {
      return build_root_path.value();
   }

   auto exe_path = get_exe_path();
   bfs::path current_path = exe_path.parent_path();
   while (current_path != current_path.root_path()) {
      if (bfs::exists(current_path / wire_build_root_file)) {
         build_root_path = current_path;
         return current_path;
      }
      current_path = current_path.parent_path();
   }
   FC_THROW_EXCEPTION(fc::exception, "Unable to find build directory (exe_path={},dll_program_path={})",
                      exe_path.string(), boost::dll::program_location().string());
}

bfs::path get_source_root_path() {
   static std::mutex mutex;
   static std::optional<bfs::path> source_root_path;
   std::scoped_lock lock(mutex);

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
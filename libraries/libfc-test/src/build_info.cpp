#include <format>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include <boost/process/v1/io.hpp>
#include <boost/process/v1/spawn.hpp>
#include <fc/io/json.hpp>
#include <fc/process.hpp>
#include <fc-test/crypto_utils.hpp>

namespace fc::test {

constexpr auto wire_build_root_file = ".wire-build-root";

namespace {
namespace bp       = boost::process;
}

std::filesystem::path get_exe_path() {
   static std::mutex mutex;
   static std::optional<std::filesystem::path> exe_path;
   std::scoped_lock lock(mutex);

   if (exe_path.has_value()) {
      return exe_path.value();
   }

   const std::filesystem::path p = program_path();
   FC_ASSERT(!p.string().empty(), "exe_path is empty");
   exe_path = p;
   return exe_path.value();
}

std::filesystem::path get_build_root_path() {
   static std::mutex mutex;
   static std::optional<std::filesystem::path> build_root_path;
   std::scoped_lock lock(mutex);

   if (build_root_path.has_value()) {
      return build_root_path.value();
   }

   auto exe_path = get_exe_path();
   std::filesystem::path current_path = exe_path.parent_path();
   while (current_path != current_path.root_path()) {
      if (std::filesystem::exists(current_path / wire_build_root_file)) {
         build_root_path = current_path;
         return current_path;
      }
      current_path = current_path.parent_path();
   }
   FC_THROW_EXCEPTION(fc::exception, "Unable to find build directory (exe_path={})", exe_path.string());
}

std::filesystem::path get_source_root_path() {
   static std::mutex mutex;
   static std::optional<std::filesystem::path> source_root_path;
   std::scoped_lock lock(mutex);

   if (source_root_path.has_value()) {
      return source_root_path.value();
   }

   auto current_path = get_build_root_path();
   while (current_path != current_path.root_path()) {
      // .git is a directory in a primary checkout but a FILE in a linked git worktree; accept both
      // so tests that load fixtures from the source tree run in worktree checkouts too.
      if (std::filesystem::exists(current_path / ".git")) {
         source_root_path = current_path;
         return current_path;
      }
      current_path = current_path.parent_path();
   }
   FC_THROW_EXCEPTION(fc::exception, "Unable to find source root directory");
}

std::filesystem::path get_test_fixtures_path() {
   return get_source_root_path() / "tests" / "fixtures";
}


}
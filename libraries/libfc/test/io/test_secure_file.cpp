#include <boost/test/unit_test.hpp>

#include <fc/filesystem.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/secure_file.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

BOOST_AUTO_TEST_SUITE(secure_file_test_suite)

#ifndef _WIN32
namespace {

constexpr mode_t permissive_file_creation_umask = S_IWGRP | S_IWOTH;
constexpr mode_t secret_file_mode              = S_IRUSR | S_IWUSR;
constexpr mode_t group_world_readable_mode     = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
constexpr auto   first_secret_contents         = "secret\n";
constexpr auto   second_secret_contents        = "replacement\n";
constexpr auto   symlink_target_contents       = "target\n";
constexpr long   fallback_name_max             = 255;
constexpr long   minimum_long_filename_name_max = 64;

/**
 * Owns an open file stream and closes its underlying descriptor when a test scope exits.
 */
using file_stream_ptr = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

/**
 * Restores the process umask when a test scope exits.
 */
class scoped_umask {
public:
   explicit scoped_umask(mode_t new_umask)
      : old_umask_(::umask(new_umask)) {}

   ~scoped_umask() {
      ::umask(old_umask_);
   }

private:
   mode_t old_umask_;
};

/**
 * Returns POSIX permission bits for a filesystem path.
 */
mode_t file_mode(const std::filesystem::path& file_path) {
   struct stat stat_result {};
   BOOST_REQUIRE_EQUAL(::stat(file_path.c_str(), &stat_result), 0);
   return stat_result.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
}

/**
 * Reads a whole file as a string for content assertions.
 */
std::string file_contents(const std::filesystem::path& file_path) {
   std::string contents;
   fc::read_file_contents(file_path, contents);
   return contents;
}

/**
 * Reads the contents of an opened file stream from the beginning.
 */
std::string stream_contents(std::FILE* file) {
   BOOST_REQUIRE_EQUAL(std::fseek(file, 0, SEEK_SET), 0);

   char       buffer[128];
   const auto bytes_read = std::fread(buffer, sizeof(char), sizeof(buffer), file);
   BOOST_REQUIRE(!std::ferror(file));
   return std::string(buffer, bytes_read);
}

/**
 * Returns a test-safe near-limit filename length for the temporary directory.
 */
size_t near_limit_file_name_length(const std::filesystem::path& directory_path) {
   const auto name_max = ::pathconf(directory_path.c_str(), _PC_NAME_MAX);
   const auto limit    = name_max > 0 ? name_max : fallback_name_max;
   BOOST_REQUIRE_GT(limit, minimum_long_filename_name_max);
   return static_cast<size_t>(limit - 1);
}

} // namespace

BOOST_AUTO_TEST_CASE(write_secure_file_creates_owner_only_file_under_permissive_umask) {
   fc::temp_directory tempdir;
   const auto         secret_path = tempdir.path() / "generated-secret.txt";

   {
      scoped_umask umask_guard(permissive_file_creation_umask);
      fc::write_secure_file(secret_path, first_secret_contents);
   }

   BOOST_REQUIRE_EQUAL(file_mode(secret_path), secret_file_mode);
   BOOST_REQUIRE_EQUAL(file_contents(secret_path), first_secret_contents);
}

BOOST_AUTO_TEST_CASE(write_secure_file_replaces_existing_file_with_owner_only_file) {
   fc::temp_directory tempdir;
   const auto         secret_path = tempdir.path() / "existing-secret.txt";

   {
      std::ofstream out(secret_path);
      out << first_secret_contents;
   }
   BOOST_REQUIRE_EQUAL(::chmod(secret_path.c_str(), group_world_readable_mode), 0);
   BOOST_REQUIRE_EQUAL(file_mode(secret_path), group_world_readable_mode);
   file_stream_ptr existing_reader(std::fopen(secret_path.c_str(), "r"), &std::fclose);
   BOOST_REQUIRE(existing_reader != nullptr);

   fc::write_secure_file(secret_path, second_secret_contents);

   BOOST_REQUIRE_EQUAL(file_mode(secret_path), secret_file_mode);
   BOOST_REQUIRE_EQUAL(file_contents(secret_path), second_secret_contents);
   BOOST_REQUIRE_EQUAL(stream_contents(existing_reader.get()), first_secret_contents);
}

BOOST_AUTO_TEST_CASE(write_secure_file_handles_near_limit_file_name) {
   fc::temp_directory tempdir;
   const auto         secret_path = tempdir.path() / std::string(near_limit_file_name_length(tempdir.path()), 's');

   fc::write_secure_file(secret_path, first_secret_contents);

   BOOST_REQUIRE_EQUAL(file_mode(secret_path), secret_file_mode);
   BOOST_REQUIRE_EQUAL(file_contents(secret_path), first_secret_contents);
}

BOOST_AUTO_TEST_CASE(secure_output_file_supports_move_construction_and_assignment) {
   fc::temp_directory tempdir;
   const auto         move_constructed_path = tempdir.path() / "move-constructed-secret.txt";
   const auto         move_assigned_path    = tempdir.path() / "move-assigned-secret.txt";
   const auto         discarded_path        = tempdir.path() / "discarded-secret.txt";

   fc::secure_output_file original(move_constructed_path);
   fc::secure_output_file move_constructed(std::move(original));
   move_constructed.write(first_secret_contents);
   move_constructed.close();

   fc::secure_output_file move_source(move_assigned_path);
   fc::secure_output_file move_assigned(discarded_path);
   move_assigned = std::move(move_source);
   move_assigned.write(second_secret_contents);
   move_assigned.close();

   BOOST_REQUIRE_EQUAL(file_contents(move_constructed_path), first_secret_contents);
   BOOST_REQUIRE_EQUAL(file_contents(move_assigned_path), second_secret_contents);
   BOOST_REQUIRE(!std::filesystem::exists(discarded_path));
}

BOOST_AUTO_TEST_CASE(secure_output_file_destruction_without_close_discards_pending_file) {
   fc::temp_directory tempdir;
   const auto         secret_path = tempdir.path() / "discarded-secret.txt";

   {
      fc::secure_output_file output_file(secret_path);
      output_file.write(first_secret_contents);
   }

   BOOST_REQUIRE(!std::filesystem::exists(secret_path));
}

BOOST_AUTO_TEST_CASE(write_secure_file_rejects_non_regular_target) {
   fc::temp_directory tempdir;
   const auto         fifo_path = tempdir.path() / "secret.fifo";

   BOOST_REQUIRE_EQUAL(::mkfifo(fifo_path.c_str(), secret_file_mode), 0);
   BOOST_CHECK_THROW(fc::write_secure_file(fifo_path, first_secret_contents), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(write_secure_file_rejects_final_symlink_target) {
   fc::temp_directory tempdir;
   const auto         target_path = tempdir.path() / "target.txt";
   const auto         link_path   = tempdir.path() / "linked-secret.txt";

   {
      std::ofstream out(target_path);
      out << symlink_target_contents;
   }
   std::filesystem::create_symlink(target_path, link_path);

   BOOST_CHECK_THROW(fc::write_secure_file(link_path, first_secret_contents), std::ios_base::failure);
   BOOST_REQUIRE_EQUAL(file_contents(target_path), symlink_target_contents);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

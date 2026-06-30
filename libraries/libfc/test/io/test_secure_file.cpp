#include <boost/test/unit_test.hpp>

#include <fc/io/fstream.hpp>
#include <fc/io/secure_file.hpp>

#include <fstream>
#include <string>

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

BOOST_AUTO_TEST_CASE(write_secure_file_restricts_existing_file_before_replacing_contents) {
   fc::temp_directory tempdir;
   const auto         secret_path = tempdir.path() / "existing-secret.txt";

   {
      std::ofstream out(secret_path);
      out << first_secret_contents;
   }
   BOOST_REQUIRE_EQUAL(::chmod(secret_path.c_str(), group_world_readable_mode), 0);
   BOOST_REQUIRE_EQUAL(file_mode(secret_path), group_world_readable_mode);

   fc::write_secure_file(secret_path, second_secret_contents);

   BOOST_REQUIRE_EQUAL(file_mode(secret_path), secret_file_mode);
   BOOST_REQUIRE_EQUAL(file_contents(secret_path), second_secret_contents);
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

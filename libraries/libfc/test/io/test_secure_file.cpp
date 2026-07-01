#include <boost/test/unit_test.hpp>

#include <fc/filesystem.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/secure_file.hpp>

#include <fstream>
#include <memory>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
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
 * Closes and releases a heap-owned POSIX file descriptor when a test scope exits.
 */
struct file_descriptor_deleter {
   void operator()(int* file_descriptor) const noexcept {
      if (file_descriptor != nullptr) {
         if (*file_descriptor >= 0) {
            ::close(*file_descriptor);
         }
         delete file_descriptor;
      }
   }
};

using file_descriptor_ptr = std::unique_ptr<int, file_descriptor_deleter>;

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
 * Reads the remaining contents of an opened file descriptor from the beginning.
 */
std::string descriptor_contents(int file_descriptor) {
   BOOST_REQUIRE_EQUAL(::lseek(file_descriptor, 0, SEEK_SET), 0);

   char       buffer[128];
   const auto bytes_read = ::read(file_descriptor, buffer, sizeof(buffer));
   BOOST_REQUIRE_GE(bytes_read, 0);
   return std::string(buffer, static_cast<size_t>(bytes_read));
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
   file_descriptor_ptr existing_reader(new int(::open(secret_path.c_str(), O_RDONLY)));
   BOOST_REQUIRE_GE(*existing_reader, 0);

   fc::write_secure_file(secret_path, second_secret_contents);

   BOOST_REQUIRE_EQUAL(file_mode(secret_path), secret_file_mode);
   BOOST_REQUIRE_EQUAL(file_contents(secret_path), second_secret_contents);
   BOOST_REQUIRE_EQUAL(descriptor_contents(*existing_reader), first_secret_contents);
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

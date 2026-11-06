#include <boost/test/unit_test.hpp>
#include <fc/io/atomic_file.hpp>
#include <fstream>

namespace {
/// Read the visible output independently of the writer's stream.
std::string read_contents(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// A private directory with an existing destination that failed publications must preserve.
struct atomic_file_fixture {
   fc::temp_directory directory;
   std::filesystem::path destination = directory.path() / "output";
   atomic_file_fixture() { std::ofstream(destination) << "old"; }
};
}

BOOST_FIXTURE_TEST_SUITE(atomic_file_tests, atomic_file_fixture)

/// Readers retain the old contents until explicit commit, for both ordinary and durable writes.
BOOST_AUTO_TEST_CASE(replacement_and_durability) {
   for (bool durable : {false, true}) {
      std::ofstream(destination) << "old";
      fc::io::atomic_file_writer writer(destination);
      writer.file().write("new", 3);
      BOOST_CHECK_EQUAL(read_contents(destination), "old");
      BOOST_CHECK(!writer.published());
      writer.commit(durable);
      BOOST_CHECK(writer.published());
      BOOST_CHECK_EQUAL(read_contents(destination), "new");
      BOOST_CHECK(!std::filesystem::exists(writer.temporary_path()));
      BOOST_CHECK_THROW(writer.commit(), std::logic_error);
   }
}

/// Publication retains the callers' ordinary creation permission policy instead of forcing mode 0600.
BOOST_AUTO_TEST_CASE(ordinary_creation_permissions) {
   const auto reference = directory.path() / "reference";
   { fc::cfile ordinary(reference); }
   fc::io::atomic_file_writer writer(destination);
   writer.commit();
   BOOST_CHECK(std::filesystem::status(destination).permissions() ==
               std::filesystem::status(reference).permissions());
}

/// Abandoned and throwing writers remove only their own sibling, including with concurrent writers.
BOOST_AUTO_TEST_CASE(cleanup_and_stale_siblings) {
   const auto prefix = "." + destination.filename().string() + ".tmp." + std::to_string(::getpid()) + ".";
   const auto stale = destination.parent_path() / (prefix + "stale");
   std::ofstream(stale) << "stale";
   fc::io::atomic_file_writer survivor(destination);
   BOOST_CHECK(survivor.temporary_path().filename().string().starts_with(prefix));
   std::filesystem::path abandoned;
   try {
      fc::io::atomic_file_writer writer(destination);
      abandoned = writer.temporary_path();
      BOOST_CHECK(abandoned != survivor.temporary_path());
      writer.file().write("partial", 7);
      throw std::runtime_error("serialization failed");
   } catch (const std::runtime_error&) {}
   BOOST_CHECK(!std::filesystem::exists(abandoned));
   BOOST_CHECK(std::filesystem::exists(survivor.temporary_path()));
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
   BOOST_CHECK_EQUAL(read_contents(stale), "stale");
   survivor.file().write("ok", 2);
   survivor.commit();
   BOOST_CHECK_EQUAL(read_contents(destination), "ok");
}

/// A failed rename must not replace a directory or retain the unpublished sibling.
BOOST_AUTO_TEST_CASE(rename_failure) {
   const auto target = directory.path() / "directory";
   std::filesystem::create_directory(target);
   std::ofstream(target / "sentinel") << "old";
   std::filesystem::path temporary;
   {
      fc::io::atomic_file_writer writer(target);
      temporary = writer.temporary_path();
      writer.file().write("new", 3);
      BOOST_CHECK_THROW(writer.commit(), std::filesystem::filesystem_error);
      BOOST_CHECK(!writer.published());
   }
   BOOST_CHECK_EQUAL(read_contents(target / "sentinel"), "old");
   BOOST_CHECK(!std::filesystem::exists(temporary));
}

/// Path-only users release their writer before the guard cleans up on serialization failure.
BOOST_AUTO_TEST_CASE(external_writer_lifecycle) {
   std::filesystem::path temporary;
   try {
      fc::io::atomic_file output(destination);
      temporary = output.temporary_path();
      std::ofstream writer(temporary);
      writer << "partial";
      throw std::runtime_error("external writer failure");
   } catch (const std::runtime_error&) {}
   BOOST_CHECK(!std::filesystem::exists(temporary));
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
   fc::io::atomic_file output(destination);
   { std::ofstream writer(output.temporary_path()); writer << "external"; }
   output.commit();
   BOOST_CHECK_EQUAL(read_contents(destination), "external");
}

/// The injectable query is independent of file creation and receives the destination filesystem.
BOOST_AUTO_TEST_CASE(space_query) {
   constexpr uint64_t available = 123;
   BOOST_CHECK_EQUAL(fc::io::atomic_file::available_space(destination, [&](const auto& parent) {
      BOOST_CHECK(parent == std::filesystem::absolute(directory.path()));
      return available;
   }), available);
   BOOST_CHECK_THROW(fc::io::atomic_file::available_space(destination, [](const auto&) -> uint64_t {
      throw std::runtime_error("space query failed");
   }), std::runtime_error);
}

/// A descriptor that fails only at close must never be published, and must not be closed twice.
BOOST_AUTO_TEST_CASE(close_failure) {
   std::filesystem::path temporary;
   {
      fc::io::atomic_file_writer writer(destination);
      temporary = writer.temporary_path();
      writer.file().write("new", 3);
      writer.file().flush();
      BOOST_REQUIRE_EQUAL(::close(writer.file().fileno()), 0);
      BOOST_CHECK_THROW(writer.commit(), std::ios_base::failure);
      BOOST_CHECK(!writer.file().is_open());
      BOOST_CHECK_THROW(writer.commit(), std::logic_error);
   }
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
   BOOST_CHECK(!std::filesystem::exists(temporary));
}

#ifdef __linux__
/// A caller catching a short write cannot subsequently publish the damaged stream, even if flushing recovers.
BOOST_AUTO_TEST_CASE(write_failure_remains_unpublishable) {
   fc::io::atomic_file_writer writer(destination);
   const int original = ::dup(writer.file().fileno());
   BOOST_REQUIRE(original >= 0);
   const int full = ::open("/dev/full", O_WRONLY);
   BOOST_REQUIRE(full >= 0);
   const int redirected = ::dup2(full, writer.file().fileno());
   ::close(full);
   BOOST_REQUIRE(redirected >= 0);
   constexpr size_t unbuffered_write_size = 64 * 1024;
   const std::string bytes(unbuffered_write_size, 'x');
   BOOST_CHECK_THROW(writer.file().write(bytes.data(), bytes.size()), std::ios_base::failure);
   const int restored = ::dup2(original, writer.file().fileno());
   ::close(original);
   BOOST_REQUIRE(restored >= 0);
   BOOST_CHECK_THROW(writer.commit(), std::ios_base::failure);
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
}

/// /dev/full injects a delayed buffered-write failure without requiring a full test filesystem.
BOOST_AUTO_TEST_CASE(flush_failure) {
   std::filesystem::path temporary;
   {
      fc::io::atomic_file_writer writer(destination);
      temporary = writer.temporary_path();
      const int full = ::open("/dev/full", O_WRONLY);
      BOOST_REQUIRE(full >= 0);
      const int redirected = ::dup2(full, writer.file().fileno());
      ::close(full);
      BOOST_REQUIRE(redirected >= 0);
      writer.file().write("buffered", 8);
      BOOST_CHECK_THROW(writer.commit(), std::ios_base::failure);
   }
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
   BOOST_CHECK(!std::filesystem::exists(temporary));
}
#endif

/// Sync setup failures before rename preserve the destination.
BOOST_AUTO_TEST_CASE(durable_failure_before_rename) {
   fc::io::atomic_file output(destination);
   std::filesystem::remove(output.temporary_path());
   BOOST_CHECK_THROW(output.commit(true), std::ios_base::failure);
   BOOST_CHECK(!output.published());
   BOOST_CHECK_EQUAL(read_contents(destination), "old");
}

/// Directory synchronization setup can fail after rename; publication status must remain truthful.
BOOST_AUTO_TEST_CASE(durable_failure_after_rename) {
   if (::geteuid() == 0)
      return; // root bypasses directory read permissions
   fc::io::atomic_file_writer writer(destination);
   writer.file().write("new", 3);
   BOOST_REQUIRE_EQUAL(::chmod(directory.path().c_str(), S_IWUSR | S_IXUSR), 0);
   bool failed = false;
   try { writer.commit(true); } catch (const std::system_error&) { failed = true; }
   const int restored = ::chmod(directory.path().c_str(), S_IRWXU);
   BOOST_REQUIRE_EQUAL(restored, 0);
   BOOST_CHECK(failed);
   BOOST_CHECK(writer.published());
   BOOST_CHECK_EQUAL(read_contents(destination), "new");
}

BOOST_AUTO_TEST_SUITE_END()

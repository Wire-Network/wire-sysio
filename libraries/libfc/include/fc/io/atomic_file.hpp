#pragma once

#include <fc/io/cfile.hpp>
#include <functional>
#include <random>

namespace fc::io {

/** Own an exclusively created sibling until explicit publication.
 * External writers must finish and check their close before commit(). Declare them after this guard
 * so they release resources before exception cleanup. Concurrent publications are last-rename-wins.
 * Crash leftovers are never reused or removed by another operation; destruction only handles normal
 * unwinding, not process termination. The destination directory must be trusted against path replacement.
 */
class atomic_file {
public:
   using space_available_provider = std::function<uint64_t(const std::filesystem::path&)>;

   /// Reserve a fresh sibling with ordinary cfile permissions (0666 & umask), without changing the destination.
   explicit atomic_file(const std::filesystem::path& destination)
      : _destination(std::filesystem::absolute(destination)) {
      std::random_device random;
      for (unsigned attempt = 0; attempt < max_reservation_attempts; ++attempt) {
         _temporary = _destination.string() + temporary_suffix + std::to_string(random()) + "-" +
                      std::to_string(random()) + "-" + std::to_string(random());
         cfile reservation;
         reservation.set_file_path(_temporary);
         try {
            reservation.open(cfile::create_new_rw_mode);
         } catch (const std::ios_base::failure& error) {
            if (error.code().value() == EEXIST)
               continue; // Never truncate or remove a file belonging to another operation.
            throw;
         }
         try {
            reservation.close_checked();
         } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(_temporary, ignored);
            throw;
         }
         return;
      }
      throw std::ios_base::failure("Cannot reserve a unique atomic file sibling");
   }

   atomic_file(const atomic_file&) = delete;
   atomic_file& operator=(const atomic_file&) = delete;

   /// Remove only this operation's unpublished sibling; never publish during destruction.
   ~atomic_file() {
      if (!_published) {
         std::error_code ignored;
         std::filesystem::remove(_temporary, ignored);
      }
   }

   /// Path reserved for an external writer, which must preserve exclusive ownership of this sibling.
   const std::filesystem::path& temporary_path() const { return _temporary; }

   /// True once rename succeeded, including when subsequent directory synchronization failed.
   bool published() const { return _published; }

   /** Publish completed, closed output. Durability is opt-in and synchronizes both file and directory.
    * Failures before rename preserve the old destination. A directory-sync failure throws after publication;
    * published() remains true and the new destination must not be removed or rolled back.
    */
   void commit(bool durable = false) {
      if (_published)
         throw std::logic_error("Atomic file already published");
      if (durable) {
         cfile completed(_temporary, cfile::update_rw_mode);
         completed.sync();
         completed.close_checked();
      }
      std::filesystem::rename(_temporary, _destination);
      _published = true;
      if (durable) {
         const int fd = ::open(_destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "Published file; cannot open parent directory");
         const int result = ::fsync(fd);
         const int error = errno;
         ::close(fd);
         if (result != 0)
            throw std::system_error(error, std::generic_category(), "Published file; cannot sync parent directory");
      }
   }

   /// Query the destination filesystem without creating a file; request-specific budgets belong to callers.
   static uint64_t available_space(const std::filesystem::path& destination,
                                   const space_available_provider& provider = {}) {
      const auto parent = std::filesystem::absolute(destination).parent_path();
      return provider ? provider(parent) : std::filesystem::space(parent).available;
   }

private:
   static constexpr auto temporary_suffix = ".tmp.";
   static constexpr unsigned max_reservation_attempts = 64;
   std::filesystem::path _destination;
   std::filesystem::path _temporary;
   bool _published = false;
};

/** cfile writes with checked completion and explicit atomic publication; no synchronization by default. */
class atomic_file_writer {
public:
   explicit atomic_file_writer(const std::filesystem::path& destination)
      : _publication(destination), _file(_publication.temporary_path(), cfile::update_rw_mode) {}

   /// Borrow the output for writes only; do not close, reopen, move, or change its path.
   cfile& file() { return _file; }
   /// Expose the sibling for diagnostics, not another writer.
   const std::filesystem::path& temporary_path() const { return _publication.temporary_path(); }
   /// Report whether rename succeeded even if durable directory synchronization failed.
   bool published() const { return _publication.published(); }

   /// Flush and check close before rename. A failed commit cannot be retried on a possibly damaged stream.
   void commit(bool durable = false) {
      if (_commit_attempted)
         throw std::logic_error("Atomic file commit already attempted");
      _commit_attempted = true;
      _file.flush();
      _file.close_checked();
      _publication.commit(durable);
   }

private:
   atomic_file _publication;
   cfile _file; // destroyed before the publication guard removes the sibling
   bool _commit_attempted = false;
};

} // namespace fc::io

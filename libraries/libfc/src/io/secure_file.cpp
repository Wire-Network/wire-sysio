#include <fc/io/secure_file.hpp>
#include <fc/scoped_exit.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fc {

namespace {

#ifndef _WIN32
constexpr mode_t      secret_file_mode        = S_IRUSR | S_IWUSR;
constexpr auto        temp_file_prefix        = ".";
constexpr auto        temp_file_separator     = ".tmp.";
constexpr auto        temp_file_fallback_name = "secure-output";
constexpr unsigned    max_temp_file_attempts  = 100;
constexpr size_t      fallback_name_max       = 255;
std::atomic<uint64_t> temp_file_counter{0};

/**
 * Returns the platform open flags needed for temporary secret output files.
 */
int secret_file_open_flags() {
   int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   return flags;
}

/**
 * Returns the platform open flags needed to sync a containing directory.
 */
int directory_open_flags() {
   int flags = O_RDONLY;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
   flags |= O_DIRECTORY;
#endif
   return flags;
}

/**
 * Returns the directory where the temporary file should be created.
 */
std::filesystem::path temp_file_directory(const std::filesystem::path& file_path) {
   const auto parent = file_path.parent_path();
   return parent.empty() ? std::filesystem::path(".") : parent;
}

/**
 * Returns the maximum filename component length supported by a directory.
 */
size_t directory_name_max(const std::filesystem::path& directory_path) noexcept {
   errno             = 0;
   const auto result = ::pathconf(directory_path.c_str(), _PC_NAME_MAX);
   if (result > 0)
      return static_cast<size_t>(result);
   return fallback_name_max;
}

/**
 * Returns a bounded filename component suitable for deriving a temporary secret-file path.
 */
std::string temp_file_base_name(const std::filesystem::path& file_path, size_t max_length) {
   auto base_name = file_path.filename().string();
   if (base_name.empty())
      base_name = temp_file_fallback_name;
   if (base_name.size() > max_length)
      base_name.resize(max_length);
   return base_name;
}

/**
 * Returns a candidate temporary path in the target directory.
 */
std::filesystem::path make_temp_file_path(const std::filesystem::path& file_path, uint64_t attempt) {
   const auto directory = temp_file_directory(file_path);
   const auto unique_id = temp_file_counter.fetch_add(1, std::memory_order_relaxed);
   const auto suffix    = std::string(temp_file_separator) + std::to_string(::getpid()) + "." +
                       std::to_string(unique_id) + "." + std::to_string(attempt);
   const auto prefix_size       = std::string_view(temp_file_prefix).size();
   const auto max_name_length   = directory_name_max(directory);
   const auto fixed_name_length = prefix_size + suffix.size();
   const auto max_base_length   = max_name_length > fixed_name_length ? max_name_length - fixed_name_length : 0;
   const auto temp_name = std::string(temp_file_prefix) + temp_file_base_name(file_path, max_base_length) + suffix;
   return directory / temp_name;
}

/**
 * Removes a temporary file without throwing.
 */
void unlink_noexcept(const std::filesystem::path& file_path) noexcept {
   if (!file_path.empty())
      ::unlink(file_path.c_str());
}

/**
 * Closes a file descriptor without throwing.
 */
void close_descriptor_noexcept(int file_descriptor) noexcept {
   if (file_descriptor >= 0)
      ::close(file_descriptor);
}
#endif

/**
 * Throws a filesystem failure tagged with the secret-file operation.
 */
[[noreturn]] void throw_file_failure(const std::filesystem::path& file_path,
                                     const std::string& action,
                                     int err) {
   throw std::ios_base::failure("secure_output_file unable to " + action + ": " + file_path.string(),
                                std::error_code(err, std::generic_category()));
}

#ifndef _WIN32
/**
 * Syncs the directory containing the published target path.
 */
void sync_target_directory(const std::filesystem::path& file_path) {
   const auto directory_path = temp_file_directory(file_path);
   int        directory_fd   = ::open(directory_path.c_str(), directory_open_flags());
   if (directory_fd == -1)
      throw_file_failure(file_path, "open target directory", errno);

   auto close_directory = fc::make_scoped_exit([&]() noexcept { close_descriptor_noexcept(directory_fd); });

   if (::fsync(directory_fd) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path, "sync published target directory", saved_errno);
   }

   close_directory.cancel();
   if (::close(directory_fd) == -1)
      throw_file_failure(file_path, "close target directory", errno);
}

/**
 * Rejects existing final targets that should not be replaced by a secret file.
 */
void throw_if_target_is_not_replaceable(const std::filesystem::path& file_path) {
   struct stat stat_result {};
   if (::lstat(file_path.c_str(), &stat_result) == -1) {
      if (errno == ENOENT)
         return;
      throw_file_failure(file_path, "stat target", errno);
   }

   if (S_ISLNK(stat_result.st_mode))
      throw_file_failure(file_path, "replace symlink target", ELOOP);

   if (!S_ISREG(stat_result.st_mode))
      throw_file_failure(file_path, "replace regular file", EINVAL);
}

/**
 * Opens a unique owner-only temporary file in the target directory.
 */
std::pair<int, std::filesystem::path> open_temp_file(const std::filesystem::path& file_path) {
   for (unsigned attempt = 0; attempt < max_temp_file_attempts; ++attempt) {
      auto temp_path = make_temp_file_path(file_path, attempt);
      int  fd        = ::open(temp_path.c_str(), secret_file_open_flags(), secret_file_mode);
      if (fd >= 0)
         return {fd, std::move(temp_path)};

      if (errno != EEXIST)
         throw_file_failure(file_path, "open temporary file", errno);
   }

   throw_file_failure(file_path, "create unique temporary file", EEXIST);
}
#endif

} // namespace

secure_output_file::secure_output_file(std::filesystem::path file_path)
   : file_path_(std::move(file_path)) {
#ifndef _WIN32
   throw_if_target_is_not_replaceable(file_path_);

   auto [fd, temp_path] = open_temp_file(file_path_);
   auto cleanup_on_failure = fc::make_scoped_exit([&]() noexcept {
      close_descriptor_noexcept(fd);
      unlink_noexcept(temp_path);
   });

   struct stat stat_result {};
   if (::fstat(fd, &stat_result) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path_, "stat temporary file", saved_errno);
   }

   if (!S_ISREG(stat_result.st_mode)) {
      throw_file_failure(file_path_, "open regular temporary file", EINVAL);
   }

   if (::fchmod(fd, secret_file_mode) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path_, "restrict temporary file permissions", saved_errno);
   }

   file_descriptor_ = fd;
   temp_file_path_  = std::move(temp_path);
   cleanup_on_failure.cancel();
#else
   file_.open(file_path_, std::ios::out | std::ios::binary | std::ios::trunc);
   if (!file_)
      throw_file_failure(file_path_, "open", errno);
#endif
}

secure_output_file::secure_output_file(secure_output_file&& other) noexcept
   : file_path_(std::move(other.file_path_)) {
#ifndef _WIN32
   file_descriptor_ = std::exchange(other.file_descriptor_, invalid_file_descriptor);
   temp_file_path_  = std::move(other.temp_file_path_);
   other.temp_file_path_.clear();
#else
   file_ = std::move(other.file_);
#endif
}

secure_output_file& secure_output_file::operator=(secure_output_file&& other) noexcept {
   if (this != &other) {
      close_noexcept();
      file_path_ = std::move(other.file_path_);
#ifndef _WIN32
      file_descriptor_ = std::exchange(other.file_descriptor_, invalid_file_descriptor);
      temp_file_path_  = std::move(other.temp_file_path_);
      other.temp_file_path_.clear();
#else
      file_ = std::move(other.file_);
#endif
   }
   return *this;
}

secure_output_file::~secure_output_file() {
   close_noexcept();
}

void secure_output_file::write(std::string_view content) {
#ifndef _WIN32
   auto*  cursor    = content.data();
   size_t remaining = content.size();
   while (remaining > 0) {
      const auto chunk  = std::min(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
      const auto result = ::write(file_descriptor_, cursor, chunk);
      if (result > 0) {
         cursor += result;
         remaining -= static_cast<size_t>(result);
      } else if (result == -1 && errno == EINTR) {
         continue;
      } else {
         throw_file_failure(file_path_, "write", result == -1 ? errno : EIO);
      }
   }
#else
   file_.write(content.data(), static_cast<std::streamsize>(content.size()));
   if (!file_)
      throw_file_failure(file_path_, "write", errno);
#endif
}

void secure_output_file::close() {
#ifndef _WIN32
   if (file_descriptor_ == invalid_file_descriptor)
      return;

   const int fd = std::exchange(file_descriptor_, invalid_file_descriptor);
   auto      cleanup_temp_file = fc::make_scoped_exit([this]() noexcept {
      unlink_noexcept(temp_file_path_);
      temp_file_path_.clear();
   });
   auto      close_descriptor   = fc::make_scoped_exit([fd]() noexcept { close_descriptor_noexcept(fd); });

   if (::fsync(fd) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path_, "sync temporary file", saved_errno);
   }

   close_descriptor.cancel();
   if (::close(fd) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path_, "close temporary file", saved_errno);
   }

   if (::rename(temp_file_path_.c_str(), file_path_.c_str()) == -1) {
      const int saved_errno = errno;
      throw_file_failure(file_path_, "commit temporary file", saved_errno);
   }

   // After rename succeeds, the final path is published; later failures can report only durability uncertainty.
   cleanup_temp_file.cancel();
   temp_file_path_.clear();
   sync_target_directory(file_path_);
#else
   if (!file_.is_open())
      return;

   file_.close();
   if (!file_)
      throw_file_failure(file_path_, "close", errno);
#endif
}

void secure_output_file::close_noexcept() noexcept {
#ifndef _WIN32
   if (file_descriptor_ != invalid_file_descriptor) {
      ::close(file_descriptor_);
      file_descriptor_ = invalid_file_descriptor;
   }
   unlink_noexcept(temp_file_path_);
   temp_file_path_.clear();
#else
   if (file_.is_open())
      file_.close();
#endif
}

void write_secure_file(const std::filesystem::path& file_path, std::string_view content) {
   secure_output_file out(file_path);
   out.write(content);
   out.close();
}

} // namespace fc

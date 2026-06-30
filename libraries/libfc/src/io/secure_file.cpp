#include <fc/io/secure_file.hpp>

#include <algorithm>
#include <cerrno>
#include <ios>
#include <limits>
#include <string>
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
constexpr mode_t secret_file_mode = S_IRUSR | S_IWUSR;
constexpr int    invalid_descriptor = -1;

/**
 * Returns the platform open flags needed for secret output files.
 */
int secret_file_open_flags() {
   int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   return flags;
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

} // namespace

secure_output_file::secure_output_file(std::filesystem::path file_path)
   : file_path_(std::move(file_path)) {
#ifndef _WIN32
   int fd = ::open(file_path_.c_str(), secret_file_open_flags(), secret_file_mode);
   if (fd == invalid_descriptor)
      throw_file_failure(file_path_, "open", errno);

   auto close_on_failure = [fd]() noexcept { ::close(fd); };

   struct stat stat_result {};
   if (::fstat(fd, &stat_result) == -1) {
      const int saved_errno = errno;
      close_on_failure();
      throw_file_failure(file_path_, "stat", saved_errno);
   }

   if (!S_ISREG(stat_result.st_mode)) {
      close_on_failure();
      throw_file_failure(file_path_, "open regular file", EINVAL);
   }

   if (::fchmod(fd, secret_file_mode) == -1) {
      const int saved_errno = errno;
      close_on_failure();
      throw_file_failure(file_path_, "restrict permissions", saved_errno);
   }

   if (::ftruncate(fd, 0) == -1) {
      const int saved_errno = errno;
      close_on_failure();
      throw_file_failure(file_path_, "truncate", saved_errno);
   }

   file_descriptor_ = fd;
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
      const auto chunk = std::min(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
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
   if (::close(fd) == -1)
      throw_file_failure(file_path_, "close", errno);
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

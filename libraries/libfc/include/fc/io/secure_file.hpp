#pragma once

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <fstream>
#endif

namespace fc {

/**
 * Owner-only output file for newly generated secret material.
 *
 * On POSIX systems content is written to an owner-only temporary file in the
 * target directory and then committed with rename after a successful close. The
 * final target is rejected when it is already a symlink or non-regular file.
 */
class secure_output_file {
public:
   /**
    * Opens an owner-only temporary file for a secret output.
    *
    * @param file_path path to the secret file to create or replace
    * @throws std::ios_base::failure when the file cannot be opened securely
    */
   explicit secure_output_file(std::filesystem::path file_path);

   secure_output_file(const secure_output_file&) = delete;
   secure_output_file& operator=(const secure_output_file&) = delete;
   secure_output_file(secure_output_file&& other) noexcept;
   secure_output_file& operator=(secure_output_file&& other) noexcept;
   ~secure_output_file();

   /**
    * Writes bytes to the opened secret file.
    *
    * @param content content to write
    * @throws std::ios_base::failure when the write fails
    */
   void write(std::string_view content);

   /**
    * Closes the opened secret file and reports close errors.
    *
    * Destruction also closes the file, but close errors are intentionally not
    * thrown from the destructor.
    *
    * @throws std::ios_base::failure when close fails
    */
   void close();

   /**
    * Returns the path associated with this secure output file.
    */
   const std::filesystem::path& path() const noexcept { return file_path_; }

private:
   void close_noexcept() noexcept;

   std::filesystem::path file_path_;

#ifndef _WIN32
   static constexpr int invalid_file_descriptor = -1;
   int                  file_descriptor_        = invalid_file_descriptor;
   std::filesystem::path temp_file_path_;
#else
   std::ofstream file_;
#endif
};

/**
 * Writes a complete secret file with owner-only permissions.
 *
 * @param file_path path to the secret file to create or replace
 * @param content content to write
 * @throws std::ios_base::failure when the file cannot be opened, written, or closed securely
 */
void write_secure_file(const std::filesystem::path& file_path, std::string_view content);

} // namespace fc

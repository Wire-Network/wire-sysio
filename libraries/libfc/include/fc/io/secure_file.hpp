#pragma once

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <fstream>
#endif

namespace fc {

/**
 * Output file for newly generated secret material.
 *
 * On POSIX systems content is written to an owner-only temporary file in the target directory. Calling close()
 * publishes the file by syncing the temporary file, renaming it into place, and syncing the target directory.
 * Destroying the object before the rename commit point discards the temporary file without publishing it. If close()
 * reports a directory sync failure after the rename succeeds, the final file may already be visible but its directory
 * entry durability could not be confirmed. Existing final symlinks and non-regular final targets are rejected, but
 * intermediate directory symlinks are still resolved by the platform.
 *
 * On Windows this helper currently falls back to plain std::ofstream semantics; owner-only permissions, final-target
 * rejection, and temporary-file rename commit guarantees are POSIX-only.
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
   /**
    * Destroys the output file, discarding any uncommitted POSIX temporary file.
    */
   ~secure_output_file();

   /**
    * Writes bytes to the opened secret file.
    *
    * @param content content to write
    * @throws std::ios_base::failure when the write fails
    */
   void write(std::string_view content);

   /**
    * Closes the opened secret file, publishes it on POSIX, and reports close errors.
    *
    * Destruction before the POSIX rename commit point discards pending output. Once the rename succeeds, a later
    * directory sync failure can still be reported even though the final file may already be visible. Close errors are
    * intentionally not thrown from the destructor.
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

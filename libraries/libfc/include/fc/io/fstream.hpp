#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <fc/filesystem.hpp>

namespace fc {
  /**
   * Grab the full contents of a file into a string object.
   * NB reading a full file into memory is a poor choice
   * if the file may be very large.
   */
  void read_file_contents( const std::filesystem::path& filename, std::string& result );


  /**
   * Read a file into a vector of bytes
   *
   * @param filename file name to read
   * @return vector of bytes
   */
  std::vector<unsigned char> read_file_contents(const std::filesystem::path& filename);
}

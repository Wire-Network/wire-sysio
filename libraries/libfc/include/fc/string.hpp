#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fc
{
  std::int64_t  to_int64( const std::string& );
  std::uint64_t to_uint64( const std::string& );
  double   to_double( const std::string& );

  class variant_object;
  std::string format_string( const std::string&, const variant_object&, bool minimize = false );

  /**
   * Convert '\t', '\r', '\n', '\\' and '"'  to "\t\r\n\\\"" if escape_ctrl == on
   * Convert all other < 32 & 127 ascii to escaped unicode "\u00xx"
   * Removes invalid utf8 characters
   * Escapes Control sequence Introducer 0x9b to \u009b
   * All other characters unmolested.
   *
   * @param str input/output string to escape/truncate
   * @param escape_ctrl if on escapes control chars in str
   * @param max_len truncate string to max_len
   * @param add_truncate_str if truncated by max_len, add add_truncate_str to end of any truncated string,
   *                         new length with be max_len + add_truncate_str.size()
   * @return pair<reference to possibly modified passed in str, true if modified>
   */
  enum class escape_control_chars { off, on };
  std::pair<std::string&, bool> escape_str( std::string& str, escape_control_chars escape_ctrl = escape_control_chars::on,
                                            std::size_t max_len = std::numeric_limits<std::size_t>::max(),
                                            std::string_view add_truncate_str = "..." );

  /**
   * Split a string by a provided delimiter
   *
   * @param str String to split
   * @param delim delimiter
   * @param max_split a maximum number of chunks allowed, the remainder are grouped as the final element, 0 denotes no limit
   * @return vector of split strings
   */
  std::vector<std::string> split(const std::string& str, char delim, std::size_t max_split = 0);


  /**
   * Convert a string to lowercase using the specified locale
   *
   * @param s String to convert to lowercase
   * @param loc Locale to use for case conversion, defaults to system locale
   * @return A new string with all characters converted to lowercase
   */
  std::string to_lower(std::string s);
}

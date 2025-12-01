#include <boost/lexical_cast.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/string.hpp>
#include <fc/utf8.hpp>
#include <format>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>

/**
 *  Implemented with std::string for now.
 */

namespace fc {
class comma_numpunct : public std::numpunct<char> {
protected:
   virtual char        do_thousands_sep() const { return ','; }
   virtual std::string do_grouping() const { return "\03"; }
};

int64_t to_int64(const std::string& i) {
   try {
      return boost::lexical_cast<int64_t>(i.c_str(), i.size());
   } catch (const boost::bad_lexical_cast& e) {
      FC_THROW_EXCEPTION(parse_error_exception, "Couldn't parse int64_t");
   }
   FC_RETHROW_EXCEPTIONS(warn, "${i} => int64_t", ("i", i))
}

uint64_t to_uint64(const std::string& i) {
   try {
      try {
         return boost::lexical_cast<uint64_t>(i.c_str(), i.size());
      } catch (const boost::bad_lexical_cast& e) {
         FC_THROW_EXCEPTION(parse_error_exception, "Couldn't parse uint64_t");
      }
      FC_RETHROW_EXCEPTIONS(warn, "${i} => uint64_t", ("i", i))
   }
   FC_CAPTURE_AND_RETHROW((i))
}

double to_double(const std::string& i) {
   try {
      return boost::lexical_cast<double>(i.c_str(), i.size());
   } catch (const boost::bad_lexical_cast& e) {
      FC_THROW_EXCEPTION(parse_error_exception, "Couldn't parse double");
   }
   FC_RETHROW_EXCEPTIONS(warn, "${i} => double", ("i", i))
}

std::pair<std::string&, bool> escape_str(std::string& str, escape_control_chars escape_ctrl, std::size_t max_len,
                                         std::string_view add_truncate_str) {
   bool modified = false, truncated = false;
   // truncate early to speed up escape
   if (str.size() > max_len) {
      str.resize(max_len);
      modified = truncated = true;
   }
   auto itr = escape_ctrl == escape_control_chars::on
                 ? std::find_if(str.begin(), str.end(),
                                [](const auto& c) {
                                   return c == '\x7f' || c == '\\' || c == '\"' || (c >= '\x00' && c <= '\x1f');
                                })
                 : std::find_if(str.begin(), str.end(), [](const auto& c) {
                      // x09 = \t, x0a = \n,                   x0d = \r
                      return c == '\x7f' || (c >= '\x00' && c <= '\x08') || c == '\x0b' || c == '\x0c' ||
                             (c >= '\x0e' && c <= '\x1f');
                   });

   if (itr != str.end() || !fc::is_valid_utf8(str)) {
      str      = escape_string(str, nullptr, escape_ctrl == escape_control_chars::on);
      modified = true;
      if (str.size() > max_len) {
         str.resize(max_len);
         truncated = true;
      }
   }

   if (truncated) {
      str += add_truncate_str;
   }

   return std::make_pair(std::ref(str), modified);
}

std::vector<std::string> split(const std::string& str, char delim, std::size_t max_split) {
   std::vector<std::string> out;
   std::string delim_str{&delim, 1};
   for (auto&& token : std::views::split(str, delim)) {
      std::string_view token_str{token.begin(), token.end()};
      if (max_split > 0 && out.size() >= max_split) {
         out.back()
            .append(delim_str)
            .append(token_str.data(), token_str.size());
      } else {
         out.emplace_back(token_str);
      }
   }
   return out;
}

} // namespace fc
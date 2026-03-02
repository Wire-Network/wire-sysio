#include <sysio/services/cron_parser.hpp>
#include <fc/exception/exception.hpp>
#include <algorithm>
#include <charconv>
#include <sstream>
#include <vector>

namespace sysio::services {

namespace {

using schedule_value = cron_service::job_schedule::schedule_value;
using exact_value = cron_service::job_schedule::exact_value;
using step_value = cron_service::job_schedule::step_value;
using range_value = cron_service::job_schedule::range_value;

// Trim whitespace from both ends
std::string_view trim(std::string_view s) {
   auto start = s.find_first_not_of(" \t\r\n");
   if (start == std::string_view::npos) return "";
   auto end = s.find_last_not_of(" \t\r\n");
   return s.substr(start, end - start + 1);
}

// Split string by delimiter
std::vector<std::string_view> split(std::string_view s, char delim) {
   std::vector<std::string_view> result;
   size_t start = 0;
   size_t end = s.find(delim);

   while (end != std::string_view::npos) {
      result.push_back(s.substr(start, end - start));
      start = end + 1;
      end = s.find(delim, start);
   }
   result.push_back(s.substr(start));
   return result;
}

// Parse uint64_t from string_view
std::optional<uint64_t> parse_uint(std::string_view s) {
   uint64_t value;
   auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
   if (ec == std::errc() && ptr == s.data() + s.size()) {
      return value;
   }
   return std::nullopt;
}

// Validate value is within range
bool validate_range(uint64_t value, uint64_t min_val, uint64_t max_val) {
   return value >= min_val && value <= max_val;
}

// Parse a single cron field (handles *, exact, range, step, list)
std::optional<std::set<schedule_value>> parse_field(std::string_view field,
                                                      uint64_t min_val,
                                                      uint64_t max_val) {
   field = trim(field);

   if (field.empty()) {
      return std::nullopt;
   }

   std::set<schedule_value> result;

   // Wildcard - empty set means "all"
   if (field == "*") {
      return result;
   }

   // Handle comma-separated list (e.g., "1,3,5,7")
   auto parts = split(field, ',');

   for (auto part : parts) {
      part = trim(part);

      // Check for step syntax (*/N or start-end/step)
      auto slash_pos = part.find('/');
      if (slash_pos != std::string_view::npos) {
         auto base = part.substr(0, slash_pos);
         auto step_str = part.substr(slash_pos + 1);

         auto step_opt = parse_uint(step_str);
         if (!step_opt || *step_opt == 0) {
            return std::nullopt; // Invalid step
         }

         if (base == "*") {
            // */N - step across entire range
            result.insert(step_value{*step_opt});
         } else {
            // start-end/step - step within range
            auto dash_pos = base.find('-');
            if (dash_pos == std::string_view::npos) {
               return std::nullopt; // Invalid syntax
            }

            auto start_str = base.substr(0, dash_pos);
            auto end_str = base.substr(dash_pos + 1);

            auto start_opt = parse_uint(start_str);
            auto end_opt = parse_uint(end_str);

            if (!start_opt || !end_opt) {
               return std::nullopt;
            }

            if (!validate_range(*start_opt, min_val, max_val) ||
                !validate_range(*end_opt, min_val, max_val) ||
                *start_opt > *end_opt) {
               return std::nullopt;
            }

            // Expand range with step into exact values
            for (uint64_t i = *start_opt; i <= *end_opt; i += *step_opt) {
               result.insert(exact_value{i});
            }
         }
      }
      // Check for range syntax (e.g., "1-5")
      else if (auto dash_pos = part.find('-'); dash_pos != std::string_view::npos) {
         auto start_str = part.substr(0, dash_pos);
         auto end_str = part.substr(dash_pos + 1);

         auto start_opt = parse_uint(start_str);
         auto end_opt = parse_uint(end_str);

         if (!start_opt || !end_opt) {
            return std::nullopt;
         }

         if (!validate_range(*start_opt, min_val, max_val) ||
             !validate_range(*end_opt, min_val, max_val) ||
             *start_opt > *end_opt) {
            return std::nullopt;
         }

         result.insert(range_value{*start_opt, *end_opt});
      }
      // Exact value (e.g., "5")
      else {
         auto value_opt = parse_uint(part);
         if (!value_opt) {
            return std::nullopt;
         }

         if (!validate_range(*value_opt, min_val, max_val)) {
            return std::nullopt;
         }

         result.insert(exact_value{*value_opt});
      }
   }

   return result;
}

} // anonymous namespace

std::optional<cron_service::job_schedule> parse_cron_schedule(std::string_view cron_expr) {
   cron_expr = trim(cron_expr);

   if (cron_expr.empty()) {
      return std::nullopt;
   }

   // Split by whitespace
   auto fields = split(cron_expr, ' ');

   // Remove empty fields (multiple spaces)
   fields.erase(
      std::remove_if(fields.begin(), fields.end(),
                     [](std::string_view s) { return trim(s).empty(); }),
      fields.end()
   );

   // Must be either 5 fields (standard cron) or 6 fields (with milliseconds)
   if (fields.size() != 5 && fields.size() != 6) {
      return std::nullopt;
   }

   cron_service::job_schedule sched;
   size_t field_idx = 0;

   // If 6 fields, first is milliseconds
   if (fields.size() == 6) {
      auto ms_field = parse_field(fields[field_idx++], 0, 59999);
      if (!ms_field) {
         return std::nullopt;
      }
      sched.milliseconds = std::move(*ms_field);
   }

   // Parse standard 5 cron fields
   // Field ranges: minute (0-59), hour (0-23), day_of_month (1-31), month (1-12), day_of_week (0-7)

   // Minutes
   auto minutes_field = parse_field(fields[field_idx++], 0, 59);
   if (!minutes_field) {
      return std::nullopt;
   }
   sched.minutes = std::move(*minutes_field);

   // Hours
   auto hours_field = parse_field(fields[field_idx++], 0, 23);
   if (!hours_field) {
      return std::nullopt;
   }
   sched.hours = std::move(*hours_field);

   // Day of month
   auto dom_field = parse_field(fields[field_idx++], 1, 31);
   if (!dom_field) {
      return std::nullopt;
   }
   sched.day_of_month = std::move(*dom_field);

   // Month
   auto month_field = parse_field(fields[field_idx++], 1, 12);
   if (!month_field) {
      return std::nullopt;
   }
   sched.month = std::move(*month_field);

   // Day of week (0 and 7 both mean Sunday)
   auto dow_field = parse_field(fields[field_idx++], 0, 7);
   if (!dow_field) {
      return std::nullopt;
   }
   sched.day_of_week = std::move(*dow_field);

   return sched;
}

cron_service::job_schedule parse_cron_schedule_or_throw(std::string_view cron_expr) {
   auto result = parse_cron_schedule(cron_expr);
   FC_ASSERT(result.has_value(), "Failed to parse cron schedule: '${expr}'", ("expr", cron_expr));
   return std::move(*result);
}

} // namespace sysio::services

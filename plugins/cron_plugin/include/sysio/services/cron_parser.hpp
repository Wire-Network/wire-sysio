#pragma once

#include <sysio/services/cron_service.hpp>
#include <string>
#include <string_view>
#include <optional>

namespace sysio::services {

/**
 * @brief Parses cron-style schedule strings into job_schedule objects
 *
 * Supports two formats:
 * 1. Standard 5-field cron format:  "minute hour day-of-month month day-of-week"
 * 2. Extended 6-field format:       "milliseconds minute hour day-of-month month day-of-week"
 *
 * Field syntax:
 * - Wildcard:        *           (matches all values)
 * - Exact value:     5           (matches exactly 5)
 * - Range:           1-5         (matches 1,2,3,4,5)
 * - Step:            * /5        (every 5 units)  [space added to avoid comment syntax]
 * - Range with step: 10-50/5     (10,15,20,25,30,35,40,45,50)
 * - List:            1,3,5,7     (matches 1,3,5,7)
 *
 * Examples:
 * - "* * * * *"              -> Every minute
 * - "0 * * * *"              -> Every hour at minute 0
 * - "0 9-17 * * 1-5"         -> Weekdays, 9 AM to 5 PM, on the hour
 * - "* /5 * * * *"           -> Every 5 minutes  [space added to avoid comment syntax]
 * - "0 0 1 * *"              -> First day of every month at midnight
 * - "0,15,30,45 * * * *"     -> Every 15 minutes (at 0,15,30,45)
 * - "5000 * * * * *"         -> Every minute at 5 seconds (extended format)
 *
 * @param cron_expr Cron expression string
 * @return job_schedule on success, std::nullopt on parse error
 */
std::optional<cron_service::job_schedule> parse_cron_schedule(std::string_view cron_expr);

/**
 * @brief Parses a cron schedule string, throwing on error
 *
 * Same as parse_cron_schedule but throws fc::exception on parse errors
 * instead of returning std::nullopt.
 *
 * @param cron_expr Cron expression string
 * @return job_schedule
 * @throws fc::exception if parse fails
 */
cron_service::job_schedule parse_cron_schedule_or_throw(std::string_view cron_expr);

} // namespace sysio::services

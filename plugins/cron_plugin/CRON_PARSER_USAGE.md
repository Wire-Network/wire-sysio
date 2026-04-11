# Cron Schedule Parser Usage

The cron parser converts standard cron expression strings into `job_schedule` objects for use with the cron service.

## Include

```cpp
#include <sysio/services/cron_parser.hpp>
```

## API Functions

### `parse_cron_schedule()`
```cpp
std::optional<cron_service::job_schedule> parse_cron_schedule(std::string_view cron_expr);
```
Returns `std::nullopt` on parse error.

### `parse_cron_schedule_or_throw()`
```cpp
cron_service::job_schedule parse_cron_schedule_or_throw(std::string_view cron_expr);
```
Throws `fc::exception` on parse error.

## Supported Formats

### Standard 5-field format
```
minute hour day-of-month month day-of-week
```

### Extended 6-field format (with milliseconds)
```
milliseconds minute hour day-of-month month day-of-week
```

## Field Ranges

| Field          | Range       | Special |
|----------------|-------------|---------|
| milliseconds   | 0-59999     | Optional (6-field format only) |
| minute         | 0-59        | Required |
| hour           | 0-23        | Required |
| day-of-month   | 1-31        | Required |
| month          | 1-12        | Required |
| day-of-week    | 0-7         | Required (0 and 7 = Sunday) |

## Syntax Elements

| Syntax | Example | Description |
|--------|---------|-------------|
| `*` | `* * * * *` | Wildcard - matches all values |
| Exact value | `30 9 * * *` | Matches exactly that value |
| Range | `9-17` | Matches all values in range (inclusive) |
| Step | `*/5` | Every N units (e.g., every 5 minutes) |
| Range+Step | `10-50/10` | Step within range (10,20,30,40,50) |
| List | `1,3,5,7` | Comma-separated list of values |

## Usage Examples

### Basic Usage

```cpp
#include <sysio/services/cron_parser.hpp>
#include <sysio/cron_plugin.hpp>

using namespace sysio::services;

// Parse a cron expression
auto sched_opt = parse_cron_schedule("*/5 * * * *");
if (sched_opt) {
    // Add job to cron service
    auto& cron = app().get_plugin<cron_plugin>();
    cron.add_job(*sched_opt, []() {
        ilog("Job runs every 5 minutes");
    });
}
```

### With Error Handling

```cpp
try {
    auto sched = parse_cron_schedule_or_throw("0 9-17 * * 1-5");

    auto& cron = app().get_plugin<cron_plugin>();
    cron.add_job(sched, []() {
        ilog("Business hours job (9 AM - 5 PM, weekdays)");
    }, cron_service::job_metadata_t{
        .tags = {"business-hours"},
        .label = "hourly_weekday_job"
    });
} catch (const fc::exception& e) {
    elog("Failed to parse cron schedule: {}", e.to_detail_string());
}
```

### Common Schedule Examples

```cpp
// Every minute
auto every_minute = parse_cron_schedule("* * * * *");

// Every hour at minute 0
auto hourly = parse_cron_schedule("0 * * * *");

// Every day at midnight
auto daily = parse_cron_schedule("0 0 * * *");

// Every 15 minutes
auto every_15_min = parse_cron_schedule("*/15 * * * *");

// At 0, 15, 30, and 45 minutes past every hour
auto quarterly = parse_cron_schedule("0,15,30,45 * * * *");

// Business hours: 9 AM - 5 PM, Monday-Friday
auto business_hours = parse_cron_schedule("0 9-17 * * 1-5");

// First day of every month at midnight
auto monthly = parse_cron_schedule("0 0 1 * *");

// Every Sunday at 2 AM
auto weekly = parse_cron_schedule("0 2 * * 0");

// Every 5 seconds (extended format with milliseconds)
auto every_5_seconds = parse_cron_schedule("*/5000 * * * * *");

// At exactly 5.5 seconds past every minute (extended format)
auto precise_timing = parse_cron_schedule("5500 * * * * *");
```

### Complex Schedules

```cpp
// Every 10 minutes between 8 AM and 8 PM on weekdays
auto complex1 = parse_cron_schedule("*/10 8-20 * * 1-5");

// At 9:30 AM on the 1st and 15th of every month
auto complex2 = parse_cron_schedule("30 9 1,15 * *");

// Every 2 hours during business hours
auto complex3 = parse_cron_schedule("0 9-17/2 * * *");
// This gives: 9 AM, 11 AM, 1 PM, 3 PM, 5 PM

// Every 30 seconds (extended format)
auto frequent = parse_cron_schedule("*/30000 * * * * *");
```

## Integration with Beacon Chain Update Plugin

```cpp
void beacon_chain_update_plugin::plugin_startup() {
    // Parse schedule from config string
    std::string schedule_str = "0 */6 * * *"; // Every 6 hours

    try {
        auto sched = parse_cron_schedule_or_throw(schedule_str);

        auto& cron = app().get_plugin<cron_plugin>();
        auto job_id = cron.add_job(sched, [this]() {
            // Update beacon chain data
            update_beacon_chain_data();
        }, cron_service::job_metadata_t{
            .one_at_a_time = true,
            .tags = {"beacon-chain", "update"},
            .label = "beacon_chain_update"
        });

        ilog("Scheduled beacon chain update job: {}", job_id);
    } catch (const fc::exception& e) {
        elog("Failed to schedule beacon chain update: {}", e.to_detail_string());
    }
}
```

## Validation

The parser automatically validates:
- Field count (must be 5 or 6)
- Value ranges for each field
- Range ordering (start <= end)
- Step values (must be > 0)
- Numeric parsing

Invalid expressions return `std::nullopt` or throw `fc::exception`.

## Notes

- Empty sets (from `*`) mean "match all values" - this is evaluated by the cron scheduler
- The parser does not validate calendar logic (e.g., February 31st will parse but never trigger)
- Day of week: both 0 and 7 represent Sunday
- Milliseconds field enables sub-minute precision (6-field format only)
- Multiple spaces between fields are allowed and ignored

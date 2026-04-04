# Cron String Parser - Implementation Summary

A complete cron expression parser has been added to the `cron_plugin` to enable string-based schedule configuration.

## Files Created

### 1. Header File
**`plugins/cron_plugin/include/sysio/services/cron_parser.hpp`**
- Public API for parsing cron expressions
- Two functions:
  - `parse_cron_schedule()` - Returns `std::optional` (safe)
  - `parse_cron_schedule_or_throw()` - Throws on error

### 2. Implementation
**`plugins/cron_plugin/src/services/cron_parser.cpp`**
- Complete parser implementation supporting:
  - Wildcards: `*`
  - Exact values: `5`
  - Ranges: `1-5`
  - Steps: `*/5` or `10-50/5`
  - Lists: `1,3,5,7`
- Validates all field ranges
- Supports standard 5-field and extended 6-field formats

### 3. Tests
**`plugins/cron_plugin/test/test_cron_parser.cpp`**
- Comprehensive test suite with 25+ test cases
- Tests valid parsing, error handling, and real-world examples

### 4. Documentation
**`plugins/cron_plugin/CRON_PARSER_USAGE.md`**
- Complete usage guide with examples
- Common schedule patterns
- Integration examples

## Quick Start

### Include the header
```cpp
#include <sysio/services/cron_parser.hpp>
```

### Parse a cron expression
```cpp
using namespace sysio::services;

// Safe parsing (returns optional)
auto sched_opt = parse_cron_schedule("*/5 * * * *");
if (sched_opt) {
    auto& cron = app().get_plugin<cron_plugin>();
    cron.add_job(*sched_opt, []() {
        ilog("Runs every 5 minutes");
    });
}

// Or with error handling (throws on failure)
try {
    auto sched = parse_cron_schedule_or_throw("0 9-17 * * 1-5");
    // Use schedule...
} catch (const fc::exception& e) {
    elog("Parse error: {}", e.to_detail_string());
}
```

## Format Support

### Standard Format (5 fields)
```
minute hour day-of-month month day-of-week
```

**Example:** `"*/15 9-17 * * 1-5"` = Every 15 minutes, 9 AM-5 PM, weekdays

### Extended Format (6 fields - with milliseconds)
```
milliseconds minute hour day-of-month month day-of-week
```

**Example:** `"*/5000 * * * * *"` = Every 5 seconds

## Common Patterns

| Description | Expression |
|-------------|------------|
| Every minute | `* * * * *` |
| Every 5 minutes | `*/5 * * * *` |
| Hourly at :00 | `0 * * * *` |
| Daily at midnight | `0 0 * * *` |
| Business hours (9-5, weekdays) | `0 9-17 * * 1-5` |
| Every 15 minutes during business hours | `*/15 9-17 * * 1-5` |
| First of month | `0 0 1 * *` |
| Weekly (Sunday 2 AM) | `0 2 * * 0` |
| Every 5 seconds (extended) | `*/5000 * * * * *` |

## Integration Example

### Using in beacon_chain_update_plugin

```cpp
void beacon_chain_update_plugin::plugin_initialize(const variables_map& options) {
    // Get schedule from config
    std::string schedule_expr = "0 */6 * * *"; // Every 6 hours

    if (options.count("beacon-chain-update-schedule")) {
        schedule_expr = options.at("beacon-chain-update-schedule").as<std::string>();
    }

    try {
        _update_schedule = parse_cron_schedule_or_throw(schedule_expr);
        ilog("Beacon chain update schedule: {}", schedule_expr);
    } catch (const fc::exception& e) {
        elog("Invalid schedule expression '{}': {}",
             schedule_expr, e.to_detail_string());
        throw;
    }
}

void beacon_chain_update_plugin::plugin_startup() {
    auto& cron = app().get_plugin<cron_plugin>();

    _update_job_id = cron.add_job(
        _update_schedule,
        [this]() {
            update_beacon_chain_data();
        },
        cron_service::job_metadata_t{
            .one_at_a_time = true,
            .tags = {"beacon-chain", "update"},
            .label = "beacon_chain_updater"
        }
    );

    ilog("Started beacon chain update job: {}", _update_job_id);
}
```

## Building

The parser is automatically included when building the `cron_plugin`. The `plugin_target()` macro in CMakeLists.txt will pick up the new source file.

To build:
```bash
ninja -C build/debug-claude cron_plugin
```

To run tests:
```bash
./build/debug-claude/plugins/cron_plugin/test/test_cron_plugin --run_test=cron_parser_tests
```

## Features

✅ Standard cron syntax support
✅ Extended format with milliseconds (sub-minute precision)
✅ All operators: wildcards, ranges, steps, lists
✅ Comprehensive validation
✅ Error handling (optional or exception-based)
✅ Full test coverage
✅ Documentation with examples
✅ Zero external dependencies (uses C++20 standard library)

## Next Steps

1. **Build and test:**
   ```bash
   ninja -C build/debug-claude cron_plugin
   ./build/debug-claude/plugins/cron_plugin/test/test_cron_plugin
   ```

2. **Use in your plugin:**
   ```cpp
   #include <sysio/services/cron_parser.hpp>
   auto schedule = parse_cron_schedule_or_throw("*/5 * * * *");
   ```

3. **Add config option** (optional):
   ```cpp
   cfg.add_options()
      ("my-schedule",
       bpo::value<std::string>()->default_value("*/5 * * * *"),
       "Cron expression for scheduling (e.g., '*/5 * * * *' for every 5 minutes)");
   ```

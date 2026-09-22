#include <sysio/query_engine_plugin/query_config.hpp>

#include <charconv>
#include <limits>

namespace sysio::query_engine_plugin {

namespace {
/// Parse decimal option text without accepting a leading minus through unsigned wraparound.
template <typename T>
T option_value(const boost::program_options::variables_map& options, const char* name, T fallback) {
   if (!options.count(name))
      return fallback;
   const auto& text = options.at(name).as<std::string>();
   T value{};
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
      throw query_error(error_kind::INVALID_PARAMS, "Query option must be a representable unsigned integer",
                        std::nullopt, name);
   return value;
}
} // namespace

void add_options(boost::program_options::options_description& options) {
   options.add_options()(
      option::worker_threads,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::worker_threads)),
      "Query worker count; the optional HTTP adapter has an equal separate worker count");
   options.add_options()(
      option::max_in_flight,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_in_flight)),
      "Maximum admitted queries, including pending callbacks; also the independent HTTP ingress cap");
   options.add_options()(
      option::max_query_bytes,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_query_bytes)),
      "Maximum UTF-8 SQL bytes before parsing");
   options.add_options()(
      option::timeout_ms,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::timeout_ms)),
      "HTTP query deadline in milliseconds, including queue time; C++ execute defaults to no timeout");
   options.add_options()(
      option::max_capture_ms,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_capture_ms)),
      "Maximum milliseconds in one coherent chain capture");
   options.add_options()(
      option::max_abi_bytes,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_abi_bytes)),
      "Maximum ABI blob bytes per selected owner, checked before copying");
   options.add_options()(
      option::max_scan_rows,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_scan_rows)),
      "Maximum candidate rows across all selected owners");
   options.add_options()(
      option::max_raw_bytes,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_raw_bytes)),
      "Maximum copied ABI, raw row and capture overhead bytes per query");
   options.add_options()(
      option::max_memory_bytes,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_memory_bytes)),
      "Maximum cumulative accounted allocation bytes per query");
   options.add_options()(
      option::max_groups,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_groups)),
      "Maximum aggregate groups per query");
   options.add_options()(
      option::max_result_rows,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_result_rows)),
      "Maximum output rows after offset and SQL/per-call limit");
   options.add_options()(
      option::max_response_bytes,
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::max_response_bytes)),
      "Maximum encoded JSON success envelope bytes");
}

query_config parse_config(const boost::program_options::variables_map& options) {
   query_config config;
   config.worker_threads = option_value(options, option::worker_threads, defaults::worker_threads);
   config.max_in_flight = option_value(options, option::max_in_flight, defaults::max_in_flight);
   config.max_query_bytes = option_value(options, option::max_query_bytes, defaults::max_query_bytes);
   config.timeout_ms = option_value(options, option::timeout_ms, defaults::timeout_ms);
   config.max_capture_ms = option_value(options, option::max_capture_ms, defaults::max_capture_ms);
   config.max_abi_bytes = option_value(options, option::max_abi_bytes, defaults::max_abi_bytes);
   config.max_scan_rows = option_value(options, option::max_scan_rows, defaults::max_scan_rows);
   config.max_raw_bytes = option_value(options, option::max_raw_bytes, defaults::max_raw_bytes);
   config.max_memory_bytes = option_value(options, option::max_memory_bytes, defaults::max_memory_bytes);
   config.max_groups = option_value(options, option::max_groups, defaults::max_groups);
   config.max_result_rows = option_value(options, option::max_result_rows, defaults::max_result_rows);
   config.max_response_bytes = option_value(options, option::max_response_bytes, defaults::max_response_bytes);
   config.validate();
   return config;
}

query_error::query_error(error_kind kind, std::string message, std::optional<source_span> span,
                         std::optional<std::string> limit)
   : std::runtime_error(std::move(message))
   , kind(kind)
   , span(span)
   , limit(std::move(limit)) {}

void query_config::validate() const {
   const uint64_t positive[] = {worker_threads,   max_in_flight, max_query_bytes, timeout_ms,
                                max_capture_ms,   max_abi_bytes, max_scan_rows,   max_raw_bytes,
                                max_memory_bytes, max_groups,    max_result_rows, max_response_bytes};
   for (auto limit : positive)
      if (!limit)
         throw query_error(error_kind::INVALID_PARAMS, "Every query option must be positive");
   if (max_capture_ms > timeout_ms || max_raw_bytes > max_memory_bytes ||
       max_memory_bytes > std::numeric_limits<uint64_t>::max() / max_in_flight)
      throw query_error(error_kind::INVALID_PARAMS, "Incompatible query limits or admitted memory overflow");
}

query_budget::query_budget(query_config config, now_function now, const std::optional<query_options>& options)
   : config(config)
   , options(options.value_or(query_options{.timeout_ms = config.timeout_ms}))
   , now(std::move(now))
   , started(this->now())
   , deadline(clock::time_point::max()) {
   config.validate();
   if (this->options.timeout_ms < constants::no_timeout)
      throw query_error(error_kind::INVALID_PARAMS, "Timeout must be -1 or a nonnegative millisecond count",
                        std::nullopt, option::timeout_ms);
   if (this->options.timeout_ms != constants::no_timeout) {
      const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max()).count();
      if (this->options.timeout_ms > maximum)
         throw query_error(error_kind::INVALID_PARAMS, "Timeout exceeds the monotonic clock range", std::nullopt,
                           option::timeout_ms);
      const auto duration =
         std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(this->options.timeout_ms));
      if (started > deadline - duration)
         throw query_error(error_kind::INVALID_PARAMS, "Timeout exceeds the monotonic clock range", std::nullopt,
                           option::timeout_ms);
      deadline = started + duration;
   }
}

void query_budget::check() const {
   if (now() >= deadline)
      throw query_error(error_kind::QUERY_TIMEOUT, "Query deadline exceeded");
   if (cancelled.load(std::memory_order_relaxed))
      throw query_error(error_kind::QUERY_CANCELLED, "Query cancelled");
}

void query_budget::check_capture(clock::time_point capture_start) const {
   check();
   if (now() - capture_start >= std::chrono::milliseconds(config.max_capture_ms))
      throw query_error(error_kind::QUERY_TIMEOUT, "Query capture deadline exceeded", std::nullopt,
                        option::max_capture_ms);
}

void query_budget::assert_limit(uint64_t value, uint64_t maximum, const char* option) const {
   check();
   if (value > maximum)
      throw query_error(error_kind::QUERY_LIMIT, "Query resource limit exceeded", std::nullopt, option);
}

void query_budget::charge_memory(uint64_t bytes) {
   assert_limit(bytes, config.max_memory_bytes - accounted_bytes, option::max_memory_bytes);
   accounted_bytes += bytes;
}

void query_budget::charge_raw(uint64_t rows, uint64_t bytes) {
   assert_limit(rows, config.max_scan_rows - scanned_rows, option::max_scan_rows);
   assert_limit(bytes, config.max_raw_bytes - raw_bytes, option::max_raw_bytes);
   charge_memory(bytes);
   scanned_rows += rows;
   raw_bytes += bytes;
}

uint64_t query_budget::elapsed_us() const {
   return std::chrono::duration_cast<std::chrono::microseconds>(now() - started).count();
}

} // namespace sysio::query_engine_plugin

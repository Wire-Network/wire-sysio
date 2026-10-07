#include <sysio/query_engine_plugin/query_config.hpp>

#include <algorithm>
#include <charconv>
#include <limits>

namespace sysio::query_engine {

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

/// A millisecond option as a duration, with the built-in default as its fallback.
std::chrono::milliseconds millisecond_option(const boost::program_options::variables_map& options, const char* name,
                                             std::chrono::milliseconds fallback) {
   return std::chrono::milliseconds(option_value<uint32_t>(options, name, static_cast<uint32_t>(fallback.count())));
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
      boost::program_options::value<std::string>()->default_value(std::to_string(defaults::timeout.count())),
      "Query deadline in milliseconds, including queue time; C++ callers may opt out per call");
   // No default: producer_plugin's read-only transaction budget governs unless an operator sets a
   // tighter capture bound, and a configured value may never exceed that budget.
   options.add_options()(option::max_capture_ms, boost::program_options::value<std::string>(),
                         "Maximum milliseconds in one chain read callback (ABI copy or data capture); defaults "
                         "to producer_plugin's read-only transaction time and may not exceed it");
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
   config.timeout = millisecond_option(options, option::timeout_ms, defaults::timeout);
   // An explicit capture bound stands as given. The built-in default is clamped to the request
   // timeout, so a short timeout alone never fails validation; the plugin later replaces that default
   // with producer_plugin's read-only transaction budget.
   config.max_capture =
      options.count(option::max_capture_ms)
         ? std::chrono::microseconds(
              millisecond_option(options, option::max_capture_ms, std::chrono::milliseconds::zero()))
         : std::min(defaults::max_capture, std::chrono::duration_cast<std::chrono::microseconds>(config.timeout));
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
   const uint64_t positive[] = {worker_threads, max_in_flight,    max_query_bytes, max_abi_bytes,   max_scan_rows,
                                max_raw_bytes,  max_memory_bytes, max_groups,      max_result_rows, max_response_bytes};
   for (auto limit : positive)
      if (!limit)
         throw query_error(error_kind::INVALID_PARAMS, "Every query option must be positive");
   if (timeout <= std::chrono::milliseconds::zero() || max_capture <= std::chrono::microseconds::zero())
      throw query_error(error_kind::INVALID_PARAMS, "Every query option must be positive");
   if (max_capture > timeout || max_raw_bytes > max_memory_bytes ||
       max_memory_bytes > std::numeric_limits<uint64_t>::max() / max_in_flight)
      throw query_error(error_kind::INVALID_PARAMS, "Incompatible query limits or admitted memory overflow");
}

query_budget::query_budget(query_config config, now_function now, const std::optional<query_options>& options)
   : config(config)
   , options(options.value_or(query_options{}))
   , now(std::move(now))
   , started(this->now())
   , deadline(clock::time_point::max()) {
   config.validate();
   // An unset per-call timeout applies the configured deadline; only constants::no_deadline opts out.
   const auto timeout = this->options.timeout.value_or(config.timeout);
   if (timeout < std::chrono::milliseconds::zero())
      throw query_error(error_kind::INVALID_PARAMS, "Timeout must be a nonnegative millisecond count", std::nullopt,
                        option::timeout_ms);
   if (timeout == constants::no_deadline)
      return;
   const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max());
   if (timeout > maximum)
      throw query_error(error_kind::INVALID_PARAMS, "Timeout exceeds the monotonic clock range", std::nullopt,
                        option::timeout_ms);
   const auto duration = std::chrono::duration_cast<clock::duration>(timeout);
   if (started > deadline - duration)
      throw query_error(error_kind::INVALID_PARAMS, "Timeout exceeds the monotonic clock range", std::nullopt,
                        option::timeout_ms);
   deadline = started + duration;
}

void query_budget::check() const {
   if (now() >= deadline)
      throw deadline_error();
   if (cancelled.load(std::memory_order_relaxed))
      throw query_error(error_kind::QUERY_CANCELLED, "Query cancelled");
}

query_error query_budget::deadline_error() const {
   return query_error(error_kind::QUERY_TIMEOUT, "Query deadline exceeded", std::nullopt,
                      cut_by_read_window.load(std::memory_order_relaxed)
                         ? std::optional<std::string>(bound::read_window)
                         : std::nullopt);
}

void query_budget::check_capture(clock::time_point capture_start) {
   check();
   const auto current = now();
   if (current - capture_start >= config.max_capture)
      throw query_error(error_kind::QUERY_TIMEOUT, "Query capture deadline exceeded", std::nullopt,
                        option::max_capture_ms);
   if (capture_deadline && current >= *capture_deadline) {
      read_window_cuts.fetch_add(1, std::memory_order_relaxed);
      cut_by_read_window = true;
      throw query_error(error_kind::QUERY_TIMEOUT, "Chain read window ended before the capture completed", std::nullopt,
                        bound::read_window);
   }
}

void query_budget::assert_limit(uint64_t value, uint64_t maximum, const char* limit) const {
   check();
   if (value > maximum)
      throw query_error(error_kind::QUERY_LIMIT, "Query resource limit exceeded", std::nullopt, limit);
}

void query_budget::charge_memory(uint64_t bytes) {
   assert_limit(bytes, config.max_memory_bytes - accounted_bytes, option::max_memory_bytes);
   accounted_bytes += bytes;
   peak_accounted_bytes = std::max(peak_accounted_bytes, accounted_bytes);
}

void query_budget::release_memory(uint64_t bytes) noexcept {
   accounted_bytes -= std::min(bytes, accounted_bytes);
}

query_budget::charges query_budget::checkpoint() const noexcept {
   return {scanned_rows, raw_bytes, accounted_bytes};
}

void query_budget::restore(const charges& saved) noexcept {
   scanned_rows = saved.scanned_rows;
   raw_bytes = saved.raw_bytes;
   accounted_bytes = saved.accounted_bytes;
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

} // namespace sysio::query_engine

#include <sysio/trace_api/trx_id_index.hpp>
#include <sysio/trace_api/trx_id_index_test_utils.hpp>

#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t entry_count = 1u << 18;
constexpr std::size_t query_count = 1u << 16;
constexpr std::size_t lookup_count = 1u << 20;
constexpr std::size_t open_count = 1u << 10;
constexpr std::size_t sample_count = 7;
constexpr std::size_t collision_group_size = 32;
constexpr std::size_t collision_group_stride = collision_group_size * 2;
constexpr std::uint64_t random_seed = 0x7472616365696478;
constexpr std::uint64_t missing_prefix_bit = std::uint64_t{1} << 63;
constexpr std::string_view normal_file = "normal.log";
constexpr std::string_view collision_file = "collisions.log";
constexpr std::string_view hit_label = "hit";
constexpr std::string_view miss_label = "miss";
constexpr std::string_view collision_hit_label = "collision_hit";
constexpr std::string_view collision_miss_label = "collision_miss";
constexpr std::string_view open_label = "open";
constexpr std::string_view csv_header = "scenario,sample,operations,elapsed_ns,checksum\n";
constexpr std::string_view invalid_index_message = "benchmark index is invalid";
constexpr std::string_view incorrect_result_message = "benchmark lookup checksum is incorrect";

/// Keep deterministic query inputs and their independent expected block-number sum outside the timed region.
struct query_batch {
   std::vector<sysio::chain::transaction_id_type> ids;
   std::uint64_t expected_checksum = 0;
};

/// Measure the production lookup loop, checking all results through an observable checksum.
void measure_lookup(std::string_view label, const sysio::trace_api::trx_id_index_reader& reader,
                    const query_batch& queries) {
   for (std::size_t sample = 0; sample < sample_count; ++sample) {
      std::uint64_t checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t operation = 0; operation < lookup_count; ++operation)
         checksum += reader.lookup(queries.ids[operation & (query_count - 1)]).value_or(0);
      const auto elapsed =
         std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
      if (checksum != queries.expected_checksum * (lookup_count / query_count))
         throw std::runtime_error(std::string(incorrect_result_message));
      std::cout << label << ',' << sample << ',' << lookup_count << ',' << elapsed << ',' << checksum << '\n';
   }
}

/// Exercise either a typical sparse hash table or independent groups of colliding prefixes.
void measure_table(const std::filesystem::path& path, bool collisions) {
   std::mt19937_64 random(random_seed);
   std::vector<std::uint64_t> prefixes;
   prefixes.reserve(entry_count);
   sysio::trace_api::trx_id_index_writer writer;
   for (std::size_t index = 0; index < entry_count; ++index) {
      const auto high = collisions ? index % collision_group_size : index + 1;
      const auto low =
         collisions ? (index / collision_group_size) * collision_group_stride : static_cast<std::uint32_t>(random());
      const auto prefix = (std::uint64_t{high} << 32) | low;
      prefixes.push_back(prefix);
      writer.add(sysio::trace_api::test_common::make_trx_id(prefix), static_cast<std::uint32_t>(index + 1));
   }
   writer.write(path);
   query_batch hits;
   query_batch misses;
   hits.ids.reserve(query_count);
   misses.ids.reserve(query_count);
   for (std::size_t index = 0; index < query_count; ++index) {
      const auto entry = random() % entry_count;
      hits.ids.push_back(sysio::trace_api::test_common::make_trx_id(prefixes[entry]));
      hits.expected_checksum += entry + 1;
      misses.ids.push_back(sysio::trace_api::test_common::make_trx_id(prefixes[entry] ^ missing_prefix_bit));
   }
   sysio::trace_api::trx_id_index_reader reader(path);
   if (!reader.valid())
      throw std::runtime_error(std::string(invalid_index_message));
   measure_lookup(collisions ? collision_hit_label : hit_label, reader, hits);
   measure_lookup(collisions ? collision_miss_label : miss_label, reader, misses);

   if (!collisions) {
      for (std::size_t sample = 0; sample < sample_count; ++sample) {
         std::size_t checksum = 0;
         const auto start = std::chrono::steady_clock::now();
         for (std::size_t operation = 0; operation < open_count; ++operation) {
            sysio::trace_api::trx_id_index_reader opened(path);
            checksum += opened.valid();
         }
         const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
         if (checksum != open_count)
            throw std::runtime_error(std::string(invalid_index_message));
         std::cout << open_label << ',' << sample << ',' << open_count << ',' << elapsed << ',' << checksum << '\n';
      }
   }
}

} // namespace

/// Emit raw timing samples for comparison against the same benchmark linked to the previous reader.
int main() {
   fc::temp_directory directory;
   std::cout << csv_header;
   measure_table(directory.path() / normal_file, false);
   measure_table(directory.path() / collision_file, true);
}

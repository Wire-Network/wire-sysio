#include <fc/container/flat.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/variant.hpp>

#include <boost/test/unit_test.hpp>

#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

/// Ordered by `key` only; `tag` tells apart equivalent elements. Every copy and move is counted, so a test can bound
/// the element transfers made to build a container.
struct counted {
   inline static size_t transfers = 0;

   uint32_t key = 0;
   uint32_t tag = 0;

   counted() = default;
   counted(uint32_t key, uint32_t tag) : key(key), tag(tag) {}
   counted(const counted& other) : key(other.key), tag(other.tag) { ++transfers; }
   counted(counted&& other) noexcept : key(other.key), tag(other.tag) { ++transfers; }
   counted& operator=(const counted& other) {
      key = other.key;
      tag = other.tag;
      ++transfers;
      return *this;
   }
   counted& operator=(counted&& other) noexcept {
      key = other.key;
      tag = other.tag;
      ++transfers;
      return *this;
   }

   friend bool operator<(const counted& a, const counted& b) { return a.key < b.key; }
};

/// Distinct keys in each input; large enough that one shift per out-of-order element exceeds the bound many times.
constexpr uint32_t key_count = 4096;

/// Transfers allowed for `n` input elements: a constant number per element per level of an n log n sort.
constexpr size_t max_transfers(size_t n) { return n * std::bit_width(n) * 4; }

/// Every key twice, descending, the second copy of each key after all the first copies. Tags count up in input order.
std::vector<counted> descending_with_duplicates() {
   std::vector<counted> input;
   input.reserve(2 * key_count);
   uint32_t tag = 0;
   for (uint32_t pass = 0; pass < 2; ++pass)
      for (uint32_t key = key_count; key-- > 0;)
         input.emplace_back(key, tag++);
   return input;
}

/// The tag of the first and second occurrence of `key` in descending_with_duplicates().
uint32_t first_tag(uint32_t key) { return key_count - 1 - key; }
uint32_t second_tag(uint32_t key) { return key_count + first_tag(key); }

/// `input` as the [key, value] pairs a flat_map is read from and packed as.
std::vector<std::pair<uint32_t, counted>> as_pairs(const std::vector<counted>& input) {
   std::vector<std::pair<uint32_t, counted>> pairs;
   pairs.reserve(input.size());
   for (const counted& c : input)
      pairs.emplace_back(c.key, c);
   return pairs;
}

/// Holds the unique keys ascending, each with its first occurrence's tag.
template <typename Set>
void check_unique(const Set& set) {
   BOOST_REQUIRE_EQUAL(set.size(), key_count);
   uint32_t key = 0;
   for (const counted& c : set) {
      BOOST_REQUIRE_EQUAL(c.key, key);
      BOOST_REQUIRE_EQUAL(c.tag, first_tag(key));
      ++key;
   }
}

/// Holds every element ascending by key; equivalent elements keep their input order.
template <typename Set>
void check_multi(const Set& set) {
   BOOST_REQUIRE_EQUAL(set.size(), 2 * key_count);
   auto itr = set.begin();
   for (uint32_t key = 0; key < key_count; ++key) {
      BOOST_REQUIRE_EQUAL(itr->key, key);
      BOOST_REQUIRE_EQUAL(itr->tag, first_tag(key));
      ++itr;
      BOOST_REQUIRE_EQUAL(itr->key, key);
      BOOST_REQUIRE_EQUAL(itr->tag, second_tag(key));
      ++itr;
   }
}

/// Map counterparts of check_unique and check_multi.
template <typename Map>
void check_unique_map(const Map& map) {
   BOOST_REQUIRE_EQUAL(map.size(), key_count);
   uint32_t key = 0;
   for (const auto& [k, v] : map) {
      BOOST_REQUIRE_EQUAL(k, key);
      BOOST_REQUIRE_EQUAL(v.tag, first_tag(key));
      ++key;
   }
}

template <typename Map>
void check_multi_map(const Map& map) {
   BOOST_REQUIRE_EQUAL(map.size(), 2 * key_count);
   auto itr = map.begin();
   for (uint32_t key = 0; key < key_count; ++key) {
      BOOST_REQUIRE_EQUAL(itr->first, key);
      BOOST_REQUIRE_EQUAL(itr->second.tag, first_tag(key));
      ++itr;
      BOOST_REQUIRE_EQUAL(itr->first, key);
      BOOST_REQUIRE_EQUAL(itr->second.tag, second_tag(key));
      ++itr;
   }
}

/// Decodes `source` into a `Container` with `decode`, returning it with the element transfers the decode made.
template <typename Container, typename Decode>
std::pair<Container, size_t> decode_counted(Decode&& decode) {
   Container output;
   counted::transfers = 0;
   decode(output);
   const size_t transfers = counted::transfers;
   return {std::move(output), transfers};
}

} // namespace

FC_REFLECT(counted, (key)(tag))

BOOST_AUTO_TEST_SUITE(flat_container_tests)

/// Descending input with duplicates reads into a flat container equal to inserting in order, in n log n transfers.
BOOST_AUTO_TEST_CASE(from_variant_descending_input) {
   const std::vector<counted> input = descending_with_duplicates();
   const fc::variant          set_var(input);
   const fc::variant          map_var(as_pairs(input));
   const size_t               bound = max_transfers(input.size());

   {
      auto [set, transfers] = decode_counted<fc::flat_set<counted>>([&](auto& out) { fc::from_variant(set_var, out); });
      check_unique(set);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [set, transfers] =
         decode_counted<fc::flat_multiset<counted>>([&](auto& out) { fc::from_variant(set_var, out); });
      check_multi(set);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [map, transfers] =
         decode_counted<fc::flat_map<uint32_t, counted>>([&](auto& out) { fc::from_variant(map_var, out); });
      check_unique_map(map);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [map, transfers] =
         decode_counted<fc::flat_multimap<uint32_t, counted>>([&](auto& out) { fc::from_variant(map_var, out); });
      check_multi_map(map);
      BOOST_TEST(transfers <= bound);
   }
}

/// The binary form of descending input with duplicates unpacks the same way.
BOOST_AUTO_TEST_CASE(unpack_descending_input) {
   const std::vector<counted> input      = descending_with_duplicates();
   const std::vector<char>    set_packed = fc::raw::pack(input);
   const std::vector<char>    map_packed = fc::raw::pack(as_pairs(input));
   const size_t               bound      = max_transfers(input.size());

   const auto unpack_from = [](const std::vector<char>& packed) {
      return [&packed](auto& out) {
         fc::datastream<const char*> ds(packed.data(), packed.size());
         fc::raw::unpack(ds, out);
         BOOST_REQUIRE_EQUAL(ds.remaining(), 0u);
      };
   };

   {
      auto [set, transfers] = decode_counted<fc::flat_set<counted>>(unpack_from(set_packed));
      check_unique(set);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [set, transfers] = decode_counted<fc::flat_multiset<counted>>(unpack_from(set_packed));
      check_multi(set);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [map, transfers] = decode_counted<fc::flat_map<uint32_t, counted>>(unpack_from(map_packed));
      check_unique_map(map);
      BOOST_TEST(transfers <= bound);
   }
   {
      auto [map, transfers] = decode_counted<fc::flat_multimap<uint32_t, counted>>(unpack_from(map_packed));
      check_multi_map(map);
      BOOST_TEST(transfers <= bound);
   }
}

/// Decoding replaces earlier contents, and sorted or empty input decodes unchanged.
BOOST_AUTO_TEST_CASE(decode_replaces_contents) {
   const std::vector<uint32_t> sorted{1, 2, 3, 5, 8};
   const std::vector<uint32_t> unsorted{8, 3, 5, 3, 1, 2, 8};

   fc::flat_set<uint32_t> set{100, 200};
   fc::from_variant(fc::variant(sorted), set);
   BOOST_CHECK(std::vector<uint32_t>(set.begin(), set.end()) == sorted);
   fc::from_variant(fc::variant(unsorted), set);
   BOOST_CHECK(std::vector<uint32_t>(set.begin(), set.end()) == sorted);
   fc::from_variant(fc::variant(std::vector<uint32_t>{}), set);
   BOOST_TEST(set.empty());

   const std::vector<char> packed = fc::raw::pack(unsorted);
   fc::flat_set<uint32_t>  unpacked{100, 200};
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, unpacked);
   BOOST_CHECK(std::vector<uint32_t>(unpacked.begin(), unpacked.end()) == sorted);
}

/// A failed decode leaves the container empty.
BOOST_AUTO_TEST_CASE(failed_decode_leaves_empty) {
   // Two elements announced, one present.
   const std::vector<char> truncated = fc::raw::pack(std::make_tuple(fc::unsigned_int{2}, uint32_t{7}));
   fc::flat_set<uint32_t>  set{100, 200};
   fc::datastream<const char*> ds(truncated.data(), truncated.size());
   BOOST_CHECK_THROW(fc::raw::unpack(ds, set), fc::exception);
   BOOST_TEST(set.empty());

   fc::flat_map<uint32_t, uint32_t> map{{1, 1}};
   const fc::variant not_pairs(std::vector<std::string>{"a"});
   BOOST_CHECK_THROW(fc::from_variant(not_pairs, map), fc::exception);
   BOOST_TEST(map.empty());
}

BOOST_AUTO_TEST_SUITE_END()

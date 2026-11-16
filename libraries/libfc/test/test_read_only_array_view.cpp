#include <fc/read_only_array_view.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>

#if defined(__unix__) || defined(__APPLE__)
   #include <sys/mman.h>
   #include <unistd.h>
#endif

namespace {

/// Native record with nontrivial default initialization, matching a trace bucket's lifetime requirements.
struct record {
   std::uint64_t key = 0;
   std::uint32_t value = 0;
   std::uint32_t reserved = 0;
};

/// Trivially copyable implicit-lifetime record that cannot be default constructed.
struct no_default_record {
   no_default_record() = delete;
   std::uint64_t value;
};

/// A nontrivial owner must never be interpreted as a native record by the view.
struct nontrivial_record {
   ~nontrivial_record() {}
};

/// Verify constraints without instantiating a view for an unsupported element type.
template <typename T>
concept has_read_only_view = requires { typename fc::read_only_array_view<T>; };

static_assert(has_read_only_view<record>);
static_assert(has_read_only_view<no_default_record>);
static_assert(!has_read_only_view<nontrivial_record>);
static_assert(!has_read_only_view<const record>);
static_assert(!has_read_only_view<volatile record>);
static_assert(!has_read_only_view<void>);
static_assert(!has_read_only_view<record[]>);
static_assert(fc::read_only_array_view<record>{}.empty());

#if defined(__unix__) || defined(__APPLE__)
/// Release a guarded mapping even when a test assertion aborts the case.
struct guarded_mapping {
   static constexpr std::size_t page_count = 3;
   std::size_t page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
   std::size_t size = page_count * page_size;
   void* base = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

   ~guarded_mapping() {
      if (base != MAP_FAILED)
         ::munmap(base, size);
   }
};
#endif

} // namespace

BOOST_AUTO_TEST_SUITE(read_only_array_view_tests)

/// Empty construction and reset must not access a null storage pointer.
BOOST_AUTO_TEST_CASE(empty_views) {
   fc::read_only_array_view<record> view;
   BOOST_CHECK(view.empty());
   BOOST_CHECK_EQUAL(view.size(), 0u);
   view = {nullptr, 0};
   BOOST_CHECK(view.empty());
}

/// Every field and every array position must survive byte-backed reads without changing the source bytes.
BOOST_AUTO_TEST_CASE(native_record_representations) {
   constexpr std::array expected{
      record{},
      record{std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint32_t>::max(), 0},
      record{1, 2, 3},
   };
   alignas(record) std::array<std::byte, sizeof(expected)> storage;
   std::memcpy(storage.data(), expected.data(), storage.size());
   const auto original = storage;
   const fc::read_only_array_view<record> view(storage.data(), expected.size());
   BOOST_CHECK_EQUAL(view.size(), expected.size());
   BOOST_CHECK(!view.empty());
   for (std::size_t index = 0; index < view.size(); ++index) {
      auto actual = view[index];
      BOOST_CHECK_EQUAL(actual.key, expected[index].key);
      BOOST_CHECK_EQUAL(actual.value, expected[index].value);
      BOOST_CHECK_EQUAL(actual.reserved, expected[index].reserved);
      actual.key = 0;
   }
   BOOST_CHECK(storage == original);
}

/// Reads must not invoke default constructors, including for a type whose default constructor is deleted.
BOOST_AUTO_TEST_CASE(deleted_default_constructor) {
   constexpr std::uint64_t expected = std::numeric_limits<std::uint64_t>::max();
   alignas(no_default_record) std::array<std::byte, sizeof(no_default_record)> storage;
   std::memcpy(storage.data(), &expected, sizeof(expected));
   const fc::read_only_array_view<no_default_record> view(storage.data(), 1);
   BOOST_CHECK_EQUAL(view[0].value, expected);
}

/// Scalar elements and view copies must preserve values and remain independent of a reset view.
BOOST_AUTO_TEST_CASE(scalar_elements_and_copy) {
   constexpr std::array<std::uint32_t, 3> expected{0, 1, std::numeric_limits<std::uint32_t>::max()};
   alignas(std::uint32_t) std::array<std::byte, sizeof(expected)> storage;
   std::memcpy(storage.data(), expected.data(), storage.size());
   fc::read_only_array_view<std::uint32_t> view(storage.data(), expected.size());
   const auto copy = view;
   view = {};
   BOOST_CHECK(view.empty());
   for (std::size_t index = 0; index < copy.size(); ++index)
      BOOST_CHECK_EQUAL(copy[index], expected[index]);
}

#if defined(__unix__) || defined(__APPLE__)
/// Construction must touch no pages; reads must touch only their record and never write into read-only storage.
BOOST_AUTO_TEST_CASE(read_only_mapping_with_guard_pages) {
   guarded_mapping mapping;
   BOOST_REQUIRE(mapping.base != MAP_FAILED);
   BOOST_REQUIRE_EQUAL(mapping.page_size % sizeof(record), 0u);

   // All pages are inaccessible during construction, catching eager copies and self-memmove fallbacks.
   const fc::read_only_array_view<record> view(mapping.base, mapping.size / sizeof(record));
   auto* readable = static_cast<std::byte*>(mapping.base) + mapping.page_size;
   BOOST_REQUIRE_EQUAL(::mprotect(readable, mapping.page_size, PROT_READ | PROT_WRITE), 0);
   constexpr record first{1, 2, 3};
   constexpr record last{std::numeric_limits<std::uint64_t>::max(), 4, 5};
   std::memcpy(readable, &first, sizeof(first));
   std::memcpy(readable + mapping.page_size - sizeof(last), &last, sizeof(last));
   BOOST_REQUIRE_EQUAL(::mprotect(readable, mapping.page_size, PROT_READ), 0);

   const auto first_index = mapping.page_size / sizeof(record);
   const auto last_index = first_index * 2 - 1;
   BOOST_CHECK_EQUAL(view[first_index].key, first.key);
   BOOST_CHECK_EQUAL(view[first_index].value, first.value);
   BOOST_CHECK_EQUAL(view[last_index].key, last.key);
   BOOST_CHECK_EQUAL(view[last_index].value, last.value);
   BOOST_CHECK_EQUAL(view[last_index].reserved, last.reserved);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

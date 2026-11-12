#include <fc/exception/exception.hpp>
#include <fc/bitset.hpp>
#include <fc/container/flat.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/datastream_crc.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/raw_variant.hpp>
#include <fc/network/message_buffer.hpp>
#include <fc/variant.hpp>

#include <boost/container/deque.hpp>
#include <boost/test/unit_test.hpp>

#include <sstream>

using namespace fc;

struct A {
   int                        x;
   const float                y;
   const std::optional<std::string> z;

   bool operator==(const A&) const = default;
};
FC_REFLECT(A, (x)(y)(z))

/// Count construction separately from packed bytes to catch eager count-driven element construction.
struct raw_counted_element {
   inline static size_t constructions = 0;
   uint8_t value = 0;
   raw_counted_element() { ++constructions; }
};
FC_REFLECT(raw_counted_element, (value))

/// Only value is serialized; decoding into existing sequence elements must retain the other member.
struct raw_reused_element {
   uint32_t value = 0;
   uint32_t retained = 0;
};
FC_REFLECT(raw_reused_element, (value))

/// A valid zero-byte element must not be rejected because its count exceeds the remaining byte count.
struct raw_empty_element {};
FC_REFLECT_EMPTY(raw_empty_element)

/// Deque decoding must not acquire a new copy/move requirement.
struct raw_immovable_element {
   uint32_t value = 0;
   raw_immovable_element() = default;
   raw_immovable_element(const raw_immovable_element&) = delete;
   raw_immovable_element(raw_immovable_element&&) = delete;
};
FC_REFLECT(raw_immovable_element, (value))

BOOST_AUTO_TEST_SUITE(raw_test_suite)

/// Truncated element counts must not construct an entire sequence before reading its first element.
BOOST_AUTO_TEST_CASE(sequence_allocation_follows_decoding) {
   const auto packed = raw::pack(unsigned_int{MAX_NUM_ARRAY_ELEMENTS});
   const auto check = [&]<typename Sequence>() {
      Sequence output;
      raw_counted_element::constructions = 0;
      datastream<const char*> stream(packed.data(), packed.size());
      BOOST_CHECK_THROW(raw::unpack(stream, output), fc::exception);
      BOOST_CHECK_LE(raw_counted_element::constructions, 1u);
      BOOST_CHECK_LE(output.size(), 1u);
   };
   check.template operator()<std::vector<raw_counted_element>>();
   check.template operator()<std::deque<raw_counted_element>>();
   check.template operator()<boost::container::vector<raw_counted_element>>();
   check.template operator()<boost::container::deque<raw_counted_element>>();
}

/// A forged byte count on both bounded and unknown-length streams fails before destination allocation.
BOOST_AUTO_TEST_CASE(byte_allocation_follows_reads) {
   const auto packed = raw::pack(unsigned_int{MAX_SIZE_OF_BYTE_ARRAYS});
   std::vector<char> output;
   datastream<const char*> input(packed.data(), packed.size());
   BOOST_CHECK_THROW(raw::unpack(input, output), fc::exception);
   BOOST_CHECK_EQUAL(output.capacity(), 0u);

   input.seekp(0);
   bounded_datastream logical_bound(input, MAX_SIZE_OF_BYTE_ARRAYS);
   BOOST_CHECK_THROW(raw::unpack(logical_bound, output), fc::exception);
   BOOST_CHECK_EQUAL(output.capacity(), 0u);

   input.seekp(0);
   datastream_mirror mirror(input);
   BOOST_CHECK_THROW(raw::unpack(mirror, output), fc::exception);
   BOOST_CHECK_EQUAL(output.capacity(), 0u);

   input.seekp(0);
   boost::container::vector<char> boost_output;
   BOOST_CHECK_THROW(raw::unpack(input, boost_output), fc::exception);
   BOOST_CHECK_EQUAL(boost_output.capacity(), 0u);

   temp_cfile temporary("wb+");
   auto& file = temporary.file();
   file.write(packed.data(), packed.size());
   file.seek(0);
   auto file_stream = file.create_datastream();
   BOOST_CHECK_THROW(raw::unpack(file_stream, output), std::ios_base::failure);
   BOOST_CHECK_EQUAL(output.capacity(), 0u);
}

/// Hashed/flat containers and bitsets must not allocate from a count whose first element is missing.
BOOST_AUTO_TEST_CASE(other_container_counts_do_not_preallocate) {
   const auto packed = raw::pack(unsigned_int{MAX_NUM_ARRAY_ELEMENTS});
   const auto check = [&]<typename Container>() {
      Container output;
      datastream<const char*> stream(packed.data(), packed.size());
      BOOST_CHECK_THROW(raw::unpack(stream, output), fc::exception);
      BOOST_CHECK(output.empty());
      if constexpr (requires { output.capacity(); })
         BOOST_CHECK_EQUAL(output.capacity(), 0u);
      if constexpr (requires { output.bucket_count(); })
         BOOST_CHECK_LE(output.bucket_count(), 2u);
   };
   check.template operator()<std::unordered_set<std::string>>();
   check.template operator()<std::unordered_map<std::string, uint32_t>>();
   check.template operator()<fc::flat_set<std::string>>();
   check.template operator()<fc::flat_multiset<std::string>>();
   check.template operator()<fc::flat_map<std::string, uint32_t>>();
   check.template operator()<fc::flat_multimap<std::string, uint32_t>>();

   const auto bit_count = raw::pack(unsigned_int{MAX_NUM_ARRAY_ELEMENTS * bitset::bits_per_block});
   datastream<const char*> stream(bit_count.data(), bit_count.size());
   bitset bits;
   BOOST_CHECK_THROW(raw::unpack(stream, bits), fc::exception);
   BOOST_CHECK_EQUAL(bits.num_blocks(), 0u);

   variant_object object;
   datastream<const char*> object_stream(packed.data(), packed.size());
   BOOST_CHECK_THROW(raw::unpack(object_stream, object), fc::exception);
   BOOST_CHECK_EQUAL(object.size(), 0u);
}

/// Preserve in-place reuse, Boost vector reset behavior, zero-byte elements and nonmovable deque elements.
BOOST_AUTO_TEST_CASE(incremental_sequences_preserve_decoded_values) {
   const auto packed = raw::pack(std::make_tuple(unsigned_int{1}, uint32_t{7}));
   const auto check = [&]<typename Sequence>(uint32_t expected_retained) {
      Sequence output(2);
      output.front().retained = 42;
      datastream<const char*> stream(packed.data(), packed.size());
      raw::unpack(stream, output);
      BOOST_REQUIRE_EQUAL(output.size(), 1u);
      BOOST_CHECK_EQUAL(output.front().value, 7u);
      BOOST_CHECK_EQUAL(output.front().retained, expected_retained);
   };
   check.template operator()<std::vector<raw_reused_element>>(42);
   check.template operator()<std::deque<raw_reused_element>>(42);
   check.template operator()<boost::container::deque<raw_reused_element>>(42);
   check.template operator()<boost::container::vector<raw_reused_element>>(0);

   const auto empty_elements = raw::pack(unsigned_int{31});
   BOOST_CHECK_EQUAL(raw::unpack<std::vector<raw_empty_element>>(empty_elements).size(), 31u);

   datastream<const char*> stream(packed.data(), packed.size());
   std::deque<raw_immovable_element> immovable;
   raw::unpack(stream, immovable);
   BOOST_REQUIRE_EQUAL(immovable.size(), 1u);
   BOOST_CHECK_EQUAL(immovable.front().value, 7u);

   // The existing null-variant decoder leaves a reused destination unchanged.
   const auto null_value = raw::pack(std::vector<variant>{variant{}});
   std::vector<variant> variants{variant{int64_t{42}}};
   datastream<const char*> null_stream(null_value.data(), null_value.size());
   raw::unpack(null_stream, variants);
   BOOST_REQUIRE_EQUAL(variants.size(), 1u);
   BOOST_CHECK_EQUAL(variants.front().as_int64(), 42);

   const std::vector<bitset> bitsets{bitset("101"), bitset("100001")};
   BOOST_CHECK(raw::unpack<std::vector<bitset>>(raw::pack(bitsets)) == bitsets);
}

/// Chunked byte reads preserve each supported stream contract, including boolean remaining() and short reads.
BOOST_AUTO_TEST_CASE(byte_chunks_preserve_stream_contracts) {
   const std::vector<char> expected(raw::detail::unpack_chunk_bytes * 3 + 17, 'x');
   const auto packed = raw::pack(expected);
   datastream<std::stringbuf> streambuf(std::string(packed.begin(), packed.end()));
   std::vector<char> output;
   raw::unpack(streambuf, output);
   BOOST_CHECK(output == expected);

   std::istringstream input(std::string(packed.begin(), packed.end()));
   raw::unpack(input, output);
   BOOST_CHECK(output == expected);

   message_buffer<65536> buffer;
   std::memcpy(buffer.write_ptr(), packed.data(), packed.size());
   buffer.advance_write_ptr(packed.size());
   auto peek = buffer.create_peek_datastream();
   raw::unpack(peek, output);
   BOOST_CHECK(output == expected);
   BOOST_CHECK_EQUAL(buffer.bytes_to_read(), packed.size());
   auto message = buffer.create_datastream();
   raw::unpack(message, output);
   BOOST_CHECK(output == expected);
   BOOST_CHECK_EQUAL(buffer.bytes_to_read(), 0u);

   datastream<const char*> memory(packed.data(), packed.size());
   datastream_crc crc(memory);
   raw::unpack(crc, output);
   BOOST_CHECK(output == expected);

   const auto short_bytes = raw::pack(unsigned_int{5000}, 'z');
   std::istringstream short_input(std::string(short_bytes.begin(), short_bytes.end()));
   output.assign(5000, 'a');
   raw::unpack(short_input, output);
   BOOST_REQUIRE_EQUAL(output.size(), 5000u);
   BOOST_CHECK_EQUAL(output.front(), 'z');
   BOOST_CHECK(std::all_of(output.begin() + 1, output.end(), [](char value) { return value == 'a'; }));

   // The pre-existing Boost fundamental-vector format encodes count bytes, not count*sizeof(uint32_t).
   const auto fundamental = raw::pack(std::make_tuple(unsigned_int{3}, std::array<char, 3>{1, 2, 3}));
   boost::container::vector<uint32_t> boost_output;
   datastream<const char*> fundamental_stream(fundamental.data(), fundamental.size());
   raw::unpack(fundamental_stream, boost_output);
   BOOST_REQUIRE_EQUAL(boost_output.size(), 3u);
   BOOST_CHECK_EQUAL(std::memcmp(boost_output.data(), fundamental.data() + 1, 3), 0);
   const auto* storage = reinterpret_cast<const char*>(boost_output.data());
   BOOST_CHECK(std::all_of(storage + 3, storage + boost_output.size() * sizeof(uint32_t),
                           [](char value) { return value == 0; }));
}


BOOST_AUTO_TEST_CASE(bitset_test)
{
   fc::bitset bs1 { fc::bitset("00011110") }; // bit set size 8

   char buff[32];
   datastream<char*> ds(buff, sizeof(buff));

   fc::raw::pack( ds, bs1 );

   fc::bitset bs2;
   bs2.resize(8);
   ds.seekp(0);
   fc::raw::unpack( ds, bs2 );

   // 0b00011110
   BOOST_CHECK(!bs2.test(0));
   BOOST_CHECK(bs2.test(1));
   BOOST_CHECK(bs2.test(2));
   BOOST_CHECK(bs2.test(2));
   BOOST_CHECK(bs2.test(3));
   BOOST_CHECK(bs2.test(4));
   BOOST_CHECK(!bs2.test(5));
   BOOST_CHECK(!bs2.test(6));
   BOOST_CHECK(!bs2.test(7));
}

BOOST_AUTO_TEST_CASE(bitset_large_test)
{
   fc::bitset bs1;
   bs1.resize(12345);

   bs1.set(42);
   bs1.set(23);
   bs1.set(12000);

   auto packed = fc::raw::pack(bs1);
   auto unpacked = fc::raw::unpack<fc::bitset>(packed);

   BOOST_TEST(unpacked[42]);
   BOOST_TEST(unpacked[23]);
   BOOST_TEST(unpacked[12000]);
   unpacked.flip(42);
   unpacked.flip(23);
   unpacked.flip(12000);
   BOOST_TEST(unpacked.none());
}

BOOST_AUTO_TEST_CASE(bitset_pack_unpack)
{
   std::vector<char> serialized; // to accumulate serialization bits from all calls to `check_pack_unpack`

   auto check_pack_unpack = [&](const fc::bitset& bs) {
      auto bytes = fc::raw::pack(bs);
      serialized.insert(serialized.end(), bytes.begin(), bytes.end());
      auto bs2 = fc::raw::unpack<fc::bitset>(bytes);
      BOOST_TEST(bs2 == bs);
   };

   check_pack_unpack(fc::bitset(""));
   check_pack_unpack(fc::bitset("0"));
   check_pack_unpack(fc::bitset("1"));
   check_pack_unpack(fc::bitset("01"));
   check_pack_unpack(fc::bitset("0101"));
   check_pack_unpack(fc::bitset("010100"));
   check_pack_unpack(fc::bitset("001010100"));
   check_pack_unpack(fc::bitset("0101010100"));
   check_pack_unpack(fc::bitset("0110011010100"));
   check_pack_unpack(fc::bitset("00110011010100"));
   check_pack_unpack(fc::bitset("0000000001010100"));
   check_pack_unpack(fc::bitset("01110011010100101"));
   check_pack_unpack(fc::bitset("0111001101010010111"));
   check_pack_unpack(fc::bitset("011100110101001011101"));
   check_pack_unpack(fc::bitset("0111001101010010111011001"));
   check_pack_unpack(fc::bitset("01110011010100101110110010"));
   check_pack_unpack(fc::bitset("01110011010100101110110010010"));
   check_pack_unpack(fc::bitset("0111001101010010111011001001011"));
   check_pack_unpack(fc::bitset("011100110101001011101100100100110"));
   check_pack_unpack(fc::bitset("01110011010100101110110010010011000"));
   check_pack_unpack(fc::bitset("01110011010100101001001001100000010000110"));
   check_pack_unpack(fc::bitset("0111001101010010111011001001001100000000000000110"));
   check_pack_unpack(fc::bitset("01110011010100101111001101100100100111111111111111111001"));

   // verify that expected serialization doesn't change. bits generated with spring 1.2.
   std::string ser_bits{"00010001010201040506140954000a54010dd40c0ed40c10540011a5e60013979a03155d6a0e19d9a5e6001ab24bcd"
                        "011d925d6a0e1f4b76a9392126d9a5e600239864979a032986c024a5e60031060026d9a5e60038f9ff9f64f35273"};
   std::vector<char> expected_serialization(ser_bits.size() / 2);
   fc::from_hex(ser_bits, expected_serialization.data(), expected_serialization.size());

   BOOST_TEST(serialized == expected_serialization);
}

BOOST_AUTO_TEST_CASE(bitset_small_test)
{
   fc::bitset bs1;
   bs1.resize(21);

   bs1.set(2);
   bs1.set(7);

   auto packed = fc::raw::pack(bs1);
   auto unpacked = fc::raw::unpack<fc::bitset>(packed);

   BOOST_TEST(unpacked[2]);
   BOOST_TEST(unpacked[7]);
   unpacked.flip(2);
   unpacked.flip(7);
   BOOST_TEST(unpacked.none());
}

BOOST_AUTO_TEST_CASE(struct_serialization) {
   char buff[512];
   datastream<char*> ds(buff, sizeof(buff));

   A a{2, 2.2, "abc"};
   fc::raw::pack(ds, a);

   A a2{0, 0};
   ds.seekp(0);
   fc::raw::unpack(ds, a2);
   bool same = a == a2;
   BOOST_TEST(same);

}

// Verify std::optional is unpacked correctly, especially an empty optional will always
// be unpacked to an empty optional even the target is not empty
BOOST_AUTO_TEST_CASE(unpacking_optional) {
   // source is empty
   char buff[8];
   datastream<char*> ds(buff, sizeof(buff));
   std::optional<uint32_t> s;  // no value
   fc::raw::pack(ds, s);

   {  // target has value. This test used to fail.
      std::optional<uint32_t> t = 10;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   {  // target reused for multiple unpackings. This test used to fail.
      char buff[8];
      datastream<char*> ds1(buff, sizeof(buff));
      std::optional<uint32_t> s1 = 15;
      fc::raw::pack(ds1, s1);

      std::optional<uint32_t> t;  // target is empty initially

      // Unpacking to t the first time so t has value
      ds1.seekp(0);
      fc::raw::unpack(ds1, t);
      BOOST_TEST((s1 == t));

      // Unpacking to t the second time. Afterwards, t does not have value.
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   { // target is empty.
      std::optional<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   // Source has value
   s = 5;
   ds.seekp(0);
   fc::raw::pack(ds, s);

   {  // target has value.
      std::optional<uint32_t> t = 10;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   { // target is empty.
      std::optional<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }
}

// Verify std::shared_ptr is unpacked correctly, especially a null shared_ptr will always
// be unpacked to a null shared_ptr even if the target was not null.
BOOST_AUTO_TEST_CASE(packing_shared_ptr) {
   // source is null
   char buff[8];
   datastream<char*> ds(buff, sizeof(buff));
   std::shared_ptr<uint32_t> s;  // null_ptr
   fc::raw::pack(ds, s);

   {  // target has value. This test used to fail.
      std::shared_ptr<uint32_t> t = std::make_shared<uint32_t>(10);
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST(!t);
   }

   {  // target is null.
      std::shared_ptr<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST(!t);
   }

   // source is not null
   ds.seekp(0);
   s = std::make_shared<uint32_t>(50);
   fc::raw::pack(ds, s);

   {  // target has value.
      std::shared_ptr<uint32_t> t = std::make_shared<uint32_t>(10);
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((*s == *t));
   }

   {  // target is null.
      std::shared_ptr<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((*s == *t));
   }
}

// Verify std::set is unpacked correctly, especially an empty set will always
// be unpacked to an empty set even if the target was not empty.
BOOST_AUTO_TEST_CASE(packing_set) {
   //==== source empty
   char buff[16];
   datastream<char*> ds(buff, sizeof(buff));
   std::set<uint32_t> s;  // empty
   fc::raw::pack(ds, s);

   {  // target is not empty. This test used to fail.
      std::set<uint32_t> t {10};
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST(t.empty());
   }

   {  // target is empty.
      std::set<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST(t.empty());
   }

   // Source has values
   ds.seekp(0);
   s = {1, 2};
   fc::raw::pack(ds, s);

   {  // target is not empty. This test used to fail (ending up with {1, 2, 3}).
      std::set<uint32_t> t {3};
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   {  // target is empty.
      std::set<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }
}

// Verify std::list is unpacked correctly, especially an empty list will always
// be unpacked to an empty list even if the target was not empty.
BOOST_AUTO_TEST_CASE(packing_list) {
   //==== source empty
   char buff[16];
   datastream<char*> ds(buff, sizeof(buff));
   std::list<uint32_t> s;  // empty
   fc::raw::pack(ds, s);

   {  // target has value. This test used to fail.
      std::list<uint32_t> t {10};
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((t.size() == 0));
   }

   {  // target is empty.
      std::list<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((t.size() == 0));
   }

   // Source has values
   ds.seekp(0);
   s = {1, 2};
   fc::raw::pack(ds, s);

   {  // target is not empty. This test used to fail (ending up with {1, 2, 3}).
      std::list<uint32_t> t {3};
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }

   {  // target is empty.
      std::list<uint32_t> t;
      ds.seekp(0);
      fc::raw::unpack(ds, t);
      BOOST_TEST((s == t));
   }
}

// SSO-encoded variants must wire-pack as the legacy string_type so peers running
// older code (no SSO) can deserialize them.  pack(variant) normalises the SSO tag
// (13) to string_type (9) on emit; this round-trip locks the invariant.
BOOST_AUTO_TEST_CASE(variant_sso_wire_roundtrip)
{
   const std::vector<std::string> samples = {
      "",                     // empty -> SSO branch
      "x",                    // 1 byte SSO
      "1234567890ABCD",       // 14 bytes -- exactly at sso_max_length boundary
      "this string is far too long to fit in the SSO inline buffer" // forces heap
   };
   for (const auto& s : samples) {
      fc::variant src(s);
      char buf[256];
      datastream<char*> ds(buf, sizeof(buf));
      fc::raw::pack(ds, src);

      datastream<const char*> rd(buf, sizeof(buf));
      fc::variant dst;
      fc::raw::unpack(rd, dst);

      BOOST_TEST(dst.is_string());
      BOOST_TEST(dst.get_string() == s);
   }
}

BOOST_AUTO_TEST_SUITE_END()

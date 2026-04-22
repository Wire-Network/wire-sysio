#include <boost/test/unit_test.hpp>
#include <fc/filesystem.hpp>

#include <sysio/trace_api/bloom_sidecar.hpp>
#include <sysio/trace_api/trace.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace sysio;
using namespace sysio::trace_api;
using sysio::chain::name;
using sysio::chain::operator""_n;

namespace {

/// Build a minimal action_trace_v0 with only the fields the bloom consumes.
action_trace_v0 act(name receiver, name account, name action) {
   action_trace_v0 a{};
   a.receiver = receiver;
   a.account  = account;
   a.action   = action;
   return a;
}

/// Pack three actions into a single-transaction block_trace so tests can exercise add_block without pulling in the
/// full extraction machinery.  Field defaults are fine - bloom_builder only reads receiver/account/action.
block_trace_v0 block_with(std::vector<action_trace_v0> actions) {
   transaction_trace_v0 t{};
   t.actions = std::move(actions);
   block_trace_v0 bt{};
   bt.transactions.push_back(std::move(t));
   return bt;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bloom_sidecar_tests)

/// A slice with a known set of receivers yields hits for every inserted receiver and (receiver, action) pair, and
/// misses for names not inserted.  The miss rate for well-separated unknowns should be at or below the target FPR;
/// with 32 inserted items at p=0.01 we expect roughly zero false positives on the 8 probe names we test.
BOOST_AUTO_TEST_CASE(roundtrip_hits_and_misses) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_roundtrip.log";

   const std::vector<name> present  = { "alice"_n, "bob"_n, "charlie"_n, "sysio.token"_n };
   const std::vector<name> absent   = { "dave"_n, "eve"_n, "unknown.acc"_n, "never.seen"_n };
   const name transfer = "transfer"_n;
   const name setabi   = "setabi"_n;

   bloom_builder b;
   for (auto r : present) {
      b.add_action(act(r, r, transfer));
   }
   BOOST_REQUIRE_EQUAL(b.receiver_count(),    present.size());
   BOOST_REQUIRE_EQUAL(b.recv_action_count(), present.size());
   b.finalize_and_write(path);

   bloom_reader r(path);
   BOOST_REQUIRE(r.valid());

   for (auto rcv : present) {
      BOOST_TEST_INFO("present receiver " << rcv.to_string());
      BOOST_CHECK(r.may_contain_receiver(rcv));
      BOOST_CHECK(r.may_contain_recv_action(rcv, transfer));
   }

   std::size_t false_positives = 0;
   for (auto rcv : absent) {
      if (r.may_contain_receiver(rcv)) ++false_positives;
   }
   BOOST_CHECK_LE(false_positives, 1u);

   // A (receiver, action) probe with a present receiver but unseen action should almost always miss, since the
   // composite key has more entropy than receiver alone.
   std::size_t composite_fp = 0;
   for (auto rcv : present) {
      if (r.may_contain_recv_action(rcv, setabi)) ++composite_fp;
   }
   BOOST_CHECK_LE(composite_fp, 1u);
}

/// Building from an empty slice still produces a valid file; probes always miss.  This is the "no transactions in
/// any block of the slice" case.
BOOST_AUTO_TEST_CASE(empty_builder_produces_valid_file_all_miss) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_empty.log";

   bloom_builder b;
   BOOST_REQUIRE(b.empty());
   b.finalize_and_write(path);

   bloom_reader r(path);
   BOOST_REQUIRE(r.valid());

   for (auto n : { "alice"_n, "bob"_n, "sysio.token"_n, "anything"_n }) {
      BOOST_CHECK(!r.may_contain_receiver(n));
      BOOST_CHECK(!r.may_contain_recv_action(n, "transfer"_n));
   }
}

/// add_block feeds every action_trace in every transaction into both filters.
BOOST_AUTO_TEST_CASE(add_block_walks_all_transactions) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_block.log";

   bloom_builder b;
   const auto bt = block_with({
      act("alice"_n,       "sysio.token"_n, "transfer"_n),
      act("sysio.token"_n, "sysio.token"_n, "transfer"_n),
      act("bob"_n,         "sysio.token"_n, "transfer"_n),
   });
   b.add_block(bt);
   b.finalize_and_write(path);

   bloom_reader r(path);
   BOOST_REQUIRE(r.valid());
   BOOST_CHECK(r.may_contain_receiver("alice"_n));
   BOOST_CHECK(r.may_contain_receiver("bob"_n));
   BOOST_CHECK(r.may_contain_receiver("sysio.token"_n));
   BOOST_CHECK(r.may_contain_recv_action("alice"_n, "transfer"_n));
   BOOST_CHECK(r.may_contain_recv_action("bob"_n,   "transfer"_n));
}

/// Missing file: reader is invalid and probes default to true so the caller falls back to scanning the slice
/// instead of silently dropping matches.
BOOST_AUTO_TEST_CASE(missing_file_is_invalid_fail_safe_true) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "does_not_exist.log";

   bloom_reader r(path);
   BOOST_CHECK(!r.valid());
   BOOST_CHECK(r.may_contain_receiver("alice"_n));
   BOOST_CHECK(r.may_contain_recv_action("alice"_n, "transfer"_n));
}

/// Bad magic is rejected.  Overwrites the first byte of a freshly built file.
BOOST_AUTO_TEST_CASE(bad_magic_is_invalid) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_badmagic.log";

   bloom_builder b;
   b.add_action(act("alice"_n, "sysio.token"_n, "transfer"_n));
   b.finalize_and_write(path);

   {
      std::fstream f(path.string(), std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(0);
      char bad = 'X';
      f.write(&bad, 1);
   }

   bloom_reader r(path);
   BOOST_CHECK(!r.valid());
}

/// A single flipped bit in the body invalidates the CRC, and the reader rejects the file rather than trusting
/// potentially corrupted filter state.
BOOST_AUTO_TEST_CASE(corrupted_body_rejected_by_crc) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_crc.log";

   bloom_builder b;
   b.add_action(act("alice"_n, "sysio.token"_n, "transfer"_n));
   b.finalize_and_write(path);

   // Flip a bit inside the first receiver-bloom byte (well after the header, well before the trailing CRC).
   const auto hdr_size = sizeof(bloom::header);
   {
      std::fstream f(path.string(), std::ios::binary | std::ios::in | std::ios::out);
      f.seekg(hdr_size);
      char b0 = 0;
      f.read(&b0, 1);
      b0 ^= 0x01;
      f.seekp(hdr_size);
      f.write(&b0, 1);
   }

   bloom_reader r(path);
   BOOST_CHECK(!r.valid());
}

/// Truncated file: drop the last few bytes.  Reader detects the size mismatch and rejects the file.
BOOST_AUTO_TEST_CASE(truncated_file_rejected) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_trunc.log";

   bloom_builder b;
   b.add_action(act("alice"_n, "sysio.token"_n, "transfer"_n));
   b.finalize_and_write(path);

   const auto full_size = std::filesystem::file_size(path);
   BOOST_REQUIRE(full_size > 8);
   std::filesystem::resize_file(path, full_size - 8);

   bloom_reader r(path);
   BOOST_CHECK(!r.valid());
}

/// File written with a different bloom version number is rejected.  Prevents silently mis-probing a future-format
/// file.
BOOST_AUTO_TEST_CASE(version_mismatch_rejected) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_ver.log";

   bloom_builder b;
   b.add_action(act("alice"_n, "sysio.token"_n, "transfer"_n));
   b.finalize_and_write(path);

   // Rewrite the version field to a future value.  The reader compares against bloom::file_version and the CRC
   // recomputation will not rescue the file either, but version gate fires first.
   {
      std::fstream f(path.string(), std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(offsetof(bloom::header, version));
      uint32_t bumped = bloom::file_version + 1;
      f.write(reinterpret_cast<const char*>(&bumped), sizeof(bumped));
   }

   bloom_reader r(path);
   BOOST_CHECK(!r.valid());
}

/// Regression pin against boost::bloom capacity round-trip.  boost::bloom's detail/core.hpp:480 documents the
/// invariant filter{f.capacity()}.capacity() == f.capacity(), which we rely on to reconstruct the filter from the
/// saved bit count.  If a future boost upgrade quietly breaks this, bloom_reader::load would start rejecting every
/// sidecar on the array-size guard and every query would silently scan instead of skip.  This test freezes the
/// invariant by inserting a known set, writing, reading back, and probing every inserted item for a hit.
BOOST_AUTO_TEST_CASE(filter_capacity_roundtrip_invariant) {
   fc::temp_directory tempdir;
   const auto path = tempdir.path() / "bloom_capacity.log";

   // Range of item counts spanning the min_capacity floor (32) up into busy-slice territory (1000), so a rounding
   // regression that only shows up at certain sizes has a chance to trigger.
   for (std::size_t n : { std::size_t{1}, std::size_t{10}, std::size_t{50}, std::size_t{500}, std::size_t{1000} }) {
      BOOST_TEST_INFO("n=" << n);

      bloom_builder b;
      for (std::size_t i = 0; i < n; ++i) {
         // Synthesize distinct names; name stores as uint64 so any distinct 64-bit values work.
         chain::name receiver(0x1000'0000'0000'0000ull | static_cast<uint64_t>(i));
         chain::name action  (0x2000'0000'0000'0000ull | static_cast<uint64_t>(i));
         action_trace_v0 a{};
         a.receiver = receiver;
         a.account  = receiver;
         a.action   = action;
         b.add_action(a);
      }
      b.finalize_and_write(path);

      bloom_reader r(path);
      BOOST_REQUIRE(r.valid());

      for (std::size_t i = 0; i < n; ++i) {
         chain::name receiver(0x1000'0000'0000'0000ull | static_cast<uint64_t>(i));
         chain::name action  (0x2000'0000'0000'0000ull | static_cast<uint64_t>(i));
         BOOST_REQUIRE_MESSAGE(r.may_contain_receiver(receiver),
                               "receiver " << receiver.to_string() << " should probe as present");
         BOOST_REQUIRE_MESSAGE(r.may_contain_recv_action(receiver, action),
                               "(receiver, action) should probe as present");
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()

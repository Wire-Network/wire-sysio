#include <sysio/chain/block_log.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/crypto/blake3.hpp>

#include <cstring>

using namespace sysio;
using namespace testing;
using namespace chain;

#include <snapshot_tester.hpp>
#include <snapshot_attest_fixture.hpp>

using snapshot_attest_test_support::snapshot_attest_fixture;
using snapshot_attest_test_support::single_provider_minimum;
using snapshot_attest_test_support::snapshot_provider_account;
using snapshot_attest_test_support::to_contract_snapshot_hash;

namespace {

/// Nonzero byte pattern used to construct a deliberately incorrect snapshot hash.
constexpr unsigned char invalid_snapshot_hash_fill = 0xab;

} // anonymous namespace

// ===========================================================================
BOOST_AUTO_TEST_SUITE(snapshot_attest_tests)

// ---------------------------------------------------------------------------
// Test: snapshot root hash is captured and matches what's stored on-chain
// after a successful attestation vote
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(snapshot_hash_matches_attestation, snapshot_attest_fixture) { try {
   // Set fixed K so one vote is sufficient.
   set_snap_config(single_provider_minimum);

   // Take a snapshot
   control->abort_block();
   auto block_num = control->head().block_num();
   auto block_id = control->head().id();

   fc::temp_directory tempdir;
   auto writer = std::make_shared<threaded_snapshot_writer>(
      tempdir.path() / "snap.bin");
   control->write_snapshot(writer);
   writer->finalize();
   auto root_hash = writer->get_root_hash();

   // Convert blake3 root_hash to sha256 for the contract (both are 32 bytes)
   const auto hash_as_sha256 = to_contract_snapshot_hash(root_hash);

   // Submit attestation vote — K=1, so this creates the attested record.
   produce_block(); // need a new block so we can push actions
   vote_snapshot(snapshot_provider_account, block_id, hash_as_sha256);
   produce_blocks();

   // Read the attested record from chain state
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(*this, block_num, on_chain_hash);

   BOOST_REQUIRE(found);
   BOOST_REQUIRE_EQUAL(hash_as_sha256.str(), on_chain_hash.str());

   ilog("Snapshot at block #{} with hash {} verified against on-chain attestation",
        block_num, root_hash.str());

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test: snapshot loaded from file has the same root hash
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(snapshot_roundtrip_preserves_hash, snapshot_attest_fixture) { try {
   produce_blocks(5);
   control->abort_block();

   // Write snapshot
   fc::temp_directory tempdir;
   auto snap_path = tempdir.path() / "roundtrip_snap.bin";
   auto writer = std::make_shared<threaded_snapshot_writer>(snap_path);
   control->write_snapshot(writer);
   writer->finalize();
   auto original_hash = writer->get_root_hash();

   // Read snapshot back and verify hash
   auto reader = std::make_shared<threaded_snapshot_reader>(snap_path);
   reader->validate();
   auto loaded_hash = reader->get_root_hash();

   BOOST_REQUIRE_EQUAL(original_hash.str(), loaded_hash.str());

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test: hash mismatch is detectable — wrong hash on-chain vs snapshot
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(snapshot_hash_mismatch_detected, snapshot_attest_fixture) { try {
   set_snap_config(single_provider_minimum);

   control->abort_block();
   auto block_num = control->head().block_num();
   auto block_id = control->head().id();

   // Write snapshot and get its real hash
   fc::temp_directory tempdir;
   auto snap_path = tempdir.path() / "mismatch_snap.bin";
   auto writer = std::make_shared<threaded_snapshot_writer>(snap_path);
   control->write_snapshot(writer);
   writer->finalize();
   auto real_hash = writer->get_root_hash();

   // Submit a deliberately incorrect hash on-chain.
   fc::sha256 wrong_hash;
   std::memset(wrong_hash.data(), invalid_snapshot_hash_fill, wrong_hash.data_size());

   produce_block();
   vote_snapshot(snapshot_provider_account, block_id, wrong_hash);
   produce_blocks();

   // Read on-chain record
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(*this, block_num, on_chain_hash);
   BOOST_REQUIRE(found);

   // Verify the on-chain hash does NOT match the real snapshot hash
   BOOST_REQUIRE_NE(to_contract_snapshot_hash(real_hash).str(), on_chain_hash.str());

   ilog("Mismatch correctly detected: on-chain {} vs snapshot {}",
        on_chain_hash.str(), real_hash.str());

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test: no attestation record exists for a given block — detectable
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(snapshot_no_attestation_detected, snapshot_attest_fixture) { try {
   produce_blocks(5);

   // Take a snapshot but do NOT submit any attestation vote
   control->abort_block();
   auto block_num = control->head().block_num();

   fc::temp_directory tempdir;
   auto writer = std::make_shared<threaded_snapshot_writer>(
      tempdir.path() / "no_attest_snap.bin");
   control->write_snapshot(writer);
   writer->finalize();

   produce_block();

   // Try to read — should not find any record
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(*this, block_num, on_chain_hash);
   BOOST_REQUIRE(!found);

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test: snapshot loaded in a new tester preserves attestation records
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(attestation_survives_snapshot_load, snapshot_attest_fixture) { try {
   set_snap_config(single_provider_minimum);

   // Get current state and create attestation
   control->abort_block();
   auto pre_snap_block_num = control->head().block_num();
   auto pre_snap_block_id = control->head().id();

   // Write snapshot to get hash at this point
   fc::temp_directory tempdir;
   auto pre_snap_path = tempdir.path() / "pre_attest.bin";
   auto pre_writer = std::make_shared<threaded_snapshot_writer>(pre_snap_path);
   control->write_snapshot(pre_writer);
   pre_writer->finalize();
   auto pre_hash = pre_writer->get_root_hash();

   const auto hash_as_sha256 = to_contract_snapshot_hash(pre_hash);

   // Submit attestation for that snapshot
   produce_block();
   vote_snapshot(snapshot_provider_account, pre_snap_block_id, hash_as_sha256);
   produce_blocks(2);

   // Verify attestation exists
   fc::sha256 on_chain_hash;
   BOOST_REQUIRE(read_snap_record(*this, pre_snap_block_num, on_chain_hash));
   BOOST_REQUIRE_EQUAL(hash_as_sha256.str(), on_chain_hash.str());

   // Now take a NEW snapshot (which includes the attestation table data)
   control->abort_block();
   auto snap_path = tempdir.path() / "with_attest.bin";
   auto writer = std::make_shared<threaded_snapshot_writer>(snap_path);
   control->write_snapshot(writer);
   writer->finalize();

   // Load from snapshot in a new tester
   auto reader = std::make_shared<threaded_snapshot_reader>(snap_path);
   snapshotted_tester snap_chain(get_config(), reader, 1);

   // The attestation record should survive the snapshot round-trip
   fc::sha256 loaded_on_chain_hash;
   bool found = read_snap_record(snap_chain, pre_snap_block_num, loaded_on_chain_hash);

   BOOST_REQUIRE(found);
   BOOST_REQUIRE_EQUAL(hash_as_sha256.str(), loaded_on_chain_hash.str());

   ilog("Attestation record for block #{} survived snapshot round-trip", pre_snap_block_num);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/testing/tester.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/crypto/blake3.hpp>

using namespace sysio;
using namespace testing;
using namespace chain;

#include <snapshot_tester.hpp>

using mvo = fc::mutable_variant_object;

// ---------------------------------------------------------------------------
// Test fixture: sets up a chain with sysio.system deployed, producers ranked,
// and snapshot providers registered with an attested snapshot record on-chain.
// ---------------------------------------------------------------------------
class snapshot_attest_fixture : public validating_tester {
public:
   abi_serializer sys_abi_ser;
   abi_serializer token_abi_ser;

   snapshot_attest_fixture() {
      produce_blocks(2);

      create_accounts({"sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
                        "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n, "sysio.rex"_n});
      produce_blocks(100);

      // Deploy token contract
      set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
      set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
      set_privileged("sysio.token"_n);
      {
         const auto* accnt = control->find_account_metadata("sysio.token"_n);
         abi_def abi;
         abi_serializer::to_abi(accnt->abi, abi);
         token_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
      }

      // Create and issue core token
      base_tester::push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mvo()
         ("issuer", name(config::system_account_name))
         ("maximum_supply", core_from_string("10000000000.0000")));
      base_tester::push_action("sysio.token"_n, "issue"_n, config::system_account_name, mvo()
         ("to", name(config::system_account_name))
         ("quantity", core_from_string("1000000000.0000"))
         ("memo", ""));

      // Deploy system contract
      set_code(config::system_account_name, test_contracts::sysio_system_wasm());
      set_abi(config::system_account_name, test_contracts::sysio_system_abi());
      base_tester::push_action(config::system_account_name, "init"_n,
                               config::system_account_name, mvo()("version", 0)("core", symbol(CORE_SYMBOL).to_string()));
      {
         const auto* accnt = control->find_account_metadata(config::system_account_name);
         abi_def abi;
         abi_serializer::to_abi(accnt->abi, abi);
         sys_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      produce_blocks();

      // Create producer + snap provider accounts with resources
      create_accounts({"producer1"_n, "snapprov1"_n, "snapprov2"_n});
      produce_blocks();

      // Register producer
      push_sys_action("producer1"_n, "regproducer"_n, mvo()
         ("producer", "producer1")
         ("producer_key", get_public_key("producer1"_n, "active"))
         ("url", "")
         ("location", 0));
      produce_blocks();

      // Set rank
      push_sys_action(config::system_account_name, "setrank"_n, mvo()
         ("producer", "producer1")
         ("rank", 1));
      produce_blocks();

      // Register snapshot providers
      push_sys_action("producer1"_n, "regsnapprov"_n, mvo()
         ("producer", "producer1")
         ("snap_account", "snapprov1"));
      produce_blocks();
   }

   action_result push_sys_action(const account_name& signer, const action_name& name, const variant_object& data) {
      string action_type_name = sys_abi_ser.get_action_type(name);
      action act;
      act.account = config::system_account_name;
      act.name = name;
      act.data = sys_abi_ser.variant_to_binary(action_type_name, data,
                                                abi_serializer::create_yield_function(abi_serializer_max_time));
      return base_tester::push_contract_paid_action(std::move(act), signer.to_uint64_t());
   }

   // Submit a snapshot hash vote on-chain
   void vote_snapshot(const account_name& snap_account, const block_id_type& block_id, const fc::sha256& snapshot_hash) {
      auto result = push_sys_action(snap_account, "votesnaphash"_n, mvo()
         ("snap_account", snap_account)
         ("block_id", block_id)
         ("snapshot_hash", snapshot_hash));
      BOOST_REQUIRE_EQUAL("", result);
   }

   // Set snapshot attestation config
   void set_snap_config(uint32_t min_providers, uint32_t threshold_pct) {
      auto result = push_sys_action(config::system_account_name, "setsnpcfg"_n, mvo()
         ("min_providers", min_providers)
         ("threshold_pct", threshold_pct));
      BOOST_REQUIRE_EQUAL("", result);
   }

   // Read a snaprecord from the chain database for a given block_num
   // Returns true if found, and fills in on_chain_hash
   bool read_snap_record(const chainbase::database& db, uint32_t block_num, fc::sha256& on_chain_hash) {
      const auto* t_id = db.find<table_id_object, by_code_scope_table>(
         boost::make_tuple(config::system_account_name, config::system_account_name, "snaprecords"_n));
      if (!t_id) return false;

      const auto& kv_index = db.get_index<key_value_index, by_scope_primary>();
      auto it = kv_index.find(boost::make_tuple(t_id->id, static_cast<uint64_t>(block_num)));
      if (it == kv_index.end()) return false;

      // snap_record layout: uint64_t block_num(8) + checksum256 block_id(32) + checksum256 snapshot_hash(32) + uint32_t attested_at_block(4)
      constexpr size_t hash_offset = sizeof(uint64_t) + 32;
      constexpr size_t hash_size = 32;
      if (it->value.size() < hash_offset + hash_size) return false;

      memcpy(on_chain_hash.data(), it->value.data() + hash_offset, hash_size);
      return true;
   }

   // Create a snapshot and return (path, root_hash, block_num)
   struct snapshot_info {
      std::filesystem::path path;
      fc::crypto::blake3    root_hash;
      uint32_t              block_num;
   };

   snapshot_info create_test_snapshot() {
      control->abort_block();

      fc::temp_directory tempdir;
      auto snap_path = tempdir.path() / "test_snapshot.bin";
      auto writer = std::make_shared<threaded_snapshot_writer>(snap_path);
      control->write_snapshot(writer);
      writer->finalize();

      return {snap_path, writer->get_root_hash(), control->head().block_num()};
   }
};

// ===========================================================================
BOOST_AUTO_TEST_SUITE(snapshot_attest_tests)

// ---------------------------------------------------------------------------
// Test: snapshot root hash is captured and matches what's stored on-chain
// after a successful attestation vote
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(snapshot_hash_matches_attestation, snapshot_attest_fixture) { try {
   // Set config: single vote sufficient (min_providers=1, threshold=100)
   set_snap_config(1, 100);
   produce_blocks();

   // Take a snapshot
   control->abort_block();
   auto block_num = control->head().block_num();
   auto block_id = control->head().id();

   auto writer = std::make_shared<threaded_snapshot_writer>(
      fc::temp_directory().path() / "snap.bin");
   control->write_snapshot(writer);
   writer->finalize();
   auto root_hash = writer->get_root_hash();

   // Convert blake3 root_hash to sha256 for the contract (both are 32 bytes)
   fc::sha256 hash_as_sha256;
   memcpy(hash_as_sha256.data(), root_hash.data(), 32);

   // Submit attestation vote — quorum=1, so this creates the attested record
   produce_block(); // need a new block so we can push actions
   vote_snapshot("snapprov1"_n, block_id, hash_as_sha256);
   produce_blocks();

   // Read the attested record from chain state
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(control->db(), block_num, on_chain_hash);

   BOOST_REQUIRE(found);
   BOOST_REQUIRE_EQUAL(hash_as_sha256.str(), on_chain_hash.str());

   // Verify it matches the original snapshot root hash
   BOOST_REQUIRE(memcmp(root_hash.data(), on_chain_hash.data(), 32) == 0);

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
   set_snap_config(1, 100);
   produce_blocks();

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

   // Submit a WRONG hash on-chain (all zeros won't match)
   fc::sha256 wrong_hash;
   memset(wrong_hash.data(), 0xAB, 32);

   produce_block();
   vote_snapshot("snapprov1"_n, block_id, wrong_hash);
   produce_blocks();

   // Read on-chain record
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(control->db(), block_num, on_chain_hash);
   BOOST_REQUIRE(found);

   // Verify the on-chain hash does NOT match the real snapshot hash
   BOOST_REQUIRE(memcmp(real_hash.data(), on_chain_hash.data(), 32) != 0);

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

   auto writer = std::make_shared<threaded_snapshot_writer>(
      fc::temp_directory().path() / "no_attest_snap.bin");
   control->write_snapshot(writer);
   writer->finalize();

   produce_block();

   // Try to read — should not find any record
   fc::sha256 on_chain_hash;
   bool found = read_snap_record(control->db(), block_num, on_chain_hash);
   BOOST_REQUIRE(!found);

} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// Test: snapshot loaded in a new tester preserves attestation records
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(attestation_survives_snapshot_load, snapshot_attest_fixture) { try {
   set_snap_config(1, 100);
   produce_blocks();

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

   fc::sha256 hash_as_sha256;
   memcpy(hash_as_sha256.data(), pre_hash.data(), 32);

   // Submit attestation for that snapshot
   produce_block();
   vote_snapshot("snapprov1"_n, pre_snap_block_id, hash_as_sha256);
   produce_blocks(2);

   // Verify attestation exists
   fc::sha256 on_chain_hash;
   BOOST_REQUIRE(read_snap_record(control->db(), pre_snap_block_num, on_chain_hash));
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
   bool found = false;
   {
      const auto& db = snap_chain.control->db();
      const auto* t_id = db.find<table_id_object, by_code_scope_table>(
         boost::make_tuple(config::system_account_name, config::system_account_name, "snaprecords"_n));
      if (t_id) {
         const auto& kv_index = db.get_index<key_value_index, by_scope_primary>();
         auto it = kv_index.find(boost::make_tuple(t_id->id, static_cast<uint64_t>(pre_snap_block_num)));
         if (it != kv_index.end() && it->value.size() >= sizeof(uint64_t) + 32 + 32) {
            memcpy(loaded_on_chain_hash.data(), it->value.data() + sizeof(uint64_t) + 32, 32);
            found = true;
         }
      }
   }

   BOOST_REQUIRE(found);
   BOOST_REQUIRE_EQUAL(hash_as_sha256.str(), loaded_on_chain_hash.str());

   ilog("Attestation record for block #{} survived snapshot round-trip", pre_snap_block_num);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

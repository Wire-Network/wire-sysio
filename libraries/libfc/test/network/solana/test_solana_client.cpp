// SPDX-License-Identifier: MIT
#include <boost/test/unit_test.hpp>

#include <fc/network/solana/solana_borsh.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_system_programs.hpp>
#include <fc/network/solana/solana_types.hpp>

namespace solana = fc::network::solana;
using namespace solana;

BOOST_AUTO_TEST_SUITE(solana_client_tests)

//=============================================================================
// Pubkey Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_pubkey_base58_roundtrip) {
   // Well-known System Program address
   std::string system_program = "11111111111111111111111111111111";
   auto pk = pubkey::from_base58(system_program);
   BOOST_CHECK_EQUAL(pk.to_base58(), system_program);

   // All zeros should encode to base58 ones (1 is zero in base58)
   pubkey zero_pk;
   std::ranges::fill(zero_pk.data, 0);
   std::string zero_b58 = zero_pk.to_base58();
   BOOST_CHECK_EQUAL(zero_b58, "11111111111111111111111111111111");
}

BOOST_AUTO_TEST_CASE(test_pubkey_system_program) {
   auto pk = system::program_ids::SYSTEM_PROGRAM;
   BOOST_CHECK_EQUAL(pk.to_base58(), "11111111111111111111111111111111");
}

BOOST_AUTO_TEST_CASE(test_pubkey_token_program) {
   auto pk = system::program_ids::TOKEN_PROGRAM;
   BOOST_CHECK_EQUAL(pk.to_base58(), "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
}

BOOST_AUTO_TEST_CASE(test_pubkey_is_zero) {
   pubkey zero_pk;
   BOOST_CHECK(zero_pk.is_zero());

   // System program address "11111...1" in base58 is actually all zeros
   // (in base58, "1" represents the zero byte)
   auto system_pk = system::program_ids::SYSTEM_PROGRAM;
   BOOST_CHECK(system_pk.is_zero());

   // Token program should NOT be zero
   auto token_pk = system::program_ids::TOKEN_PROGRAM;
   BOOST_CHECK(!token_pk.is_zero());
}

//=============================================================================
// Signature Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_signature_base58_roundtrip) {
   // Create a signature with known data
   solana::signature sig;
   for (size_t i = 0; i < solana::signature::SIZE; ++i) {
      sig.data[i] = static_cast<uint8_t>(i);
   }

   std::string b58 = sig.to_base58();
   auto decoded = solana::signature::from_base58(b58);

   BOOST_CHECK(sig == decoded);
}

//=============================================================================
// Compact-u16 Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_compact_u16_single_byte) {
   // Values 0-127 should encode as single byte
   for (uint16_t v = 0; v < 128; ++v) {
      auto encoded = compact_u16::encode(v);
      BOOST_CHECK_EQUAL(encoded.size(), 1u);
      BOOST_CHECK_EQUAL(encoded[0], v);

      auto [decoded, bytes_read] = compact_u16::decode(encoded.data(), encoded.size());
      BOOST_CHECK_EQUAL(decoded, v);
      BOOST_CHECK_EQUAL(bytes_read, 1u);
   }
}

BOOST_AUTO_TEST_CASE(test_compact_u16_two_bytes) {
   // Values 128-16383 should encode as two bytes
   uint16_t test_values[] = {128, 255, 256, 1000, 16383};
   for (uint16_t v : test_values) {
      auto encoded = compact_u16::encode(v);
      BOOST_CHECK_EQUAL(encoded.size(), 2u);

      auto [decoded, bytes_read] = compact_u16::decode(encoded.data(), encoded.size());
      BOOST_CHECK_EQUAL(decoded, v);
      BOOST_CHECK_EQUAL(bytes_read, 2u);
   }
}

BOOST_AUTO_TEST_CASE(test_compact_u16_three_bytes) {
   // Values 16384-65535 should encode as three bytes
   uint16_t test_values[] = {16384, 32768, 65535};
   for (uint16_t v : test_values) {
      auto encoded = compact_u16::encode(v);
      BOOST_CHECK_EQUAL(encoded.size(), 3u);

      auto [decoded, bytes_read] = compact_u16::decode(encoded.data(), encoded.size());
      BOOST_CHECK_EQUAL(decoded, v);
      BOOST_CHECK_EQUAL(bytes_read, 3u);
   }
}

//=============================================================================
// Borsh Encoder Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_borsh_primitives) {
   borsh::encoder enc;

   enc.write_u8(0x12);
   enc.write_u16(0x3456);
   enc.write_u32(0x789ABCDE);
   enc.write_u64(0x123456789ABCDEF0ULL);
   enc.write_bool(true);
   enc.write_bool(false);

   auto data = enc.finish();

   borsh::decoder dec(data);
   BOOST_CHECK_EQUAL(dec.read_u8(), 0x12);
   BOOST_CHECK_EQUAL(dec.read_u16(), 0x3456);
   BOOST_CHECK_EQUAL(dec.read_u32(), 0x789ABCDE);
   BOOST_CHECK_EQUAL(dec.read_u64(), 0x123456789ABCDEF0ULL);
   BOOST_CHECK_EQUAL(dec.read_bool(), true);
   BOOST_CHECK_EQUAL(dec.read_bool(), false);
   BOOST_CHECK_EQUAL(dec.remaining(), 0u);
}

BOOST_AUTO_TEST_CASE(test_borsh_string) {
   borsh::encoder enc;

   std::string test_str = "Hello, Solana!";
   enc.write_string(test_str);

   auto data = enc.finish();

   borsh::decoder dec(data);
   BOOST_CHECK_EQUAL(dec.read_string(), test_str);
}

BOOST_AUTO_TEST_CASE(test_borsh_bytes) {
   borsh::encoder enc;

   std::vector<uint8_t> test_bytes = {0x01, 0x02, 0x03, 0x04, 0x05};
   enc.write_bytes(test_bytes);

   auto data = enc.finish();

   borsh::decoder dec(data);
   auto decoded = dec.read_bytes();
   BOOST_CHECK(decoded == test_bytes);
}

BOOST_AUTO_TEST_CASE(test_borsh_pubkey) {
   borsh::encoder enc;

   auto pk = system::program_ids::TOKEN_PROGRAM;
   enc.write_pubkey(pk);

   auto data = enc.finish();

   borsh::decoder dec(data);
   auto decoded = dec.read_pubkey();
   BOOST_CHECK(decoded == pk);
}

BOOST_AUTO_TEST_CASE(test_borsh_option) {
   // Test Some value
   {
      borsh::encoder enc;
      enc.write_option(std::optional<uint64_t>(12345));

      auto data = enc.finish();
      borsh::decoder dec(data);
      auto decoded = dec.read_option<uint64_t>();
      BOOST_CHECK(decoded.has_value());
      BOOST_CHECK_EQUAL(*decoded, 12345);
   }

   // Test None value
   {
      borsh::encoder enc;
      enc.write_option(std::optional<uint64_t>(std::nullopt));

      auto data = enc.finish();
      borsh::decoder dec(data);
      auto decoded = dec.read_option<uint64_t>();
      BOOST_CHECK(!decoded.has_value());
   }
}

BOOST_AUTO_TEST_CASE(test_borsh_vec) {
   borsh::encoder enc;

   std::vector<uint32_t> test_vec = {1, 2, 3, 4, 5};
   enc.write_vec(test_vec);

   auto data = enc.finish();

   borsh::decoder dec(data);
   auto decoded = dec.read_vec<uint32_t>();
   BOOST_CHECK(decoded == test_vec);
}

//=============================================================================
// Message Serialization Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_message_serialization_roundtrip) {
   message msg;

   // Set up header
   msg.header.num_required_signatures = 1;
   msg.header.num_readonly_signed_accounts = 0;
   msg.header.num_readonly_unsigned_accounts = 1;

   // Add account keys
   msg.account_keys.push_back(pubkey::from_base58("4fYNw3dojWmQ4dXtSGE9epjRGy9pFSx62YypT7avPYvA"));
   msg.account_keys.push_back(pubkey::from_base58("11111111111111111111111111111111"));

   // Set blockhash
   msg.recent_blockhash = pubkey::from_base58("4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn");

   // Add a simple instruction
   compiled_instruction instr;
   instr.program_id_index = 1;
   instr.account_indices = {0};
   instr.data = {0x02, 0x00, 0x00, 0x00};  // Transfer instruction
   msg.instructions.push_back(instr);

   // Serialize
   auto serialized = msg.serialize();
   BOOST_CHECK(!serialized.empty());

   // Deserialize
   auto deserialized = message::deserialize(serialized);

   // Verify
   BOOST_CHECK_EQUAL(deserialized.header.num_required_signatures, msg.header.num_required_signatures);
   BOOST_CHECK_EQUAL(deserialized.header.num_readonly_signed_accounts, msg.header.num_readonly_signed_accounts);
   BOOST_CHECK_EQUAL(deserialized.header.num_readonly_unsigned_accounts, msg.header.num_readonly_unsigned_accounts);
   BOOST_CHECK_EQUAL(deserialized.account_keys.size(), msg.account_keys.size());
   BOOST_CHECK(deserialized.recent_blockhash == msg.recent_blockhash);
   BOOST_CHECK_EQUAL(deserialized.instructions.size(), msg.instructions.size());
}

//=============================================================================
// Transaction Serialization Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_transaction_serialization_roundtrip) {
   transaction tx;

   // Set up message
   tx.msg.header.num_required_signatures = 1;
   tx.msg.header.num_readonly_signed_accounts = 0;
   tx.msg.header.num_readonly_unsigned_accounts = 1;

   tx.msg.account_keys.push_back(pubkey::from_base58("4fYNw3dojWmQ4dXtSGE9epjRGy9pFSx62YypT7avPYvA"));
   tx.msg.account_keys.push_back(pubkey::from_base58("11111111111111111111111111111111"));

   tx.msg.recent_blockhash = pubkey::from_base58("4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn");

   compiled_instruction instr;
   instr.program_id_index = 1;
   instr.account_indices = {0};
   instr.data = {0x02, 0x00, 0x00, 0x00};
   tx.msg.instructions.push_back(instr);

   // Add a dummy signature
   solana::signature sig;
   std::fill(sig.data.begin(), sig.data.end(), 0xAB);
   tx.signatures.push_back(sig);

   // Serialize
   auto serialized = tx.serialize();
   BOOST_CHECK(!serialized.empty());

   // Deserialize
   auto deserialized = transaction::deserialize(serialized);

   // Verify
   BOOST_CHECK_EQUAL(deserialized.signatures.size(), tx.signatures.size());
   BOOST_CHECK(deserialized.signatures[0] == tx.signatures[0]);
}

//=============================================================================
// System Instructions Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_system_transfer_instruction) {
   auto from = pubkey::from_base58("4fYNw3dojWmQ4dXtSGE9epjRGy9pFSx62YypT7avPYvA");
   auto to = pubkey::from_base58("Cw93m7FLMTVc3JdTLd7JGDFTtMJaG6y5Z6kkVTyWXZVS");
   uint64_t lamports = 1000000000;  // 1 SOL

   auto instr = system::instructions::transfer(from, to, lamports);

   BOOST_CHECK(instr.program_id == system::program_ids::SYSTEM_PROGRAM);
   BOOST_CHECK_EQUAL(instr.accounts.size(), 2u);
   BOOST_CHECK(instr.accounts[0].key == from);
   BOOST_CHECK(instr.accounts[0].is_signer);
   BOOST_CHECK(instr.accounts[0].is_writable);
   BOOST_CHECK(instr.accounts[1].key == to);
   BOOST_CHECK(!instr.accounts[1].is_signer);
   BOOST_CHECK(instr.accounts[1].is_writable);
   BOOST_CHECK(!instr.data.empty());
}

BOOST_AUTO_TEST_CASE(test_system_create_account_instruction) {
   auto from = pubkey::from_base58("4fYNw3dojWmQ4dXtSGE9epjRGy9pFSx62YypT7avPYvA");
   auto new_account = pubkey::from_base58("Cw93m7FLMTVc3JdTLd7JGDFTtMJaG6y5Z6kkVTyWXZVS");
   uint64_t lamports = 1000000;
   uint64_t space = 100;
   auto owner = system::program_ids::TOKEN_PROGRAM;

   auto instr = system::instructions::create_account(from, new_account, lamports, space, owner);

   BOOST_CHECK(instr.program_id == system::program_ids::SYSTEM_PROGRAM);
   BOOST_CHECK_EQUAL(instr.accounts.size(), 2u);
   BOOST_CHECK(instr.accounts[0].key == from);
   BOOST_CHECK(instr.accounts[0].is_signer);
   BOOST_CHECK(instr.accounts[1].key == new_account);
   BOOST_CHECK(instr.accounts[1].is_signer);
}

//=============================================================================
// Compute Budget Instructions Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_compute_budget_set_units) {
   auto instr = system::compute_budget::set_compute_unit_limit(200000);

   BOOST_CHECK(instr.program_id == system::program_ids::COMPUTE_BUDGET_PROGRAM);
   BOOST_CHECK(instr.accounts.empty());
   BOOST_CHECK(!instr.data.empty());
}

BOOST_AUTO_TEST_CASE(test_compute_budget_set_price) {
   auto instr = system::compute_budget::set_compute_unit_price(1000);

   BOOST_CHECK(instr.program_id == system::program_ids::COMPUTE_BUDGET_PROGRAM);
   BOOST_CHECK(instr.accounts.empty());
   BOOST_CHECK(!instr.data.empty());
}

//=============================================================================
// IDL Discriminator Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_instruction_discriminator) {
   // Test known discriminator values
   // Anchor discriminator for "initialize" instruction
   auto disc = idl::compute_instruction_discriminator("initialize");

   // The discriminator should be 8 bytes
   BOOST_CHECK_EQUAL(disc.size(), 8u);

   // Verify it's deterministic
   auto disc2 = idl::compute_instruction_discriminator("initialize");
   BOOST_CHECK(disc == disc2);

   // Different instruction names should have different discriminators
   auto disc_other = idl::compute_instruction_discriminator("transfer");
   BOOST_CHECK(disc != disc_other);
}

BOOST_AUTO_TEST_CASE(test_account_discriminator) {
   auto disc = idl::compute_account_discriminator("Counter");
   BOOST_CHECK_EQUAL(disc.size(), 8u);

   // Verify it's different from instruction discriminator with same name
   auto instr_disc = idl::compute_instruction_discriminator("Counter");
   BOOST_CHECK(disc != instr_disc);
}

//=============================================================================
// IDL Type Parsing Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_idl_type_primitive) {
   fc::variant v = "u64";
   auto t = idl::idl_type::from_variant(v);

   BOOST_CHECK(t.is_primitive());
   BOOST_CHECK(t.kind == idl::type_kind::primitive);
   BOOST_CHECK(t.primitive.has_value());
   BOOST_CHECK(t.get_primitive() == idl::primitive_type::u64);
   BOOST_CHECK(!t.is_option());
   BOOST_CHECK(!t.is_vec());

   // Test to_string conversion
   BOOST_CHECK_EQUAL(t.to_string(), "u64");
}

BOOST_AUTO_TEST_CASE(test_idl_type_bool) {
   fc::variant v = "bool";
   auto t = idl::idl_type::from_variant(v);

   BOOST_CHECK(t.is_primitive());
   BOOST_CHECK(t.get_primitive() == idl::primitive_type::bool_t);
   BOOST_CHECK_EQUAL(t.to_string(), "bool");
}

BOOST_AUTO_TEST_CASE(test_idl_type_pubkey) {
   // Test both "pubkey" and "publicKey" variants
   fc::variant v1 = "pubkey";
   auto t1 = idl::idl_type::from_variant(v1);
   BOOST_CHECK(t1.is_primitive());
   BOOST_CHECK(t1.get_primitive() == idl::primitive_type::pubkey);

   fc::variant v2 = "publicKey";
   auto t2 = idl::idl_type::from_variant(v2);
   BOOST_CHECK(t2.is_primitive());
   BOOST_CHECK(t2.get_primitive() == idl::primitive_type::pubkey);
}

BOOST_AUTO_TEST_CASE(test_idl_type_option) {
   fc::mutable_variant_object obj;
   obj("option", "u64");

   auto t = idl::idl_type::from_variant(fc::variant(obj));

   BOOST_CHECK(t.is_option());
   BOOST_CHECK(t.kind == idl::type_kind::option);
   BOOST_CHECK(t.option_inner != nullptr);
   BOOST_CHECK(t.option_inner->is_primitive());
   BOOST_CHECK(t.option_inner->get_primitive() == idl::primitive_type::u64);
   BOOST_CHECK_EQUAL(t.to_string(), "Option<u64>");
}

BOOST_AUTO_TEST_CASE(test_idl_type_vec) {
   fc::mutable_variant_object obj;
   obj("vec", "pubkey");

   auto t = idl::idl_type::from_variant(fc::variant(obj));

   BOOST_CHECK(t.is_vec());
   BOOST_CHECK(t.kind == idl::type_kind::vec);
   BOOST_CHECK(t.vec_element != nullptr);
   BOOST_CHECK(t.vec_element->is_primitive());
   BOOST_CHECK(t.vec_element->get_primitive() == idl::primitive_type::pubkey);
   BOOST_CHECK_EQUAL(t.to_string(), "Vec<pubkey>");
}

BOOST_AUTO_TEST_CASE(test_idl_type_array) {
   fc::mutable_variant_object obj;
   fc::variants arr;
   arr.push_back("u8");
   arr.push_back(32);
   obj("array", arr);

   auto t = idl::idl_type::from_variant(fc::variant(obj));

   BOOST_CHECK(t.is_array());
   BOOST_CHECK(t.kind == idl::type_kind::array);
   BOOST_CHECK(t.array_element != nullptr);
   BOOST_CHECK(t.array_element->get_primitive() == idl::primitive_type::u8);
   BOOST_CHECK_EQUAL(*t.array_len, 32u);
   BOOST_CHECK_EQUAL(t.to_string(), "[u8; 32]");
}

BOOST_AUTO_TEST_CASE(test_idl_type_defined) {
   fc::mutable_variant_object obj;
   obj("defined", "MyStruct");

   auto t = idl::idl_type::from_variant(fc::variant(obj));

   BOOST_CHECK(t.is_defined());
   BOOST_CHECK(t.kind == idl::type_kind::defined);
   BOOST_CHECK_EQUAL(t.get_defined_name(), "MyStruct");
   BOOST_CHECK_EQUAL(t.to_string(), "MyStruct");
}

BOOST_AUTO_TEST_CASE(test_idl_primitive_type_conversions) {
   // Test string -> enum conversions using magic_enum
   auto u8_opt = idl::primitive_type_from_string("u8");
   BOOST_CHECK(u8_opt.has_value());
   BOOST_CHECK(*u8_opt == idl::primitive_type::u8);

   auto string_opt = idl::primitive_type_from_string("string");
   BOOST_CHECK(string_opt.has_value());
   BOOST_CHECK(*string_opt == idl::primitive_type::string);

   // Test enum -> string conversions
   BOOST_CHECK_EQUAL(idl::primitive_type_to_string(idl::primitive_type::u64), "u64");
   BOOST_CHECK_EQUAL(idl::primitive_type_to_string(idl::primitive_type::bool_t), "bool");
   BOOST_CHECK_EQUAL(idl::primitive_type_to_string(idl::primitive_type::pubkey), "pubkey");

   // Unknown type should return nullopt
   auto unknown = idl::primitive_type_from_string("unknown_type");
   BOOST_CHECK(!unknown.has_value());
}

BOOST_AUTO_TEST_CASE(test_idl_pda_seed_kind) {
   // Test the pda_seed_kind enum
   idl::pda_seed seed1(idl::pda_seed_kind::const_value, "some_value");
   BOOST_CHECK(seed1.kind == idl::pda_seed_kind::const_value);

   idl::pda_seed seed2(idl::pda_seed_kind::arg, "amount");
   BOOST_CHECK(seed2.kind == idl::pda_seed_kind::arg);

   idl::pda_seed seed3(idl::pda_seed_kind::account, "owner");
   BOOST_CHECK(seed3.kind == idl::pda_seed_kind::account);
}

//=============================================================================
// Commitment Tests
//=============================================================================

BOOST_AUTO_TEST_CASE(test_commitment_to_string) {
   BOOST_CHECK_EQUAL(to_string(commitment_t::processed), "processed");
   BOOST_CHECK_EQUAL(to_string(commitment_t::confirmed), "confirmed");
   BOOST_CHECK_EQUAL(to_string(commitment_t::finalized), "finalized");
}

BOOST_AUTO_TEST_SUITE_END()

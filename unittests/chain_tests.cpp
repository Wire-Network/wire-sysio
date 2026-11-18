#include <sysio/chain/controller.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/transaction.hpp>
#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

#include "fork_test_utilities.hpp"
#include "test_cfd_transaction.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

/// The binary layout of a packed transaction with every field set directly, for forms the constructors do not build.
struct raw_packed_transaction {
   vector<signature_type> signatures;
   uint8_t                compression = 0;
   bytes                  packed_context_free_data;
   bytes                  packed_trx;
};

/// The binary layout of a transaction receipt whose transaction is a raw_packed_transaction.
struct raw_transaction_receipt : transaction_receipt_header {
   raw_packed_transaction trx;
};

/// A transaction with one `reqauth` action, signed by the system account; `expiration` sets it apart from another.
signed_transaction make_reqauth_trx(base_tester& chain,
                                   uint32_t expiration = base_tester::DEFAULT_EXPIRATION_DELTA) {
   signed_transaction trx;
   trx.actions.emplace_back(vector<permission_level>{{config::system_account_name, config::active_name}},
                            config::system_account_name, "reqauth"_n, fc::raw::pack(config::system_account_name));
   chain.set_transaction_headers(trx, expiration);
   trx.sign(chain.get_private_key(config::system_account_name, "active"), chain.get_chain_id());
   return trx;
}

/// `trx` with one context-free action and its context-free data entry.
signed_transaction with_context_free_data(signed_transaction trx) {
   trx.context_free_actions.emplace_back(vector<permission_level>{}, cf_action{});
   trx.context_free_data.emplace_back(fc::raw::pack(cf_action{}.payload));
   return trx;
}

/// zlib-compresses `data` the way clients compress a packed transaction's fields.
bytes zlib_compress(const bytes& data) {
   namespace bio = boost::iostreams;
   bytes out;
   bio::filtering_ostream comp;
   comp.push(bio::zlib_compressor(bio::zlib::best_compression));
   comp.push(bio::back_inserter(out));
   bio::write(comp, data.data(), data.size());
   bio::close(comp);
   return out;
}

} // namespace

FC_REFLECT(raw_packed_transaction, (signatures)(compression)(packed_context_free_data)(packed_trx))
FC_REFLECT_DERIVED(raw_transaction_receipt, (sysio::chain::transaction_receipt_header), (trx))

namespace {

/// `blk`, which holds one receipt, packed with that receipt's transaction laid out as `trx`.
bytes pack_with_receipt(const signed_block& blk, const raw_packed_transaction& trx) {
   const raw_transaction_receipt receipt{static_cast<const transaction_receipt_header&>(blk.transactions.front()), trx};
   bytes packed = fc::raw::pack(static_cast<const signed_block_header&>(blk));
   for (const bytes& part : {fc::raw::pack(vector<raw_transaction_receipt>{receipt}), fc::raw::pack(blk.qc),
                             fc::raw::pack(blk.block_extensions)})
      packed.insert(packed.end(), part.begin(), part.end());
   return packed;
}

} // namespace

BOOST_AUTO_TEST_SUITE(chain_tests)

BOOST_AUTO_TEST_CASE( replace_producer_keys ) try {
   validating_tester tester;

   const auto new_key = get_public_key(name("newkey"), config::active_name.to_string());

   // make sure new keys is not used
   for(const auto& prod : tester.control->active_producers().producers) {
      for(const auto& key : std::get<block_signing_authority_v0>(prod.authority).keys){  
         BOOST_REQUIRE(key.key != new_key);
      }
   }

   // Add test with instant-finality enabled
   // - Not implemented yet

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE_TEMPLATE( replace_account_keys, T, validating_testers ) try {
   T tester;
   const name usr = config::system_account_name;
   const name active_permission = config::active_name;
   const auto& rlm = tester.control->get_resource_limits_manager();
   const auto* perm = tester.control->db().template find<permission_object, by_owner>(boost::make_tuple(usr, active_permission));
   BOOST_REQUIRE(perm != NULL);

   const int64_t old_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   const auto old_usr_auth = perm->auth;
   const auto new_key = get_public_key(name("newkey"), "active");
   const authority expected_authority(new_key);
   BOOST_REQUIRE(old_usr_auth != expected_authority);
   const auto old_ram_usg = rlm.get_account_ram_usage(usr);

   BOOST_REQUIRE_NO_THROW(tester.control->replace_account_keys(usr, active_permission, new_key));
   const int64_t new_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   const auto new_ram_usg = rlm.get_account_ram_usage(usr);
   BOOST_REQUIRE_EQUAL(old_ram_usg + (new_size - old_size), new_ram_usg);
   const auto new_usr_auth = perm->auth;
   BOOST_REQUIRE(new_usr_auth == expected_authority);

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE_TEMPLATE( decompressed_size_over_limit, T, testers ) try {
   T chain;

   // build a transaction, add cf data, sign
   cf_action                        cfa;
   sysio::chain::signed_transaction trx;
   sysio::chain::action             act({}, cfa);
   trx.context_free_actions.insert(trx.context_free_actions.end(), 129, act);
   // this is a over limit size 4*20*129*1024 = 1032*1024 > 10M
   for(int i = 0; i < 129; ++i){
      vector<uint32_t> v(20*1024, 1);
      trx.context_free_data.emplace_back(fc::raw::pack<vector<uint32_t>>(v));
   }
   // add a normal action along with cfa
   dummy_action         da = {DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C};
   sysio::chain::action act1(
       std::vector<sysio::chain::permission_level>{{"testapi"_n, config::active_name}}, da);
   trx.actions.push_back(act1);
   chain.set_transaction_headers(trx);
   auto sig = trx.sign(chain.get_private_key("testapi"_n, "active"), chain.get_chain_id());

   // pack
   packed_transaction pt(trx, packed_transaction::compression_type::zlib);
   // try unpack and throw
   bytes packed_txn = pt.get_packed_transaction();
   bytes pcfd = pt.get_packed_context_free_data();
   vector<signature_type>  sigs;
   sigs.push_back(sig);
   BOOST_REQUIRE_EXCEPTION(packed_transaction copy( std::move(packed_txn), std::move(sigs), std::move(pcfd), packed_transaction::compression_type::zlib ),
                           tx_decompression_error,
                           [](const tx_decompression_error& e) {
                              return e.to_detail_string().find("Exceeded maximum decompressed transaction size") != std::string::npos;
                           });
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE_TEMPLATE( decompressed_size_under_limit, T, testers ) try {
   T chain;

   // build a transaction, add cf data, sign
   cf_action                        cfa;
   sysio::chain::signed_transaction trx;
   sysio::chain::action             act({}, cfa);
   constexpr size_t num_cf_actions = 129;
   trx.context_free_actions.insert(trx.context_free_actions.end(), num_cf_actions, act);
   // this is a over limit size 4*129*1024 = ~2M < 10M
   for(size_t i = 0; i < num_cf_actions; ++i){
      vector<uint32_t> v(1024, 1);
      trx.context_free_data.emplace_back(fc::raw::pack<vector<uint32_t>>(v));
   }
   // add a normal action along with cfa
   dummy_action         da = {DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C};
   sysio::chain::action act1(
       std::vector<sysio::chain::permission_level>{{"testapi"_n, config::active_name}}, da);
   trx.actions.push_back(act1);
   chain.set_transaction_headers(trx);
   auto sig = trx.sign(chain.get_private_key("testapi"_n, "active"), chain.get_chain_id());

   // pack
   packed_transaction pt(trx, packed_transaction::compression_type::zlib);
   // try unpack
   bytes packed_txn = pt.get_packed_transaction();
   bytes pcfd = pt.get_packed_context_free_data();
   vector<signature_type>  sigs;
   sigs.push_back(sig);
   packed_transaction copy( std::move(packed_txn), std::move(sigs), std::move(pcfd), packed_transaction::compression_type::zlib );
   //passes if no exception is thrown

} FC_LOG_AND_RETHROW()

// Packed context-free data is its list and nothing after it. With no context-free data, no bytes and the packed empty
// list are both accepted.
BOOST_AUTO_TEST_CASE_TEMPLATE( packed_context_free_data_without_trailing_bytes, T, testers ) try {
   T chain;

   const signed_transaction trx = make_reqauth_trx(chain);

   const bytes packed_trx = fc::raw::pack(static_cast<const transaction&>(trx));
   const auto pack_with = [&](bytes packed_cfd) {
      return packed_transaction(bytes(packed_trx), vector<signature_type>(trx.signatures), std::move(packed_cfd),
                                packed_transaction::compression_type::none);
   };
   const auto check_trailing_rejected = [&](bytes packed_cfd) {
      BOOST_CHECK_EXCEPTION(pack_with(std::move(packed_cfd)), tx_extra_data,
                            fc_exception_message_is("packed_transaction contains extra data beyond context-free data"));
   };

   const bytes empty_list = fc::raw::pack(vector<bytes>{});
   BOOST_CHECK(pack_with({}).get_context_free_data().empty());
   BOOST_CHECK(pack_with(empty_list).get_context_free_data().empty());

   bytes empty_list_then_byte = empty_list;
   empty_list_then_byte.push_back(0);
   check_trailing_rejected(empty_list_then_byte);

   bytes entry_then_byte = fc::raw::pack(vector<bytes>{bytes{}});
   entry_then_byte.push_back(0);
   check_trailing_rejected(entry_then_byte);

   // The packed empty list is how clients send a transaction without context-free data.
   auto with_empty_list = pack_with(empty_list);
   BOOST_REQUIRE(chain.push_transaction(with_empty_list)->receipt);
   chain.produce_block();

} FC_LOG_AND_RETHROW()

// Binary unpack rejects a compressed transaction, and an unknown compression value, before inflating anything. The
// constructors still accept a compressed transaction, as clients submit one.
BOOST_AUTO_TEST_CASE_TEMPLATE( binary_unpack_rejects_compression, T, testers ) try {
   T chain;
   const signed_transaction trx = make_reqauth_trx(chain);

   const packed_transaction compressed(trx, packed_transaction::compression_type::zlib);
   BOOST_REQUIRE(compressed.get_compression() == packed_transaction::compression_type::zlib);
   BOOST_CHECK_EXCEPTION(fc::raw::unpack<packed_transaction>(fc::raw::pack(compressed)), tx_compression_not_allowed,
                         fc_exception_message_is("packed transaction cannot be compressed"));
   // Reflected deserialization from JSON rejects it too; abi_serializer goes through the constructors instead.
   packed_transaction from_reflection;
   BOOST_CHECK_EXCEPTION(fc::from_variant(fc::variant(compressed), from_reflection), tx_compression_not_allowed,
                         fc_exception_message_is("packed transaction cannot be compressed"));

   const raw_packed_transaction unknown_compression{trx.signatures, 2, {},
                                                    fc::raw::pack(static_cast<const transaction&>(trx))};
   BOOST_CHECK_EXCEPTION(fc::raw::unpack<packed_transaction>(fc::raw::pack(unknown_compression)),
                         tx_compression_not_allowed,
                         fc_exception_message_is("packed transaction cannot be compressed"));

   const packed_transaction uncompressed(trx);
   BOOST_CHECK_EQUAL(fc::raw::unpack<packed_transaction>(fc::raw::pack(uncompressed)).id(), trx.id());

} FC_LOG_AND_RETHROW()

// Binary unpack rejects any packed context-free data, the packed empty list and a longer encoding of it included,
// before unpacking it. The constructors accept the packed empty list and keep no packed context-free data for it.
BOOST_AUTO_TEST_CASE_TEMPLATE( binary_unpack_rejects_context_free_data, T, testers ) try {
   T chain;
   const signed_transaction trx = make_reqauth_trx(chain);
   const bytes packed_trx = fc::raw::pack(static_cast<const transaction&>(trx));
   const auto check_rejected = [](const bytes& packed) {
      BOOST_CHECK_EXCEPTION(fc::raw::unpack<packed_transaction>(packed), transaction_exception,
                            fc_exception_message_is("context-free data is not supported"));
   };

   const packed_transaction with_cfd(with_context_free_data(trx));
   BOOST_REQUIRE(!with_cfd.get_packed_context_free_data().empty());
   check_rejected(fc::raw::pack(with_cfd));

   const bytes long_empty_list{'\x80', '\x00'};
   BOOST_REQUIRE(fc::raw::unpack<vector<bytes>>(long_empty_list).empty());
   check_rejected(fc::raw::pack(raw_packed_transaction{trx.signatures, 0, long_empty_list, packed_trx}));

   const bytes empty_list = fc::raw::pack(vector<bytes>{});
   check_rejected(fc::raw::pack(raw_packed_transaction{trx.signatures, 0, empty_list, packed_trx}));

   const packed_transaction from_empty_list(bytes(packed_trx), vector<signature_type>(trx.signatures),
                                            bytes(empty_list), packed_transaction::compression_type::none);
   BOOST_CHECK(from_empty_list.get_packed_context_free_data().empty());
   BOOST_CHECK_EQUAL(fc::raw::unpack<packed_transaction>(fc::raw::pack(from_empty_list)).id(), trx.id());

} FC_LOG_AND_RETHROW()

// Binary unpack rejects a block whose receipt holds a compressed transaction or packed context-free data, the packed
// empty list included.
BOOST_AUTO_TEST_CASE_TEMPLATE( binary_unpack_rejects_block_with_unsupported_receipt, T, testers ) try {
   T chain;
   signed_transaction trx = make_reqauth_trx(chain);
   chain.push_transaction(trx);
   const signed_block_ptr blk = chain.produce_block();
   BOOST_REQUIRE_EQUAL(blk->transactions.size(), 1u);
   BOOST_CHECK_EQUAL(fc::raw::unpack<signed_block>(fc::raw::pack(*blk)).transactions.front().trx.id(), trx.id());

   mutable_block_ptr compressed = blk->clone();
   compressed->transactions.front().trx = packed_transaction(trx, packed_transaction::compression_type::zlib);
   BOOST_CHECK_EXCEPTION(fc::raw::unpack<signed_block>(fc::raw::pack(*compressed)), tx_compression_not_allowed,
                         fc_exception_message_is("packed transaction cannot be compressed"));

   mutable_block_ptr with_cfd = blk->clone();
   with_cfd->transactions.front().trx = packed_transaction(with_context_free_data(trx));
   BOOST_CHECK_EXCEPTION(fc::raw::unpack<signed_block>(fc::raw::pack(*with_cfd)), transaction_exception,
                         fc_exception_message_is("context-free data is not supported"));

   // The constructors clear the packed empty list, so the receipt holding it is laid out field by field.
   raw_packed_transaction receipt_trx{trx.signatures, 0, {}, fc::raw::pack(static_cast<const transaction&>(trx))};
   BOOST_REQUIRE(pack_with_receipt(*blk, receipt_trx) == fc::raw::pack(*blk));
   receipt_trx.packed_context_free_data = fc::raw::pack(vector<bytes>{});
   BOOST_CHECK_EXCEPTION(fc::raw::unpack<signed_block>(pack_with_receipt(*blk, receipt_trx)), transaction_exception,
                         fc_exception_message_is("context-free data is not supported"));

} FC_LOG_AND_RETHROW()

// A client sends a transaction without context-free data with no packed context-free data or the packed empty list,
// either one zlib-compressed. The node keeps no packed context-free data for any of them, so the block it produces
// unpacks from binary and validates on another node.
BOOST_AUTO_TEST_CASE_TEMPLATE( client_forms_without_context_free_data_validate_from_binary_block, T, testers ) try {
   T chain;
   T validator;

   struct client_form {
      packed_transaction::compression_type compression;
      bytes                                packed_context_free_data;
   };
   const bytes empty_list = fc::raw::pack(vector<bytes>{});
   const vector<client_form> forms{
      {packed_transaction::compression_type::none, {}},
      {packed_transaction::compression_type::none, empty_list},
      {packed_transaction::compression_type::zlib, {}},
      {packed_transaction::compression_type::zlib, zlib_compress(empty_list)},
   };

   uint32_t expiration = base_tester::DEFAULT_EXPIRATION_DELTA;
   for (const client_form& form : forms) {
      const signed_transaction trx = make_reqauth_trx(chain, expiration++);
      bytes packed_trx = fc::raw::pack(static_cast<const transaction&>(trx));
      if (form.compression == packed_transaction::compression_type::zlib)
         packed_trx = zlib_compress(packed_trx);
      packed_transaction ptrx(std::move(packed_trx), vector<signature_type>(trx.signatures),
                              bytes(form.packed_context_free_data), form.compression);
      ptrx.decompress(); // as chain_plugin does for a pushed transaction
      BOOST_REQUIRE(chain.push_transaction(ptrx)->receipt);
   }

   const signed_block_ptr blk = chain.produce_block();
   BOOST_REQUIRE_EQUAL(blk->transactions.size(), forms.size());
   for (const transaction_receipt& receipt : blk->transactions) {
      BOOST_CHECK(receipt.trx.get_compression() == packed_transaction::compression_type::none);
      BOOST_CHECK(receipt.trx.get_packed_context_free_data().empty());
   }

   const signed_block_ptr from_binary =
      std::make_shared<signed_block>(fc::raw::unpack<signed_block>(fc::raw::pack(*blk)));
   validator.push_block(from_binary);
   BOOST_CHECK_EQUAL(validator.control->head().id(), blk->calculate_id());

} FC_LOG_AND_RETHROW()

// verify accepted_block signals validated blocks
BOOST_AUTO_TEST_CASE_TEMPLATE( signal_validated_blocks, T, testers ) try {
   T chain;
   T validator;

   signed_block_ptr accepted_block;
   block_id_type accepted_id;
   auto c = chain.control->accepted_block().connect([&](block_signal_params t) {
      const auto& [ block, id ] = t;
      auto block_num = block->block_num();
      BOOST_CHECK(block);
      BOOST_CHECK(chain.fetch_block_by_id(id) == block);
      BOOST_CHECK(chain.block_exists(id));
      BOOST_CHECK(chain.fetch_block_by_number(block_num) == block);
      BOOST_REQUIRE(chain.control->fetch_block_header_by_number(block_num));
      BOOST_CHECK(chain.control->fetch_block_header_by_number(block_num)->calculate_id() == id);
      BOOST_REQUIRE(chain.control->fetch_block_header_by_id(id));
      BOOST_CHECK(chain.control->fetch_block_header_by_id(id)->calculate_id() == id);
      accepted_block = block;
      accepted_id = id;
   });
   signed_block_ptr validated_block;
   block_id_type validated_id;
   auto c2 = validator.control->accepted_block().connect([&](block_signal_params t) {
      const auto& [ block, id ] = t;
      auto block_num = block->block_num();
      BOOST_CHECK(block);
      BOOST_CHECK(validator.fetch_block_by_id(id) == block);
      BOOST_CHECK(validator.block_exists(id));
      BOOST_CHECK(validator.fetch_block_by_number(block_num) == block);
      BOOST_REQUIRE(validator.control->fetch_block_header_by_number(block_num));
      BOOST_CHECK(validator.control->fetch_block_header_by_number(block_num)->calculate_id() == id);
      BOOST_REQUIRE(validator.control->fetch_block_header_by_id(id));
      BOOST_CHECK(validator.control->fetch_block_header_by_id(id)->calculate_id() == id);
      validated_block = block;
      validated_id = id;
   });

   chain.produce_block();
   validator.push_block(accepted_block);

   chain.create_account("hello"_n);
   auto produced_block = chain.produce_block();
   validator.push_block(accepted_block);
   BOOST_CHECK(produced_block->calculate_id() == accepted_id);
   BOOST_CHECK(accepted_id == validated_id);

} FC_LOG_AND_RETHROW()

// verify applied_transaction signals trx in blocks
BOOST_AUTO_TEST_CASE( signal_applied_transaction ) try {
   savanna_tester chain;

   chain.produce_block();

   transaction_trace_ptr last_trace;
   auto c = chain.control->applied_transaction().connect([&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> x) {
      auto& t = std::get<0>(x);
      if (std::get<1>(x)->get_transaction().actions.at(0).name != "onblock"_n)
         last_trace = t;
   } );

   {
      transaction_trace_ptr create_account_trace = chain.create_account("hello"_n);
      BOOST_REQUIRE(last_trace);
      BOOST_REQUIRE(create_account_trace);
      BOOST_TEST(create_account_trace->id == last_trace->id);
      BOOST_TEST(create_account_trace->elapsed.count() == last_trace->elapsed.count());
      BOOST_TEST(create_account_trace->block_num == last_trace->block_num);
      signed_block_ptr block = chain.produce_block();
      BOOST_REQUIRE(block);
      bool found = false;
      for (auto& r : block->transactions) {
         found = true;
         BOOST_TEST(r.trx.get_transaction().actions.at(0).name == "newaccount"_n);
      }
      BOOST_TEST(found);
   }
   // abort block with create account
   {
      last_trace = nullptr;
      transaction_trace_ptr create_account_trace = chain.create_account("hello2"_n);
      BOOST_REQUIRE(last_trace);
      BOOST_REQUIRE(create_account_trace);
      BOOST_TEST(create_account_trace->id == last_trace->id);
      BOOST_TEST(create_account_trace->elapsed.count() == last_trace->elapsed.count());
      BOOST_TEST(create_account_trace->block_num == last_trace->block_num);
      signed_block_ptr empty_block = chain.produce_empty_block(); // aborts block, places in unapplied trx queue
      auto trx_meta = chain.get_unapplied_transaction_queue().get_trx(create_account_trace->id);
      BOOST_REQUIRE(trx_meta);
      BOOST_REQUIRE(empty_block);
      BOOST_TEST(empty_block->transactions.empty());
      last_trace = nullptr;
      signed_block_ptr block = chain.produce_block();
      BOOST_REQUIRE(block);
      BOOST_REQUIRE(last_trace);
      BOOST_TEST(create_account_trace->id == last_trace->id);
      BOOST_TEST(create_account_trace->block_num < last_trace->block_num); // different block
      BOOST_TEST(trx_meta->elapsed.count() == std::max(last_trace->elapsed, create_account_trace->elapsed).count()); // verify trx_meta updated
      BOOST_TEST(block->block_num() == last_trace->block_num);
      bool found = false;
      for (auto& r : block->transactions) {
         found = true;
         BOOST_TEST(r.trx.get_transaction().actions.at(0).name == "newaccount"_n);
      }
      BOOST_TEST(found);
   }

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

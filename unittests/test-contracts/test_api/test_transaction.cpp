#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/transaction.hpp>

#include "test_api.hpp"

#pragma pack(push, 1)
template <uint64_t ACCOUNT, uint64_t NAME>
struct test_action_action {
   static sysio::name get_account() {
      return sysio::name{ACCOUNT};
   }

   static sysio::name get_name() {
      return sysio::name{NAME};
   }

   std::vector<char> data;

   template <typename DataStream>
   friend DataStream& operator<< ( DataStream& ds, const test_action_action& a ) {
      for ( auto c : a.data )
         ds << c;
      return ds;
   }
};


template <uint64_t ACCOUNT, uint64_t NAME>
struct test_dummy_action {
   static sysio::name get_account() {
      return sysio::name{ACCOUNT};
   }

   static sysio::name get_name() {
      return sysio::name{NAME};
   }
   char a;
   unsigned long long b;
   int32_t c;

   template <typename DataStream>
   friend DataStream& operator<< ( DataStream& ds, const test_dummy_action& da ) {
      ds << da.a;
      ds << da.b;
      ds << da.c;
      return ds;
   }

   template <typename DataStream>
   friend DataStream& operator>> ( DataStream& ds, test_dummy_action& da ) {
      ds >> da.a;
      ds >> da.b;
      ds >> da.c;
      return ds;
   }
};
#pragma pack(pop)

void copy_data( char* data, size_t data_len, std::vector<char>& data_out ) {
   for (unsigned int i=0; i < data_len; i++)
      data_out.push_back(data[i]);
}

void test_transaction::send_action() {
   using namespace sysio;
   test_dummy_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action_normal" )> test_action =
      { DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C };

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_action", "read_action_normal" )}, test_action );

   act.send();
}

void test_transaction::send_action_empty() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "assert_true" )> test_action;

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_action", "assert_true" )}, test_action );

   act.send();
}

/**
 * cause failure due to a large action payload, larger than max_inline_action_size of 512K
 */
void test_transaction::send_action_large() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action" )> test_action;
   test_action.data.resize(512*1024+1);

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "read_action")}, test_action );
   act.send();
   sysio_assert( false, "send_message_large() should've thrown an error" );
}

/**
 * send an inline action that is 4K
 */
void test_transaction::send_action_4k() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action" )> test_action;
   test_action.data.resize(4*1024);

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "read_action")}, test_action );

   act.send();
}

/**
 * send an inline action that is 512K (limit is < 512K)
 * the limit includes the size of the action
 */
void test_transaction::send_action_512k() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action" )> test_action;

   test_action.data.resize(1);
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action temp_act( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "read_action")}, test_action );

   size_t action_size = pack_size(temp_act);
   test_action.data.resize(512*1024-action_size-2); // check is < 512K

   // send at limit (512K - 1)
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "read_action")}, test_action );

   if (pack_size(act) != 512*1024-1) {
      std::string err = "send_action_512k action size is: " + std::to_string(action_size) + " not 512K-1";
      sysio_assert(false, err.c_str());
   }

   act.send();
}

/**
 * send many inline actions that are 512K (limit is < 512K)
 * the limit includes the size of the action
 */
void test_transaction::send_many_actions_512k() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_transaction", "send_action_512k" )> test_action;

   test_action.data.resize(1);
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_transaction", "send_action_512k")}, test_action );

   // 65 * 512K > wasm memory limit, which is ok because each gets their own wasm instantiation
   for (size_t i = 0; i < 65; ++i) {
      act.send();
   }
}

/**
 * cause failure due recursive loop
 */
void test_transaction::send_action_recurse() {
   using namespace sysio;
   char buffer[1024];
   sysio::read_action_data( buffer, 1024 );

   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_transaction", "send_action_recurse" )> test_action;
   copy_data( buffer, 1024, test_action.data );

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_transaction", "send_action_recurse" )}, test_action );

   act.send();
}

/**
 * cause failure due to inline TX failure
 */
void test_transaction::send_action_inline_fail() {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "assert_false" )> test_action;

   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   action act( permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_action", "assert_false" )}, test_action );

   act.send();
}

void test_transaction::test_tapos_block_prefix() {
   using namespace sysio;
   int tbp;
   sysio::read_action_data( (char*)&tbp, sizeof(int) );
   sysio_assert( tbp == sysio::tapos_block_prefix(), "tapos_block_prefix does not match" );
}

void test_transaction::test_tapos_block_num() {
   using namespace sysio;
   int tbn;
   sysio::read_action_data( (char*)&tbn, sizeof(int) );
   sysio_assert( tbn == sysio::tapos_block_num(), "tapos_block_num does not match" );
}

void test_transaction::test_read_transaction() {
   using namespace sysio;
   checksum256 h;
   auto size = sysio::transaction_size();
   char buf[size];
   uint32_t read = sysio::read_transaction( buf, size );
   sysio_assert( size == read, "read_transaction failed");
   h = sysio::sha256(buf, read);
   print(h);
}

void test_transaction::test_transaction_size() {
   using namespace sysio;
   uint32_t trans_size = 0;
   sysio::read_action_data( (char*)&trans_size, sizeof(uint32_t) );
   print( "size: ", sysio::transaction_size() );
   sysio_assert( trans_size == sysio::transaction_size(), "transaction size does not match" );
}

void test_transaction::send_transaction(uint64_t receiver, uint64_t, uint64_t) {
   using namespace sysio;
   dummy_action payload = { DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C };

   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action_normal" )> test_action;
   copy_data( (char*)&payload, sizeof(dummy_action), test_action.data );

   auto trx = transaction();
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };

   trx.actions.emplace_back(permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_action", "read_action_normal" )}, test_action);
   trx.send( 0, name{receiver} );
}

void test_transaction::send_action_sender( uint64_t receiver, uint64_t, uint64_t ) {
   using namespace sysio;
   uint64_t cur_send;
   sysio::read_action_data( &cur_send, sizeof(name) );

   auto trx = transaction();
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };

   trx.actions.emplace_back(permissions, name{"testapi"}, name{WASM_TEST_ACTION( "test_action", "test_current_sender" )}, &cur_send);
   trx.send( 0, name{receiver} );
}

void test_transaction::send_transaction_empty( uint64_t receiver, uint64_t, uint64_t ) {
   using namespace sysio;
   auto trx = transaction();
   trx.send( 0, name{receiver} );

   sysio_assert( false, "send_transaction_empty() should've thrown an error" );
}

void test_transaction::send_transaction_trigger_error_handler( uint64_t receiver, uint64_t, uint64_t ) {
   using namespace sysio;
   test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "assert_false" )> test_action;

   auto trx = transaction();
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };

   trx.actions.emplace_back( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "assert_false")}, test_action );
   trx.send(0, name{receiver});
}

void test_transaction::assert_false_error_handler( const sysio::transaction& dtrx ) {
   sysio_assert( dtrx.actions.size() == 1, "transaction should only have one action" );
   sysio_assert( dtrx.actions[0].account == "testapi"_n, "transaction has wrong code" );
   sysio_assert( dtrx.actions[0].name.value == WASM_TEST_ACTION("test_action", "assert_false"), "transaction has wrong name" );
   sysio_assert( dtrx.actions[0].authorization.size() == 1, "action should only have one authorization" );
   sysio_assert( dtrx.actions[0].authorization[0].actor == "testapi"_n, "action's authorization has wrong actor" );
   sysio_assert( dtrx.actions[0].authorization[0].permission == "active"_n, "action's authorization has wrong permission" );
}

/**
 * cause failure due to a large transaction size
 */
void test_transaction::send_transaction_large( uint64_t receiver, uint64_t, uint64_t ) {
   using namespace sysio;
   auto trx = transaction();
   std::vector<permission_level> permissions = { {"testapi"_n, "active"_n} };
   for (int i = 0; i < 32; i ++) {
      char large_message[1024];
      test_action_action<"testapi"_n.value, WASM_TEST_ACTION( "test_action", "read_action_normal" )> test_action;
      copy_data( large_message, 1024, test_action.data );
      trx.actions.emplace_back( permissions, name{"testapi"}, name{WASM_TEST_ACTION("test_action", "read_action_normal")}, test_action );
   }

   trx.send( 0, name{receiver} );

   sysio_assert( false, "send_transaction_large() should've thrown an error" );
}

void test_transaction::send_cf_action() {
   using namespace sysio;
   action act( std::vector<permission_level>{}, "dummy"_n, "event1"_n, std::vector<char>{} );
   act.send_context_free();
}

void test_transaction::send_cf_action_fail() {
   using namespace sysio;
   action act( std::vector<permission_level>{{"dummy"_n, "active"_n}}, "dummy"_n, "event1"_n, std::vector<char>{} );
   act.send_context_free();
   sysio_assert( false, "send_cfa_action_fail() should've thrown an error" );
}

void test_transaction::stateful_api() {
   char buf[4] = {1};
   db_store_i64( sysio::name{"testtrans"}.value, sysio::name{"table"}.value, sysio::name{"testtrans"}.value, 0, buf, 4 );
}

void test_transaction::context_free_api() {
   char buf[128] = {0};
   sysio::get_context_free_data( 0, buf, sizeof(buf) );
}

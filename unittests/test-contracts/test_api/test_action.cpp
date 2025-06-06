#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/datastream.hpp>
#include <sysio/sysio.hpp>
#include <sysio/print.hpp>
#include <sysio/transaction.hpp>

#include "test_api.hpp"

using namespace sysio;

void test_action::read_action_normal() {

   char buffer[100];
   uint32_t total = 0;

   sysio_assert( action_data_size() == sizeof(dummy_action), "action_size() == sizeof(dummy_action)" );

   total = read_action_data( buffer, 30 );
   sysio_assert( total == sizeof(dummy_action) , "read_action(30)" );

   total = read_action_data( buffer, 100 );
   sysio_assert(total == sizeof(dummy_action) , "read_action(100)" );

   total = read_action_data(buffer, 5);
   sysio_assert( total == 5 , "read_action(5)" );

   total = read_action_data(buffer, sizeof(dummy_action) );
   sysio_assert( total == sizeof(dummy_action), "read_action(sizeof(dummy_action))" );

   dummy_action *dummy13 = reinterpret_cast<dummy_action *>(buffer);

   sysio_assert( dummy13->a == DUMMY_ACTION_DEFAULT_A, "dummy13->a == DUMMY_ACTION_DEFAULT_A" );
   sysio_assert( dummy13->b == DUMMY_ACTION_DEFAULT_B, "dummy13->b == DUMMY_ACTION_DEFAULT_B" );
   sysio_assert( dummy13->c == DUMMY_ACTION_DEFAULT_C, "dummy13->c == DUMMY_ACTION_DEFAULT_C" );
}

void test_action::test_dummy_action() {
   char buffer[100];
   int total = 0;

   // get_action
   total = get_action( 1, 0, buffer, 0 );
   total = get_action( 1, 0, buffer, static_cast<size_t>(total) );
   sysio_assert( total > 0, "get_action failed" );
   sysio::action act = sysio::get_action( 1, 0 );
   sysio_assert( act.authorization.back().actor == "testapi"_n, "incorrect permission actor" );
   sysio_assert( act.authorization.back().permission == "active"_n, "incorrect permission name" );
   sysio_assert( sysio::pack_size(act) == static_cast<size_t>(total), "pack_size does not match get_action size" );
   sysio_assert( act.account == "testapi"_n, "expected testapi account" );

   dummy_action dum13 = act.data_as<dummy_action>();

   if ( dum13.b == 200 ) {
      // attempt to access context free only api
      sysio::get_context_free_data( 0, nullptr, 0 );
      sysio_assert( false, "get_context_free_data() not allowed in non-context free action" );
   } else {
      sysio_assert( dum13.a == DUMMY_ACTION_DEFAULT_A, "dum13.a == DUMMY_ACTION_DEFAULT_A" );
      sysio_assert( dum13.b == DUMMY_ACTION_DEFAULT_B, "dum13.b == DUMMY_ACTION_DEFAULT_B" );
      sysio_assert( dum13.c == DUMMY_ACTION_DEFAULT_C, "dum13.c == DUMMY_ACTION_DEFAULT_C" );
   }
}

void test_action::read_action() {
   print("action size: " + std::to_string(action_data_size()));
   void* p = malloc(action_data_size());
   read_action_data(p, action_data_size());
}

void test_action::read_action_to_0() {
   read_action_data( (void *)0, action_data_size() );
}

void test_action::read_action_to_64k() {
   read_action_data( (void *)((1<<16)-2), action_data_size());
}

void test_action::test_cf_action() {

   sysio::action act = sysio::get_action( 0, 0 );
   cf_action cfa = act.data_as<cf_action>();
   if ( cfa.payload == 100 ) {
      // verify read of get_context_free_data, also verifies system api access
      int size = sysio::get_context_free_data( cfa.cfd_idx, nullptr, 0 );
      sysio_assert( size > 0, "size determination failed" );
      std::vector<char> cfd( static_cast<size_t>(size) );
      size = sysio::get_context_free_data( cfa.cfd_idx, &cfd[0], static_cast<size_t>(size) );
      sysio_assert(static_cast<size_t>(size) == cfd.size(), "get_context_free_data failed" );
      uint32_t v = sysio::unpack<uint32_t>( &cfd[0], cfd.size() );
      sysio_assert( v == cfa.payload, "invalid value" );

      // verify crypto api access
      char test[] = "test";
      auto hash = sha256( test, sizeof(test) );
      sysio::assert_sha256( test, sizeof(test), hash );
      // verify action api access
      action_data_size();
      // verify console api access
      sysio::print("test\n");
      // verify memory api access
      uint32_t i = 42;
      memccpy( &v, &i, sizeof(i), sizeof(i) );
      // verify transaction api access
      sysio_assert(sysio::transaction_size() > 0, "transaction_size failed");
      // verify softfloat api access
      float f1 = 1.0f, f2 = 2.0f;
      float f3 = f1 + f2;
      sysio_assert( f3 >  2.0f, "Unable to add float.");
      // verify context_free_system_api
      sysio_assert( true, "verify sysio_assert can be called" );


   } else if ( cfa.payload == 200 ) {
      // attempt to access non context free api, privileged_api
      is_privileged(act.name.value);
      sysio_assert( false, "privileged_api should not be allowed" );
   } else if ( cfa.payload == 201 ) {
      // attempt to access non context free api, producer_api
      get_active_producers( nullptr, 0 );
      sysio_assert( false, "producer_api should not be allowed" );
   } else if ( cfa.payload == 202 ) {
      // attempt to access non context free api, db_api
      db_store_i64( "testapi"_n.value, "testapi"_n.value, "testapi"_n.value, 0, "test", 4 );
      sysio_assert( false, "db_api should not be allowed" );
   } else if ( cfa.payload == 203 ) {
      // attempt to access non context free api, db_api
      uint64_t i = 0;
      db_idx64_store( "testapi"_n.value, "testapi"_n.value, "testapi"_n.value, 0, &i );
      sysio_assert( false, "db_api should not be allowed" );
   } else if ( cfa.payload == 204 ) {
      db_find_i64( "testapi"_n.value, "testapi"_n.value, "testapi"_n.value, 1 );
      sysio_assert( false, "db_api should not be allowed" );
   } else if ( cfa.payload == 205 ) {
      // attempt to access non context free api, send action
      sysio::action dum_act;
      dum_act.send();
      sysio_assert( false, "action send should not be allowed" );
   } else if ( cfa.payload == 206 ) {
      sysio::require_auth("test"_n);
      sysio_assert( false, "authorization_api should not be allowed" );
   } else if ( cfa.payload == 207 || cfa.payload == 208 ) {
      // 207 is obsolete as now() is removed from system.h
      current_time();
      sysio_assert( false, "system_api should not be allowed" );
   } else if ( cfa.payload == 209 ) {
      publication_time();
      sysio_assert( false, "system_api should not be allowed" );
   } else if ( cfa.payload == 210 ) {
      sysio::internal_use_do_not_use::send_inline( (char*)"hello", 6 );
      sysio_assert( false, "transaction_api should not be allowed" );
   } else if ( cfa.payload == 211 ) {
      sysio::send_deferred( "testapi"_n.value, "testapi"_n, "hello", 6, 0 );
      sysio_assert( false, "transaction_api should not be allowed" );
   } else if ( cfa.payload == 212 ) {
      set_action_return_value("hi", 2);
      sysio_assert( false, "set_action_return_value should not be allowed" );
   }

}

void test_action::require_notice( uint64_t receiver, uint64_t code, uint64_t action ) {
   (void)code;(void)action;
   if( receiver == "testapi"_n.value ) {
      sysio::require_recipient( "acc1"_n );
      sysio::require_recipient( "acc2"_n );
      sysio::require_recipient( "acc1"_n, "acc2"_n );
      sysio_assert( false, "Should've failed" );
   } else if ( receiver == "acc1"_n.value || receiver == "acc2"_n.value ) {
      return;
   }
   sysio_assert( false, "Should've failed" );
}

void test_action::require_notice_tests( uint64_t receiver, uint64_t code, uint64_t action ) {
   sysio::print( "require_notice_tests" );
   if( receiver == "testapi"_n.value ) {
      sysio::print("require_recipient( \"acc5\"_n )");
      sysio::require_recipient("acc5"_n);
   } else if( receiver == "acc5"_n.value ) {
      sysio::print("require_recipient( \"testapi\"_n )");
      sysio::require_recipient("testapi"_n);
   }
}

void test_action::require_auth() {
   print("require_auth");
   sysio::require_auth("acc3"_n);
   sysio::require_auth("acc4"_n);
}

void test_action::assert_false() {
   sysio_assert( false, "test_action::assert_false" );
}

void test_action::assert_true() {
   sysio_assert( true, "test_action::assert_true" );
}

void test_action::assert_true_cf() {
   sysio_assert( true, "test_action::assert_true" );
}

void test_action::test_abort() {
   abort();
   sysio_assert( false, "should've aborted" );
}

void test_action::test_publication_time() {
   uint64_t pub_time = 0;
   uint32_t total = read_action_data( &pub_time, sizeof(uint64_t) );
   sysio_assert( total == sizeof(uint64_t), "total == sizeof(uint64_t)" );
   time_point msec{ microseconds{static_cast<int64_t>(pub_time)}};
   sysio_assert( msec == publication_time(), "pub_time == publication_time()" );
}

void test_action::test_current_receiver( uint64_t receiver, uint64_t code, uint64_t action ) {
   (void)code;(void)action;
   name cur_rec;
   read_action_data( &cur_rec, sizeof(name) );

   sysio_assert( receiver == cur_rec.value, "the current receiver does not match" );
}

void test_action::test_current_time() {
   uint64_t tmp = 0;
   uint32_t total = read_action_data( &tmp, sizeof(uint64_t) );
   sysio_assert( total == sizeof(uint64_t), "total == sizeof(uint64_t)" );
   sysio_assert( tmp == current_time(), "tmp == current_time()" );
}

void test_action::test_assert_code() {
   uint64_t code = 0;
   uint32_t total = read_action_data(&code, sizeof(uint64_t));
   sysio_assert( total == sizeof(uint64_t), "total == sizeof(uint64_t)" );
   sysio_assert_code( false, code );
}

void test_action::test_ram_billing_in_notify( uint64_t receiver, uint64_t code, uint64_t action ) {
   uint128_t tmp = 0;
   uint32_t total = sysio::read_action_data( &tmp, sizeof(uint128_t) );
   sysio_assert( total == sizeof(uint128_t), "total == sizeof(uint128_t)" );

   uint64_t to_notify = tmp >> 64;
   uint64_t payer = tmp & 0xFFFFFFFFFFFFFFFFULL;

   if( code == receiver ) {
      sysio::require_recipient( name{to_notify} );
   } else {
      sysio_assert( to_notify == receiver, "notified recipient other than the one specified in to_notify" );

      // Remove main table row if it already exists.
      int itr = db_find_i64( receiver, "notifytest"_n.value, "notifytest"_n.value, "notifytest"_n.value );
      if( itr >= 0 )
         db_remove_i64( itr );

      // Create the main table row simply for the purpose of charging code more RAM.
      if( payer != 0 )
         db_store_i64( "notifytest"_n.value, "notifytest"_n.value, payer, "notifytest"_n.value, &to_notify, sizeof(to_notify) );
   }
}

void test_action::test_action_ordinal1(uint64_t receiver, uint64_t code, uint64_t action) {
   uint64_t _self = receiver;
   if (receiver == "testapi"_n.value) {
      print("exec 1");
      sysio::require_recipient( "bob"_n ); //-> exec 2 which would then cause execution of 4, 10

      sysio::action act1({name(_self), "active"_n}, name(_self),
                         name(WASM_TEST_ACTION("test_action", "test_action_ordinal2")),
                         std::tuple<>());
      act1.send(); // -> exec 5 which would then cause execution of 6, 7, 8

      if (is_account("fail1"_n)) {
         sysio_assert(false, "fail at point 1");
      }

      sysio::action act2({name(_self), "active"_n}, name(_self),
                         name(WASM_TEST_ACTION("test_action", "test_action_ordinal3")),
                         std::tuple<>());
      act2.send(); // -> exec 9

      set_action_return_value( &sysio::pack(unsigned_int(1))[0], sysio::pack_size(unsigned_int(1)) );
      sysio::require_recipient( "charlie"_n ); // -> exec 3 which would then cause execution of 11

   } else if (receiver == "bob"_n.value) {
      print("exec 2");
      sysio::action act1({name(_self), "active"_n}, name(_self),
                         name(WASM_TEST_ACTION("test_action", "test_action_ordinal_foo")),
                         std::tuple<>());
      act1.send(); // -> exec 10

      set_action_return_value( &sysio::pack(std::string("bob"))[0], sysio::pack_size(std::string("bob")) );
      sysio::require_recipient( "david"_n );  // -> exec 4
   } else if (receiver == "charlie"_n.value) {
      print("exec 3");
      sysio::action act1({name(_self), "active"_n}, name(_self),
                         name(WASM_TEST_ACTION("test_action", "test_action_ordinal_bar")),
                         std::tuple<>()); // exec 11
      act1.send();

      set_action_return_value( &sysio::pack(std::string("charlie"))[0], sysio::pack_size(std::string("charlie")) );
      if (is_account("fail3"_n)) {
         sysio_assert(false, "fail at point 3");
      }

   } else if (receiver == "david"_n.value) {
      print("exec 4");
      set_action_return_value( &sysio::pack(std::string("david"))[0], sysio::pack_size(std::string("david")) );
   } else {
      sysio_assert(false, "assert failed at test_action::test_action_ordinal1");
   }
}
void test_action::test_action_ordinal2(uint64_t receiver, uint64_t code, uint64_t action) {
   uint64_t _self = receiver;
   if (receiver == "testapi"_n.value) {
      print("exec 5");
      sysio::require_recipient( "david"_n ); // -> exec 6
      sysio::require_recipient( "erin"_n ); // -> exec 7

      sysio::action act1({name(_self), "active"_n}, name(_self),
                         name(WASM_TEST_ACTION("test_action", "test_action_ordinal4")),
                         std::tuple<>());
      act1.send(); // -> exec 8
      set_action_return_value( &sysio::pack("five"_n)[0], sysio::pack_size("five"_n) );
   } else if (receiver == "david"_n.value) {
      print("exec 6");
      set_action_return_value( &sysio::pack(true)[0], sysio::pack_size(true) );
   } else if (receiver == "erin"_n.value) {
      print("exec 7");
      set_action_return_value( &sysio::pack(signed_int(7))[0], sysio::pack_size(signed_int(7)) );
   } else {
      sysio_assert(false, "assert failed at test_action::test_action_ordinal2");
   }
}
void test_action::test_action_ordinal4(uint64_t receiver, uint64_t code, uint64_t action) {
   print("exec 8");
   // no set_action_return_value
}
void test_action::test_action_ordinal3(uint64_t receiver, uint64_t code, uint64_t action) {
   print("exec 9");

   if (is_account("failnine"_n)) {
      sysio_assert(false, "fail at point 9");
   }
   set_action_return_value( &sysio::pack(unsigned_int(9))[0], sysio::pack_size(unsigned_int(9)) );
}
void test_action::test_action_ordinal_foo(uint64_t receiver, uint64_t code, uint64_t action) {
   print("exec 10");
   set_action_return_value( &sysio::pack(13.23)[0], sysio::pack_size(13.23) );
}
void test_action::test_action_ordinal_bar(uint64_t receiver, uint64_t code, uint64_t action) {
   print("exec 11");
   set_action_return_value( &sysio::pack(11.42f)[0], sysio::pack_size(11.42f) );
}

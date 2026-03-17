#include "test_api_db.hpp"
#include <sysio/transaction.hpp>

using namespace sysio;

using namespace sysio::internal_use_do_not_use;

void test_api_db::primary_i64_general()
{
   uint64_t receiver = get_self().value;
   auto table1 = "table1"_n.value;

   int alice_itr = db_store_i64( receiver, table1, receiver, "alice"_n.value, "alice's info", strlen("alice's info") );
   db_store_i64( receiver, table1, receiver, "bob"_n.value, "bob's info", strlen("bob's info") );
   db_store_i64( receiver, table1, receiver, "charlie"_n.value, "charlie's info", strlen("charlies's info") );
   db_store_i64( receiver, table1, receiver, "allyson"_n.value, "allyson's info", strlen("allyson's info") );


   // find
   {
      uint64_t prim = 0;
      int itr_next = db_next_i64( alice_itr, &prim );
      int itr_next_expected = db_find_i64( receiver, receiver, table1, "allyson"_n.value );
      sysio_assert( itr_next == itr_next_expected && prim == "allyson"_n.value, "primary_i64_general - db_find_i64"  );
      itr_next = db_next_i64( itr_next, &prim );
      itr_next_expected = db_find_i64( receiver, receiver, table1, "bob"_n.value );
      sysio_assert( itr_next == itr_next_expected && prim == "bob"_n.value, "primary_i64_general - db_next_i64" );
   }

   // next
   {
      int charlie_itr = db_find_i64( receiver, receiver, table1, "charlie"_n.value );
      // nothing after charlie
      uint64_t prim = 0;
      int end_itr = db_next_i64( charlie_itr, &prim );
      sysio_assert( end_itr < 0, "primary_i64_general - db_next_i64" );
      // prim didn't change
      sysio_assert( prim == 0, "primary_i64_general - db_next_i64" );
   }

   // previous
   {
      int charlie_itr = db_find_i64( receiver, receiver, table1, "charlie"_n.value );
      uint64_t prim = 0;
      int itr_prev = db_previous_i64( charlie_itr, &prim );
      int itr_prev_expected = db_find_i64( receiver, receiver, table1, "bob"_n.value );
      sysio_assert( itr_prev == itr_prev_expected && prim == "bob"_n.value, "primary_i64_general - db_previous_i64" );

      itr_prev = db_previous_i64( itr_prev, &prim );
      itr_prev_expected = db_find_i64( receiver, receiver, table1, "allyson"_n.value );
      sysio_assert( itr_prev == itr_prev_expected && prim == "allyson"_n.value, "primary_i64_general - db_previous_i64" );

      itr_prev = db_previous_i64( itr_prev, &prim );
      itr_prev_expected = db_find_i64( receiver, receiver, table1, "alice"_n.value );
      sysio_assert( itr_prev == itr_prev_expected && prim == "alice"_n.value, "primary_i64_general - db_previous_i64" );

      itr_prev = db_previous_i64( itr_prev, &prim );
      sysio_assert( itr_prev < 0 && prim == "alice"_n.value, "primary_i64_general - db_previous_i64" );
   }

   // remove
   {
      int itr = db_find_i64( receiver, receiver, table1, "alice"_n.value );
      sysio_assert( itr >= 0, "primary_i64_general - db_find_i64" );
      db_remove_i64( itr );
      itr = db_find_i64( receiver, receiver, table1, "alice"_n.value );
      sysio_assert( itr < 0, "primary_i64_general - db_find_i64" );
   }

   // get
   {
      int itr = db_find_i64( receiver, receiver, table1, "bob"_n.value );
      sysio_assert( itr >= 0, "" );
      uint32_t buffer_len = 5;
      char value[50];
      auto len = db_get_i64( itr, value, buffer_len );
      value[buffer_len] = '\0';
      std::string s(value);
      sysio_assert( uint32_t(len) == buffer_len, "primary_i64_general - db_get_i64" );
      sysio_assert( s == "bob's", "primary_i64_general - db_get_i64  - 5" );

      buffer_len = 20;
      len = db_get_i64( itr, value, 0 );
      len = db_get_i64( itr, value, (uint32_t)len );
      value[len] = '\0';
      std::string sfull(value);
      sysio_assert( sfull == "bob's info", "primary_i64_general - db_get_i64 - full" );
   }

   // update
   {
      int itr = db_find_i64( receiver, receiver, table1, "bob"_n.value );
      sysio_assert( itr >= 0, "" );
      const char* new_value = "bob's new info";
      uint32_t new_value_len = strlen(new_value);
      db_update_i64( itr, receiver, new_value, new_value_len );
      char ret_value[50];
      db_get_i64( itr, ret_value, new_value_len );
      ret_value[new_value_len] = '\0';
      std::string sret(ret_value);
      sysio_assert( sret == "bob's new info", "primary_i64_general - db_update_i64" );
   }
}

void test_api_db::primary_i64_lowerbound()
{
   uint64_t receiver = get_self().value;
   auto table = "mytable"_n.value;
   db_store_i64( receiver, table, receiver, "alice"_n.value, "alice's info", strlen("alice's info") );
   db_store_i64( receiver, table, receiver, "bob"_n.value, "bob's info", strlen("bob's info") );
   db_store_i64( receiver, table, receiver, "charlie"_n.value, "charlie's info", strlen("charlies's info") );
   db_store_i64( receiver, table, receiver, "emily"_n.value, "emily's info", strlen("emily's info") );
   db_store_i64( receiver, table, receiver, "allyson"_n.value, "allyson's info", strlen("allyson's info") );
   db_store_i64( receiver, table, receiver, "joe"_n.value, "nothing here", strlen("nothing here") );

   const std::string err = "primary_i64_lowerbound";

   {
      int lb = db_lowerbound_i64( receiver, receiver, table, "alice"_n.value );
      sysio_assert( lb == db_find_i64(receiver, receiver, table, "alice"_n.value), err.c_str() );
   }
   {
      int lb = db_lowerbound_i64( receiver, receiver, table, "billy"_n.value );
      sysio_assert( lb == db_find_i64(receiver, receiver, table, "bob"_n.value), err.c_str() );
   }
   {
      int lb = db_lowerbound_i64( receiver, receiver, table, "frank"_n.value );
      sysio_assert( lb == db_find_i64(receiver, receiver, table, "joe"_n.value), err.c_str() );
   }
   {
      int lb = db_lowerbound_i64( receiver, receiver, table, "joe"_n.value );
      sysio_assert( lb == db_find_i64(receiver, receiver, table, "joe"_n.value), err.c_str() );
   }
   {
      int lb = db_lowerbound_i64( receiver, receiver, table, "kevin"_n.value );
      sysio_assert( lb < 0, err.c_str() );
   }
}

void test_api_db::primary_i64_upperbound()
{
   uint64_t receiver = get_self().value;
   auto table = "mytable"_n.value;
   const std::string err = "primary_i64_upperbound";
   {
      int ub = db_upperbound_i64( receiver, receiver, table, "alice"_n.value );
      sysio_assert( ub == db_find_i64(receiver, receiver, table, "allyson"_n.value), err.c_str() );
   }
   {
      int ub = db_upperbound_i64( receiver, receiver, table, "billy"_n.value );
      sysio_assert( ub == db_find_i64(receiver, receiver, table, "bob"_n.value), err.c_str() );
   }
   {
      int ub = db_upperbound_i64( receiver, receiver, table, "frank"_n.value );
      sysio_assert( ub == db_find_i64(receiver, receiver, table, "joe"_n.value), err.c_str() );
   }
   {
      int ub = db_upperbound_i64( receiver, receiver, table, "joe"_n.value );
      sysio_assert( ub < 0, err.c_str() );
   }
   {
      int ub = db_upperbound_i64( receiver, receiver, table, "kevin"_n.value );
      sysio_assert( ub < 0, err.c_str() );
   }
}

void test_api_db::idx64_general()
{
   uint64_t receiver = get_self().value;
   const auto table = "myindextable"_n.value;

   typedef uint64_t secondary_type;

   struct record {
      uint64_t ssn;
      secondary_type name;
   };

   record records[] = { {265, "alice"_n.value},
                        {781, "bob"_n.value},
                        {234, "charlie"_n.value},
                        {650, "allyson"_n.value},
                        {540, "bob"_n.value},
                        {976, "emily"_n.value},
                        {110, "joe"_n.value} };

   for ( uint32_t i = 0; i < sizeof(records)/sizeof(records[0]); ++i ) {
      db_idx64_store( receiver, table, receiver, records[i].ssn, &records[i].name );
   }

   // find_primary
   {
      secondary_type sec = 0;
      int itr = db_idx64_find_primary( receiver, receiver, table, &sec, 999 );
      sysio_assert( itr < 0 && sec == 0, "idx64_general - db_idx64_find_primary" );
      itr = db_idx64_find_primary( receiver, receiver, table, &sec, 110 );
      sysio_assert( itr >= 0 && sec == "joe"_n.value, "idx64_general - db_idx64_find_primary" );
      uint64_t prim_next = 0;
      int itr_next = db_idx64_next( itr, &prim_next );
      sysio_assert( itr_next < 0 && prim_next == 0, "idx64_general - db_idx64_find_primary" );
   }

   // iterate forward starting with charlie
   {
      secondary_type sec = 0;
      int itr = db_idx64_find_primary( receiver, receiver, table, &sec, 234 );
      sysio_assert( itr >= 0 && sec == "charlie"_n.value, "idx64_general - db_idx64_find_primary" );

      uint64_t prim_next = 0;
      int itr_next = db_idx64_next( itr, &prim_next );
      sysio_assert( itr_next >= 0 && prim_next == 976, "idx64_general - db_idx64_next" );
      secondary_type sec_next = 0;
      int itr_next_expected = db_idx64_find_primary( receiver, receiver, table, &sec_next, prim_next );
      sysio_assert( itr_next == itr_next_expected && sec_next == "emily"_n.value, "idx64_general - db_idx64_next" );

      itr_next = db_idx64_next( itr_next, &prim_next );
      sysio_assert( itr_next >= 0 && prim_next == 110, "idx64_general - db_idx64_next" );
      itr_next_expected = db_idx64_find_primary( receiver, receiver, table, &sec_next, prim_next );
      sysio_assert( itr_next == itr_next_expected && sec_next == "joe"_n.value, "idx64_general - db_idx64_next" );

      itr_next = db_idx64_next( itr_next, &prim_next );
      sysio_assert( itr_next < 0 && prim_next == 110, "idx64_general - db_idx64_next" );
   }

   // iterate backward staring with second bob
   {
      secondary_type sec = 0;
      int itr = db_idx64_find_primary( receiver, receiver, table, &sec, 781 );
      sysio_assert( itr >= 0 && sec == "bob"_n.value, "idx64_general - db_idx64_find_primary" );

      uint64_t prim_prev = 0;
      int itr_prev = db_idx64_previous( itr, &prim_prev );
      sysio_assert( itr_prev >= 0 && prim_prev == 540, "idx64_general - db_idx64_previous" );

      secondary_type sec_prev = 0;
      int itr_prev_expected = db_idx64_find_primary( receiver, receiver, table, &sec_prev, prim_prev );
      sysio_assert( itr_prev == itr_prev_expected && sec_prev == "bob"_n.value, "idx64_general - db_idx64_previous" );

      itr_prev = db_idx64_previous( itr_prev, &prim_prev );
      sysio_assert( itr_prev >= 0 && prim_prev == 650, "idx64_general - db_idx64_previous" );
      itr_prev_expected = db_idx64_find_primary( receiver, receiver, table, &sec_prev, prim_prev );
      sysio_assert( itr_prev == itr_prev_expected && sec_prev == "allyson"_n.value, "idx64_general - db_idx64_previous" );

      itr_prev = db_idx64_previous( itr_prev, &prim_prev );
      sysio_assert( itr_prev >= 0 && prim_prev == 265, "idx64_general - db_idx64_previous" );
      itr_prev_expected = db_idx64_find_primary( receiver, receiver, table, &sec_prev, prim_prev );
      sysio_assert( itr_prev == itr_prev_expected && sec_prev == "alice"_n.value, "idx64_general - db_idx64_previous" );

      itr_prev = db_idx64_previous( itr_prev, &prim_prev );
      sysio_assert( itr_prev < 0 && prim_prev == 265, "idx64_general - db_idx64_previous" );
   }

   // find_secondary
   {
      uint64_t prim = 0;
      auto sec = "bob"_n.value;
      int itr = db_idx64_find_secondary( receiver, receiver, table, &sec, &prim );
      sysio_assert( itr >= 0 && prim == 540, "idx64_general - db_idx64_find_secondary" );

      sec = "emily"_n.value;
      itr = db_idx64_find_secondary( receiver, receiver, table, &sec, &prim );
      sysio_assert( itr >= 0 && prim == 976, "idx64_general - db_idx64_find_secondary" );

      sec = "frank"_n.value;
      itr = db_idx64_find_secondary( receiver, receiver, table, &sec, &prim );
      sysio_assert( itr < 0 && prim == 976, "idx64_general - db_idx64_find_secondary" );
   }

   // update and remove
   {
      uint64_t one_more_bob = "bob"_n.value;
      const uint64_t ssn = 421;
      int itr = db_idx64_store( receiver, table, receiver, ssn, &one_more_bob );
      uint64_t new_name = "billy"_n.value;
      db_idx64_update( itr, receiver, &new_name );
      secondary_type sec = 0;
      int sec_itr = db_idx64_find_primary( receiver, receiver, table, &sec, ssn );
      sysio_assert( sec_itr == itr && sec == new_name, "idx64_general - db_idx64_update" );
      db_idx64_remove(itr);
      int itrf = db_idx64_find_primary( receiver, receiver, table, &sec, ssn );
      sysio_assert( itrf < 0, "idx64_general - db_idx64_remove" );
   }
}

void test_api_db::idx64_lowerbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "myindextable"_n.value;
   typedef uint64_t secondary_type;
   const std::string err = "idx64_lowerbound";
   {
      secondary_type lb_sec = "alice"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 265;
      int lb = db_idx64_lowerbound( receiver, receiver, table, &lb_sec, &lb_prim );
      sysio_assert( lb_prim == ssn && lb_sec == "alice"_n.value, err.c_str() );
      sysio_assert( lb == db_idx64_find_primary(receiver, receiver, table, &lb_sec, ssn), err.c_str() );
   }
   {
      secondary_type lb_sec = "billy"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 540;
      int lb = db_idx64_lowerbound( receiver, receiver, table, &lb_sec, &lb_prim );
      sysio_assert( lb_prim == ssn && lb_sec == "bob"_n.value, err.c_str() );
      sysio_assert( lb == db_idx64_find_primary(receiver, receiver, table, &lb_sec, ssn), err.c_str() );
   }
   {
      secondary_type lb_sec = "joe"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 110;
      int lb = db_idx64_lowerbound( receiver, receiver, table, &lb_sec, &lb_prim );
      sysio_assert( lb_prim == ssn && lb_sec == "joe"_n.value, err.c_str() );
      sysio_assert( lb == db_idx64_find_primary(receiver, receiver, table, &lb_sec, ssn), err.c_str() );
   }
   {
      secondary_type lb_sec = "kevin"_n.value;
      uint64_t lb_prim = 0;
      int lb = db_idx64_lowerbound( receiver, receiver, table, &lb_sec, &lb_prim );
      sysio_assert( lb_prim == 0 && lb_sec == "kevin"_n.value, err.c_str() );
      sysio_assert( lb < 0, "" );
   }
   // Test write order
   { // aligned
      size_t prim_off = 0;
      size_t sec_off = 0;
      secondary_type lb_sec = "alice"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 265;
      char buf[16];
      secondary_type* lb_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* lb_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(lb_sec_ptr, &lb_sec, sizeof(lb_sec));
      int lb = db_idx64_lowerbound( receiver, receiver, table, lb_sec_ptr, lb_prim_ptr );
      memcpy(&lb_sec, lb_sec_ptr, sizeof(lb_sec));
      memcpy(&lb_prim, lb_prim_ptr, sizeof(lb_prim));
      sysio_assert( lb_prim != ssn && lb_sec == "alice"_n.value, err.c_str() );
   }
   { // unaligned
      size_t prim_off = 4;
      size_t sec_off = 4;
      secondary_type lb_sec = "alice"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 265;
      char buf[16];
      secondary_type* lb_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* lb_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(lb_sec_ptr, &lb_sec, sizeof(lb_sec));
      int lb = db_idx64_lowerbound( receiver, receiver, table, lb_sec_ptr, lb_prim_ptr );
      memcpy(&lb_sec, lb_sec_ptr, sizeof(lb_sec));
      memcpy(&lb_prim, lb_prim_ptr, sizeof(lb_prim));
      sysio_assert( lb_prim == ssn && lb_sec != "alice"_n.value, err.c_str() );
   }
   { // primary aligned, secondary unaligned
      size_t prim_off = 0;
      size_t sec_off = 1;
      secondary_type lb_sec = "alice"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 265;
      char buf[16];
      secondary_type* lb_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* lb_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(lb_sec_ptr, &lb_sec, sizeof(lb_sec));
      int lb = db_idx64_lowerbound( receiver, receiver, table, lb_sec_ptr, lb_prim_ptr );
      memcpy(&lb_sec, lb_sec_ptr, sizeof(lb_sec));
      memcpy(&lb_prim, lb_prim_ptr, sizeof(lb_prim));
      sysio_assert( lb_prim != ssn && lb_sec == "alice"_n.value, err.c_str() );
   }
   { // primary unaligned, secondary aligned
      size_t prim_off = 1;
      size_t sec_off = 0;
      secondary_type lb_sec = "alice"_n.value;
      uint64_t lb_prim = 0;
      const uint64_t ssn = 265;
      char buf[16];
      secondary_type* lb_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* lb_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(lb_sec_ptr, &lb_sec, sizeof(lb_sec));
      int lb = db_idx64_lowerbound( receiver, receiver, table, lb_sec_ptr, lb_prim_ptr );
      memcpy(&lb_sec, lb_sec_ptr, sizeof(lb_sec));
      memcpy(&lb_prim, lb_prim_ptr, sizeof(lb_prim));
      sysio_assert( lb_prim == ssn && lb_sec != "alice"_n.value, err.c_str() );
   }
}

void test_api_db::idx64_upperbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "myindextable"_n.value;
   typedef uint64_t secondary_type;
   const std::string err = "idx64_upperbound";
   {
      secondary_type ub_sec = "alice"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t allyson_ssn = 650;
      int ub = db_idx64_upperbound( receiver, receiver, table, &ub_sec, &ub_prim );
      sysio_assert( ub_prim == allyson_ssn && ub_sec == "allyson"_n.value, "" );
      sysio_assert( ub == db_idx64_find_primary(receiver, receiver, table, &ub_sec, allyson_ssn), err.c_str() );
   }
   {
      secondary_type ub_sec = "billy"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t bob_ssn = 540;
      int ub = db_idx64_upperbound( receiver, receiver, table, &ub_sec, &ub_prim );
      sysio_assert( ub_prim == bob_ssn && ub_sec == "bob"_n.value, "" );
      sysio_assert( ub == db_idx64_find_primary(receiver, receiver, table, &ub_sec, bob_ssn), err.c_str() );
   }
   {
      secondary_type ub_sec = "joe"_n.value;
      uint64_t ub_prim = 0;
      int ub = db_idx64_upperbound( receiver, receiver, table, &ub_sec, &ub_prim );
      sysio_assert( ub_prim == 0 && ub_sec == "joe"_n.value, err.c_str() );
      sysio_assert( ub < 0, err.c_str() );
   }
   {
      secondary_type ub_sec = "kevin"_n.value;
      uint64_t ub_prim = 0;
      int ub = db_idx64_upperbound( receiver, receiver, table, &ub_sec, &ub_prim );
      sysio_assert( ub_prim == 0 && ub_sec == "kevin"_n.value, err.c_str() );
      sysio_assert( ub < 0, err.c_str() );
   }
   { // aligned
      size_t prim_off = 0;
      size_t sec_off = 0;
      secondary_type ub_sec = "alice"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t allyson_ssn = 650;
      char buf[16];
      secondary_type* ub_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* ub_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(ub_sec_ptr, &ub_sec, sizeof(ub_sec));
      int ub = db_idx64_upperbound( receiver, receiver, table, ub_sec_ptr, ub_prim_ptr );
      memcpy(&ub_sec, ub_sec_ptr, sizeof(ub_sec));
      memcpy(&ub_prim, ub_prim_ptr, sizeof(ub_prim));
      sysio_assert( ub_prim != allyson_ssn && ub_sec == "allyson"_n.value, err.c_str() );
   }
   { // unaligned
      size_t prim_off = 4;
      size_t sec_off = 4;
      secondary_type ub_sec = "alice"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t allyson_ssn = 650;
      char buf[16];
      secondary_type* ub_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* ub_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(ub_sec_ptr, &ub_sec, sizeof(ub_sec));
      int ub = db_idx64_upperbound( receiver, receiver, table, ub_sec_ptr, ub_prim_ptr );
      memcpy(&ub_sec, ub_sec_ptr, sizeof(ub_sec));
      memcpy(&ub_prim, ub_prim_ptr, sizeof(ub_prim));
      sysio_assert( ub_prim == allyson_ssn && ub_sec != "allyson"_n.value, err.c_str() );
   }
   { // primary aligned, secondary unaligned
      size_t prim_off = 0;
      size_t sec_off = 1;
      secondary_type ub_sec = "alice"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t allyson_ssn = 650;
      char buf[16];
      secondary_type* ub_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* ub_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(ub_sec_ptr, &ub_sec, sizeof(ub_sec));
      int ub = db_idx64_upperbound( receiver, receiver, table, ub_sec_ptr, ub_prim_ptr );
      memcpy(&ub_sec, ub_sec_ptr, sizeof(ub_sec));
      memcpy(&ub_prim, ub_prim_ptr, sizeof(ub_prim));
      sysio_assert( ub_prim != allyson_ssn && ub_sec == "allyson"_n.value, err.c_str() );
   }
   { // primary unaligned, secondary aligned
      size_t prim_off = 1;
      size_t sec_off = 0;
      secondary_type ub_sec = "alice"_n.value;
      uint64_t ub_prim = 0;
      const uint64_t allyson_ssn = 650;
      char buf[16];
      secondary_type* ub_sec_ptr = reinterpret_cast<secondary_type*>(buf + sec_off);
      uint64_t* ub_prim_ptr = reinterpret_cast<uint64_t*>(buf + prim_off);
      memcpy(ub_sec_ptr, &ub_sec, sizeof(ub_sec));
      int ub = db_idx64_upperbound( receiver, receiver, table, ub_sec_ptr, ub_prim_ptr );
      memcpy(&ub_sec, ub_sec_ptr, sizeof(ub_sec));
      memcpy(&ub_prim, ub_prim_ptr, sizeof(ub_prim));
      sysio_assert( ub_prim == allyson_ssn && ub_sec != "allyson"_n.value, err.c_str() );
   }
}

void test_api_db::test_invalid_access( name _code, uint64_t val, uint32_t index, bool store )
{
   uint64_t code = _code.value;
   uint64_t receiver = get_self().value;
   uint64_t scope = "access"_n.value;
   uint64_t table = scope;
   uint64_t pk    = scope;

   int32_t itr = -1;
   uint64_t value = 0;
   switch( index ) {
      case 1:
         itr = db_idx64_find_primary( code, scope, table, &value, pk );
      break;
      case 0:
      default:
         itr = db_find_i64( code, scope, table, pk );
      break;
   }
   if(store) {
      uint64_t value_to_store = val;
      if( itr < 0 ) {
         switch(index) {
            case 1:
               db_idx64_store( scope, table, receiver, pk, &value_to_store );
            break;
            case 0:
            default:
               db_store_i64( scope, table, receiver, pk, &value_to_store, sizeof(value_to_store) );
            break;
         }
      } else {
         switch(index) {
            case 1:
               db_idx64_update( itr, receiver, &value_to_store);
            break;
            case 0:
            default:
               db_update_i64( itr, receiver, &value_to_store, sizeof(value_to_store) );
            break;
         }
      }
      //sysio::print("test_invalid_access: stored ", value_to_store, "\n");
   } else {
      sysio_assert( itr >= 0, "test_invalid_access: could not find row" );
      switch(index) {
         case 1:
         break;
         case 0:
         default:
            sysio_assert( db_get_i64( itr, &value, sizeof(value) ) == sizeof(value),
                          "test_invalid_access: value in primary table was incorrect size" );
         break;
      }
      //sysio::print("test_invalid_access: expected ", val, " and retrieved ", value, "\n");
      sysio_assert( value == val, "test_invalid_access: value did not match" );
   }
}

void test_api_db::idx_double_nan_create_fail() {
   uint64_t receiver = get_self().value;
   double x = 0.0;
   x = x / x; // create a NaN
   db_idx_double_store( "nan"_n.value, "nan"_n.value, receiver, 0, &x ); // should fail
}

void test_api_db::idx_double_nan_modify_fail() {
   uint64_t receiver = get_self().value;
   double x = 0.0;
   db_idx_double_store( "nan"_n.value, "nan"_n.value, receiver, 0, &x );
   auto itr = db_idx_double_find_primary( receiver, "nan"_n.value, "nan"_n.value, &x, 0 );
   x = 0.0;
   x = x / x; // create a NaN
   db_idx_double_update( itr, 0, &x ); // should fail
}

void test_api_db::idx_double_nan_lookup_fail( uint32_t lookup_type ) {
   uint64_t receiver = get_self().value;

   uint64_t pk;
   double x = 0.0;
   db_idx_double_store( "nan"_n.value, "nan"_n.value, receiver, 0, &x );
   x = x / x; // create a NaN
   switch(lookup_type) {
      case 0: // find
         db_idx_double_find_secondary( receiver, "nan"_n.value, "nan"_n.value, &x, &pk );
      break;
      case 1: // lower bound
         db_idx_double_lowerbound( receiver, "nan"_n.value, "nan"_n.value, &x, &pk );
      break;
      case 2: // upper bound
         db_idx_double_upperbound( receiver, "nan"_n.value, "nan"_n.value, &x, &pk );
      break;
      default:
         sysio_assert( false, "idx_double_nan_lookup_fail: unexpected lookup_type" );
   }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"

void test_api_db::misaligned_secondary_key256_tests() {
   uint64_t receiver = get_self().value;
   auto key = sysio::checksum256::make_from_word_sequence<uint64_t>( 0ULL, 0ULL, 0ULL, 42ULL );
   char* ptr = (char*)(&key);
   ptr += 1;
   // test that store doesn't crash on unaligned data
   db_idx256_store( "testapi"_n.value, "testtable"_n.value, "testapi"_n.value, 1, (uint128_t*)(ptr), 2 );
   // test that find_primary doesn't crash on unaligned data
   db_idx256_find_primary( "testapi"_n.value, "testtable"_n.value, "testapi"_n.value, (uint128_t*)(ptr), 2,0 );
}

#pragma clang diagnostic pop

// ============================================================================
// idx128 comprehensive tests
// ============================================================================

void test_api_db::idx128_general()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxaatbl"_n.value;

   struct record { uint64_t id; uint128_t sec; };
   record records[] = {
      {100, (uint128_t)1000}, {200, (uint128_t)3000}, {300, (uint128_t)2000},
      {400, (uint128_t)500},  {500, (uint128_t)4000}
   };
   // Secondary order: 500(400), 1000(100), 2000(300), 3000(200), 4000(500)

   for (uint32_t i = 0; i < sizeof(records)/sizeof(records[0]); ++i)
      db_idx128_store(receiver, table, receiver, records[i].id, &records[i].sec);

   // find_primary
   {
      uint128_t sec = 0;
      int itr = db_idx128_find_primary(receiver, receiver, table, &sec, 100);
      sysio_assert(itr >= 0 && sec == (uint128_t)1000, "idx128_general - find_primary");
      itr = db_idx128_find_primary(receiver, receiver, table, &sec, 999);
      sysio_assert(itr < 0, "idx128_general - find_primary not found");
   }

   // iterate forward from smallest secondary (500 → id=400)
   {
      uint128_t sec = 0;
      int itr = db_idx128_find_primary(receiver, receiver, table, &sec, 400);
      sysio_assert(itr >= 0 && sec == (uint128_t)500, "idx128_general - find 400");

      uint64_t prim = 0;
      int next = db_idx128_next(itr, &prim);
      sysio_assert(next >= 0 && prim == 100, "idx128_general - next 1");
      next = db_idx128_next(next, &prim);
      sysio_assert(next >= 0 && prim == 300, "idx128_general - next 2");
      next = db_idx128_next(next, &prim);
      sysio_assert(next >= 0 && prim == 200, "idx128_general - next 3");
      next = db_idx128_next(next, &prim);
      sysio_assert(next >= 0 && prim == 500, "idx128_general - next 4");
      next = db_idx128_next(next, &prim);
      sysio_assert(next < 0, "idx128_general - next end");
   }

   // iterate backward from largest secondary (4000 → id=500)
   {
      uint128_t sec = 0;
      int itr = db_idx128_find_primary(receiver, receiver, table, &sec, 500);
      sysio_assert(itr >= 0 && sec == (uint128_t)4000, "idx128_general - find 500");

      uint64_t prim = 0;
      int prev = db_idx128_previous(itr, &prim);
      sysio_assert(prev >= 0 && prim == 200, "idx128_general - prev 1");
      prev = db_idx128_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 300, "idx128_general - prev 2");
      prev = db_idx128_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 100, "idx128_general - prev 3");
      prev = db_idx128_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 400, "idx128_general - prev 4");
      prev = db_idx128_previous(prev, &prim);
      sysio_assert(prev < 0, "idx128_general - prev end");
   }

   // find_secondary
   {
      uint128_t sec = (uint128_t)2000;
      uint64_t prim = 0;
      int itr = db_idx128_find_secondary(receiver, receiver, table, &sec, &prim);
      sysio_assert(itr >= 0 && prim == 300, "idx128_general - find_secondary");
      sec = (uint128_t)9999;
      itr = db_idx128_find_secondary(receiver, receiver, table, &sec, &prim);
      sysio_assert(itr < 0, "idx128_general - find_secondary not found");
   }

   // update
   {
      uint128_t sec = 0;
      int itr = db_idx128_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx128_general - update find");
      uint128_t new_sec = (uint128_t)9999;
      db_idx128_update(itr, receiver, &new_sec);
      sec = 0;
      itr = db_idx128_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(sec == (uint128_t)9999, "idx128_general - update verify");
   }

   // remove
   {
      uint128_t sec = 0;
      int itr = db_idx128_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx128_general - remove find");
      db_idx128_remove(itr);
      itr = db_idx128_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr < 0, "idx128_general - remove verify");
   }
}

void test_api_db::idx128_lowerbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxaatbl"_n.value;
   const char* err = "idx128_lowerbound";
   // Data from idx128_general (minus removed id=300): 500(400), 1000(100), 3000(200), 4000(500), 9999 removed
   {
      uint128_t sec = (uint128_t)500;
      uint64_t prim = 0;
      int lb = db_idx128_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 400, err);
   }
   {
      uint128_t sec = (uint128_t)750;
      uint64_t prim = 0;
      int lb = db_idx128_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 100, err); // next after 750 is 1000(100)
   }
   {
      uint128_t sec = (uint128_t)99999;
      uint64_t prim = 0;
      int lb = db_idx128_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb < 0, err);
   }
}

void test_api_db::idx128_upperbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxaatbl"_n.value;
   const char* err = "idx128_upperbound";
   {
      uint128_t sec = (uint128_t)500;
      uint64_t prim = 0;
      int ub = db_idx128_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub >= 0 && prim == 100, err); // strictly > 500 → 1000(100)
   }
   {
      uint128_t sec = (uint128_t)4000;
      uint64_t prim = 0;
      int ub = db_idx128_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub < 0, err); // nothing > 4000
   }
}

// ============================================================================
// idx256 comprehensive tests
// ============================================================================

void test_api_db::idx256_general()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxbbtbl"_n.value;

   // Use uint128_t[2] as checksum256 key; compare by first word then second
   struct key256 { uint128_t w[2]; };
   struct record { uint64_t id; key256 sec; };
   record records[] = {
      {100, {{(uint128_t)0, (uint128_t)1000}}},
      {200, {{(uint128_t)0, (uint128_t)3000}}},
      {300, {{(uint128_t)0, (uint128_t)2000}}},
      {400, {{(uint128_t)0, (uint128_t)500}}},
      {500, {{(uint128_t)0, (uint128_t)4000}}}
   };

   for (uint32_t i = 0; i < sizeof(records)/sizeof(records[0]); ++i)
      db_idx256_store(receiver, table, receiver, records[i].id, records[i].sec.w, 2);

   // find_primary
   {
      key256 sec = {{0,0}};
      int itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 100);
      sysio_assert(itr >= 0 && sec.w[1] == (uint128_t)1000, "idx256_general - find_primary");
      itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 999);
      sysio_assert(itr < 0, "idx256_general - find_primary not found");
   }

   // iterate forward from smallest (500 → id=400)
   {
      key256 sec = {{0,0}};
      int itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 400);
      sysio_assert(itr >= 0, "idx256_general - find 400");

      uint64_t prim = 0;
      int next = db_idx256_next(itr, &prim);
      sysio_assert(next >= 0 && prim == 100, "idx256_general - next 1");
      next = db_idx256_next(next, &prim);
      sysio_assert(next >= 0 && prim == 300, "idx256_general - next 2");
      next = db_idx256_next(next, &prim);
      sysio_assert(next >= 0 && prim == 200, "idx256_general - next 3");
      next = db_idx256_next(next, &prim);
      sysio_assert(next >= 0 && prim == 500, "idx256_general - next 4");
      next = db_idx256_next(next, &prim);
      sysio_assert(next < 0, "idx256_general - next end");
   }

   // iterate backward from largest (4000 → id=500)
   {
      key256 sec = {{0,0}};
      int itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 500);
      sysio_assert(itr >= 0, "idx256_general - find 500");

      uint64_t prim = 0;
      int prev = db_idx256_previous(itr, &prim);
      sysio_assert(prev >= 0 && prim == 200, "idx256_general - prev 1");
      prev = db_idx256_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 300, "idx256_general - prev 2");
      prev = db_idx256_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 100, "idx256_general - prev 3");
      prev = db_idx256_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 400, "idx256_general - prev 4");
      prev = db_idx256_previous(prev, &prim);
      sysio_assert(prev < 0, "idx256_general - prev end");
   }

   // find_secondary
   {
      key256 sec = {{(uint128_t)0, (uint128_t)2000}};
      uint64_t prim = 0;
      int itr = db_idx256_find_secondary(receiver, receiver, table, sec.w, 2, &prim);
      sysio_assert(itr >= 0 && prim == 300, "idx256_general - find_secondary");
   }

   // update
   {
      key256 sec = {{0,0}};
      int itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 300);
      sysio_assert(itr >= 0, "idx256_general - update find");
      key256 new_sec = {{(uint128_t)0, (uint128_t)9999}};
      db_idx256_update(itr, receiver, new_sec.w, 2);
      sec = {{0,0}};
      itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 300);
      sysio_assert(sec.w[1] == (uint128_t)9999, "idx256_general - update verify");
   }

   // remove
   {
      key256 sec = {{0,0}};
      int itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 300);
      sysio_assert(itr >= 0, "idx256_general - remove find");
      db_idx256_remove(itr);
      itr = db_idx256_find_primary(receiver, receiver, table, sec.w, 2, 300);
      sysio_assert(itr < 0, "idx256_general - remove verify");
   }
}

void test_api_db::idx256_lowerbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxbbtbl"_n.value;
   const char* err = "idx256_lowerbound";
   {
      uint128_t sec[2] = {(uint128_t)0, (uint128_t)500};
      uint64_t prim = 0;
      int lb = db_idx256_lowerbound(receiver, receiver, table, sec, 2, &prim);
      sysio_assert(lb >= 0 && prim == 400, err);
   }
   {
      uint128_t sec[2] = {(uint128_t)0, (uint128_t)750};
      uint64_t prim = 0;
      int lb = db_idx256_lowerbound(receiver, receiver, table, sec, 2, &prim);
      sysio_assert(lb >= 0 && prim == 100, err);
   }
   {
      uint128_t sec[2] = {(uint128_t)0, (uint128_t)99999};
      uint64_t prim = 0;
      int lb = db_idx256_lowerbound(receiver, receiver, table, sec, 2, &prim);
      sysio_assert(lb < 0, err);
   }
}

void test_api_db::idx256_upperbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxbbtbl"_n.value;
   const char* err = "idx256_upperbound";
   {
      uint128_t sec[2] = {(uint128_t)0, (uint128_t)500};
      uint64_t prim = 0;
      int ub = db_idx256_upperbound(receiver, receiver, table, sec, 2, &prim);
      sysio_assert(ub >= 0 && prim == 100, err);
   }
   {
      uint128_t sec[2] = {(uint128_t)0, (uint128_t)4000};
      uint64_t prim = 0;
      int ub = db_idx256_upperbound(receiver, receiver, table, sec, 2, &prim);
      sysio_assert(ub < 0, err);
   }
}

// ============================================================================
// idx_double comprehensive tests
// ============================================================================

void test_api_db::idx_double_general()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxdbltbl"_n.value;

   struct record { uint64_t id; double sec; };
   record records[] = {
      {100, 1.5}, {200, 3.5}, {300, 2.5}, {400, 0.5}, {500, 4.5}
   };
   // Secondary order: 0.5(400), 1.5(100), 2.5(300), 3.5(200), 4.5(500)

   for (uint32_t i = 0; i < sizeof(records)/sizeof(records[0]); ++i)
      db_idx_double_store(receiver, table, receiver, records[i].id, &records[i].sec);

   // find_primary
   {
      double sec = 0.0;
      int itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 100);
      sysio_assert(itr >= 0 && sec == 1.5, "idx_double_general - find_primary");
      itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 999);
      sysio_assert(itr < 0, "idx_double_general - find_primary not found");
   }

   // iterate forward from smallest (0.5 → id=400)
   {
      double sec = 0.0;
      int itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 400);
      sysio_assert(itr >= 0 && sec == 0.5, "idx_double_general - find 400");

      uint64_t prim = 0;
      int next = db_idx_double_next(itr, &prim);
      sysio_assert(next >= 0 && prim == 100, "idx_double_general - next 1");
      next = db_idx_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 300, "idx_double_general - next 2");
      next = db_idx_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 200, "idx_double_general - next 3");
      next = db_idx_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 500, "idx_double_general - next 4");
      next = db_idx_double_next(next, &prim);
      sysio_assert(next < 0, "idx_double_general - next end");
   }

   // iterate backward from largest (4.5 → id=500)
   {
      double sec = 0.0;
      int itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 500);

      uint64_t prim = 0;
      int prev = db_idx_double_previous(itr, &prim);
      sysio_assert(prev >= 0 && prim == 200, "idx_double_general - prev 1");
      prev = db_idx_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 300, "idx_double_general - prev 2");
      prev = db_idx_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 100, "idx_double_general - prev 3");
      prev = db_idx_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 400, "idx_double_general - prev 4");
      prev = db_idx_double_previous(prev, &prim);
      sysio_assert(prev < 0, "idx_double_general - prev end");
   }

   // find_secondary
   {
      double sec = 2.5;
      uint64_t prim = 0;
      int itr = db_idx_double_find_secondary(receiver, receiver, table, &sec, &prim);
      sysio_assert(itr >= 0 && prim == 300, "idx_double_general - find_secondary");
   }

   // update
   {
      double sec = 0.0;
      int itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx_double_general - update find");
      double new_sec = 9.9;
      db_idx_double_update(itr, receiver, &new_sec);
      sec = 0.0;
      itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(sec == 9.9, "idx_double_general - update verify");
   }

   // remove
   {
      double sec = 0.0;
      int itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx_double_general - remove find");
      db_idx_double_remove(itr);
      itr = db_idx_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr < 0, "idx_double_general - remove verify");
   }
}

void test_api_db::idx_double_lowerbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxdbltbl"_n.value;
   const char* err = "idx_double_lowerbound";
   {
      double sec = 0.5;
      uint64_t prim = 0;
      int lb = db_idx_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 400, err);
   }
   {
      double sec = 0.75;
      uint64_t prim = 0;
      int lb = db_idx_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 100, err);
   }
   {
      double sec = 99.9;
      uint64_t prim = 0;
      int lb = db_idx_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb < 0, err);
   }
}

void test_api_db::idx_double_upperbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxdbltbl"_n.value;
   const char* err = "idx_double_upperbound";
   {
      double sec = 0.5;
      uint64_t prim = 0;
      int ub = db_idx_double_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub >= 0 && prim == 100, err);
   }
   {
      double sec = 4.5;
      uint64_t prim = 0;
      int ub = db_idx_double_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub < 0, err);
   }
}

// ============================================================================
// idx_long_double comprehensive tests
// ============================================================================

void test_api_db::idx_long_double_general()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxldtbl"_n.value;

   struct record { uint64_t id; long double sec; };
   record records[] = {
      {100, 1.5L}, {200, 3.5L}, {300, 2.5L}, {400, 0.5L}, {500, 4.5L}
   };

   for (uint32_t i = 0; i < sizeof(records)/sizeof(records[0]); ++i)
      db_idx_long_double_store(receiver, table, receiver, records[i].id, &records[i].sec);

   // find_primary
   {
      long double sec = 0.0L;
      int itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 100);
      sysio_assert(itr >= 0 && sec == 1.5L, "idx_long_double_general - find_primary");
      itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 999);
      sysio_assert(itr < 0, "idx_long_double_general - find_primary not found");
   }

   // iterate forward
   {
      long double sec = 0.0L;
      int itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 400);
      sysio_assert(itr >= 0, "idx_long_double_general - find 400");

      uint64_t prim = 0;
      int next = db_idx_long_double_next(itr, &prim);
      sysio_assert(next >= 0 && prim == 100, "idx_long_double_general - next 1");
      next = db_idx_long_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 300, "idx_long_double_general - next 2");
      next = db_idx_long_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 200, "idx_long_double_general - next 3");
      next = db_idx_long_double_next(next, &prim);
      sysio_assert(next >= 0 && prim == 500, "idx_long_double_general - next 4");
      next = db_idx_long_double_next(next, &prim);
      sysio_assert(next < 0, "idx_long_double_general - next end");
   }

   // iterate backward
   {
      long double sec = 0.0L;
      int itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 500);

      uint64_t prim = 0;
      int prev = db_idx_long_double_previous(itr, &prim);
      sysio_assert(prev >= 0 && prim == 200, "idx_long_double_general - prev 1");
      prev = db_idx_long_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 300, "idx_long_double_general - prev 2");
      prev = db_idx_long_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 100, "idx_long_double_general - prev 3");
      prev = db_idx_long_double_previous(prev, &prim);
      sysio_assert(prev >= 0 && prim == 400, "idx_long_double_general - prev 4");
      prev = db_idx_long_double_previous(prev, &prim);
      sysio_assert(prev < 0, "idx_long_double_general - prev end");
   }

   // find_secondary
   {
      long double sec = 2.5L;
      uint64_t prim = 0;
      int itr = db_idx_long_double_find_secondary(receiver, receiver, table, &sec, &prim);
      sysio_assert(itr >= 0 && prim == 300, "idx_long_double_general - find_secondary");
   }

   // update
   {
      long double sec = 0.0L;
      int itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx_long_double_general - update find");
      long double new_sec = 9.9L;
      db_idx_long_double_update(itr, receiver, &new_sec);
      sec = 0.0L;
      itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(sec == 9.9L, "idx_long_double_general - update verify");
   }

   // remove
   {
      long double sec = 0.0L;
      int itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr >= 0, "idx_long_double_general - remove find");
      db_idx_long_double_remove(itr);
      itr = db_idx_long_double_find_primary(receiver, receiver, table, &sec, 300);
      sysio_assert(itr < 0, "idx_long_double_general - remove verify");
   }
}

void test_api_db::idx_long_double_lowerbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxldtbl"_n.value;
   const char* err = "idx_long_double_lowerbound";
   {
      long double sec = 0.5L;
      uint64_t prim = 0;
      int lb = db_idx_long_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 400, err);
   }
   {
      long double sec = 0.75L;
      uint64_t prim = 0;
      int lb = db_idx_long_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb >= 0 && prim == 100, err);
   }
   {
      long double sec = 99.9L;
      uint64_t prim = 0;
      int lb = db_idx_long_double_lowerbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(lb < 0, err);
   }
}

void test_api_db::idx_long_double_upperbound()
{
   uint64_t receiver = get_self().value;
   const auto table = "idxldtbl"_n.value;
   const char* err = "idx_long_double_upperbound";
   {
      long double sec = 0.5L;
      uint64_t prim = 0;
      int ub = db_idx_long_double_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub >= 0 && prim == 100, err);
   }
   {
      long double sec = 4.5L;
      uint64_t prim = 0;
      int ub = db_idx_long_double_upperbound(receiver, receiver, table, &sec, &prim);
      sysio_assert(ub < 0, err);
   }
}

// ============================================================================
// Action I/O tests
// ============================================================================

void test_api_db::test_action_data_size( uint64_t val )
{
   // CDT packs the uint64_t action parameter; verify action_data_size returns exact byte count
   sysio_assert(sysio::action_data_size() == sizeof(uint64_t), "action_data_size mismatch");
}

void test_api_db::test_read_action_data( uint64_t val )
{
   // Read back the raw action payload and verify the uint64_t round-trips exactly
   char buf[sizeof(uint64_t)];
   uint32_t sz = sysio::read_action_data(buf, sizeof(buf));
   sysio_assert(sz == sizeof(uint64_t), "read_action_data size mismatch");

   uint64_t recovered = 0;
   memcpy(&recovered, buf, sizeof(recovered));
   sysio_assert(recovered == val, "read_action_data value mismatch");
}

void test_api_db::test_current_receiver()
{
   sysio_assert(sysio::current_receiver() == get_self(), "current_receiver mismatch");
}

// ============================================================================
// Transaction metadata tests
// ============================================================================

void test_api_db::test_transaction_size()
{
   size_t sz = sysio::transaction_size();
   sysio_assert(sz > 0, "transaction_size must be > 0");

   // Verify read_transaction agrees with transaction_size
   char buf[512];
   size_t read_sz = sysio::read_transaction(buf, sizeof(buf));
   sysio_assert(read_sz == sz, "read_transaction size != transaction_size");
}

void test_api_db::test_expiration()
{
   uint32_t exp = sysio::expiration();
   // Expiration is a UTC timestamp in seconds; must be non-zero and reasonable
   // (after 2020-01-01 = 1577836800)
   sysio_assert(exp > 1577836800u, "expiration too small");
}

void test_api_db::test_tapos()
{
   // tapos_block_num and tapos_block_prefix are derived from the reference block
   // They must be deterministic for the same transaction
   int bn = sysio::tapos_block_num();
   int bp = sysio::tapos_block_prefix();

   // Call again — must return identical values (deterministic)
   sysio_assert(bn == sysio::tapos_block_num(), "tapos_block_num not deterministic");
   sysio_assert(bp == sysio::tapos_block_prefix(), "tapos_block_prefix not deterministic");

   // block_prefix is a hash-derived value, should be non-zero in practice
   sysio_assert(bp != 0, "tapos_block_prefix is zero");
}

void test_api_db::test_read_transaction()
{
   size_t sz = sysio::transaction_size();
   sysio_assert(sz > 0 && sz <= 4096, "transaction_size out of range");

   // Read into buffer and verify we get exactly sz bytes
   char buf[4096];
   size_t read_sz = sysio::read_transaction(buf, sz);
   sysio_assert(read_sz == sz, "read_transaction returned wrong size");

   // Reading with 0 buffer returns the size without writing
   size_t probe_sz = sysio::read_transaction(buf, 0);
   sysio_assert(probe_sz == sz, "read_transaction probe returned wrong size");
}

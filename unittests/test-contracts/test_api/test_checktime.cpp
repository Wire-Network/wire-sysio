#include <vector>

#include <sysio/crypto.hpp>
#include <sysio/sysio.hpp>
#include <sysio/print.hpp>

#include "test_api.hpp"
using namespace sysio;

void test_checktime::checktime_pass() {
   int p = 0;
   for ( int i = 0; i < 10000; i++ )
      p += i;

   sysio::print(p);
}

static void checktime_failure_common() {
   volatile unsigned long long bound{}; // `volatile' necessary to prevent loop optimization
   read_action_data( (char*)&bound, sizeof(bound) );

   int p = 0;
   for ( unsigned long long i = 0; i < bound; i++ )
      for ( unsigned long long j = 0; j < bound; j++ )
         p += i+j+bound;

   sysio::print(p);
}

void test_checktime::checktime_failure() {
   checktime_failure_common();
}

// used by tests where authorization is not allowed, like read-only transactions
void test_checktime::checktime_no_auth_failure() {
   checktime_failure_common();
}

constexpr size_t size = 20000000;

void test_checktime::checktime_sha1_failure() {
   char* ptr = new char[size];
   auto res = sha1( ptr, size );
}

void test_checktime::checktime_assert_sha1_failure() {
   char* ptr = new char[size];
   checksum160 res;
   assert_sha1( ptr, size, res );
}

void test_checktime::checktime_sha256_failure() {
   char* ptr = new char[size];
   auto res = sha256( ptr, size );
}

void test_checktime::checktime_assert_sha256_failure() {
   char* ptr = new char[size];
   checksum256 res;
   assert_sha256( ptr, size, res );
}

void test_checktime::checktime_sha512_failure() {
   char* ptr = new char[size];
   auto res = sha512( ptr, size );
}

void test_checktime::checktime_assert_sha512_failure() {
   char* ptr = new char[size];
   checksum512 res;
   assert_sha512( ptr, size, res );
}

void test_checktime::checktime_ripemd160_failure() {
   char* ptr = new char[size];
   auto res = ripemd160( ptr, size );
}

void test_checktime::checktime_assert_ripemd160_failure() {
   char* ptr = new char[size];
   checksum160 res;
   assert_ripemd160( ptr, size, res );
}

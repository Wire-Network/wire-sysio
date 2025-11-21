#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] snapshot_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void increment( uint32_t value );

   struct [[sysio::table("data")]] main_record {
      uint64_t           id         = 0;
      double             index_f64  = 0.0;
      long double        index_f128 = 0.0L;
      uint64_t           index_i64  = 0ULL;
      uint128_t          index_i128 = 0ULL;
      sysio::checksum256 index_i256;

      uint64_t                  primary_key()const    { return id; }
      double                    get_index_f64()const  { return index_f64 ; }
      long double               get_index_f128()const { return index_f128; }
      uint64_t                  get_index_i64()const  { return index_i64 ; }
      uint128_t                 get_index_i128()const { return index_i128; }
      const sysio::checksum256& get_index_i256()const { return index_i256; }

      SYSLIB_SERIALIZE( main_record, (id)(index_f64)(index_f128)(index_i64)(index_i128)(index_i256) )
   };

   using data_table = sysio::multi_index<"data"_n, main_record,
      sysio::indexed_by< "byf"_n,    sysio::const_mem_fun< main_record, double,
                                                           &main_record::get_index_f64 > >,
      sysio::indexed_by< "byff"_n,   sysio::const_mem_fun< main_record, long double,
                                                           &main_record::get_index_f128> >,
      sysio::indexed_by< "byi"_n,    sysio::const_mem_fun< main_record, uint64_t,
                                                           &main_record::get_index_i64 > >,
      sysio::indexed_by< "byii"_n,   sysio::const_mem_fun< main_record, uint128_t,
                                                           &main_record::get_index_i128 > >,
      sysio::indexed_by< "byiiii"_n, sysio::const_mem_fun< main_record, const sysio::checksum256&,
                                                           &main_record::get_index_i256 > >
   >;

   struct [[sysio::table("test")]] test_record {
      uint64_t id = 0;
      sysio::checksum256 payload;
      uint64_t primary_key() const {return id;}
   };
   using test_table = sysio::multi_index<"test"_n, test_record>;
   [[sysio::action]] void add(sysio::name scope, uint64_t id, sysio::checksum256 payload);
   [[sysio::action]] void remove(sysio::name scope, uint64_t id);
   [[sysio::action]] void verify(sysio::name scope, uint64_t id, sysio::checksum256 payload);
};

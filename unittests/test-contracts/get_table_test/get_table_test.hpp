/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#pragma once

#include <sysio/sysio.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_table.hpp>


namespace sysio {
   namespace internal_use_do_not_use {
      extern "C" {
      __attribute__((sysio_wasm_import))
      int32_t alt_bn128_add( const char* op1_data, uint32_t op1_length,
                             const char* op2_data, uint32_t op2_length,
                             char* result , uint32_t result_length);

      __attribute__((sysio_wasm_import))
      int32_t alt_bn128_mul( const char* op1_data, uint32_t op1_length,
                             const char* op2_data, uint32_t op2_length,
                             char* result , uint32_t result_length);

      __attribute__((sysio_wasm_import))
      int32_t alt_bn128_pair( const char* op1_data, uint32_t op1_length);

      __attribute__((sysio_wasm_import))
      int32_t mod_exp(const char* base_data, uint32_t base_length,
                      const char* exp_data, uint32_t exp_length,
                      const char* mod_data, uint32_t mod_length,
                      char* result, uint32_t result_length);

      __attribute__((sysio_wasm_import))
      int32_t blake2_f( uint32_t rounds,
                        const char* state, uint32_t len_state,
                        const char* message, uint32_t len_message,
                        const char* t0_offset, uint32_t len_t0_offset,
                        const char* t1_offset, uint32_t len_t1_offset,
                        int32_t final,
                        char* result, uint32_t len_result);

      __attribute__((sysio_wasm_import))
      void sha3( const char* input_data, uint32_t input_length,
                 char* output_data, uint32_t output_length, int32_t keccak);

      __attribute__((sysio_wasm_import))
      int32_t k1_recover( const char* signature_data, uint32_t signature_length,
                          const char* digest_data, uint32_t digest_length,
                          char* output_data, uint32_t output_length);
      }
   }
}

using namespace sysio;

class [[sysio::contract]] get_table_test : public sysio::contract {
    public:
    using sysio::contract::contract;

    // Number object
    struct [[sysio::table]] numobj {
        uint64_t        key;
        uint64_t        sec64;
        uint128_t       sec128;
        double          secdouble;
        long double     secldouble;

        uint64_t primary_key() const { return key; }
        uint64_t sec64_key() const { return sec64; }
        uint128_t sec128_key() const { return sec128; }
        double secdouble_key() const { return secdouble; }
        long double secldouble_key() const { return secldouble; }
    };

    // Hash object
    struct [[sysio::table]] hashobj {
        uint64_t        key;
        std::string     hash_input;
        checksum256     sec256;
        checksum160     sec160;

        uint64_t primary_key() const { return key; }
        checksum256 sec256_key() const { return sec256; }
        checksum256 sec160_key() const { return checksum256(sec160.get_array()); }
    };

    typedef sysio::multi_index< "numobjs"_n, numobj,
                                indexed_by<"bysec1"_n, const_mem_fun<numobj, uint64_t, &numobj::sec64_key>>,
                                indexed_by<"bysec2"_n, const_mem_fun<numobj, uint128_t, &numobj::sec128_key>>,
                                indexed_by<"bysec3"_n, const_mem_fun<numobj, double, &numobj::secdouble_key>>,
                                indexed_by<"bysec4"_n, const_mem_fun<numobj, long double, &numobj::secldouble_key>>
                                > numobjs;

    typedef sysio::multi_index< "hashobjs"_n, hashobj,
                            indexed_by<"bysec1"_n, const_mem_fun<hashobj, checksum256, &hashobj::sec256_key>>,
                            indexed_by<"bysec2"_n, const_mem_fun<hashobj, checksum256, &hashobj::sec160_key>>
                            > hashobjs;

    // Struct-keyed kv::table — drives the ABI-aware BE key codec's struct
    // expansion on the live get_table_rows path. Mirrors the v6 registry
    // tables (e.g. sysio.chains `chains`), whose primary key is the reflected
    // struct `slug_name { value: uint64 }`. abigen emits
    // `key_types: ["code"->"slug_name"]` for this table, so JSON bounds and
    // `next_key` pagination must round-trip the nested `{ "code": { "value": N } }`
    // key shape — coverage a flat scalar key cannot provide.
    struct slug_name {
        uint64_t value = 0;
        SYSLIB_SERIALIZE(slug_name, (value))
    };

    struct structobj_key {
        slug_name code;
        uint64_t primary_key() const { return code.value; }
        SYSLIB_SERIALIZE(structobj_key, (code))
    };

    struct [[sysio::table("structobjs")]] structobj {
        slug_name code;
        uint64_t  payload = 0;
        SYSLIB_SERIALIZE(structobj, (code)(payload))
    };

    typedef sysio::kv::table< "structobjs"_n, structobj_key, structobj > structobjs;

   [[sysio::action]]
   void addnumobj(uint64_t input);

   [[sysio::action]]
   void modifynumobj(uint64_t id);

   [[sysio::action]]
   void erasenumobj(uint64_t id);


   [[sysio::action]]
   void addhashobj(std::string hashinput);

   /// Insert a row into the struct-keyed kv::table `structobjs`.
   /// @param code     the slug_name value forming the struct primary key
   /// @param payload  arbitrary row payload
   [[sysio::action]]
   void addstruct(uint64_t code, uint64_t payload);


};

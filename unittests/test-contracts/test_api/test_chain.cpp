#include <sysio/action.hpp>
#include <sysio/sysio.hpp>
#include <sysio/system.hpp>

#include "test_api.hpp"

#pragma pack(push, 1)
struct producers {
   char len;
   capi_name producers[21];
};
struct ram_usage {
    name account;
    int64_t ram_bytes;
};
#pragma pack(pop)

void test_chain::test_activeprods() {
  producers act_prods;
  read_action_data( &act_prods, sizeof(producers) );
   
  sysio_assert( act_prods.len == 21, "producers.len != 21" );

  producers api_prods;
  get_active_producers( api_prods.producers, sizeof(sysio::name)*21 );

  for( int i = 0; i < 21 ; ++i )
      sysio_assert( api_prods.producers[i] == act_prods.producers[i], "Active producer" );
}

void test_chain::test_get_ram_usage() {
    ram_usage ram_usage;
    read_action_data( &ram_usage, sizeof(ram_usage) );
    int64_t ram_bytes = get_ram_usage( ram_usage.account );
    if (ram_bytes != ram_usage.ram_bytes) {
        std::string err = "ram_bytes " + std::to_string(ram_bytes) + " != ram_usage.ram_bytes " + std::to_string(ram_usage.ram_bytes);
        sysio_assert(ram_bytes == ram_usage.ram_bytes, err.c_str());
    }

    uint64_t receiver = "testapi"_n.value;
    auto table1 = "table1"_n.value;
    auto payer = ram_usage.account.value;

    std::string info = "tom's info";
    // creates the table and adds an entry
    db_store_i64( receiver, table1, payer, "tom"_n.value, info.c_str(), info.size() );
    int64_t billable_size_v_table_id_object = 112; // see config::billable_size_v<table_id_object>;
    int64_t billable_size_v_key_value_object = 112; // see config::billable_size_v<key_value_object>)
    int64_t billable_size = (int64_t)(info.size() + billable_size_v_table_id_object + billable_size_v_key_value_object);

    int64_t expected_ram = ram_usage.ram_bytes + billable_size;

    ram_bytes = get_ram_usage( ram_usage.account );
    if (ram_bytes != expected_ram) {
        std::string err = "ram_bytes " + std::to_string(ram_bytes) + " != expected_ram " + std::to_string(expected_ram);
        sysio_assert(ram_bytes == expected_ram, err.c_str());
    }

    // table already created, just adds an entry
    db_store_i64( receiver, table1, payer, "tom"_n.value+1, info.c_str(), info.size() );
    expected_ram += (int64_t)(info.size() + billable_size_v_key_value_object);

    ram_bytes = get_ram_usage( ram_usage.account );
    if (ram_bytes != expected_ram) {
        std::string err = "ram_bytes " + std::to_string(ram_bytes) + " != after store expected_ram " + std::to_string(expected_ram);
        sysio_assert(ram_bytes == expected_ram, err.c_str());
    }
}
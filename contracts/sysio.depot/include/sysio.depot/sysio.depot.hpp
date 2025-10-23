#pragma once

#include <sysio/asset.hpp>
#include <sysio/sysio.hpp>

#include <sysio.depot/sysio.opp.hpp>
#include <sysio.system/non_wasm_types.hpp>

namespace sysiosystem {
class system_contract;
}

namespace sysio {

enum chain_kind : uint8_t {
  chain_wire = 0,
  chain_ethereum = 1,
  chain_solana = 2,
  chain_sui = 3,
};

enum swap_status_type : uint8_t {

};

// A completely stubbed contract for sysio.depot.
class [[sysio::contract("sysio.depot")]] depot : public contract {
public:
  using contract::contract;

  ACTION reservedelta(const chain_kind &kind, const asset &delta);

  ACTION swapquote(const chain_kind &source_chain, const asset &source_amount,
                   const chain_kind &target_chain_kind);

private:
  // TABLES
  TABLE reserve_s {

    /**
     * External chain kind
     *
     * @see chain_kind
     */
    chain_kind chain;

    /**
     * Reserve/Pool balance
     */
    asset balance;

    chain_kind by_chain() const { return chain; }

    /**
     * Symbol index
     *
     * @return symbol index
     */
    uint64_t by_symbol() const { return balance.symbol.raw(); }

    /**
     * Same as `primary_key()`
     * Chain << 64 | Symbol
     */
    uint128_t by_chain_symbol() const {
      uint128_t chain_symbol = chain;
      chain_symbol <<= 64;

      return chain_symbol | balance.symbol.raw();
    }

    /**
     * Chain << 64 | Symbol
     */
    uint128_t primary_key() const { return by_chain_symbol(); }
  };

  using reserve_t = multi_index<
      "reserve"_n, reserve_s,
      indexed_by<"bychain"_n,
                 const_mem_fun<reserve_s, chain_kind, &reserve_s::by_chain>>,
      indexed_by<"bysymbol"_n,
                 const_mem_fun<reserve_s, uint64_t, &reserve_s::by_symbol>>
      // indexed_by<"bychainsym"_n, const_mem_fun<reserve_s, uint128_t,
      // &reserve_s::by_chain_symbol>>
      >;

  TABLE swap_s {

    /**
     * Primary key
     */
    uint64_t key;

    /**
     * Swap status
     */
    swap_status_type status;

    /**
     * Source External Chain
     *
     * @see chain_kind
     */
    chain_kind source_chain;

    /**
     * Source asset
     */
    asset source_amount;

    /**
     * Target External Chain
     *
     * @see chain_kind
     */
    chain_kind target_chain;

    /**
     * Source asset
     */
    asset target_amount;

    chain_kind by_source_chain() const { return source_chain; }

    chain_kind by_target_chain() const { return target_chain; }

    /**
     * Source status value combining chain and status bytes
     *
     * @return uint16_t value with chain in first byte and status in second
     * byte
     */
    uint16_t by_source_chain_status() const {
      return (static_cast<uint16_t>(source_chain) << 8) |
             static_cast<uint16_t>(status);
    }

    /**
     * Target status value combining chain and status bytes
     *
     * @return uint16_t value with chain in first byte and status in second
     * byte
     */
    uint16_t by_target_chain_status() const {
      return (static_cast<uint16_t>(target_chain) << 8) |
             static_cast<uint16_t>(status);
    }

    /**
     * Source Compound index value combining chain, status & symbol
     *
     * @return Compound index value combining chain, status & symbol
     */
    uint128_t by_source_chain_status_symbol() const {
      uint128_t compound_value = source_chain;
      compound_value <<= 96;
      compound_value |= (static_cast<uint128_t>(status) << 64);
      return compound_value | source_amount.symbol.raw();
    }

    /**
     * Target Compound index value combining chain, status & symbol
     *
     * @return Compound index value combining chain, status & symbol
     */
    uint128_t by_target_chain_status_symbol() const {
      uint128_t compound_value = target_chain;
      compound_value <<= 96;
      compound_value |= (static_cast<uint128_t>(status) << 64);
      return compound_value | target_amount.symbol.raw();
    }

    /**
     * Source Chain << 64 | Symbol
     */
    uint128_t by_source_chain_symbol() const {
      uint128_t chain_symbol = source_chain;
      chain_symbol <<= 64;

      return chain_symbol | source_amount.symbol.raw();
    }

    uint128_t by_target_chain_symbol() const {
      uint128_t chain_symbol = target_chain;
      chain_symbol <<= 64;

      return chain_symbol | target_amount.symbol.raw();
    }

    /**
     * unique id
     */
    uint128_t primary_key() const { return key; }
  };

  using swap_t = multi_index<
      "swap"_n, swap_s,
      indexed_by<"byschain"_n,
                 const_mem_fun<swap_s, chain_kind, &swap_s::by_source_chain>>,
      indexed_by<"byschst"_n, const_mem_fun<swap_s, uint16_t,
                                            &swap_s::by_source_chain_status>>,
      indexed_by<"byschstsym"_n,
                 const_mem_fun<swap_s, uint128_t,
                               &swap_s::by_source_chain_status_symbol>>,
      indexed_by<"bytchain"_n,
                 const_mem_fun<swap_s, chain_kind, &swap_s::by_target_chain>>,
      indexed_by<"bytchst"_n, const_mem_fun<swap_s, uint16_t,
                                            &swap_s::by_target_chain_status>>,
      indexed_by<"bytchstsym"_n,
                 const_mem_fun<swap_s, uint128_t,
                               &swap_s::by_target_chain_status_symbol>>>;
};

} // namespace sysio

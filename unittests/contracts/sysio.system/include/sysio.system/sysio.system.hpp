#pragma once

#include <sysio/asset.hpp>
#include <sysio/binary_extension.hpp>
#include <sysio/privileged.hpp>
#include <sysio/producer_schedule.hpp>
#include <sysio/singleton.hpp>
#include <sysio/system.hpp>
#include <sysio/time.hpp>

#include <sysio.system/native.hpp>

#include <deque>
#include <optional>
#include <string>
#include <type_traits>

namespace sysiosystem {

   using sysio::asset;
   using sysio::binary_extension;
   using sysio::block_timestamp;
   using sysio::check;
   using sysio::const_mem_fun;
   using sysio::datastream;
   using sysio::indexed_by;
   using sysio::name;
   using sysio::same_payer;
   using sysio::symbol;
   using sysio::symbol_code;
   using sysio::time_point;
   using sysio::time_point_sec;
   using sysio::unsigned_int;

   template<typename E, typename F>
   static inline auto has_field( F flags, E field )
   -> std::enable_if_t< std::is_integral_v<F> && std::is_unsigned_v<F> &&
                        std::is_enum_v<E> && std::is_same_v< F, std::underlying_type_t<E> >, bool>
   {
      return ( (flags & static_cast<F>(field)) != 0 );
   }

   template<typename E, typename F>
   static inline auto set_field( F flags, E field, bool value = true )
   -> std::enable_if_t< std::is_integral_v<F> && std::is_unsigned_v<F> &&
                        std::is_enum_v<E> && std::is_same_v< F, std::underlying_type_t<E> >, F >
   {
      if( value )
         return ( flags | static_cast<F>(field) );
      else
         return ( flags & ~static_cast<F>(field) );
   }

   static constexpr uint32_t seconds_per_year      = 52 * 7 * 24 * 3600;
   static constexpr uint32_t seconds_per_day       = 24 * 3600;
   static constexpr uint32_t seconds_per_hour      = 3600;
   static constexpr int64_t  useconds_per_year     = int64_t(seconds_per_year) * 1000'000ll;
   static constexpr int64_t  useconds_per_day      = int64_t(seconds_per_day) * 1000'000ll;
   static constexpr int64_t  useconds_per_hour     = int64_t(seconds_per_hour) * 1000'000ll;
   static constexpr uint32_t blocks_per_day        = 2 * seconds_per_day; // half seconds per day

#ifdef SYSTEM_BLOCKCHAIN_PARAMETERS
   struct blockchain_parameters_v1 : sysio::blockchain_parameters
   {
      sysio::binary_extension<uint32_t> max_action_return_value_size;
      SYSLIB_SERIALIZE_DERIVED( blockchain_parameters_v1, sysio::blockchain_parameters,
                                (max_action_return_value_size) )
   };
   using blockchain_parameters_t = blockchain_parameters_v1;
#else
   using blockchain_parameters_t = sysio::blockchain_parameters;
#endif

  /**
   * The `sysio.system` smart contract; it defines the structures and actions needed for blockchain's core functionality.
   * 
   * Just like in the `sysio.bios` sample contract implementation, there are a few actions which are not implemented at the contract level (`newaccount`, `updateauth`, `deleteauth`, `linkauth`, `unlinkauth`, `canceldelay`, `onerror`, `setabi`, `setcode`), they are just declared in the contract so they will show in the contract's ABI and users will be able to push those actions to the chain via the account holding the `sysio.system` contract, but the implementation is at the SYSIO core level. They are referred to as SYSIO native actions.
   * 
   * - Users can stake tokens for CPU and Network bandwidth, and then vote for producers or
   *    delegate their vote to a proxy.
   * - Producers register in order to be voted for, and can claim per-block and per-vote rewards.
   * - Users can buy and sell RAM at a market-determined price.
   */
  
   // Defines new global state parameters.
   struct [[sysio::table("global"), sysio::contract("sysio.system")]] sysio_global_state : sysio::blockchain_parameters {
      uint64_t free_ram()const { return max_ram_size - total_ram_bytes_reserved; }

      uint64_t             max_ram_size = 64ll*1024 * 1024 * 1024;
      uint64_t             total_ram_bytes_reserved = 0;

      block_timestamp      last_producer_schedule_update;
      time_point           last_pervote_bucket_fill;
      uint32_t             total_unpaid_blocks = 0; /// all blocks which have been produced but not paid
      int64_t              total_activated_stake = 0;
      time_point           thresh_activated_stake_time;
      uint16_t             last_producer_schedule_size = 0;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE_DERIVED( sysio_global_state, sysio::blockchain_parameters,
                                (max_ram_size)(total_ram_bytes_reserved)
                                (last_producer_schedule_update)
                                (total_unpaid_blocks)(total_activated_stake)(thresh_activated_stake_time)
                                (last_producer_schedule_size) )
   };

   inline sysio::block_signing_authority convert_to_block_signing_authority( const sysio::public_key& producer_key ) {
      return sysio::block_signing_authority_v0{ .threshold = 1, .keys = {{producer_key, 1}} };
   }

   // Defines `producer_info` structure to be stored in `producer_info` table, added after version 1.0
   struct [[sysio::table, sysio::contract("sysio.system")]] producer_info {
      name                                                     owner;
      sysio::public_key                                        producer_key; /// a packed public key object
      bool                                                     is_active = true;
      std::string                                              url;
      uint32_t                                                 unpaid_blocks = 0;
      time_point                                               last_claim_time;
      uint16_t                                                 location = 0;
      sysio::block_signing_authority                           producer_authority; // added in version 1.9.0

      uint64_t primary_key()const { return owner.value;                             }
      bool     active()const      { return is_active;                               }
      void     deactivate()       { producer_key = public_key(); producer_authority = sysio::block_signing_authority{}; is_active = false; }

      const sysio::block_signing_authority& get_producer_authority()const {
         return producer_authority;
      }

      SYSLIB_SERIALIZE( producer_info, (owner)(producer_key)(is_active)(url)(unpaid_blocks)(last_claim_time)(location)(producer_authority) )
   };

   typedef sysio::multi_index< "producers"_n, producer_info > producers_table;

   typedef sysio::singleton< "global"_n, sysio_global_state >   global_state_singleton;

   /**
    * The `sysio.system` smart contract is provided by `block.one` as a sample system contract, and it defines the structures and actions needed for blockchain's core functionality.
    *
    * Just like in the `sysio.bios` sample contract implementation, there are a few actions which are not implemented at the contract level (`newaccount`, `updateauth`, `deleteauth`, `linkauth`, `unlinkauth`, `canceldelay`, `onerror`, `setabi`, `setcode`), they are just declared in the contract so they will show in the contract's ABI and users will be able to push those actions to the chain via the account holding the `sysio.system` contract, but the implementation is at the SYSIO core level. They are referred to as SYSIO native actions.
    *
    * - Users can stake tokens for CPU and Network bandwidth, and then vote for producers or
    *    delegate their vote to a proxy.
    * - Producers register in order to be voted for, and can claim per-block and per-vote rewards.
    * - Users can buy and sell RAM at a market-determined price.
    */
   class [[sysio::contract("sysio.system")]] system_contract : public native {

      private:
         producers_table          _producers;
         global_state_singleton   _global;
         sysio_global_state       _gstate;

      public:
         static constexpr sysio::name active_permission{"active"_n};
         static constexpr sysio::name token_account{"sysio.token"_n};
         static constexpr sysio::name null_account{"sysio.null"_n};

         system_contract( name s, name code, datastream<const char*> ds );
         ~system_contract();

         // Actions:
         /**
          * The Init action initializes the system contract for a version and a symbol.
          * Only succeeds when:
          * - version is 0 and
          * - symbol is found and
          * - system token supply is greater than 0,
          * - and system contract wasn’t already been initialized.
          *
          * @param version - the version, has to be 0,
          * @param core - the system symbol.
          */
         [[sysio::action]]
         void init( unsigned_int version, const symbol& core );

         /**
          * On block action. This special action is triggered when a block is applied by the given producer
          * and cannot be generated from any other source. It is used to pay producers and calculate
          * missed blocks of other producers. Producer pay is deposited into the producer's stake
          * balance and can be withdrawn over time. Once a minute, it may update the active producer config from the
          * producer votes. The action also populates the blockinfo table.
          *
          * @param header - the block header produced.
          */
         [[sysio::action]]
         void onblock( ignore<block_header> header );

         /**
          * Set account limits action sets the resource limits of an account
          *
          * @param account - name of the account whose resource limit to be set,
          * @param ram_bytes - ram limit in absolute bytes,
          * @param net_weight - fractionally proportionate net limit of available resources based on (weight / total_weight_of_all_accounts),
          * @param cpu_weight - fractionally proportionate cpu limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[sysio::action]]
         void setalimits( const name& account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight );

         /**
          * Set producers action, sets a new list of active producers, by proposing a schedule change, once the block that
          * contains the proposal becomes irreversible, the schedule is promoted to "pending"
          * automatically. Once the block that promotes the schedule is irreversible, the schedule will
          * become "active".
          *
          * @param schedule - New list of active producers to set
          */
         [[sysio::action]]
         void setprods( const std::vector<sysio::producer_authority>& schedule );

         /**
          * Set account RAM limits action, which sets the RAM limits of an account
          *
          * @param account - name of the account whose resource limit to be set,
          * @param ram_bytes - ram limit in absolute bytes.
          */
         [[sysio::action]]
         void setacctram( const name& account, const std::optional<int64_t>& ram_bytes );

         /**
          * Set account NET limits action, which sets the NET limits of an account
          *
          * @param account - name of the account whose resource limit to be set,
          * @param net_weight - fractionally proportionate net limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[sysio::action]]
         void setacctnet( const name& account, const std::optional<int64_t>& net_weight );

         /**
          * Set account CPU limits action, which sets the CPU limits of an account
          *
          * @param account - name of the account whose resource limit to be set,
          * @param cpu_weight - fractionally proportionate cpu limit of available resources based on (weight / total_weight_of_all_accounts).
          */
         [[sysio::action]]
         void setacctcpu( const name& account, const std::optional<int64_t>& cpu_weight );


         /**
          * The activate action, activates a protocol feature
          *
          * @param feature_digest - hash of the protocol feature to activate.
          */
         [[sysio::action]]
         void activate( const sysio::checksum256& feature_digest );

         /**
          * Register producer action, indicates that a particular account wishes to become a producer,
          * this action will create a `producer_config` and a `producer_info` object for `producer` scope
          * in producers tables.
          *
          * @param producer - account registering to be a producer candidate,
          * @param producer_key - the public key of the block producer, this is the key used by block producer to sign blocks,
          * @param url - the url of the block producer, normally the url of the block producer presentation website,
          * @param location - is the country code as defined in the ISO 3166, https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
          *
          * @pre Producer to register is an account
          * @pre Authority of producer to register
          */
         [[sysio::action]]
         void regproducer( const name& producer, const public_key& producer_key, const std::string& url, uint16_t location );

         /**
          * Register producer action, indicates that a particular account wishes to become a producer,
          * this action will create a `producer_config` and a `producer_info` object for `producer` scope
          * in producers tables.
          *
          * @param producer - account registering to be a producer candidate,
          * @param producer_authority - the weighted threshold multisig block signing authority of the block producer used to sign blocks,
          * @param url - the url of the block producer, normally the url of the block producer presentation website,
          * @param location - is the country code as defined in the ISO 3166, https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
          *
          * @pre Producer to register is an account
          * @pre Authority of producer to register
          */
         [[sysio::action]]
         void regproducer2( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location );

         /**
          * Unregister producer action, deactivates the block producer with account name `producer`.
          *
          * Deactivate the block producer with account name `producer`.
          * @param producer - the block producer account to unregister.
          */
         [[sysio::action]]
         void unregprod( const name& producer );

         /**
          * Set ram action sets the ram supply.
          * @param max_ram_size - the amount of ram supply to set.
          */
         [[sysio::action]]
         void setram( uint64_t max_ram_size );

         /**
          * Set the blockchain parameters. By tunning these parameters a degree of
          * customization can be achieved.
          * @param params - New blockchain parameters to set.
          */
         [[sysio::action]]
         void setparams( const blockchain_parameters_t& params );

#ifdef SYSTEM_CONFIGURABLE_WASM_LIMITS
         /**
          * Sets the WebAssembly limits.  Valid parameters are "low",
          * "default" (equivalent to low), and "high".  A value of "high"
          * allows larger contracts to be deployed.
          */
         [[sysio::action]]
         void wasmcfg( const name& settings );
#endif

         /**
          * Set privilege status for an account. Allows to set privilege status for an account (turn it on/off).
          * @param account - the account to set the privileged status for.
          * @param is_priv - 0 for false, > 0 for true.
          */
         [[sysio::action]]
         void setpriv( const name& account, uint8_t is_priv );

         /**
          * Remove producer action, deactivates a producer by name, if not found asserts.
          * @param producer - the producer account to deactivate.
          */
         [[sysio::action]]
         void rmvproducer( const name& producer );

         /**
          * limitauthchg opts into or out of restrictions on updateauth, deleteauth, linkauth, and unlinkauth.
          *
          * If either allow_perms or disallow_perms is non-empty, then opts into restrictions. If
          * allow_perms is non-empty, then the authorized_by argument of the restricted actions must be in
          * the vector, or the actions will abort. If disallow_perms is non-empty, then the authorized_by
          * argument of the restricted actions must not be in the vector, or the actions will abort.
          *
          * If both allow_perms and disallow_perms are empty, then opts out of the restrictions. limitauthchg
          * aborts if both allow_perms and disallow_perms are non-empty.
          *
          * @param account - account to change
          * @param allow_perms - permissions which may use the restricted actions
          * @param disallow_perms - permissions which may not use the restricted actions
          */
         [[sysio::action]]
         void limitauthchg( const name& account, const std::vector<name>& allow_perms, const std::vector<name>& disallow_perms );

         /**
          * On Link Auth notify to catch auth.ext stuff for sig-em
          */
         [[sysio::on_notify("auth.msg::onlinkauth")]]
         void onlinkauth(const name &user, const name &permission, const sysio::public_key &pub_key);

         using init_action = sysio::action_wrapper<"init"_n, &system_contract::init>;
         using setacctram_action = sysio::action_wrapper<"setacctram"_n, &system_contract::setacctram>;
         using setacctnet_action = sysio::action_wrapper<"setacctnet"_n, &system_contract::setacctnet>;
         using setacctcpu_action = sysio::action_wrapper<"setacctcpu"_n, &system_contract::setacctcpu>;
         using activate_action = sysio::action_wrapper<"activate"_n, &system_contract::activate>;
         using regproducer_action = sysio::action_wrapper<"regproducer"_n, &system_contract::regproducer>;
         using regproducer2_action = sysio::action_wrapper<"regproducer2"_n, &system_contract::regproducer2>;
         using unregprod_action = sysio::action_wrapper<"unregprod"_n, &system_contract::unregprod>;
         using setram_action = sysio::action_wrapper<"setram"_n, &system_contract::setram>;
         using rmvproducer_action = sysio::action_wrapper<"rmvproducer"_n, &system_contract::rmvproducer>;
         using setpriv_action = sysio::action_wrapper<"setpriv"_n, &system_contract::setpriv>;
         using setalimits_action = sysio::action_wrapper<"setalimits"_n, &system_contract::setalimits>;
         using setparams_action = sysio::action_wrapper<"setparams"_n, &system_contract::setparams>;

      private:
         // Implementation details:

         //defined in sysio.system.cpp
         static sysio_global_state get_default_parameters();

         // defined in voting.cpp
         void register_producer( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location );
         void update_elected_producers( const block_timestamp& timestamp );

         // defined in block_info.cpp
         void add_to_blockinfo_table(const sysio::checksum256& previous_block_id, const sysio::block_timestamp timestamp) const;
   };

}

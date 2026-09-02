#pragma once

#include <sysio/asset.hpp>
#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/binary_extension.hpp>
#include <sysio/crypto.hpp>
#include <sysio/privileged.hpp>
#include <sysio/producer_schedule.hpp>
#include <sysio/system.hpp>
#include <sysio/time.hpp>
#include <sysio/instant_finality.hpp>

#include <sysio.system/emissions.hpp>
#include <sysio.system/native.hpp>
#include <sysio.system/producer_rank.hpp>
#include <sysio.system/snapshot_attest.hpp>

#include <limits>
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
   using sysio::name;
   using sysio::kv::same_payer;
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


   static constexpr size_t   max_producers         = 21;
   /// Minimum number of producers -- and, in lock-step, finalizers -- that
   /// update_ranked_producers will ever publish. Savanna's finality threshold
   /// is (N*2)/3 + 1, so remaining live with at least one faulty finalizer
   /// requires N >= 4 (N=4 -> threshold 3 tolerates 1 fault; N=3 -> threshold 3
   /// tolerates 0). When fewer producers than this are collateral-eligible the
   /// last good schedule is retained rather than concentrating consensus onto
   /// too few nodes. Raising it trades more aggressive removal of ineligible
   /// producers for a stronger anti-concentration floor.
   static constexpr size_t   min_schedule_size     = 4;
   static constexpr uint32_t seconds_per_year      = 52 * 7 * 24 * 3600;
   static constexpr uint32_t seconds_per_day       = 24 * 3600;
   static constexpr uint32_t seconds_per_hour      = 3600;
   static constexpr int64_t  useconds_per_year     = int64_t(seconds_per_year) * 1000'000ll;
   static constexpr int64_t  useconds_per_day      = int64_t(seconds_per_day) * 1000'000ll;
   static constexpr int64_t  useconds_per_hour     = int64_t(seconds_per_hour) * 1000'000ll;
   static constexpr uint32_t blocks_per_day        = 2 * seconds_per_day; // half seconds per day
   static constexpr uint32_t blocks_per_round      = 12; // sysio::chain::config::producer_repetitions
   static constexpr uint32_t min_blocks_per_round_for_pay = 6;
   static constexpr uint32_t no_prev_block        = std::numeric_limits<uint32_t>::max(); // sentinel: no previous block

   // All fields (including max_action_return_value_size, KV limits) are now
   // in the base sysio::blockchain_parameters struct.
   using blockchain_parameters_t = sysio::blockchain_parameters;

   // Defines new global state parameters.
   struct [[sysio::table("global"), sysio::contract("sysio.system")]] sysio_global_state : sysio::blockchain_parameters {
      uint64_t free_ram()const { return max_ram_size - total_ram_bytes_reserved; }

      uint64_t             max_ram_size = 64ll*1024 * 1024 * 1024;
      uint64_t             total_ram_bytes_reserved = 0;

      block_timestamp      last_producer_schedule_update;
      time_point           last_pervote_bucket_fill;
      uint32_t             total_unpaid_blocks = 0; /// all blocks which have been produced but not paid
      uint16_t             last_producer_schedule_size = 0;

      /// Producer of the previous block -- the cursor `onblock` walks to attribute a MISSED round:
      /// the producers sitting between this one and the current block's producer in the active
      /// schedule produced nothing in their slot. Whether that walk is meaningful is decided by
      /// comparing the live schedule against the `prodsched` snapshot, since CDT exposes no
      /// schedule version.
      name                 last_producer;

      /// Rescore cursor. A weight change (`setscorecfg`) or a `req_prod_collat` change
      /// invalidates every stored `rank_score`, and the producers table is unbounded because
      /// `regproducer` is permissionless. Rather than a mass rewrite, `rescore_generation` is
      /// bumped and `onblock` drains `rescore_cursor` a bounded number of rows per schedule-rebuild
      /// tick. The cursor walks PRIMARY-key order: rescoring mutates the secondary key, so walking
      /// `prodrank` would revisit or skip rows.
      uint64_t             rescore_cursor = 0;
      /// Non-zero while a rescore sweep is in progress.
      uint32_t             rescore_generation = 0;

      /// `config_timestamp_ms` of the `req_prod_collat` entries the stored scores were computed
      /// against, or 0 when that requirement vector was empty.
      ///
      /// The collateral factor is a RATIO against those minimums, so changing them invalidates
      /// every stored score at once -- and `sysio.opreg::setconfig` is the only writer, on a
      /// contract this one cannot reach into. Rather than have opreg notify (a ten-parameter
      /// handler that would still miss any future writer), `onblock` compares this stamp against
      /// the live config inside the throttle it already pays for and opens a sweep on a mismatch.
      /// setconfig re-stamps every entry unconditionally, so an unrelated field change opens a
      /// sweep too; that is harmless, since rescoring a row whose score is unchanged writes
      /// nothing.
      uint64_t             scored_collateral_stamp = 0;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE_DERIVED( sysio_global_state, sysio::blockchain_parameters,
                                (max_ram_size)(total_ram_bytes_reserved)
                                (last_producer_schedule_update)(last_pervote_bucket_fill)
                                (total_unpaid_blocks)
                                (last_producer_schedule_size)
                                (last_producer)
                                (rescore_cursor)(rescore_generation)
                                (scored_collateral_stamp) )
   };

   inline sysio::block_signing_authority convert_to_block_signing_authority( const sysio::public_key& producer_key ) {
      return sysio::block_signing_authority_v0{ .threshold = 1, .keys = {{producer_key, 1}} };
   }

   struct producer_key_t {
      uint64_t owner;
      SYSLIB_SERIALIZE(producer_key_t, (owner))
   };

   // Defines `producer_info` structure to be stored in `producer_info` table, added after version 1.0
   struct [[sysio::table("producers"), sysio::contract("sysio.system")]] producer_info {
      name                                                     owner;
      sysio::public_key                                        producer_key; /// a packed public key object
      /// Packed ordering key: producer_tier in the high bits, inverted composite score below.
      /// NOT a rank -- `rank` is position in the "prodrank" index among schedulable producers,
      /// derived by iteration. Defaults to the demoted tier's worst score so a registered but
      /// never-scored row can never outrank a scored one.
      uint64_t                                                 rank_score = producer_rank::unscored();
      bool                                                     is_active = true;
      std::string                                              url;
      uint32_t                                                 unpaid_blocks = 0;
      time_point                                               last_claim_time;
      uint16_t                                                 location = 0;
      sysio::block_signing_authority                           producer_authority; // added in version 1.9.0
      uint32_t                                                 last_block_num = no_prev_block;
      uint16_t                                                 current_round_blocks = 0;   // blocks in current (in-progress) round
      uint32_t                                                 eligible_rounds      = 0;   // rounds meeting >= min_blocks threshold (per epoch)
      /// Rounds this producer was scheduled for and produced nothing in, consecutively. Reset to 0
      /// the moment it produces. At prodscorecfg's max_consecutive_missed_rounds it sets
      /// `is_demoted`; see producer_rank.hpp.
      uint32_t                                                 consecutive_missed_rounds = 0;
      /// Demoted to standby for missing rounds. Categorical -- no score overcomes it. Cleared only
      /// by `regproducer`, which is the single door back from both a voluntary `unregprod` park and
      /// an involuntary demotion.
      bool                                                     is_demoted = false;
      /// Snapshot attestations credited this pay period; reset alongside the block counters.
      uint32_t                                                 snapshot_attestations = 0;

      uint64_t by_rank_score()const { return rank_score; }
      bool     active()const      { return is_active;                               }
      void     deactivate()       { producer_key = public_key(); producer_authority = sysio::block_signing_authority{}; is_active = false; }

      const sysio::block_signing_authority& get_producer_authority()const {
         return producer_authority;
      }

      SYSLIB_SERIALIZE( producer_info, (owner)(producer_key)(rank_score)(is_active)(url)(unpaid_blocks)(last_claim_time)(location)(producer_authority)
                         (last_block_num)(current_round_blocks)(eligible_rounds)
                         (consecutive_missed_rounds)(is_demoted)(snapshot_attestations) )
   };

   using producers_table = sysio::kv::table< "producers"_n, producer_key_t, producer_info,
                              sysio::kv::index<"prodrank"_n, const_mem_fun<producer_info, uint64_t, &producer_info::by_rank_score>>
                           >;

   struct finkey_key_t {
      uint64_t id;
      SYSLIB_SERIALIZE(finkey_key_t, (id))
   };

   // finalizer_key_info stores information about a finalizer key.
   struct [[sysio::table("finkeys"), sysio::contract("sysio.system")]] finalizer_key_info {
      uint64_t          id;                   // automatically generated ID for the key in the table
      name              finalizer_name;       // name of the finalizer owning the key
      std::string       finalizer_key;        // finalizer key in base64url format
      std::vector<char> finalizer_key_binary; // finalizer key in binary format in Affine little endian non-montgomery g1

      uint64_t    by_fin_name() const { return finalizer_name.value; }
      // Use binary format to hash. It is more robust and less likely to change
      // than the base64url text encoding of it.
      checksum256 by_fin_key()  const { return sysio::sha256(finalizer_key_binary.data(), finalizer_key_binary.size()); }

      bool is_active(uint64_t finalizer_active_key_id) const { return id == finalizer_active_key_id ; }

      SYSLIB_SERIALIZE( finalizer_key_info, (id)(finalizer_name)(finalizer_key)(finalizer_key_binary) )
   };
   using finalizer_keys_table = sysio::kv::table<
      "finkeys"_n, finkey_key_t, finalizer_key_info,
      sysio::kv::index<"byfinname"_n, const_mem_fun<finalizer_key_info, uint64_t, &finalizer_key_info::by_fin_name>>,
      sysio::kv::index<"byfinkey"_n, const_mem_fun<finalizer_key_info, checksum256, &finalizer_key_info::by_fin_key>>
   >;

   struct finalizer_key_t {
      uint64_t finalizer_name;
      SYSLIB_SERIALIZE(finalizer_key_t, (finalizer_name))
   };

   // finalizer_info stores information about a finalizer.
   struct [[sysio::table("finalizers"), sysio::contract("sysio.system")]] finalizer_info {
      name              finalizer_name;           // finalizer's name
      uint64_t          active_key_id;            // finalizer's active finalizer key's id in finalizer_keys_table, for fast finding key information
      std::vector<char> active_key_binary;        // finalizer's active finalizer key's binary format in Affine little endian non-montgomery g1
      uint32_t          finalizer_key_count = 0;  // number of finalizer keys registered by this finalizer

      SYSLIB_SERIALIZE( finalizer_info, (finalizer_name)(active_key_id)(active_key_binary)(finalizer_key_count) )
   };
   using finalizers_table = sysio::kv::table< "finalizers"_n, finalizer_key_t, finalizer_info >;

   // finalizer_auth_info stores a finalizer's key id and its finalizer authority
   struct finalizer_auth_info {
      finalizer_auth_info() = default;
      explicit finalizer_auth_info(const finalizer_info& finalizer);

      uint64_t                   key_id;        // A finalizer's key ID in finalizer_keys_table
      sysio::finalizer_authority fin_authority; // The finalizer's finalizer_authority

      bool operator==(const finalizer_auth_info& other) const {
         // Weight and description can never be changed by a user.
         // They are not considered here.
         return key_id == other.key_id &&
                fin_authority.public_key == other.fin_authority.public_key;
      };

      SYSLIB_SERIALIZE( finalizer_auth_info, (key_id)(fin_authority) )
   };

   // A single entry storing information about last proposed finalizers.
   struct [[sysio::table("lastpropfins"), sysio::contract("sysio.system")]] last_prop_finalizers_info {
      std::vector<finalizer_auth_info> last_proposed_finalizers; // sorted by ascending finalizer key id

      SYSLIB_SERIALIZE( last_prop_finalizers_info, (last_proposed_finalizers) )
   };

   using last_prop_fins_global = sysio::kv::global< "lastpropfins"_n, last_prop_finalizers_info >;

   // A single entry storing next available finalizer key_id to make sure
   // key_id in finalizers_table will never be reused.
   struct [[sysio::table("finkeyidgen"), sysio::contract("sysio.system")]] fin_key_id_generator_info {
      uint64_t next_finalizer_key_id = 0;

      SYSLIB_SERIALIZE( fin_key_id_generator_info, (next_finalizer_key_id) )
   };

   using fin_key_id_gen_global = sysio::kv::global< "finkeyidgen"_n, fin_key_id_generator_info >;

   /**
    * What the `global` singleton reads as on a chain where the row has never been written.
    *
    * A free function rather than a member because global_state_singleton names it as a template
    * argument, and that alias has to exist before system_contract is declared.
    */
   sysio_global_state default_global_state();

   // cached_global, not global: onblock reads this three times and mutates it twice, then calls
   // update_ranked_producers which mutates it twice more, so caching collapses one action's worth
   // of traffic into a single kv_get and a single kv_set. A plain global forced an unconditional
   // write from ~system_contract(), which made EVERY action -- including the read-only view
   // actions -- fail inside a read-only transaction with "cannot store a KV record when executing
   // a readonly transaction".
   //
   // The defaults ride on the type. That is what lets the handle be a plain member with nothing to
   // initialize in the constructor body: only five functions touch this singleton at all, and every
   // other action now neither reads the row nor computes the defaults. An action that only reads it
   // still owes no write.
   using global_state_singleton =
      sysio::kv::cached_global< "global"_n, sysio_global_state, &default_global_state >;

   /**
    * The `sysio.system` smart contract is provided by `Wire.Network` as a sample system contract, and it defines the
    * structures and actions needed for blockchain's core functionality.
    *
    * Just like in the `sysio.bios` sample contract implementation, there are a few actions which are not implemented
    * at the contract level (`newaccount`, `updateauth`, `deleteauth`, `linkauth`, `unlinkauth`, `setabi`, `setcode`),
    * they are just declared in the contract so they will show in the contract's ABI and users will be able to push
    * those actions to the chain via the account holding the `sysio.system` contract, but the implementation is at the
    * SYSIO core level. They are referred to as SYSIO native actions.
    *
    */
   class [[sysio::contract("sysio.system")]] system_contract : public native {

      private:
         producers_table          _producers;
         finalizer_keys_table     _finalizer_keys;
         finalizers_table         _finalizers;
         last_prop_fins_global    _last_prop_finalizers;
         std::optional<std::vector<finalizer_auth_info>> _last_prop_finalizers_cached;
         fin_key_id_gen_global    _fin_key_id_generator;
         global_state_singleton   _global;

      public:
         static constexpr sysio::name active_permission{"active"_n};
         static constexpr sysio::name token_account{"sysio.token"_n};
         static constexpr sysio::name null_account{"sysio.null"_n};

         system_contract( name s, name code, datastream<const char*> ds );

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
          * balance and can be withdrawn over time. The action also populates the blockinfo table.
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
          * Set producers action, sets a new list of active producers, by proposing a schedule change, once the block that
          * contains the proposal becomes irreversible, the schedule is promoted to "pending"
          * automatically. Once the block that promotes the schedule is irreversible, the schedule will
          * become "active".
          *
          * @param schedule - New list of active producers to set
          */
         [[sysio::action]]
         void setprodkeys( const std::vector<sysio::producer_key>& schedule );

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
          * @note Registration alone does not schedule the producer. To be placed
          *       in the active schedule the account must also be an ACTIVE
          *       OPERATOR_TYPE_PRODUCER operator in sysio.opreg (i.e. have posted
          *       the required slashable collateral). Eligibility is enforced when
          *       the schedule is built, so withdrawing that collateral -- or
          *       being slashed or terminated -- drops the producer.
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
          * @note Registration alone does not schedule the producer. To be placed
          *       in the active schedule the account must also be an ACTIVE
          *       OPERATOR_TYPE_PRODUCER operator in sysio.opreg (i.e. have posted
          *       the required slashable collateral). Eligibility is enforced when
          *       the schedule is built, so withdrawing that collateral -- or
          *       being slashed or terminated -- drops the producer.
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
          * Install the producer-score weights.
          *
          * Each weight scales one normalised factor of the composite score that orders the
          * `prodrank` index; a weight of 0 removes that factor's influence entirely, which is how
          * `relay` / `api` / `benchmark` ship until an attestation path exists for them. Changing a
          * weight invalidates every stored `rank_score`, so this bumps the global's rescore
          * generation and `onblock` drains the cursor.
          *
          * @param weights - the full weight set plus the demotion threshold.
          *
          * @pre Require the authority of the contract itself
          */
         [[sysio::action]]
         void setscorecfg( const producer_rank::producer_score_config& weights );

         /**
          * Action to register a finalizer key by a registered producer.
          * If this was registered before (and still exists) even
          * by other block producers, the registration will fail.
          * If this is the first registered finalizer key of the producer,
          * it will also implicitly be marked active.
          * A registered producer can have multiple registered finalizer keys.
          *
          * @param finalizer_name - account registering `finalizer_key`,
          * @param finalizer_key - key to be registered. The key is in base64url format.
          * @param proof_of_possession - a valid Proof of Possession signature to show the producer owns the private key of the finalizer_key. The signature is in base64url format.
          *
          * @pre `finalizer_name` must be a registered producer
          * @pre `finalizer_key` must be in base64url format
          * @pre `proof_of_possession` must be a valid of proof of possession signature
          * @pre Authority of `finalizer_name` to register. `linkauth` may be used to allow a lower authrity to exectute this action.
          */
         [[sysio::action]]
         void regfinkey( const name& finalizer_name, const std::string& finalizer_key, const std::string& proof_of_possession);

         /**
          * Action to activate a finalizer key. If the finalizer is currently an
          * active block producer (in top 21), then immediately change Finalizer Policy.
          * A finalizer may only have one active finalizer key. Activating a
          * finalizer key implicitly deactivates the previously active finalizer
          * key of that finalizer.
          *
          * @param finalizer_name - account activating `finalizer_key`,
          * @param finalizer_key - key to be activated.
          *
          * @pre `finalizer_key` must be a registered finalizer key in base64url format
          * @pre Authority of `finalizer_name`
          */
         [[sysio::action]]
         void actfinkey( const name& finalizer_name, const std::string& finalizer_key );

         /**
          * Action to delete a finalizer key. An active finalizer key may not be deleted
          * unless it is the last registered finalizer key. If it is the last one,
          * it will be deleted.
          *
          * @param finalizer_name - account deleting `finalizer_key`,
          * @param finalizer_key - key to be deleted.
          *
          * @pre `finalizer_key` must be a registered finalizer key in base64url format
          * @pre `finalizer_key` must not be active, unless it is the last registered finalizer key
          * @pre Authority of `finalizer_name`
          */
         [[sysio::action]]
         void delfinkey( const name& finalizer_name, const std::string& finalizer_key );

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

         /**
          * Sets the WebAssembly limits.  Valid parameters are "low",
          * "default" (equivalent to low), and "high".  A value of "high"
          * allows larger contracts to be deployed.
          */
         [[sysio::action]]
         void wasmcfg( const name& settings );

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

         /**
          * Rescore a producer whose collateral standing just changed on sysio.opreg.
          *
          * `sysio.opreg::processprod` notifies this contract on every producer balance change --
          * not only on an eligibility transition -- because producer rank is scored on the
          * collateral actually posted: a top-up must raise the score and a withdraw must lower it.
          * The handler recomputes from authoritative tables, so it needs no argument beyond the
          * account and asserts no authority of its own: `require_recipient` delivers it only from
          * sysio.opreg, and its sole effect is to bring a derived value back in step.
          */
         [[sysio::on_notify("sysio.opreg::processprod")]]
         void onprocessprod( name account, bool was_eligible, bool is_eligible );

         // ---- Emissions actions (defined in emissions.cpp) ----

         /**
          * Set or update emission configuration parameters.
          * Must be called before any other emissions actions.
          */
         [[sysio::action]]
         void setemitcfg(const emissions::emission_config& cfg);

         /**
          * Sets the starting time for Node Owner distributions.
          */
         [[sysio::action]]
         void setinittime(const sysio::time_point_sec& no_reward_init_time);

         /**
          * Called inline by sysio.roa when a Node Owner is registered.
          */
         [[sysio::action]]
         void addnodeowner(const sysio::name& account_name, uint8_t tier);

         /**
          * Claim vested Node Owner distribution.
          */
         [[sysio::action]]
         void claimnodedis(const sysio::name& account_name);

         /**
          * Claim epoch pay credited by payepoch — a producer, standby or batch-operator share.
          * Drains the caller's `payclaims` row and transfers the whole balance out.
          *
          * payepoch credits rather than transfers because it runs inline from
          * sysio.epoch::advance: `sysio.token::transfer` notifies the recipient, and a recipient
          * whose notify handler aborts (or burns CPU) would abort advance and stall epoch
          * advancement chain-wide. Moving the transfer here puts it under the claimant's own
          * authority, so a hostile recipient can only block its own payout.
          *
          * The T5 category buckets (`sysio.ops` capex, `sysio.gov` governance) are NOT credited
          * here — payepoch transfers to them directly. A claim needs `require_auth(account_name)`
          * and neither can ever produce it: `sysio.roa` forces `net_weight`/`cpu_weight` to zero
          * for every `sysio`-prefixed account, so they cannot pay for a transaction, and unlike
          * `sysio.dclaim` they carry no contract that could emit the claim inline. They are
          * protocol-owned holding accounts with no code, so the notify-handler threat the pull
          * model defends against does not exist for them. See the note at the push site in
          * emissions.cpp for the standing constraint that keeps it that way.
          *
          * Auth: the claiming account.
          */
         [[sysio::action]]
         void claimpay(const sysio::name& account_name);

         /**
          * Read-only: view claimable Node Owner distributions.
          */
         [[sysio::action, sysio::read_only]]
         emissions::node_claim_result viewnodedist(const sysio::name& account_name);

         /**
          * Initialize the T5 treasury emissions system.
          */
         [[sysio::action]]
         void initt5(const sysio::time_point_sec& start_time);

         /**
          * Pay emissions for the given sysio.epoch index. Called inline by
          * sysio.epoch::advance on a pay-epoch (i.e., the period boundary
          * defined by emit_cfg.pay_cadence_epochs). Auth: require_auth(
          * "sysio.epoch").
          *
          * `period_emission` is the gate-computed sum of pending accrued
          * emissions plus this epoch's per-epoch share. payepoch trusts that
          * value (single-trx semantics make recomputation unnecessary) and
          * distributes it across producer / batch / capital / capex / gov
          * pools as today, scaled to the period.
          *
          * `batch_op_groups` is the full state.batch_op_groups vector from
          * sysio.epoch; payepoch reads t5state.batch_group_epochs to weight
          * the batch pool proportionally to each group's active-epoch count
          * over the period, normalized by the ACTUAL accrued-epoch count (the
          * sum of those counters) rather than the configured
          * pay_cadence_epochs, which a mid-period setemitcfg change or the
          * shortened genesis period can make disagree. Groups active in zero
          * epochs are skipped, which happens whenever the accrued count is
          * smaller than batch_op_groups.size(); skipping costs them nothing,
          * since a zero count already weights their allocation to zero.
          *
          * Runtime conditions (config missing, treasury exhausted, balance
          * insufficient) are caught upstream by the gate, which records the
          * block in sysio.epoch's blocklog and prevents advance from
          * proceeding.
          */
         [[sysio::action]]
         void payepoch(uint32_t epoch_index,
                       std::vector<std::vector<sysio::name>> batch_op_groups,
                       int64_t period_emission);

         /**
          * Accrue this epoch's per-epoch emission share onto t5state, without
          * paying. Called inline by sysio.epoch::advance on EVERY successful
          * epoch — including a pay epoch, where advance queues this action
          * FIRST and payepoch after it, so FIFO inline ordering means payepoch
          * observes the post-accrue state. Auth: require_auth("sysio.epoch").
          *
          * Increments t5state.pending_emission_amount by `per_epoch_emission`
          * and bumps t5state.batch_group_epochs[batch_group_index] by 1, so
          * the next payepoch sees the period total + per-group counts.
          *
          * Because it also runs on the pay epoch, the counter sum that
          * `payepoch` normalizes by includes the epoch being paid. Reading this
          * as "non-pay epochs only" understates that sum by one and is how the
          * configured-cadence divisor came to look correct.
          *
          * No transfers happen here. Treasury / balance gating is the
          * gate's responsibility upstream.
          */
         [[sysio::action]]
         void accrueepoch(uint32_t epoch_index,
                          uint8_t  batch_group_index,
                          int64_t  per_epoch_emission);

         /**
          * Fund a sysio.dclaim capital draw against the T5 drainable pool.
          * Called inline by sysio.dclaim::onreward as each STAKING_REWARD
          * lands, so dclaim is funded the moment the claim ledger row is
          * written and the staker can claim immediately. Auth: dclaim.
          *
          * Never throws (OPP-handler never-throw contract): if the pool
          * cannot cover `amount`, the transfer caps at what's available
          * and the unfunded delta is accrued to t5state.capital_shortfall_total
          * for operator visibility.
          *
          * Amounts actually transferred count toward t5state.total_distributed
          * so the emission curve auto-throttles via its remaining-headroom
          * clamp.
          */
         [[sysio::action]]
         void fundclaim(int64_t amount);

         /**
          * Read-only: current T5 treasury emission state.
          */
         [[sysio::action, sysio::read_only]]
         emissions::epoch_info_result viewepoch();

         /**
          * Read-only: all emission configuration values.
          */
         [[sysio::action, sysio::read_only]]
         emissions::emission_config viewemitcfg();

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

         // defined in voting.cpp
         void register_producer( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location );
         void update_ranked_producers( const block_timestamp& timestamp );

         /// Recompute and store one producer's packed `rank_score`. Called from every path that can
         /// move a scoring input: regproducer (tier clear), the opreg eligibility notification
         /// (collateral), onblock (miss counter), and the rescore sweep.
         void rescore_producer( const name& producer );

         /// Attribute missed rounds to the producers the active schedule skipped, and demote any
         /// that crossed the threshold. Runs on every block; see producer_pay.cpp.
         void record_round_participation( const name& current_producer );

         /// Charge one producer a missed round, demoting it if that crosses the threshold.
         void record_missed_round( const name& producer, uint32_t max_consecutive_missed_rounds );

         /// Open a rescore sweep when sysio.opreg's producer collateral minimums have moved since
         /// the stored scores were computed. See `sysio_global_state::scored_collateral_stamp`.
         void detect_collateral_config_change();

         /// Drain a bounded slice of the rescore cursor when weights or collateral minimums changed.
         void drain_rescore_cursor();

         // defined in sysio.system.cpp

         // defined in block_info.cpp
         void add_to_blockinfo_table(const sysio::checksum256& previous_block_id, const sysio::block_timestamp timestamp) const;

         // defined in finalizer_key.cpp
         bool is_savanna_consensus();
         void set_proposed_finalizers( std::vector<finalizer_auth_info> finalizers );
         const std::vector<finalizer_auth_info>& get_last_proposed_finalizers();
         uint64_t get_next_finalizer_key_id();
         finalizer_info get_finalizer( const name& finalizer_name ) const;
   };

}

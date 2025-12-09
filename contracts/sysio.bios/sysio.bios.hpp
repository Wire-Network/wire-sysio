#pragma once

#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/sysio.hpp>
#include <sysio/fixed_bytes.hpp>
#include <sysio/privileged.hpp>
#include <sysio/producer_schedule.hpp>

/**
 * SYSIO Contracts
 *
 * @details The design of the SYSIO blockchain calls for a number of smart contracts that are run at a
 * privileged permission level in order to support functions such as block producer registration and
 * voting, token staking for CPU and network bandwidth, RAM purchasing, multi-sig, etc. These smart
 * contracts are referred to as the system, token, msig and wrap (formerly known as sudo) contracts.
 *
 * This repository contains examples of these privileged contracts that are useful when deploying,
 * managing, and/or using an SYSIO blockchain. They are provided for reference purposes:
 * - sysio.bios
 * - sysio.system
 * - sysio.msig
 * - sysio.wrap
 *
 * The following unprivileged contract(s) are also part of the system.
 * - sysio.token
 */

namespace sysiobios {

   using sysio::action_wrapper;
   using sysio::check;
   using sysio::checksum256;
   using sysio::ignore;
   using sysio::name;
   using sysio::permission_level;
   using sysio::public_key;

   /**
    * A weighted permission.
    *
    * @details Defines a weighted permission, that is a permission which has a weight associated.
    * A permission is defined by an account name plus a permission name. The weight is going to be
    * used against a threshold, if the weight is equal or greater than the threshold set then authorization
    * will pass.
    */
   struct permission_level_weight {
      permission_level  permission;
      uint16_t          weight;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE( permission_level_weight, (permission)(weight) )
   };

   /**
    * Weighted key.
    *
    * @details A weighted key is defined by a public key and an associated weight.
    */
   struct key_weight {
      sysio::public_key  key;
      uint16_t           weight;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE( key_weight, (key)(weight) )
   };

   /**
    * Wait weight.
    *
    * @details A wait weight is defined by a number of seconds to wait for and a weight.
    */
   struct wait_weight {
      uint32_t           wait_sec;
      uint16_t           weight;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE( wait_weight, (wait_sec)(weight) )
   };

   /**
    * Blockchain authority.
    *
    * @details An authority is defined by:
    * - a vector of key_weights (a key_weight is a public key plus a weight),
    * - a vector of permission_level_weights, (a permission_level is an account name plus a permission name)
    * - a vector of wait_weights (a wait_weight is defined by a number of seconds to wait and a weight)
    * - a threshold value
    */
   struct authority {
      uint32_t                              threshold = 0;
      std::vector<key_weight>               keys;
      std::vector<permission_level_weight>  accounts;
      std::vector<wait_weight>              waits;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE( authority, (threshold)(keys)(accounts)(waits) )
   };

   /**
    * Blockchain block header.
    *
    * @details A block header is defined by:
    * - a timestamp,
    * - the producer that created it,
    * - a confirmed flag default as zero,
    * - a link to previous block,
    * - a link to the transaction merkel root,
    * - a link to action root,
    * - a schedule version,
    * - and a producers' schedule.
    */
   struct block_header {
      uint32_t                                  timestamp;
      name                                      producer;
      uint16_t                                  confirmed = 0;
      checksum256                               previous;
      checksum256                               transaction_mroot;
      checksum256                               action_mroot;
      uint32_t                                  schedule_version = 0;
      std::optional<sysio::producer_schedule>   new_producers;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE(block_header, (timestamp)(producer)(confirmed)(previous)(transaction_mroot)(action_mroot)
                                     (schedule_version)(new_producers))
   };

   struct finalizer_authority {
      std::string   description;
      uint64_t      weight = 0;  // weight that this finalizer's vote has for meeting threshold
      std::string   public_key;  // public key of the finalizer in base64 format
      std::string   pop;         // proof of possession of private key in base64 format

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE(finalizer_authority, (description)(weight)(public_key)(pop))
   };

   constexpr size_t max_finalizers = 64*1024;
   constexpr size_t max_finalizer_description_size = 256;

   /**
    * finalizer_policy
    *
    * List of finalizer authorties along with the threshold
    */
   struct finalizer_policy {
      uint64_t                         threshold = 0; // quorum threshold
      std::vector<finalizer_authority> finalizers;

      // explicit serialization macro is not necessary, used here only to improve compilation time
      SYSLIB_SERIALIZE(finalizer_policy, (threshold)(finalizers));
   };

   /**
    * @defgroup sysiobios sysio.bios
    * @ingroup sysiocontracts
    *
    * sysio.bios is a minimalistic system contract that only supplies the actions that are absolutely
    * critical to bootstrap a chain and nothing more.
    *
    * @{
    */
   class [[sysio::contract("sysio.bios")]] bios : public sysio::contract {
      public:
         using contract::contract;
         /**
          * @{
          * These actions map one-on-one with the ones defined in
          * [Native Action Handlers](@ref native_action_handlers) section.
          * They are present here so they can show up in the abi file and thus user can send them
          * to this contract, but they have no specific implementation at this contract level,
          * they will execute the implementation at the core level and nothing else.
          */
         /**
          * New account action
          *
          * @details Called after a new account is created. This code enforces resource-limits rules
          * for new accounts as well as new account naming conventions.
          *
          * 1. accounts cannot contain '.' symbols which forces all acccounts to be 12
          * characters long without '.' until a future account auction process is implemented
          * which prevents name squatting.
          *
          * 2. new accounts must stake a minimal number of tokens (as set in system parameters)
          * therefore, this method will execute an inline buyram from receiver for newacnt in
          * an amount equal to the current new account creation fee.
          */
         [[sysio::action]]
         void newaccount( name             creator,
                          name             name,
                          ignore<authority> owner,
                          ignore<authority> active){}
         /**
          * Update authorization action.
          *
          * @details Updates pemission for an account.
          *
          * @param account - the account for which the permission is updated,
          * @param pemission - the permission name which is updated,
          * @param parem - the parent of the permission which is updated,
          * @param aut - the json describing the permission authorization.
          */
         [[sysio::action]]
         void updateauth(  ignore<name>  account,
                           ignore<name>  permission,
                           ignore<name>  parent,
                           ignore<authority> auth ) {}

         /**
          * Delete authorization action.
          *
          * @details Deletes the authorization for an account's permision.
          *
          * @param account - the account for which the permission authorization is deleted,
          * @param permission - the permission name been deleted.
          */
         [[sysio::action]]
         void deleteauth( ignore<name>  account,
                          ignore<name>  permission ) {}

         /**
          * Link authorization action.
          *
          * @details Assigns a specific action from a contract to a permission you have created. Five system
          * actions can not be linked `updateauth`, `deleteauth`, `linkauth`, and `unlinkauth`.
          * This is useful because when doing authorization checks, the SYSIO based blockchain starts with the
          * action needed to be authorized (and the contract belonging to), and looks up which permission
          * is needed to pass authorization validation. If a link is set, that permission is used for authoraization
          * validation otherwise then active is the default, with the exception of `sysio.any`.
          * `sysio.any` is an implicit permission which exists on every account; you can link actions to `sysio.any`
          * and that will make it so linked actions are accessible to any permissions defined for the account.
          *
          * @param account - the permission's owner to be linked and the payer of the RAM needed to store this link,
          * @param code - the owner of the action to be linked,
          * @param type - the action to be linked,
          * @param requirement - the permission to be linked.
          */
         [[sysio::action]]
         void linkauth(  ignore<name>    account,
                         ignore<name>    code,
                         ignore<name>    type,
                         ignore<name>    requirement  ) {}

         /**
          * Unlink authorization action.
          *
          * @details It's doing the reverse of linkauth action, by unlinking the given action.
          *
          * @param account - the owner of the permission to be unlinked and the receiver of the freed RAM,
          * @param code - the owner of the action to be unlinked,
          * @param type - the action to be unlinked.
          */
         [[sysio::action]]
         void unlinkauth( ignore<name>  account,
                          ignore<name>  code,
                          ignore<name>  type ) {}

         /**
          * Set code action.
          *
          * @details Sets the contract code for an account.
          *
          * @param account - the account for which to set the contract code.
          * @param vmtype - reserved, set it to zero.
          * @param vmversion - reserved, set it to zero.
          * @param code - the code content to be set, in the form of a blob binary..
          */
         [[sysio::action]]
         void setcode( name account, uint8_t vmtype, uint8_t vmversion, const std::vector<char>& code ) {}

         /** @}*/

         /**
          * Set abi for contract.
          *
          * @details Set the abi for contract identified by `account` name. Creates an entry in the abi_hash_table
          * index, with `account` name as key, if it is not already present and sets its value with the abi hash.
          * Otherwise it is updating the current abi hash value for the existing `account` key.
          *
          * @param account - the name of the account to set the abi for
          * @param abi     - the abi hash represented as a vector of characters
          */
         [[sysio::action]]
         void setabi( name account, const std::vector<char>& abi );

         /**
          * Set privilege status for an account.
          *
          * @details Allows to set privilege status for an account (turn it on/off).
          * @param account - the account to set the privileged status for.
          * @param is_priv - 0 for false, > 0 for true.
          */
         [[sysio::action]]
         void setpriv( name account, uint8_t is_priv );

         /**
          * Set the resource limits of an account
          *
          * @details Set the resource limits of an account
          *
          * @param account - name of the account whose resource limit to be set
          * @param ram_bytes - ram limit in absolute bytes
          * @param net_weight - fractionally proportionate net limit of available resources based on (weight / total_weight_of_all_accounts)
          * @param cpu_weight - fractionally proportionate cpu limit of available resources based on (weight / total_weight_of_all_accounts)
          */
         [[sysio::action]]
         void setalimits( name account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight );

         /**
          * Set a new list of active producers, that is, a new producers' schedule.
          *
          * @details Set a new list of active producers, by proposing a schedule change, once the block that
          * contains the proposal becomes irreversible, the schedule is promoted to "pending"
          * automatically. Once the block that promotes the schedule is irreversible, the schedule will
          * become "active".
          *
          * @param schedule - New list of active producers to set
          */
         [[sysio::action]]
         void setprods( const std::vector<sysio::producer_authority>& schedule );

         /**
          * Set a new list of active producers, that is, a new producers' schedule.
          *
          * @details Set a new list of active producers, by proposing a schedule change, once the block that
          * contains the proposal becomes irreversible, the schedule is promoted to "pending"
          * automatically. Once the block that promotes the schedule is irreversible, the schedule will
          * become "active".
          *
          * @param schedule - New list of active producers to set
          */
         [[sysio::action]]
         void setprodkeys( const std::vector<sysio::producer_key>& schedule );

         /**
          * Propose new finalizer policy that, unless superseded by a later
          * finalizer policy, will eventually become the active finalizer policy.
          *
          * @param finalizer_policy - proposed finalizer policy
          */
         [[sysio::action]]
         void setfinalizer( const finalizer_policy& finalizer_policy );

         /**
          * Set the blockchain parameters
          *
          * @details Set the blockchain parameters. By tuning these parameters, various degrees of customization can be achieved.
          *
          * @param params - New blockchain parameters to set
          */
         [[sysio::action]]
         void setparams( const sysio::blockchain_parameters& params );

         /**
          * Check if an account has authorization to access current action.
          *
          * @details Checks if the account name `from` passed in as param has authorization to access
          * current action, that is, if it is listed in the action’s allowed permissions vector.
          *
          * @param from - the account name to authorize
          */
         [[sysio::action]]
         void reqauth( name from );

         /**
          * Activates a protocol feature.
          *
          * @details Activates a protocol feature
          *
          * @param feature_digest - hash of the protocol feature to activate.
          */
         [[sysio::action]]
         void activate( const sysio::checksum256& feature_digest );

         /**
          * Asserts that a protocol feature has been activated.
          *
          * @details Asserts that a protocol feature has been activated
          *
          * @param feature_digest - hash of the protocol feature to check for activation.
          */
         [[sysio::action]]
         void reqactivated( const sysio::checksum256& feature_digest );

         /**
          * Abi hash structure
          *
          * @details Abi hash structure is defined by contract owner and the contract hash.
          */
         struct [[sysio::table]] abi_hash {
            name              owner;
            checksum256       hash;
            uint64_t primary_key()const { return owner.value; }

            SYSLIB_SERIALIZE( abi_hash, (owner)(hash) )
         };

         /**
          * Multi index table that stores the contracts' abi index by their owners/accounts.
          */
         typedef sysio::multi_index< "abihash"_n, abi_hash > abi_hash_table;

         using newaccount_action = action_wrapper<"newaccount"_n, &bios::newaccount>;
         using updateauth_action = action_wrapper<"updateauth"_n, &bios::updateauth>;
         using deleteauth_action = action_wrapper<"deleteauth"_n, &bios::deleteauth>;
         using linkauth_action = action_wrapper<"linkauth"_n, &bios::linkauth>;
         using unlinkauth_action = action_wrapper<"unlinkauth"_n, &bios::unlinkauth>;
         using setcode_action = action_wrapper<"setcode"_n, &bios::setcode>;
         using setabi_action = action_wrapper<"setabi"_n, &bios::setabi>;
         using setpriv_action = action_wrapper<"setpriv"_n, &bios::setpriv>;
         using setalimits_action = action_wrapper<"setalimits"_n, &bios::setalimits>;
         using setprods_action = action_wrapper<"setprods"_n, &bios::setprods>;
         using setfinalizer_action = action_wrapper<"setfinalizer"_n, &bios::setfinalizer>;
         using setparams_action = action_wrapper<"setparams"_n, &bios::setparams>;
         using reqauth_action = action_wrapper<"reqauth"_n, &bios::reqauth>;
         using activate_action = action_wrapper<"activate"_n, &bios::activate>;
         using reqactivated_action = action_wrapper<"reqactivated"_n, &bios::reqactivated>;
   };
   /** @}*/ // end of @defgroup sysiobios sysio.bios
} /// namespace sysiobios

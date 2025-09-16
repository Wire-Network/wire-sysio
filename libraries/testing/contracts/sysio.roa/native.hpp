#pragma once

#include <sysio/action.hpp>
#include <sysio/binary_extension.hpp>
#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/fixed_bytes.hpp>
#include <sysio/ignore.hpp>
#include <sysio/print.hpp>
#include <sysio/privileged.hpp>
#include <sysio/producer_schedule.hpp>

namespace sysiosystem {

   using sysio::binary_extension;
   using sysio::checksum256;
   using sysio::ignore;
   using sysio::name;
   using sysio::permission_level;
   using sysio::public_key;

   /**
    * A weighted permission.
    *
    * Defines a weighted permission, that is a permission which has a weight associated.
    * A permission is defined by an account name plus a permission name.
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
    * A weighted key is defined by a public key and an associated weight.
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
    * A wait weight is defined by a number of seconds to wait for and a weight.
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
    * An authority is defined by:
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
    * A block header is defined by:
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

   /**
    * abi_hash is the structure underlying the abihash table and consists of:
    * - `owner`: the account owner of the contract's abi
    * - `hash`: is the sha256 hash of the abi/binary
    */
   struct [[sysio::table("abihash"), sysio::contract("sysio.system")]] abi_hash {
      name              owner;
      checksum256       hash;
      uint64_t primary_key()const { return owner.value; }

      SYSLIB_SERIALIZE( abi_hash, (owner)(hash) )
   };

   void check_auth_change(name contract, name account, const binary_extension<name>& authorized_by);

   // Method parameters commented out to prevent generation of code that parses input data.
   /**
    * The SYSIO core `native` contract that governs authorization and contracts' abi.
    */
   class [[sysio::contract("sysio.system")]] native : public sysio::contract {
      public:

         using sysio::contract::contract;

         /**
          * These actions map one-on-one with the ones defined in core layer of SYSIO, that's where their implementation
          * actually is done.
          * They are present here only so they can show up in the abi file and thus user can send them
          * to this contract, but they have no specific implementation at this contract level,
          * they will execute the implementation at the core layer and nothing else.
          */

         /**
          * New account action is called after a new account is created. This code enforces resource-limits rules
          * for new accounts as well as new account naming conventions.
          */
         [[sysio::action]]
         void newaccount( const name&       creator,
                          const name&       name,
                          ignore<authority> owner,
                          ignore<authority> active);

         /**
          * Update authorization action updates permission for an account.
          *
          * This contract enforces additional rules:
          *
          * 1. If authorized_by is present and not "", then the contract does a
          *    require_auth2(account, authorized_by).
          * 2. If the account has opted into limitauthchg, then authorized_by
          *    must be present and not "".
          * 3. If the account has opted into limitauthchg, and allow_perms is
          *    not empty, then authorized_by must be in the array.
          * 4. If the account has opted into limitauthchg, and disallow_perms is
          *    not empty, then authorized_by must not be in the array.
          *
          * @param account - the account for which the permission is updated
          * @param permission - the permission name which is updated
          * @param parent - the parent of the permission which is updated
          * @param auth - the json describing the permission authorization
          * @param authorized_by - the permission which is authorizing this change
          */
         [[sysio::action]]
         void updateauth( name                   account,
                          name                   permission,
                          name                   parent,
                          authority              auth,
                          binary_extension<name> authorized_by ) {
            check_auth_change(get_self(), account, authorized_by);
         }

         /**
          * Delete authorization action deletes the authorization for an account's permission.
          *
          * This contract enforces additional rules:
          *
          * 1. If authorized_by is present and not "", then the contract does a
          *    require_auth2(account, authorized_by).
          * 2. If the account has opted into limitauthchg, then authorized_by
          *    must be present and not "".
          * 3. If the account has opted into limitauthchg, and allow_perms is
          *    not empty, then authorized_by must be in the array.
          * 4. If the account has opted into limitauthchg, and disallow_perms is
          *    not empty, then authorized_by must not be in the array.
          *
          * @param account - the account for which the permission authorization is deleted,
          * @param permission - the permission name been deleted.
          * @param authorized_by - the permission which is authorizing this change
          */
         [[sysio::action]]
         void deleteauth( name                   account,
                          name                   permission,
                          binary_extension<name> authorized_by ) {
            if (permission == name("auth.ext")) require_recipient(name("auth.msg")); // Sig EM auth.ext catch: only auth.msg can remove auth.ext permission
            else check_auth_change(get_self(), account, authorized_by);
         }

         /**
          * Link authorization action assigns a specific action from a contract to a permission you have created. Five system
          * actions can not be linked `updateauth`, `deleteauth`, `linkauth`, `unlinkauth`, and `canceldelay`.
          * This is useful because when doing authorization checks, the SYSIO based blockchain starts with the
          * action needed to be authorized (and the contract belonging to), and looks up which permission
          * is needed to pass authorization validation. If a link is set, that permission is used for authorization
          * validation otherwise then active is the default, with the exception of `sysio.any`.
          * `sysio.any` is an implicit permission which exists on every account; you can link actions to `sysio.any`
          * and that will make it so linked actions are accessible to any permissions defined for the account.
          *
          * This contract enforces additional rules:
          *
          * 1. If authorized_by is present and not "", then the contract does a
          *    require_auth2(account, authorized_by).
          * 2. If the account has opted into limitauthchg, then authorized_by
          *    must be present and not "".
          * 3. If the account has opted into limitauthchg, and allow_perms is
          *    not empty, then authorized_by must be in the array.
          * 4. If the account has opted into limitauthchg, and disallow_perms is
          *    not empty, then authorized_by must not be in the array.
          *
          * @param account - the permission's owner to be linked and the payer of the RAM needed to store this link,
          * @param code - the owner of the action to be linked,
          * @param type - the action to be linked,
          * @param requirement - the permission to be linked.
          * @param authorized_by - the permission which is authorizing this change
          */
         [[sysio::action]]
         void linkauth( name                   account,
                        name                   code,
                        name                   type,
                        name                   requirement,
                        binary_extension<name> authorized_by ) {
            check_auth_change(get_self(), account, authorized_by);
         }

         /**
          * Unlink authorization action it's doing the reverse of linkauth action, by unlinking the given action.
          *
          * This contract enforces additional rules:
          *
          * 1. If authorized_by is present and not "", then the contract does a
          *    require_auth2(account, authorized_by).
          * 2. If the account has opted into limitauthchg, then authorized_by
          *    must be present and not "".
          * 3. If the account has opted into limitauthchg, and allow_perms is
          *    not empty, then authorized_by must be in the array.
          * 4. If the account has opted into limitauthchg, and disallow_perms is
          *    not empty, then authorized_by must not be in the array.
          *
          * @param account - the owner of the permission to be unlinked and the receiver of the freed RAM,
          * @param code - the owner of the action to be unlinked,
          * @param type - the action to be unlinked.
          * @param authorized_by - the permission which is authorizing this change
          */
         [[sysio::action]]
         void unlinkauth( name                   account,
                          name                   code,
                          name                   type,
                          binary_extension<name> authorized_by ) {
            check_auth_change(get_self(), account, authorized_by);
         }

         /**
          * Set abi action sets the contract abi for an account.
          *
          * @param account - the account for which to set the contract abi.
          * @param abi - the abi content to be set, in the form of a blob binary.
          * @param memo - may be omitted
          */
         [[sysio::action]]
         void setabi( const name& account, const std::vector<char>& abi, const binary_extension<std::string>& memo );

         /**
          * Set code action sets the contract code for an account.
          *
          * @param account - the account for which to set the contract code.
          * @param vmtype - reserved, set it to zero.
          * @param vmversion - reserved, set it to zero.
          * @param code - the code content to be set, in the form of a blob binary..
          * @param memo - may be omitted
          */
         [[sysio::action]]
         void setcode( const name& account, uint8_t vmtype, uint8_t vmversion, const std::vector<char>& code,
                       const binary_extension<std::string>& memo ) {}

         using newaccount_action = sysio::action_wrapper<"newaccount"_n, &native::newaccount>;
         using updateauth_action = sysio::action_wrapper<"updateauth"_n, &native::updateauth>;
         using deleteauth_action = sysio::action_wrapper<"deleteauth"_n, &native::deleteauth>;
         using linkauth_action = sysio::action_wrapper<"linkauth"_n, &native::linkauth>;
         using unlinkauth_action = sysio::action_wrapper<"unlinkauth"_n, &native::unlinkauth>;
         using setcode_action = sysio::action_wrapper<"setcode"_n, &native::setcode>;
         using setabi_action = sysio::action_wrapper<"setabi"_n, &native::setabi>;
   };
}

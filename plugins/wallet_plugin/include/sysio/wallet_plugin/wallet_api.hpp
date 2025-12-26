#pragma once

#include <sysio/chain/transaction.hpp>
#include <sysio/chain/types.hpp>

using namespace std;
using namespace sysio::chain;

namespace sysio::wallet {

/**
 * wallet_key_entry defines a tuple with
 * - key alias/name
 * - pub key
 * - priv key
 */
struct wallet_key_entry {

   /** The alias or name assigned to identify this key pair */
   string key_name;
   /** The public key component of this key pair */
   public_key_type public_key;
   /** The private key component of this key pair */
   private_key_type private_key;
};
}

FC_REFLECT(sysio::wallet::wallet_key_entry, (key_name)(public_key)(private_key))


namespace sysio::wallet {


/**
 * The `wallet_api` class provides an interface for securely managing and interacting with a wallet.
 * It includes methods for handling private keys, locking and unlocking, password management,
 * key creation, key importing, and digital signature operations.
 */
class wallet_api {
public:
   virtual ~wallet_api() {}

   /**
    * Get the private key corresponding to a public key.  The
    * private key must already be in the wallet.
    */
   virtual private_key_type get_private_key(public_key_type pubkey) const = 0;

   /**
    * Retrieve `private_key` with key_name
    *
    * @param key_name the name associated with a given key
    * @return the private key associated with the key_name
    */
   virtual private_key_type get_private_key(string key_name) const = 0;

   /**
    * Checks whether the wallet is locked (is unable to use its private keys).
    *
    * This state can be changed by calling \c lock() or \c unlock().
    * @return true if the wallet is locked
    * @ingroup Wallet Management
    */
   virtual bool is_locked() const = 0;

   /**
    * Locks the wallet immediately
    * @ingroup Wallet Management
    */
   virtual void lock() = 0;

   /**
    * Unlocks the wallet.
    *
    * The wallet remain unlocked until the \c lock is called
    * or the program exits.
    * @param password the password previously set with \c set_password()
    * @ingroup Wallet Management
    */
   virtual void unlock(string password) = 0;

   /**
    * Checks the password of the wallet
    *
    * Validates the password on a wallet even if the wallet is already unlocked,
    * throws if bad password given.
    * @param password the password previously set with \c set_password()
    * @ingroup Wallet Management
    */
   virtual void check_password(string password) = 0;

   /**
    * Sets a new password on the wallet.
    *
    * The wallet must be either 'new' or 'unlocked' to
    * execute this command.
    * @ingroup Wallet Management
    */
   virtual void set_password(string password) = 0;

   /**
    * Dumps all private keys owned by the wallet.
    *
    * The keys are printed in WIF format.  You can import these keys into another wallet
    * using \c import_key()
    * @returns a map containing the private keys, indexed by their public key
    */
   virtual map<public_key_type, private_key_type> list_keys() = 0;


   /**
    *
    * @return map containing wallet_key_entry indexed by key name
    */
   virtual map<string, wallet_key_entry> list_keys_by_name() = 0;

   /**
    * Dumps all public keys owned by the wallet.
    * @returns a vector containing the public keys
    */
   virtual flat_set<public_key_type> list_public_keys() = 0;

   /**
    * Imports a WIF Private Key into the wallet to be used to sign transactions by an account.
    *
    * @example: import_key 5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3
    *
    * @note this is a simply shortcut to import_key(key_name, wif_key), which
    *   will use the generate unused alias function internally
    *
    * @param wif_key the WIF Private Key to import
    */
   virtual bool import_key(string wif_key) = 0;

   /**
    * Imports a WIF Private Key into the wallet to be used to sign transactions by an account.
    *
    * @example: import_key my-batch-key 5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3
    *
    * @param key_name a unique name to identify this key
    * @param wif_key the WIF Private Key to import
    */
   virtual bool import_key(string key_name, string wif_key) = 0;

   /**
    * Removes a key from the wallet.
    *
    * @example: remove_key my-batch-key
    *
    * @param key_name the name assigned to the key
    */
   virtual bool remove_key(string key_name) = 0;

   /**
    * Removes a key from the wallet.
    *
    * @example: remove_key SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV
    *
    * @param key the name assigned to the key
    */
   virtual bool remove_key(public_key_type key) = 0;
   /**
    * Creates a key within the wallet to be used to sign transactions by an account.
    *
    * @example: create_key K1
    *
    * @param key_type the key type to create. May be empty to allow wallet to pick appropriate/"best" key type
    */
   virtual string create_key(string key_type) = 0;

   /**
    * Creates a key within the wallet to be used to sign transactions by an account.
    *
    * @example: create_key my-batch-key K1
    *
    * @param key_name a unique name to identify this key
    * @param key_type the key type to create. May be empty to allow wallet to pick appropriate/"best" key type
    */
   virtual string create_key(string key_name, string key_type) = 0;


   /**
    * Updates the key name associated with a key in the wallet.
    *
    * @example: set_key_name my-old-key my-new-key
    *
    * @param current_key_name the current name of the key
    * @param new_key_name the new name to assign to the key
    * @return the new key name if successful
    */
   virtual void set_key_name(string current_key_name, string new_key_name) = 0;

   /**
    * Assigns a name to a private key in the wallet.
    *
    * @param private_key the private key to name
    * @param key_name the name to assign to the key
    * @return the assigned key name if successful
    */
   virtual void set_key_name(private_key_type private_key, string key_name) = 0;

   /**
    * Assigns a name to a public key in the wallet.
    *
    * @param public_key the public key to name
    * @param key_name the name to assign to the key
    * @return the assigned key name if successful
    */
   virtual void set_key_name(public_key_type public_key, string key_name) = 0;

   /**
    * Generate a unique key name
    *
    * As this is for retrieval and reference purposes,
    * there are no restrictions on key-names
    *
    * @param prefix to be prepended
    * @param suffix to be appended
    * @return unique keyname
    */
   virtual string generate_key_name(string prefix = "", string suffix = "") = 0;

   /**
    * Returns a signature given the digest and public_key, if this wallet can sign via that public key
    */
   virtual std::optional<signature_type> try_sign_digest(const digest_type     digest,
                                                         const public_key_type public_key) = 0;
};

} // namespace sysio::wallet

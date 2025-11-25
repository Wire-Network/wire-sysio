#include <sysio/wallet_plugin/wallet.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <list>
#include <ranges>

#include <fc/crypto/elliptic.hpp>
#include <fc/git_revision.hpp>
#include <fc/io/json.hpp>
#include <fc/crypto/aes.hpp>
#include <fc/crypto/hex.hpp>

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>

#ifndef WIN32
# include <sys/types.h>
# include <sys/stat.h>
#include <sysio/chain/exceptions.hpp>

#endif


namespace sysio::wallet::detail {

/**
 * Maximum # of attempts to generate a unique key alias/name
 */
constexpr std::uint32_t max_generate_attempts = 1000;

private_key_type derive_private_key(const std::string& prefix_string,
                                    int                sequence_number) {
   std::string sequence_string = std::to_string(sequence_number);
   fc::sha512  h               = fc::sha512::hash(prefix_string + " " + sequence_string);
   return private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(h));
}

class soft_wallet_impl {
private:
   void enable_umask_protection() {
      #ifdef __unix__
      _old_umask = umask(S_IRWXG | S_IRWXO);
      #endif
   }

   void disable_umask_protection() {
      #ifdef __unix__
      umask(_old_umask);
      #endif
   }

public:
   soft_wallet& self;

   soft_wallet_impl(soft_wallet& s, const wallet_data& initial_data)
      : self(s) {}

   virtual ~soft_wallet_impl() {}

   void encrypt_keys() {
      if (!is_locked()) {
         plain_keys_v1 data;
         data.keys           = _keys;
         data.key_by_name    = _key_by_name;
         data.checksum       = _checksum;
         auto plain_txt      = fc::raw::pack(data);
         _wallet.cipher_keys = fc::aes_encrypt(data.checksum, plain_txt);
         _wallet.version     = 1;
      }
   }

   bool copy_wallet_file(string destination_filename) {
      std::filesystem::path src_path = get_wallet_filename();
      if (!std::filesystem::exists(src_path))
         return false;
      std::filesystem::path dest_path = destination_filename + _wallet_filename_extension;
      int                   suffix    = 0;
      while (std::filesystem::exists(dest_path)) {
         ++suffix;
         dest_path = destination_filename + "-" + std::to_string(suffix) + _wallet_filename_extension;
      }
      wlog("backing up wallet ${src} to ${dest}",
           ("src", src_path)
           ("dest", dest_path));

      std::filesystem::path dest_parent = std::filesystem::absolute(dest_path).parent_path();
      try {
         enable_umask_protection();
         if (!std::filesystem::exists(dest_parent))
            std::filesystem::create_directories(dest_parent);
         std::filesystem::copy_file(src_path, dest_path, std::filesystem::copy_options::none);
         disable_umask_protection();
      } catch (...) {
         disable_umask_protection();
         throw;
      }
      return true;
   }

   bool is_locked() const {
      return _checksum == fc::sha512();
   }

   string get_wallet_filename() const { return _wallet_filename; }


   void set_key_name(string current_key_name, string new_key_name) {
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to set key name on a locked wallet");
      SYS_ASSERT(_key_by_name.contains(current_key_name), key_nonexistent_exception,
                 "Key name provided does not exist: ${keyName}", ("keyName", current_key_name));
      _key_by_name[new_key_name] = _key_by_name[current_key_name];
      _key_by_name.erase(current_key_name);


   }

   void set_key_name(private_key_type private_key, string key_name) {
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to set key name on a locked wallet");
      SYS_ASSERT(!_key_by_name.contains(key_name), key_exist_exception,
                 "Key name provided already exists (keyName=${keyName})", ("keyName", key_name));

      auto pub_key = private_key.get_public_key();
      SYS_ASSERT(_keys.contains(pub_key), key_nonexistent_exception, "Key provided does not exist (pubkey=${pubkey})",
                 ("pubkey", pub_key));

      std::erase_if(_key_by_name, [&](const auto& pair) {
         return pair.second == pub_key;
      });

      _key_by_name[key_name] = pub_key;

   }

   void set_key_name(public_key_type public_key, string key_name) {
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to set key name on a locked wallet");
      SYS_ASSERT(!_key_by_name.contains(key_name), key_exist_exception,
                 "Key name provided already exists (keyName=${keyName})", ("keyName", key_name));

      SYS_ASSERT(_keys.contains(public_key), key_nonexistent_exception, "Key provided does not exist (pubkey=${pubkey})",
                 ("pubkey", public_key));

      std::erase_if(_key_by_name, [&](const auto& pair) {
         return pair.second == public_key;
      });

      _key_by_name[key_name] = public_key;
   }

   string generate_key_name(string prefix = "", string suffix = "") {
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to generate unique key name on a locked wallet");
      for (uint32_t i = 0; i < max_generate_attempts; ++i) {
         std::string attempt = std::format("my-key-{}", i);

         if (!prefix.empty())
            attempt = prefix + "-" + attempt;
         if (!suffix.empty())
            attempt = attempt + "-" + suffix;

         if (!_key_by_name.contains(attempt))
            return attempt;
      }

      SYS_THROW(chain::key_exist_exception, "Unable to generate unique key name");
   }

   std::optional<private_key_type> try_get_private_key(const public_key_type& id) const {
      auto it = _keys.find(id);
      if (it != _keys.end())
         return it->second;
      return std::optional<private_key_type>();
   }

   std::optional<private_key_type> try_get_private_key(const string& key_name) const {
      if (!_key_by_name.contains(key_name))
         return std::nullopt;

      auto& pubkey = _key_by_name.at(key_name);
      return try_get_private_key(pubkey);
   }

   std::optional<signature_type> try_sign_digest(const digest_type digest, const public_key_type public_key) {
      auto it = _keys.find(public_key);
      if (it == _keys.end())
         return std::optional<signature_type>();
      return it->second.sign(digest);
   }

   private_key_type get_private_key(string key_name) const {
      auto has_key = try_get_private_key(key_name);
      SYS_ASSERT(has_key, chain::key_nonexistent_exception, "Key doesn't exist!");
      return *has_key;
   }

   private_key_type get_private_key(const public_key_type& id) const {
      auto has_key = try_get_private_key(id);
      SYS_ASSERT(has_key, chain::key_nonexistent_exception, "Key doesn't exist!");
      return *has_key;
   }

   bool import_key(string key_name, string wif_key) {
      private_key_type              priv(wif_key);
      sysio::chain::public_key_type wif_pub_key = priv.get_public_key();

      SYS_ASSERT(!_keys.contains(wif_pub_key), chain::key_exist_exception, "Key already in wallet (pubKey=${pubKey})", ("pubKey", wif_pub_key));
      SYS_ASSERT(!_key_by_name.contains(key_name), chain::key_exist_exception, "Key already in wallet (keyName=${keyName})", ("keyName", key_name));


      _keys[wif_pub_key] = priv;
      _key_by_name[key_name] = wif_pub_key;
         return true;



   }

   // imports the private key into the wallet, and associate it in some way (?) with the
   // given account name.
   // @returns true if the key matches a current active/owner/memo key for the named
   //          account, false otherwise (but it is stored either way)
   bool import_key(string wif_key) {
      return import_key(
         generate_key_name(),
         wif_key);
   }

   // Removes a key from the wallet (matching `key_name`)
   // @returns true if the key matches a current active/owner/memo key for the named
   //          account, false otherwise (but it is removed either way)
   bool remove_key(string key_name) {


      auto count = std::erase_if(_key_by_name, [&](const auto& pair) {
         if (pair.first == key_name) {
            _keys.erase(pair.second);
            return true;
         }
         return false;
      });
      if (count == 0)
         SYS_THROW(chain::key_nonexistent_exception, "Key not in wallet");

      return true;
   }

   // Removes a key from the wallet (matching `pub`)
   // @returns true if the key matches a current active/owner/memo key for the named
   //          account, false otherwise (but it is removed either way)
   bool remove_key(public_key_type pub) {
      std::erase_if(_key_by_name, [&](const auto& pair) {
         return pair.second == pub;
      });

      if (_keys.contains(pub)) {
         _keys.erase(pub);
         return true;
      }

      SYS_THROW(chain::key_nonexistent_exception, "Key not in wallet");
   }

   string create_key(string key_name, string key_type) {
      if (key_type.empty())
         key_type = _default_key_type;

      private_key_type priv_key;
      if (key_type == "K1")
         priv_key = fc::crypto::private_key::generate<fc::ecc::private_key_shim>();
      else if (key_type == "R1")
         priv_key = fc::crypto::private_key::generate<fc::crypto::r1::private_key_shim>();
      else
         SYS_THROW(chain::unsupported_key_type_exception, "Key type \"${kt}\" not supported by software wallet",
                ("kt", key_type));

      import_key(key_name, priv_key.to_string({}));
      return priv_key.get_public_key().to_string({});
   }

   string create_key(string key_type) {
      return create_key(generate_key_name(), key_type);
   }

   bool load_wallet_file(string wallet_filename = "") {
      // TODO:  Merge imported wallet with existing wallet,
      //        instead of replacing it
      if (wallet_filename.empty())
         wallet_filename = _wallet_filename;

      if (!std::filesystem::exists(wallet_filename))
         return false;

      _wallet = fc::json::from_file(wallet_filename).as<wallet_data>();

      return true;
   }

   void save_wallet_file(string wallet_filename = "") {
      //
      // Serialize in memory, then save to disk
      //
      // This approach lessens the risk of a partially written wallet
      // if exceptions are thrown in serialization
      //

      encrypt_keys();

      if (wallet_filename == "")
         wallet_filename = _wallet_filename;

      wlog("saving wallet to file ${fn}", ("fn", wallet_filename));

      string data = fc::json::to_pretty_string(_wallet);
      try {
         enable_umask_protection();
         //
         // Parentheses on the following declaration fails to compile,
         // due to the Most Vexing Parse.  Thanks, C++
         //
         // http://en.wikipedia.org/wiki/Most_vexing_parse
         //
         ofstream outfile{wallet_filename};
         if (!outfile) {
            elog("Unable to open file: ${fn}", ("fn", wallet_filename));
            SYS_THROW(wallet_exception, "Unable to open file: ${fn}", ("fn", wallet_filename));
         }
         outfile.write(data.c_str(), data.length());
         outfile.flush();
         outfile.close();
         disable_umask_protection();
      } catch (...) {
         disable_umask_protection();
         throw;
      }
   }

   string      _wallet_filename;
   wallet_data _wallet;

   map<public_key_type, private_key_type> _keys;
   fc::sha512                             _checksum;
   map<string, public_key_type>           _key_by_name;

   #ifdef __unix__
   mode_t _old_umask{0};
   #endif
   const string _wallet_filename_extension = ".wallet";
   const string _default_key_type          = "K1";
};

}


namespace sysio::wallet {

soft_wallet::soft_wallet(const wallet_data& initial_data)
   : my(make_shared<detail::soft_wallet_impl>(*this, initial_data)) {}

soft_wallet::~soft_wallet() {}

bool soft_wallet::copy_wallet_file(string destination_filename) {
   return my->copy_wallet_file(destination_filename);
}

string soft_wallet::get_wallet_filename() const {
   return my->get_wallet_filename();
}

bool soft_wallet::import_key(string wif_key) {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to import key on a locked wallet");
   return import_key(generate_key_name(), wif_key);
}

bool soft_wallet::import_key(string key_name, string wif_key) {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to import key on a locked wallet");

   // THROWS ON IMPORT FAILURE
   my->import_key(key_name, wif_key);
   save_wallet_file();
   return true;
}

bool soft_wallet::remove_key(string key_name) {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to remove key from a locked wallet");

   my->remove_key(key_name);
   save_wallet_file();
   return true;
}

bool soft_wallet::remove_key(public_key_type key) {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to remove key from a locked wallet");

   my->remove_key(key);
   save_wallet_file();
   return true;
}

string soft_wallet::create_key(string key_name, string key_type) {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to create key on a locked wallet");

   string ret = my->create_key(key_name, key_type);
   save_wallet_file();
   return ret;
}

string soft_wallet::create_key(string key_type) {
   return create_key(generate_key_name(), key_type);
}


bool soft_wallet::load_wallet_file(string wallet_filename) {
   return my->load_wallet_file(wallet_filename);
}

void soft_wallet::save_wallet_file(string wallet_filename) {
   my->save_wallet_file(wallet_filename);
}

bool soft_wallet::is_locked() const {
   return my->is_locked();
}

bool soft_wallet::is_new() const {
   return my->_wallet.cipher_keys.size() == 0;
}

void soft_wallet::encrypt_keys() {
   my->encrypt_keys();
}

void soft_wallet::set_key_name(string current_key_name, string new_key_name) {
   my->set_key_name(current_key_name, new_key_name);
}

void soft_wallet::set_key_name(private_key_type private_key, string key_name) {
   my->set_key_name(private_key, key_name);
}

void soft_wallet::set_key_name(public_key_type public_key, string key_name) {
   my->set_key_name(public_key, key_name);
}

string soft_wallet::generate_key_name(string prefix, string suffix) {
   return my->generate_key_name(prefix, suffix);
}

void soft_wallet::lock() {
   try {
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to lock a locked wallet");
      encrypt_keys();
      for (auto key : my->_keys)
         key.second = private_key_type();

      my->_keys.clear();
      my->_key_by_name.clear();
      my->_checksum = fc::sha512();
   } FC_CAPTURE_AND_RETHROW()
}

void soft_wallet::unlock(string password) {
   try {
      FC_ASSERT(password.size() > 0);
      auto         pw        = fc::sha512::hash(password.c_str(), password.size());
      vector<char> decrypted = fc::aes_decrypt(pw, my->_wallet.cipher_keys);
      switch (my->_wallet.version) {
      case 0: {
         auto pk = fc::raw::unpack<plain_keys_v0>(decrypted);
         FC_ASSERT(pk.checksum == pw);
         my->_keys     = std::move(pk.keys);
         my->_checksum = pk.checksum;
         my->_key_by_name.clear();

         for (auto& pubkey : views::keys(my->_keys)) {
            auto key_name              = generate_key_name();
            my->_key_by_name[key_name] = pubkey;
         }

         my->_wallet.version = 1;
         save_wallet_file();
         return;
      }
      case 1: {
         auto pk = fc::raw::unpack<plain_keys_v1>(decrypted);
         FC_ASSERT(pk.checksum == pw);
         my->_keys        = std::move(pk.keys);
         my->_key_by_name = std::move(pk.key_by_name);
         my->_checksum    = pk.checksum;
         return;
      }
      default: {
         FC_ASSERT(false, "Unknown wallet version");
      }
      }

   } SYS_RETHROW_EXCEPTIONS(chain::wallet_invalid_password_exception,
                            "Invalid password for wallet: \"${wallet_name}\"", ("wallet_name", get_wallet_filename()))
}

void soft_wallet::check_password(string password) {
   try {
      FC_ASSERT(password.size() > 0);
      auto         pw        = fc::sha512::hash(password.c_str(), password.size());
      vector<char> decrypted = fc::aes_decrypt(pw, my->_wallet.cipher_keys);
      switch (my->_wallet.version) {
      case 0: {
         auto pk = fc::raw::unpack<plain_keys_v0>(decrypted);
         FC_ASSERT(pk.checksum == pw);
         return;
      }
      case 1: {
         auto pk = fc::raw::unpack<plain_keys_v1>(decrypted);
         FC_ASSERT(pk.checksum == pw);
         return;
      }
      default: {
         FC_ASSERT(false, "Unknown wallet version");
      }
      }

   } SYS_RETHROW_EXCEPTIONS(chain::wallet_invalid_password_exception,
                            "Invalid password for wallet: \"${wallet_name}\"", ("wallet_name", get_wallet_filename()))
}

void soft_wallet::set_password(string password) {
   if (!is_new())
      SYS_ASSERT(!is_locked(), wallet_locked_exception, "The wallet must be unlocked before the password can be set");
   my->_checksum = fc::sha512::hash(password.c_str(), password.size());
   lock();
}

map<public_key_type, private_key_type> soft_wallet::list_keys() {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to list public keys of a locked wallet");
   return my->_keys;
}

map<string, wallet_key_entry> soft_wallet::list_keys_by_name() {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to list keys of a locked wallet");
   return my->_key_by_name
          | views::transform([this](const auto& pair) {
             const auto& [name, pubkey] = pair;
             return make_pair(name,
                              wallet_key_entry{name, pubkey, my->get_private_key(pubkey)});
          })
          | ranges::to<map>();
}


flat_set<public_key_type> soft_wallet::list_public_keys() {
   SYS_ASSERT(!is_locked(), wallet_locked_exception, "Unable to list private keys of a locked wallet");
   flat_set<public_key_type> keys;
   boost::copy(my->_keys | boost::adaptors::map_keys, std::inserter(keys, keys.end()));
   return keys;
}

private_key_type soft_wallet::get_private_key(public_key_type pubkey) const {
   return my->get_private_key(pubkey);
}

private_key_type soft_wallet::get_private_key(string key_name) const {
   return my->get_private_key(key_name);
}

std::optional<signature_type> soft_wallet::try_sign_digest(const digest_type digest, const public_key_type public_key) {
   return my->try_sign_digest(digest, public_key);
}

void soft_wallet::set_wallet_filename(string wallet_filename) {
   my->_wallet_filename = wallet_filename;
}

}
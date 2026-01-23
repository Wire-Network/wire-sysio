#include <sysio/chain/application.hpp>
#include <sysio/wallet_plugin/wallet_manager.hpp>
#include <sysio/wallet_plugin/wallet.hpp>
#include <sysio/chain/exceptions.hpp>
#include <boost/algorithm/string.hpp>
#include <fc/io/json.hpp>

namespace sysio {
namespace wallet {

constexpr auto file_ext = ".wallet";
constexpr auto password_prefix = "PW";

std::string gen_password() {
   auto key = private_key_type::generate();
   return password_prefix + key.to_string({});
}

bool valid_filename(const string& name) {
   if (name.empty()) return false;
   if (std::find_if(name.begin(), name.end(), !boost::algorithm::is_alnum() && !boost::algorithm::is_any_of("._-")) != name.end()) return false;
   return std::filesystem::path(name).filename().string() == name;
}

wallet_manager::wallet_manager() {
#ifdef __APPLE__
   try {
      wallets.emplace("SecureEnclave", std::make_unique<se_wallet>());
   } catch(const std::exception& ) {}
#endif
}

wallet_manager::~wallet_manager() {
   //not really required, but may spook users
   if(wallet_dir_lock)
      std::filesystem::remove(lock_path);
}

void wallet_manager::set_timeout(const std::chrono::seconds& t) {
   timeout = t;
   auto now = std::chrono::system_clock::now();
   timeout_time = now + timeout;
   SYS_ASSERT(timeout_time >= now && timeout_time.time_since_epoch().count() > 0, invalid_lock_timeout_exception,
              "Overflow on timeout_time, specified {}, now {}, timeout_time {}",
              t.count(), now.time_since_epoch().count(), timeout_time.time_since_epoch().count());
}

void wallet_manager::check_timeout() {
   if (timeout_time != timepoint_t::max()) {
      const auto& now = std::chrono::system_clock::now();
      if (now >= timeout_time) {
         lock_all();
      }
      timeout_time = now + timeout;
   }
}

std::string wallet_manager::create(const std::string& name) {
   check_timeout();

   SYS_ASSERT(valid_filename(name), wallet_exception, "Invalid filename, path not allowed in wallet name {}", name);

   auto wallet_filename = dir / (name + file_ext);

   if (std::filesystem::exists(wallet_filename)) {
      SYS_THROW(chain::wallet_exist_exception, "Wallet with name: '{}' already exists at {}",
                name, wallet_filename.string());
   }

   std::string password = gen_password();
   wallet_data d;
   auto wallet = make_unique<soft_wallet>(d);
   wallet->set_password(password);
   wallet->set_wallet_filename(wallet_filename.string());
   wallet->unlock(password);
   wallet->lock();
   wallet->unlock(password);

   // Explicitly save the wallet file here, to ensure it now exists.
   wallet->save_wallet_file();

   // If we have name in our map then remove it since we want the emplace below to replace.
   // This can happen if the wallet file is removed while sys-walletd is running.
   auto it = wallets.find(name);
   if (it != wallets.end()) {
      wallets.erase(it);
   }
   wallets.emplace(name, std::move(wallet));

   return password;
}

void wallet_manager::open(const std::string& name) {
   check_timeout();

   SYS_ASSERT(valid_filename(name), wallet_exception, "Invalid filename, path not allowed in wallet name {}", name);

   wallet_data d;
   auto wallet = std::make_unique<soft_wallet>(d);
   auto wallet_filename = dir / (name + file_ext);
   wallet->set_wallet_filename(wallet_filename.string());
   if (!wallet->load_wallet_file()) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Unable to open file: {}", wallet_filename.string());
   }

   // If we have name in our map then remove it since we want the emplace below to replace.
   // This can happen if the wallet file is added while sys-walletd is running.
   auto it = wallets.find(name);
   if (it != wallets.end()) {
      wallets.erase(it);
   }
   wallets.emplace(name, std::move(wallet));
}

std::vector<std::string> wallet_manager::list_wallets() {
   check_timeout();
   std::vector<std::string> result;
   for (const auto& i : wallets) {
      if (i.second->is_locked()) {
         result.emplace_back(i.first);
      } else {
         result.emplace_back(i.first + " *");
      }
   }
   return result;
}

map<public_key_type,private_key_type> wallet_manager::list_keys(const string& name, const string& pw) {
   check_timeout();

   if (!wallets.contains(name))
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   auto& w = wallets.at(name);
   if (w->is_locked())
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   w->check_password(pw); //throws if bad password
   return w->list_keys();
}

map<string, wallet_key_entry> wallet_manager::list_keys_by_name(const string& name, const string& pw) {
   check_timeout();

   if (!wallets.contains(name))
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   auto& w = wallets.at(name);
   if (w->is_locked())
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   w->check_password(pw); //throws if bad password
   return w->list_keys_by_name();
}

void wallet_manager::set_key_name_with_public_key(const string& name, const string& pw, const string& key_name, const string& pub_key_str) {
   check_timeout();

   if (!wallets.contains(name))
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   auto& w = wallets.at(name);
   if (w->is_locked())
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   w->check_password(pw); //throws if bad password
   w->set_key_name(public_key_type::from_string(pub_key_str), key_name);
}

void wallet_manager::set_key_name_with_private_key(const string& name, const string& pw, const string& key_name,
   const string& priv_key_str) {
   check_timeout();

   if (!wallets.contains(name))
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   auto& w = wallets.at(name);
   if (w->is_locked())
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   w->check_password(pw); //throws if bad password
   w->set_key_name(private_key_type::from_string(priv_key_str), key_name);
}

void wallet_manager::set_key_name(const string& name, const string& pw, const string& key_name,
   const string& current_key_name) {
   check_timeout();

   if (!wallets.contains(name))
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   auto& w = wallets.at(name);
   if (w->is_locked())
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   w->check_password(pw); //throws if bad password
   w->set_key_name(current_key_name, key_name);
}

flat_set<public_key_type> wallet_manager::get_public_keys() {
   check_timeout();
   SYS_ASSERT(!wallets.empty(), wallet_not_available_exception, "You don't have any wallet!");
   flat_set<public_key_type> result;
   bool is_all_wallet_locked = true;
   for (const auto& i : wallets) {
      if (!i.second->is_locked()) {
         result.merge(i.second->list_public_keys());
      }
      is_all_wallet_locked &= i.second->is_locked();
   }
   SYS_ASSERT(!is_all_wallet_locked, wallet_locked_exception, "You don't have any unlocked wallet!");
   return result;
}


void wallet_manager::lock_all() {
   // no call to check_timeout since we are locking all anyway
   for (auto& i : wallets) {
      if (!i.second->is_locked()) {
         i.second->lock();
      }
   }
}

void wallet_manager::lock(const std::string& name) {
   check_timeout();
   if (wallets.count(name) == 0) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   }
   auto& w = wallets.at(name);
   if (w->is_locked()) {
      return;
   }
   w->lock();
}

void wallet_manager::unlock(const std::string& name, const std::string& password) {
   check_timeout();
   if (wallets.count(name) == 0) {
      open( name );
   }
   auto& w = wallets.at(name);
   if (!w->is_locked()) {
      SYS_THROW(chain::wallet_unlocked_exception, "Wallet is already unlocked: {}", name);
      return;
   }
   w->unlock(password);
}

void wallet_manager::import_key(const std::string& name, const std::string& wif_key) {
   check_timeout();
   if (wallets.count(name) == 0) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   }
   auto& w = wallets.at(name);
   if (w->is_locked()) {
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   }
   w->import_key(wif_key);
}

void wallet_manager::remove_key(const std::string& name, const std::string& password, const std::string& key) {
   check_timeout();
   if (wallets.count(name) == 0) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   }
   auto& w = wallets.at(name);
   if (w->is_locked()) {
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   }
   w->check_password(password); //throws if bad password
   w->remove_key(key);
}

void wallet_manager::remove_name(const std::string& wallet_name, const std::string& password, const std::string& name) {
   check_timeout();
   if (wallets.count(wallet_name) == 0) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", wallet_name));
   }
   auto& w = wallets.at(wallet_name);
   if (w->is_locked()) {
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: ${w}", ("w", wallet_name));
   }
   w->check_password(password); //throws if bad password
   w->remove_name(name);
}

string wallet_manager::create_key(const std::string& name, const std::string& key_type) {
   check_timeout();
   if (wallets.count(name) == 0) {
      SYS_THROW(chain::wallet_nonexistent_exception, "Wallet not found: {}", name);
   }
   auto& w = wallets.at(name);
   if (w->is_locked()) {
      SYS_THROW(chain::wallet_locked_exception, "Wallet is locked: {}", name);
   }

   string upper_key_type = boost::to_upper_copy<std::string>(key_type);
   return w->create_key(upper_key_type);
}

chain::signed_transaction
wallet_manager::sign_transaction(const chain::signed_transaction& txn, const flat_set<public_key_type>& keys, const chain::chain_id_type& id) {
   check_timeout();
   chain::signed_transaction stxn(txn);

   for (const auto& pk : keys) {
      bool found = false;
      for (const auto& i : wallets) {
         if (!i.second->is_locked()) {
            std::optional<signature_type> sig = i.second->try_sign_digest(stxn.sig_digest(id, stxn.context_free_data), pk);
            if (sig) {
               stxn.signatures.push_back(*sig);
               found = true;
               break; // inner for
            }
         }
      }
      if (!found) {
         SYS_THROW(chain::wallet_missing_pub_key_exception,
                   "Public key not found in unlocked wallets {}",
                   fc::json::to_log_string(pk));
      }
   }

   return stxn;
}

chain::signature_type
wallet_manager::sign_digest(const chain::digest_type& digest, const public_key_type& key) {
   check_timeout();

   try {
      for (const auto& i : wallets) {
         if (!i.second->is_locked()) {
            std::optional<signature_type> sig = i.second->try_sign_digest(digest, key);
            if (sig)
               return *sig;
         }
      }
   } FC_LOG_AND_RETHROW();

   SYS_THROW(chain::wallet_missing_pub_key_exception, "Public key not found in unlocked wallets {}", fc::json::to_log_string(key));
}

void wallet_manager::own_and_use_wallet(const string& name, std::unique_ptr<wallet_api>&& wallet) {
   if(wallets.find(name) != wallets.end())
      SYS_THROW(wallet_exception, "Tried to use wallet name that already exists.");
   wallets.emplace(name, std::move(wallet));
}

void wallet_manager::start_lock_watch(std::shared_ptr<boost::asio::deadline_timer> t)
{
   t->async_wait([t, this](const boost::system::error_code& /*ec*/)
   {
      std::error_code ec;
      auto rc = std::filesystem::status(lock_path, ec);
      if(!ec) {
         if(rc.type() == std::filesystem::file_type::not_found) {
            appbase::app().quit();
            SYS_THROW(wallet_exception, "Lock file removed while kiod still running.  Terminating.");
         }
      }
      t->expires_from_now(boost::posix_time::seconds(1));
      start_lock_watch(t);
   });
}

void wallet_manager::initialize_lock() {
   //This is technically somewhat racy in here -- if multiple kiod are in this function at once.
   //I've considered that an acceptable tradeoff to maintain cross-platform boost constructs here
   lock_path = dir / "wallet.lock";
   {
      std::ofstream x(lock_path.string());
      SYS_ASSERT(!x.fail(), wallet_exception, "Failed to open wallet lock file at {}", lock_path.string());
   }
   wallet_dir_lock = std::make_unique<boost::interprocess::file_lock>(lock_path.string().c_str());
   if(!wallet_dir_lock->try_lock()) {
      wallet_dir_lock.reset();
      SYS_THROW(wallet_exception, "Failed to lock access to wallet directory; is another kiod running?");
   }
   auto timer = std::make_shared<boost::asio::deadline_timer>(appbase::app().get_io_context(), boost::posix_time::seconds(1));
   start_lock_watch(timer);
}

} // namespace wallet
} // namespace sysio

#include "base_actions.hpp"
#include <sysio/chain/config.hpp>

#include <fc/crypto/bls_private_key.hpp>

using namespace sysio::chain;

struct bls_options {
   std::string key_file;
   std::string private_key_str;

   // flags
   bool print_console{false};
};

class bls_actions : public base_actions<bls_options> {
public:
   void setup(CLI::App& app);

protected:
   int create_key();
   int create_pop();

private:
   std::string generate_pop_str(const fc::crypto::blslib::bls_private_key& private_key);
};

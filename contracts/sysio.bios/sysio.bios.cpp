#include "sysio.bios.hpp"

namespace sysiobios {

void bios::setabi( name account, const std::vector<char>& abi ) {
   abi_hash_table table(get_self(), get_self().value);
   auto itr = table.find( account.value );
   if( itr == table.end() ) {
      table.emplace( account, [&]( auto& row ) {
         row.owner = account;
         row.hash  = sysio::sha256(const_cast<char*>(abi.data()), abi.size());
      });
   } else {
      table.modify( itr, sysio::same_payer, [&]( auto& row ) {
         row.hash = sysio::sha256(const_cast<char*>(abi.data()), abi.size());
      });
   }
}

void bios::setpriv( name account, uint8_t is_priv ) {
   require_auth( get_self() );
   set_privileged( account, is_priv );
}

void bios::setalimits( name account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight ) {
   require_auth( get_self() );
   set_resource_limits( account, ram_bytes, net_weight, cpu_weight );
}

void bios::setprods( const std::vector<sysio::producer_authority>& schedule ) {
   require_auth( get_self() );
   set_proposed_producers( schedule );
}

void bios::setprodkeys( const std::vector<sysio::producer_key>& schedule ) {
   require_auth( get_self() );
   set_proposed_producers( schedule );
}

void bios::setparams( const sysio::blockchain_parameters& params ) {
   require_auth( get_self() );
   set_blockchain_parameters( params );
}

void bios::reqauth( name from ) {
   require_auth( from );
}

void bios::activate( const sysio::checksum256& feature_digest ) {
   require_auth( get_self() );
   preactivate_feature( feature_digest );
   print( "feature digest activated: ", feature_digest, "\n" );
}

void bios::reqactivated( const sysio::checksum256& feature_digest ) {
   check( is_feature_activated( feature_digest ), "protocol feature is not activated" );
}

}
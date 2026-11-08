#include "cpu_receiver_test.hpp"

#include <string_view>

using namespace sysio;

namespace {

constexpr name             token_account   = "sysio.token"_n;
constexpr name             transfer_action = "transfer"_n;
constexpr name             spin_action     = "spin"_n;
constexpr const char*      spun            = "spun";
constexpr uint32_t         default_spins   = 1000;
constexpr std::string_view spin_command    = "spin:";
constexpr std::string_view forward_command = "forward:";
constexpr std::string_view pay_command     = "pay:";
constexpr std::string_view forever_command = "forever";
constexpr std::string_view pay_memo        = "spin:1000"; ///< the payee loops default_spins times

bool starts_with( std::string_view text, std::string_view prefix ) {
   return text.substr( 0, prefix.size() ) == prefix;
}

void loop( uint32_t n ) {
   volatile uint64_t sum = 0;
   for( uint32_t i = 0; i < n; ++i )
      sum = sum + i;
}

uint32_t parse_count( std::string_view digits ) {
   uint32_t n = 0;
   for( char c : digits ) {
      check( c >= '0' && c <= '9', "spin count must be decimal digits" );
      n = n * 10 + static_cast<uint32_t>( c - '0' );
   }
   return n;
}

void send_transfer( name from, name to, const asset& quantity, const std::string& memo ) {
   action( permission_level{ from, "active"_n }, token_account, transfer_action,
           std::make_tuple( from, to, quantity, memo ) ).send();
}

} // namespace

void cpu_receiver_test::on_transfer( name from, name to, asset quantity, std::string memo ) {
   if( to != get_self() ) {
      loop( default_spins );
      return;
   }
   const std::string_view command = memo;
   if( starts_with( command, spin_command ) ) {
      loop( parse_count( command.substr( spin_command.size() ) ) );
   } else if( starts_with( command, forward_command ) ) {
      require_recipient( name( command.substr( forward_command.size() ) ) );
   } else if( starts_with( command, pay_command ) ) {
      send_transfer( get_self(), name( command.substr( pay_command.size() ) ), quantity, std::string( pay_memo ) );
   } else if( command == forever_command ) {
      while( true )
         loop( default_spins );
   }
}

void cpu_receiver_test::payout( name to, asset quantity, std::string memo ) {
   require_auth( get_self() );
   send_transfer( get_self(), to, quantity, memo );
}

void cpu_receiver_test::spin( uint32_t n ) {
   loop( n );
   print( spun );
}

void cpu_receiver_test::cfspin( name to, uint32_t n ) {
   action( std::vector<permission_level>{}, to, spin_action, std::make_tuple( n ) ).send_context_free();
}

void cpu_receiver_test::notify( name to ) {
   require_recipient( to );
}

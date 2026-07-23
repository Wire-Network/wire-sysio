#include <sysio/chain/chain_id_type.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>

namespace sysio { namespace chain {

   void chain_id_type::reflector_init()const {
      SYS_ASSERT( *reinterpret_cast<const fc::sha256*>(this) != fc::sha256(), chain_id_type_exception, "chain_id_type cannot be zero" );
   }

} }  // namespace sysio::chain

namespace fc {

   void to_variant(const sysio::chain::chain_id_type& cid, fc::variant& v) {
      to_variant( static_cast<const fc::sha256&>(cid), v);
   }

   void from_variant(const fc::variant& v, sysio::chain::chain_id_type& cid) {
      from_variant( v, static_cast<fc::sha256&>(cid) );
   }

   void to_json_stream(const sysio::chain::chain_id_type& cid, fc::json_writer& w) {
      to_json_stream( static_cast<const fc::sha256&>(cid), w );
   }

} // fc

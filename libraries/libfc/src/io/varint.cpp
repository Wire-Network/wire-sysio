#include <fc/io/varint.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/variant.hpp>

namespace fc
{
void to_variant( const signed_int& var,  variant& vo ) { vo = var.value; }
void from_variant( const variant& var,  signed_int& vo ) { vo.value = static_cast<int32_t>(var.as_int64()); }
void to_variant( const unsigned_int& var, variant& vo )  { vo = var.value; }
void from_variant( const variant& var,  unsigned_int& vo )  { vo.value = static_cast<uint32_t>(var.as_uint64()); }

void to_json_stream( const signed_int& var, json_writer& w )   { w.value_int32(var.value); }
void to_json_stream( const unsigned_int& var, json_writer& w ) { w.value_uint32(var.value); }
}

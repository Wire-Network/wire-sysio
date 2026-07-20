#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/abi_sinks.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/io/raw.hpp>
#include <fc/bitset.hpp>
#include <fc/io/varint.hpp>
#include <fc/time.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <iomanip>
#include <limits>
#include <sstream>

namespace sysio::chain {

   const size_t abi_serializer::max_recursion_depth;

   using std::string;
   using std::string_view;

   template <typename T>
   inline fc::variant variant_from_stream(fc::datastream<const char*>& stream) {
      T temp;
      fc::raw::unpack( stream, temp );
      return fc::variant(temp);
   }

   template <typename T>
   inline fc::variant variant_from_stream(fc::datastream<const char*>& stream, const abi_serializer::yield_function_t& yield) {
      T temp;
      fc::raw::unpack( stream, temp );
      yield(0);
      // create yield function matching fc::variant requirements, 0 for recursive depth
      return fc::variant( temp, [yield](){ yield(0); } );
   }

   template <typename T>
   auto pack_function() {
      return []( const fc::variant& var, fc::datastream<char*>& ds, bool is_array, bool is_optional, const abi_serializer::yield_function_t& yield ){
         if( is_array )
            fc::raw::pack( ds, var.as<vector<T>>() );
         else if ( is_optional )
            fc::raw::pack( ds, var.as<std::optional<T>>() );
         else
            fc::raw::pack( ds,  var.as<T>());
      };
   }

   template <typename T>
   auto pack_unpack() {
      return std::make_pair<abi_serializer::unpack_function, abi_serializer::pack_function>(
         []( fc::datastream<const char*>& stream, bool is_array, bool is_optional, const abi_serializer::yield_function_t& yield) {
            if( is_array )
               return variant_from_stream<vector<T>>(stream);
            else if ( is_optional )
               return variant_from_stream<std::optional<T>>(stream);
            return variant_from_stream<T>(stream);
         },
         pack_function<T>()
      );
   }

   template <typename T>
   auto pack_unpack_deadline() {
      return std::make_pair<abi_serializer::unpack_function, abi_serializer::pack_function>(
         []( fc::datastream<const char*>& stream, bool is_array, bool is_optional, const abi_serializer::yield_function_t& yield) {
            if( is_array )
               return variant_from_stream<vector<T>>(stream);
            else if ( is_optional )
               return variant_from_stream<std::optional<T>>(stream);
            return variant_from_stream<T>(stream, yield);
         },
         pack_function<T>()
      );
   }

   // Stream-side counterpart to `pack_unpack`.  The lambda reads a single value of type
   // `T` from the datastream and forwards to the per-type `emit(value, json_writer&)`
   // callback, which is responsible for matching the variant path's exact JSON shape
   // (quote conventions for large 64-bit ints, decimal strings for 128-bit, etc).
   template <typename T, typename Emit>
   auto pack_unpack_stream(Emit emit) {
      return [emit]( fc::datastream<const char*>& stream, bool is_array, bool is_optional,
                     const abi_serializer::yield_function_t& yield, fc::json_writer& w ) {
         if( is_array ) {
            fc::unsigned_int sz;
            fc::raw::unpack(stream, sz);
            w.begin_array();
            for( fc::unsigned_int::base_uint i = 0; i < sz.value; ++i ) {
               T val;
               fc::raw::unpack(stream, val);
               emit(val, w);
            }
            w.end_array();
         } else if( is_optional ) {
            char flag;
            fc::raw::unpack(stream, flag);
            if( flag ) {
               T val;
               fc::raw::unpack(stream, val);
               emit(val, w);
            } else {
               w.value_null();
            }
         } else {
            T val;
            fc::raw::unpack(stream, val);
            emit(val, w);
         }
      };
   }

   // Same as `pack_unpack_stream`, but invokes `yield(0)` on each value read so a
   // long signature/public_key decode can bail out at the configured deadline.
   template <typename T, typename Emit>
   auto pack_unpack_stream_deadline(Emit emit) {
      return [emit]( fc::datastream<const char*>& stream, bool is_array, bool is_optional,
                     const abi_serializer::yield_function_t& yield, fc::json_writer& w ) {
         if( is_array ) {
            fc::unsigned_int sz;
            fc::raw::unpack(stream, sz);
            w.begin_array();
            for( fc::unsigned_int::base_uint i = 0; i < sz.value; ++i ) {
               T val;
               fc::raw::unpack(stream, val);
               yield(0);
               emit(val, w);
            }
            w.end_array();
         } else if( is_optional ) {
            char flag;
            fc::raw::unpack(stream, flag);
            if( flag ) {
               T val;
               fc::raw::unpack(stream, val);
               yield(0);
               emit(val, w);
            } else {
               w.value_null();
            }
         } else {
            T val;
            fc::raw::unpack(stream, val);
            yield(0);
            emit(val, w);
         }
      };
   }

   using built_in_stream_unpack_fn = std::function<void(fc::datastream<const char*>&, bool, bool,
                                                         const abi_serializer::yield_function_t&,
                                                         fc::json_writer&)>;

   // Static dispatch table for the streaming side of `unpack_built_in`.  Built once on
   // first access and then read-only.  Variant-side overrides via
   // `add_specialized_unpack_pack` are not reflected here; the streaming consumers
   // (chain_api endpoints) do not register overrides today.
   const std::map<std::string, built_in_stream_unpack_fn, std::less<>>& get_built_in_stream_unpacks() {
      static const auto map = []() {
         std::map<std::string, built_in_stream_unpack_fn, std::less<>> m;

         // fc::to_json_stream provides the variant-path shapes for every scalar here:
         // int64/uint64 quote past fc::json_integer_quote_magnitude, float32/float64
         // quote with the digits10+2 fixed-precision to_chars form.
         auto generic = [](const auto& v, fc::json_writer& w) { fc::to_json_stream(v, w); };

         m.emplace("bool",                 pack_unpack_stream<uint8_t>(generic));
         m.emplace("int8",                 pack_unpack_stream<int8_t>(generic));
         m.emplace("uint8",                pack_unpack_stream<uint8_t>(generic));
         m.emplace("int16",                pack_unpack_stream<int16_t>(generic));
         m.emplace("uint16",               pack_unpack_stream<uint16_t>(generic));
         m.emplace("int32",                pack_unpack_stream<int32_t>(generic));
         m.emplace("uint32",               pack_unpack_stream<uint32_t>(generic));
         m.emplace("int64",                pack_unpack_stream<int64_t>(generic));
         m.emplace("uint64",               pack_unpack_stream<uint64_t>(generic));
         m.emplace("int128",               pack_unpack_stream<int128_t>(generic));
         m.emplace("uint128",              pack_unpack_stream<uint128_t>(generic));
         m.emplace("varint32",             pack_unpack_stream<fc::signed_int>(generic));
         m.emplace("varuint32",            pack_unpack_stream<fc::unsigned_int>(generic));

         m.emplace("float32",              pack_unpack_stream<float>(generic));
         m.emplace("float64",              pack_unpack_stream<double>(generic));
         m.emplace("float128",             pack_unpack_stream<softfloat128_t>(generic));

         m.emplace("time_point",           pack_unpack_stream<fc::time_point>(generic));
         m.emplace("time_point_sec",       pack_unpack_stream<fc::time_point_sec>(generic));
         m.emplace("block_timestamp_type", pack_unpack_stream<block_timestamp_type>(generic));

         m.emplace("name",                 pack_unpack_stream<name>(generic));

         m.emplace("bytes",                pack_unpack_stream<bytes>(generic));
         m.emplace("string",               pack_unpack_stream<std::string>(generic));

         m.emplace("checksum160",          pack_unpack_stream<checksum160_type>(generic));
         m.emplace("checksum256",          pack_unpack_stream<checksum256_type>(generic));
         m.emplace("checksum512",          pack_unpack_stream<checksum512_type>(generic));

         m.emplace("public_key",           pack_unpack_stream_deadline<public_key_type>(generic));
         m.emplace("signature",            pack_unpack_stream_deadline<signature_type>(generic));

         m.emplace("symbol",               pack_unpack_stream<symbol>(generic));
         m.emplace("symbol_code",          pack_unpack_stream<symbol_code>(generic));
         m.emplace("asset",                pack_unpack_stream<asset>(generic));
         m.emplace("extended_asset",       pack_unpack_stream<extended_asset>(generic));
         m.emplace("bitset",               pack_unpack_stream<fc::bitset>(generic));

         return m;
      }();
      return map;
   }

   abi_serializer::abi_serializer( abi_def abi, const yield_function_t& yield ) {
      configure_built_in_types();
      set_abi(std::move(abi), yield);
   }

   abi_serializer::abi_serializer( const abi_def& abi, const fc::microseconds& max_serialization_time) {
      configure_built_in_types();
      set_abi(abi, create_yield_function(max_serialization_time));
   }

   void abi_serializer::add_specialized_unpack_pack( const string& name,
                                                     std::pair<abi_serializer::unpack_function, abi_serializer::pack_function> unpack_pack ) {
      built_in_types[name] = std::move( unpack_pack );
   }

   const std::pair<abi_serializer::unpack_function, abi_serializer::pack_function>*
   abi_serializer::find_built_in(std::string_view type) const noexcept {
      auto it = built_in_types.find(type);
      return it == built_in_types.end() ? nullptr : &it->second;
   }

   void abi_serializer::configure_built_in_types() {

      built_in_types.emplace("bool",                      pack_unpack<uint8_t>());
      built_in_types.emplace("int8",                      pack_unpack<int8_t>());
      built_in_types.emplace("uint8",                     pack_unpack<uint8_t>());
      built_in_types.emplace("int16",                     pack_unpack<int16_t>());
      built_in_types.emplace("uint16",                    pack_unpack<uint16_t>());
      built_in_types.emplace("int32",                     pack_unpack<int32_t>());
      built_in_types.emplace("uint32",                    pack_unpack<uint32_t>());
      built_in_types.emplace("int64",                     pack_unpack<int64_t>());
      built_in_types.emplace("uint64",                    pack_unpack<uint64_t>());
      built_in_types.emplace("int128",                    pack_unpack<int128_t>());
      built_in_types.emplace("uint128",                   pack_unpack<uint128_t>());
      built_in_types.emplace("varint32",                  pack_unpack<fc::signed_int>());
      built_in_types.emplace("varuint32",                 pack_unpack<fc::unsigned_int>());

      // CDT typedefs `vint64_t` / `vuint64_t` (zpp::bits varint wrappers) to abi
      // names `varint_int64` / `varint_uint64`. The CDT DataStream operators in
      // contracts/sysio.opp.common/include/sysio.opp.common/opp_table_types.hpp
      // serialize them as plain int64/uint64 — the wire format matches the bare
      // integer type, so the abi-side decode is identical.
      built_in_types.emplace("varint_int64",              pack_unpack<int64_t>());
      built_in_types.emplace("varint_uint64",             pack_unpack<uint64_t>());

      // TODO: Add proper support for floating point types. For now this is good enough.
      built_in_types.emplace("float32",                   pack_unpack<float>());
      built_in_types.emplace("float64",                   pack_unpack<double>());
      built_in_types.emplace("float128",                  pack_unpack<softfloat128_t>());

      built_in_types.emplace("time_point",                pack_unpack<fc::time_point>());
      built_in_types.emplace("time_point_sec",            pack_unpack<fc::time_point_sec>());
      built_in_types.emplace("block_timestamp_type",      pack_unpack<block_timestamp_type>());

      built_in_types.emplace("name",                      pack_unpack<name>());

      built_in_types.emplace("bytes",                     pack_unpack<bytes>());
      built_in_types.emplace("string",                    pack_unpack<string>());

      built_in_types.emplace("checksum160",               pack_unpack<checksum160_type>());
      built_in_types.emplace("checksum256",               pack_unpack<checksum256_type>());
      built_in_types.emplace("checksum512",               pack_unpack<checksum512_type>());

      built_in_types.emplace("public_key",                pack_unpack_deadline<public_key_type>());
      built_in_types.emplace("signature",                 pack_unpack_deadline<signature_type>());

      built_in_types.emplace("symbol",                    pack_unpack<symbol>());
      built_in_types.emplace("symbol_code",               pack_unpack<symbol_code>());
      built_in_types.emplace("asset",                     pack_unpack<asset>());
      built_in_types.emplace("extended_asset",            pack_unpack<extended_asset>());
      built_in_types.emplace("bitset",                    pack_unpack<fc::bitset>());
   }

   void abi_serializer::set_abi(abi_def abi, const yield_function_t& yield) {
      impl::abi_traverse_context ctx(yield, fc::microseconds{});

      SYS_ASSERT(abi.version.starts_with("sysio::abi/1."), unsupported_abi_version_exception, "ABI has an unsupported version");

      size_t types_size = abi.types.size();
      size_t structs_size = abi.structs.size();
      size_t actions_size = abi.actions.size();
      size_t tables_size = abi.tables.size();
      size_t error_messages_size = abi.error_messages.size();
      size_t variants_size = abi.variants.value.size();
      size_t action_results_size = abi.action_results.value.size();
      size_t enums_size = abi.enums.value.size();

      typedefs.clear();
      structs.clear();
      actions.clear();
      tables.clear();
      error_messages.clear();
      variants.clear();
      enums.clear();
      action_results.clear();

      for( auto& st : abi.structs )
         structs[st.name] = std::move(st);

      for( auto& td : abi.types ) {
         SYS_ASSERT(!_is_type(td.new_type_name, ctx), duplicate_abi_type_def_exception,
                    "type already exists '{}'", impl::limit_size(td.new_type_name));
         typedefs[std::move(td.new_type_name)] = std::move(td.type);
      }

      for( auto& a : abi.actions )
         actions[std::move(a.name)] = std::move(a.type);

      for( auto& t : abi.tables )
         tables[std::move(t.name)] = std::move(t.type);

      for( auto& e : abi.error_messages )
         error_messages[std::move(e.error_code)] = std::move(e.error_msg);

      for( auto& v : abi.variants.value )
         variants[v.name] = std::move(v);

      for( auto& r : abi.action_results.value )
         action_results[std::move(r.name)] = std::move(r.result_type);

      for( auto& e : abi.enums.value )
         enums[e.name] = std::move(e);

      /**
       *  The ABI vector may contain duplicates which would make it
       *  an invalid ABI
       */
      SYS_ASSERT( typedefs.size() == types_size, duplicate_abi_type_def_exception, "duplicate type definition detected" );
      SYS_ASSERT( structs.size() == structs_size, duplicate_abi_struct_def_exception, "duplicate struct definition detected" );
      SYS_ASSERT( actions.size() == actions_size, duplicate_abi_action_def_exception, "duplicate action definition detected" );
      SYS_ASSERT( tables.size() == tables_size, duplicate_abi_table_def_exception, "duplicate table definition detected" );
      SYS_ASSERT( error_messages.size() == error_messages_size, duplicate_abi_err_msg_def_exception, "duplicate error message definition detected" );
      SYS_ASSERT( variants.size() == variants_size, duplicate_abi_variant_def_exception, "duplicate variant definition detected" );
      SYS_ASSERT( action_results.size() == action_results_size, duplicate_abi_action_results_def_exception, "duplicate action results definition detected" );
      SYS_ASSERT( enums.size() == enums_size, duplicate_abi_enum_def_exception, "duplicate enum definition detected" );

      // Initialize protobuf descriptors if protobuf_types is present
      pb_pool.reset();
      pb_factory.reset();
      if( !abi.protobuf_types.value.empty() ) {
         namespace gpb = google::protobuf;
         gpb::FileDescriptorSet fds;
         gpb::util::JsonParseOptions opts;
         opts.ignore_unknown_fields = true;
         auto status = gpb::util::JsonStringToMessage(abi.protobuf_types.value, &fds, opts);
         SYS_ASSERT( status.ok(), invalid_type_inside_abi,
                     "Failed to parse protobuf_types: {}", std::string(status.message()) );
         pb_pool = std::make_shared<gpb::DescriptorPool>();
         for( int i = 0; i < fds.file_size(); ++i ) {
            auto* fd = pb_pool->BuildFile(fds.file(i));
            SYS_ASSERT( fd != nullptr, invalid_type_inside_abi,
                        "Failed to build protobuf file descriptor: {}", fds.file(i).name() );
         }
         pb_factory = std::make_shared<gpb::DynamicMessageFactory>(pb_pool.get());
      }

      validate(ctx);
   }

   void abi_serializer::set_abi(const abi_def& abi, const fc::microseconds& max_serialization_time) {
      return set_abi(abi, create_yield_function(max_serialization_time));
   }

   bool abi_serializer::is_protobuf_type(const std::string_view& type) const {
      return type.starts_with("protobuf::") && pb_pool;
   }

   const google::protobuf::Descriptor* abi_serializer::get_pb_descriptor(const std::string_view& type) const {
      if( !pb_pool || !type.starts_with("protobuf::") ) return nullptr;
      // type is "protobuf::package.MessageType" — extract "package.MessageType"
      auto msg_name = type.substr(10); // skip "protobuf::"
      return pb_pool->FindMessageTypeByName(std::string(msg_name));
   }

   // Helper: convert a protobuf message to fc::variant (JSON object)
   static fc::variant pb_message_to_variant(const google::protobuf::Message& msg) {
      namespace gpb = google::protobuf;
      std::string json;
      gpb::util::JsonPrintOptions opts;
      opts.preserve_proto_field_names = true;
      auto status = gpb::util::MessageToJsonString(msg, &json, opts);
      SYS_ASSERT( status.ok(), unpack_exception, "Failed to convert protobuf message to JSON: {}", std::string(status.message()) );
      return fc::json::from_string(json);
   }

   // Helper: populate a protobuf message from fc::variant (JSON object)
   static void pb_variant_to_message(const fc::variant& var, google::protobuf::Message& msg) {
      namespace gpb = google::protobuf;
      auto json = fc::json::to_string(var, fc::time_point::maximum());
      gpb::util::JsonParseOptions opts;
      opts.ignore_unknown_fields = true;
      auto status = gpb::util::JsonStringToMessage(json, &msg, opts);
      SYS_ASSERT( status.ok(), pack_exception, "Failed to convert JSON to protobuf message: {}", std::string(status.message()) );
   }

   namespace {
      // Shared length-prefix decode used by both pb_binary_to_variant and
      // pb_binary_to_json_string.  Returns the decoded protobuf message, having
      // already advanced `stream` past the consumed bytes.
      std::unique_ptr<google::protobuf::Message> pb_decode_message(
            const std::string_view& type, fc::datastream<const char*>& stream,
            const google::protobuf::Descriptor* desc,
            google::protobuf::DynamicMessageFactory& factory ) {
         SYS_ASSERT( desc, unpack_exception, "Unknown protobuf type '{}'", impl::limit_size(type) );

         // Outer varuint32 length prefix covers inner length prefix + protobuf bytes.
         fc::unsigned_int outer_len;
         fc::raw::unpack(stream, outer_len);
         SYS_ASSERT( stream.remaining() >= outer_len.value, unpack_exception,
                     "Not enough data for protobuf message '{}': need {} bytes, have {}",
                     impl::limit_size(type), outer_len.value, stream.remaining() );

         // Inner varuint32 length prefix (zpp_bits size_varint format).
         fc::datastream<const char*> inner_stream(stream.pos(), outer_len.value);
         fc::unsigned_int pb_len;
         fc::raw::unpack(inner_stream, pb_len);

         SYS_ASSERT( pb_len.value <= inner_stream.remaining(), unpack_exception,
                     "Protobuf message '{}': inner length {} exceeds available data {}",
                     impl::limit_size(type), pb_len.value, inner_stream.remaining() );
         SYS_ASSERT( pb_len.value == inner_stream.remaining(), unpack_exception,
                     "Protobuf message '{}': trailing data detected (inner length {} but {} bytes remain)",
                     impl::limit_size(type), pb_len.value, inner_stream.remaining() );

         auto prototype = factory.GetPrototype(desc);
         std::unique_ptr<google::protobuf::Message> msg(prototype->New());
         SYS_ASSERT( msg->ParseFromArray(inner_stream.pos(), pb_len.value), unpack_exception,
                     "Failed to parse protobuf message '{}'", impl::limit_size(type) );
         stream.skip(outer_len.value);
         return msg;
      }
   }

   fc::variant abi_serializer::pb_binary_to_variant(const std::string_view& type, fc::datastream<const char*>& stream) const {
      auto desc = get_pb_descriptor(type);
      auto msg = pb_decode_message(type, stream, desc, *pb_factory);
      return pb_message_to_variant(*msg);
   }

   std::string abi_serializer::pb_binary_to_json_string(const std::string_view& type, fc::datastream<const char*>& stream) const {
      namespace gpb = google::protobuf;
      auto desc = get_pb_descriptor(type);
      auto msg = pb_decode_message(type, stream, desc, *pb_factory);
      std::string json;
      gpb::util::JsonPrintOptions opts;
      opts.preserve_proto_field_names = true;
      auto status = gpb::util::MessageToJsonString(*msg, &json, opts);
      SYS_ASSERT( status.ok(), unpack_exception, "Failed to convert protobuf message to JSON: {}",
                  std::string(status.message()) );
      return json;
   }

   void abi_serializer::pb_variant_to_binary(const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds) const {
      auto desc = get_pb_descriptor(type);
      SYS_ASSERT( desc, pack_exception, "Unknown protobuf type '{}'", impl::limit_size(type) );

      auto prototype = pb_factory->GetPrototype(desc);
      std::unique_ptr<google::protobuf::Message> msg(prototype->New());
      pb_variant_to_message(var, *msg);

      std::string serialized;
      SYS_ASSERT( msg->SerializeToString(&serialized), pack_exception,
                  "Failed to serialize protobuf message '{}'", impl::limit_size(type) );

      // Match CDT's zpp_bits size_varint format: outer_len + inner_varint_len + pb_bytes
      // The inner varint length prefix is what zpp_bits::out with size_varint{} produces
      fc::unsigned_int inner_len(serialized.size());
      size_t inner_len_size = fc::raw::pack_size(inner_len);
      fc::raw::pack(ds, fc::unsigned_int(inner_len_size + serialized.size()));
      fc::raw::pack(ds, inner_len);
      ds.write(serialized.data(), serialized.size());
   }

   bool abi_serializer::is_builtin_type(const std::string_view& type)const {
      return built_in_types.find(type) != built_in_types.end();
   }

   bool abi_serializer::is_integer(const std::string_view& type) const {
      return type.starts_with("uint") || type.starts_with("int");
   }

   int abi_serializer::get_integer_size(const std::string_view& type) const {
      SYS_ASSERT( is_integer(type), invalid_type_inside_abi, "{} is not an integer type",
                  impl::limit_size(type));
      if( type.starts_with("uint") ) {
         return boost::lexical_cast<int>(type.substr(4));
      } else {
         return boost::lexical_cast<int>(type.substr(3));
      }
   }

   bool abi_serializer::is_struct(const std::string_view& type)const {
      return structs.find(resolve_type(type)) != structs.end();
   }

   bool abi_serializer::is_enum(const std::string_view& type)const {
      return enums.find(resolve_type(type)) != enums.end();
   }

   bool abi_serializer::is_array(const string_view& type)const {
      return type.ends_with("[]");
   }

   std::optional<fc::unsigned_int> abi_serializer::is_szarray(const string_view& type) const {
      auto pos1 = type.find_last_of('[');
      auto pos2 = type.find_last_of(']');
      if(pos1 == string_view::npos || pos2 != type.size() - 1)
         return {};
      auto pos = pos1 + 1;
      if(pos == pos2)
         return {};

      fc::unsigned_int sz = 0;
      while(pos < pos2) {
         if( ! (type[pos] >= '0' && type[pos] <= '9') )
            return {};
         sz = 10 * sz +  (type[pos] - '0');
         ++pos;
      }
      return  std::optional<fc::unsigned_int>{sz};
   }

   bool abi_serializer::is_optional(const string_view& type)const {
      return type.ends_with("?");
   }

   bool abi_serializer::is_type(const std::string_view& type, const yield_function_t& yield)const {
      impl::abi_traverse_context ctx(yield, fc::microseconds{});
      return _is_type(type, ctx);
   }

   bool abi_serializer::is_type(const std::string_view& type, const fc::microseconds& max_serialization_time) const {
      return is_type(type, create_yield_function(max_serialization_time));
   }

   std::string_view abi_serializer::fundamental_type(const std::string_view& type)const {
      if( is_array(type) ) {
         return type.substr(0, type.size()-2);
      } else if (is_szarray (type) ){
         return type.substr(0, type.find_last_of('['));
      } else if ( is_optional(type) ) {
         return type.substr(0, type.size()-1);
      } else {
       return type;
      }
   }

   std::string_view abi_serializer::_remove_bin_extension(const std::string_view& type) {
      if( type.ends_with("$") )
         return type.substr(0, type.size()-1);
      else
         return type;
   }

   bool abi_serializer::_is_type(const std::string_view& rtype, impl::abi_traverse_context& ctx )const {
      auto h = ctx.enter_scope();
      auto type = fundamental_type(rtype);
      if( built_in_types.find(type) != built_in_types.end() ) return true;
      if( typedefs.find(type) != typedefs.end() ) return _is_type(typedefs.find(type)->second, ctx);
      if( structs.find(type) != structs.end() ) return true;
      if( variants.find(type) != variants.end() ) return true;
      if( enums.find(type) != enums.end() ) return true;
      if( is_protobuf_type(type) && get_pb_descriptor(type) ) return true;
      return false;
   }

   const struct_def& abi_serializer::get_struct(const std::string_view& type)const {
      auto itr = structs.find(resolve_type(type) );
      SYS_ASSERT( itr != structs.end(), invalid_type_inside_abi, "Unknown struct {}", impl::limit_size(type) );
      return itr->second;
   }

   void abi_serializer::validate( impl::abi_traverse_context& ctx )const {
      for( const auto& t : typedefs ) { try {
         vector<std::string_view> types_seen{t.first, t.second};
         auto itr = typedefs.find(t.second);
         while( itr != typedefs.end() ) {
            ctx.check_deadline();
            SYS_ASSERT( find(types_seen.begin(), types_seen.end(), itr->second) == types_seen.end(), abi_circular_def_exception,
                        "Circular reference in type {}", impl::limit_size(t.first) );
            types_seen.emplace_back(itr->second);
            itr = typedefs.find(itr->second);
         }
      } FC_CAPTURE_AND_RETHROW( "t: {}", t ) }
      for( const auto& t : typedefs ) { try {
         SYS_ASSERT(_is_type(t.second, ctx), invalid_type_inside_abi, "{}", impl::limit_size(t.second) );
      } FC_CAPTURE_AND_RETHROW( "t: {}", t ) }
      for( const auto& s : structs ) { try {
         if( s.second.base != type_name() ) {
            const struct_def* current = &s.second;
            vector<std::string_view> types_seen{current->name};
            while( current->base != type_name() ) {
               ctx.check_deadline();
               const struct_def& base = get_struct(current->base); //<-- force struct to inherit from another struct
               SYS_ASSERT( find(types_seen.begin(), types_seen.end(), base.name) == types_seen.end(), abi_circular_def_exception,
                           "Circular reference in struct {}", impl::limit_size(s.second.name) );
               types_seen.emplace_back(base.name);
               current = &base;
            }
         }
         for( const auto& field : s.second.fields ) { try {
            ctx.check_deadline();
            SYS_ASSERT(_is_type(_remove_bin_extension(field.type), ctx), invalid_type_inside_abi,
                       "{}", impl::limit_size(field.type) );
         } FC_CAPTURE_AND_RETHROW( "field: {}", field ) }
      } FC_CAPTURE_AND_RETHROW( "s: {}", s ) }
      for( const auto& s : variants ) { try {
         for( const auto& type : s.second.types ) { try {
            ctx.check_deadline();
            SYS_ASSERT(_is_type(type, ctx), invalid_type_inside_abi, "{}", impl::limit_size(type) );
         } FC_CAPTURE_AND_RETHROW( "type: {}", type ) }
      } FC_CAPTURE_AND_RETHROW( "s: {}", s ) }
      for( const auto& a : actions ) { try {
        ctx.check_deadline();
        SYS_ASSERT(_is_type(a.second, ctx), invalid_type_inside_abi, "{}", impl::limit_size(a.second) );
      } FC_CAPTURE_AND_RETHROW( "a: {}", a  ) }

      for( const auto& t : tables ) { try {
        ctx.check_deadline();
        SYS_ASSERT(_is_type(t.second, ctx), invalid_type_inside_abi, "{}", impl::limit_size(t.second) );
      } FC_CAPTURE_AND_RETHROW( "t: {}", t  ) }

      for( const auto& r : action_results ) { try {
        ctx.check_deadline();
        SYS_ASSERT(_is_type(r.second, ctx), invalid_type_inside_abi, "{}", impl::limit_size(r.second) );
      } FC_CAPTURE_AND_RETHROW( "r: {}", r  ) }
      for( const auto& en : enums ) { try {
        ctx.check_deadline();
        SYS_ASSERT(is_integer(en.second.type), invalid_type_inside_abi,
                   "enum '{}' has invalid underlying type '{}' (must be an integer type)", impl::limit_size(en.first), impl::limit_size(en.second.type) );

        int bit_width = get_integer_size(en.second.type);
        bool is_signed_type = en.second.type.starts_with("int");

        flat_set<string> seen_names;
        flat_set<int64_t> seen_values;
        for( const auto& ev : en.second.values ) {
           SYS_ASSERT(seen_names.insert(ev.name).second, invalid_type_inside_abi,
                      "enum '{}' has duplicate member name '{}'", impl::limit_size(en.first), impl::limit_size(ev.name) );
           SYS_ASSERT(seen_values.insert(ev.value).second, invalid_type_inside_abi,
                      "enum '{}' has duplicate value {} (member '{}')", impl::limit_size(en.first), ev.value, impl::limit_size(ev.name) );
           if( is_signed_type ) {
              int64_t lo = -(1LL << (bit_width - 1));
              int64_t hi =  (1LL << (bit_width - 1)) - 1;
              SYS_ASSERT(ev.value >= lo && ev.value <= hi, invalid_type_inside_abi,
                         "enum '{}' value '{}' ({}) out of range for '{}'",
                         impl::limit_size(en.first), impl::limit_size(ev.name), ev.value, impl::limit_size(en.second.type) );
           } else {
              uint64_t hi = (bit_width == 64) ? UINT64_MAX : (1ULL << bit_width) - 1;
              SYS_ASSERT(ev.value >= 0 && static_cast<uint64_t>(ev.value) <= hi, invalid_type_inside_abi,
                         "enum '{}' value '{}' ({}) out of range for '{}'",
                         impl::limit_size(en.first), impl::limit_size(ev.name), ev.value, impl::limit_size(en.second.type) );
           }
        }
      } FC_CAPTURE_AND_RETHROW( "enum: {}", en.first  ) }
   }

   std::string_view abi_serializer::resolve_type(const std::string_view& type)const {
      auto itr = typedefs.find(type);
      if( itr != typedefs.end() ) {
         for( auto i = typedefs.size(); i > 0; --i ) { // avoid infinite recursion
            const std::string_view& t = itr->second;
            itr = typedefs.find( t );
            if( itr == typedefs.end() ) return t;
         }
      }
      return type;
   }

   fc::variant abi_serializer::_binary_to_variant( const std::string_view& type, fc::datastream<const char *>& stream,
                                                   impl::binary_to_variant_context& ctx )const
   {
      impl::variant_sink sink(*this);
      _binary_walk(type, stream, sink, ctx);
      return std::move(sink).take_result();
   }

   fc::variant abi_serializer::_binary_to_variant( const std::string_view& type, const bytes& binary, impl::binary_to_variant_context& ctx )const
   {
      auto h = ctx.enter_scope();
      fc::datastream<const char*> ds( binary.data(), binary.size() );
      return _binary_to_variant(type, ds, ctx);
   }

   fc::variant abi_serializer::binary_to_variant( const std::string_view& type, const bytes& binary, const yield_function_t& yield, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, yield, fc::microseconds{}, type);
      ctx.short_path = short_path;
      return _binary_to_variant(type, binary, ctx);
   }

   fc::variant abi_serializer::binary_to_variant( const std::string_view& type, const bytes& binary, const fc::microseconds& max_action_data_serialization_time, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, create_depth_yield_function(), max_action_data_serialization_time, type);
      ctx.short_path = short_path;
      return _binary_to_variant(type, binary, ctx);
   }

   fc::variant abi_serializer::binary_to_variant( const std::string_view& type, fc::datastream<const char*>& binary, const yield_function_t& yield, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, yield, fc::microseconds{}, type);
      ctx.short_path = short_path;
      return _binary_to_variant(type, binary, ctx);
   }

   fc::variant abi_serializer::binary_to_variant( const std::string_view& type, fc::datastream<const char*>& binary, const fc::microseconds& max_action_data_serialization_time, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, create_depth_yield_function(), max_action_data_serialization_time, type);
      ctx.short_path = short_path;
      return _binary_to_variant(type, binary, ctx);
   }

   void abi_serializer::binary_to_json_stream( const std::string_view& type, fc::datastream<const char*>& binary, fc::json_writer& w,
                                               const yield_function_t& yield, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, yield, fc::microseconds{}, type);
      ctx.short_path = short_path;
      impl::stream_sink sink(*this, w);
      _binary_walk(type, binary, sink, ctx);
   }

   void abi_serializer::binary_to_json_stream( const std::string_view& type, fc::datastream<const char*>& binary, fc::json_writer& w,
                                               const fc::microseconds& max_action_data_serialization_time, bool short_path )const {
      impl::binary_to_variant_context ctx(*this, create_depth_yield_function(), max_action_data_serialization_time, type);
      ctx.short_path = short_path;
      impl::stream_sink sink(*this, w);
      _binary_walk(type, binary, sink, ctx);
   }

   void abi_serializer::binary_to_json_stream( const std::string_view& type, const bytes& binary, fc::json_writer& w,
                                               const yield_function_t& yield, bool short_path )const {
      fc::datastream<const char*> ds(binary.data(), binary.size());
      binary_to_json_stream(type, ds, w, yield, short_path);
   }

   void abi_serializer::binary_to_json_stream( const std::string_view& type, const bytes& binary, fc::json_writer& w,
                                               const fc::microseconds& max_action_data_serialization_time, bool short_path )const {
      fc::datastream<const char*> ds(binary.data(), binary.size());
      binary_to_json_stream(type, ds, w, max_action_data_serialization_time, short_path);
   }

   void abi_serializer::_variant_to_binary( const std::string_view& type, const fc::variant& var, fc::datastream<char *>& ds, impl::variant_to_binary_context& ctx )const
   { try {
      auto h = ctx.enter_scope();
      auto rtype = resolve_type(type);

      auto v_itr = variants.end();
      auto s_itr = structs.end();
      auto fixed_array_sz = is_szarray(rtype);

      auto pack_array = [&](const vector<fc::variant>& vars) {
         auto h1 = ctx.push_to_path(impl::array_index_path_item{});
         auto h2 = ctx.disallow_extensions_unless(false);

         int64_t i = 0;
         for (const auto& var : vars) {
            ctx.set_array_index_of_path_back(i);
            _variant_to_binary(fundamental_type(rtype), var, ds, ctx);
            ++i;
         }
      };
      if (fixed_array_sz) {
         size_t sz = *fixed_array_sz;
         ctx.hint_array_type_if_in_array();
         const vector<fc::variant>& vars = var.get_array();
         SYS_ASSERT( vars.size() == sz, pack_exception,
                     "Incorrect number of values provided ({}) for fixed-size ({}) array type", sz, vars.size());
         pack_array(vars);
      } else if( auto btype = built_in_types.find(fundamental_type(rtype)); btype != built_in_types.end() ) {
         btype->second.second(var, ds, is_array(rtype), is_optional(rtype), ctx.get_yield_function());
      } else if( is_protobuf_type(fundamental_type(rtype)) ) {
         try {
            pb_variant_to_binary(fundamental_type(rtype), var, ds);
         } SYS_RETHROW_EXCEPTIONS( pack_exception, "Unable to pack protobuf type '{}' while processing '{}'",
                                   impl::limit_size(fundamental_type(rtype)), ctx.get_path_string() )
      } else if ( is_array(rtype) ) {
         ctx.hint_array_type_if_in_array();
         const vector<fc::variant>& vars = var.get_array();
         fc::raw::pack(ds, (fc::unsigned_int)vars.size());
         pack_array(vars);
      } else if( is_optional(rtype) ) {
         char flag = !var.is_null();
         fc::raw::pack(ds, flag);
         if( flag ) {
            _variant_to_binary(fundamental_type(rtype), var, ds, ctx);
         }
      } else if( auto e_itr = enums.find(rtype); e_itr != enums.end() ) {
         // Enum type: accept string member name or integer value.
         // For string matching, tries exact match first, then prefix-stripped match
         // (e.g., "ethereum" matches "chain_kind_ethereum" by stripping the "chain_kind_" prefix).
         auto btype = built_in_types.find(e_itr->second.type);
         SYS_ASSERT( btype != built_in_types.end(), invalid_type_inside_abi,
                     "Enum '{}' has unknown underlying type '{}'", ctx.maybe_shorten(rtype), ctx.maybe_shorten(e_itr->second.type) );
         fc::variant val_to_pack;
         if( var.is_string() ) {
            auto name_str = var.get_string();
            bool found = false;
            // Pass 1: exact match
            for( const auto& ev : e_itr->second.values ) {
               if( ev.name == name_str ) {
                  val_to_pack = fc::variant(ev.value);
                  found = true;
                  break;
               }
            }
            // Pass 2: prefix-stripped match — enum members often have a common prefix
            // derived from the type name (e.g., "chain_kind_" for type "chain_kind_t").
            // Try matching "name_str" as a suffix of each member name after a '_' separator.
            if( !found ) {
               for( const auto& ev : e_itr->second.values ) {
                  auto pos = ev.name.rfind('_');
                  if( pos != std::string::npos && ev.name.substr(pos + 1) == name_str ) {
                     val_to_pack = fc::variant(ev.value);
                     found = true;
                     break;
                  }
               }
            }
            SYS_ASSERT( found, pack_exception,
                        "Unknown enum value '{}' for enum '{}' while processing '{}'",
                        ctx.maybe_shorten(name_str), ctx.maybe_shorten(rtype), ctx.get_path_string() );
         } else {
            val_to_pack = var;
         }
         btype->second.second(val_to_pack, ds, false, false, ctx.get_yield_function());
      } else if( (v_itr = variants.find(rtype)) != variants.end() ) {
         ctx.hint_variant_type_if_in_array( v_itr );
         auto& v = v_itr->second;
         SYS_ASSERT( var.is_array() && var.size() == 2, pack_exception,
                    "Expected input to be an array of two items while processing variant '{}'", ctx.get_path_string() );
         SYS_ASSERT( var[size_t(0)].is_string(), pack_exception,
                    "Encountered non-string as first item of input array while processing variant '{}'", ctx.get_path_string() );
         auto variant_type_str = var[size_t(0)].get_string();
         auto it = find(v.types.begin(), v.types.end(), variant_type_str);
         SYS_ASSERT( it != v.types.end(), pack_exception,
                     "Specified type '{}' in input array is not valid within the variant '{}'",
                     ctx.maybe_shorten(variant_type_str), ctx.get_path_string() );
         fc::raw::pack(ds, fc::unsigned_int(it - v.types.begin()));
         auto h1 = ctx.push_to_path( impl::variant_path_item{ .variant_itr = v_itr, .variant_ordinal = static_cast<uint32_t>(it - v.types.begin()) } );
         _variant_to_binary( *it, var[size_t(1)], ds, ctx );
      } else if( (s_itr = structs.find(rtype)) != structs.end() ) {
         ctx.hint_struct_type_if_in_array( s_itr );
         const auto& st = s_itr->second;

         if( var.is_object() ) {
            const auto& vo = var.get_object();

            if( st.base != type_name() ) {
               auto h2 = ctx.disallow_extensions_unless(false);
               _variant_to_binary(resolve_type(st.base), var, ds, ctx);
            }
            bool disallow_additional_fields = false;
            for( uint32_t i = 0; i < st.fields.size(); ++i ) {
               const auto& field = st.fields[i];
               bool present = vo.contains(string(field.name).c_str());
               if( present || is_optional(field.type) ) {
                  if( disallow_additional_fields )
                     SYS_THROW( pack_exception, "Unexpected field '{}' found in input object while processing struct '{}'",
                                ctx.maybe_shorten(field.name), ctx.get_path_string() );
                  {
                     auto h1 = ctx.push_to_path( impl::field_path_item{ .parent_struct_itr = s_itr, .field_ordinal = i } );
                     auto h2 = ctx.disallow_extensions_unless( &field == &st.fields.back() );
                     _variant_to_binary(_remove_bin_extension(field.type), present ? vo[field.name] : fc::variant(nullptr), ds, ctx);
                  }
               } else if( field.type.ends_with("$") && ctx.extensions_allowed() ) {
                  disallow_additional_fields = true;
               } else if( disallow_additional_fields ) {
                  SYS_THROW( abi_exception, "Encountered field '{}' without binary extension designation while processing struct '{}'",
                             ctx.maybe_shorten(field.name), ctx.get_path_string() );
               } else {
                  SYS_THROW( pack_exception, "Missing field '{}' in input object while processing struct '{}'",
                             ctx.maybe_shorten(field.name), ctx.get_path_string() );
               }
            }
         } else if( var.is_array() ) {
            const auto& va = var.get_array();
            SYS_ASSERT( st.base == type_name(), invalid_type_inside_abi,
                        "Using input array to specify the fields of the derived struct '{}'; input arrays are currently only allowed for structs without a base",
                        ctx.get_path_string() );
            for( uint32_t i = 0; i < st.fields.size(); ++i ) {
               const auto& field = st.fields[i];
               if( va.size() > i ) {
                  auto h1 = ctx.push_to_path( impl::field_path_item{ .parent_struct_itr = s_itr, .field_ordinal = i } );
                  auto h2 = ctx.disallow_extensions_unless( &field == &st.fields.back() );
                  _variant_to_binary(_remove_bin_extension(field.type), va[i], ds, ctx);
               } else if( field.type.ends_with("$") && ctx.extensions_allowed() ) {
                  break;
               } else {
                  SYS_THROW( pack_exception, "Early end to input array specifying the fields of struct '{}'; require input for field '{}'",
                             ctx.get_path_string(), ctx.maybe_shorten(field.name) );
               }
            }
         } else {
            SYS_THROW( pack_exception, "Unexpected input encountered while processing struct '{}'", ctx.get_path_string() );
         }
      } else {
         SYS_THROW( invalid_type_inside_abi, "Unknown type {}", ctx.maybe_shorten(type) );
      }
   } FC_CAPTURE_AND_RETHROW("") }

   bytes abi_serializer::_variant_to_binary( const std::string_view& type, const fc::variant& var, impl::variant_to_binary_context& ctx )const
   { try {
      auto h = ctx.enter_scope();
      if( !_is_type(type, ctx) ) {
         return var.as<bytes>();
      }

      bytes temp( 1024*1024 );
      fc::datastream<char*> ds(temp.data(), temp.size() );
      _variant_to_binary(type, var, ds, ctx);
      temp.resize(ds.tellp());
      return temp;
   } FC_CAPTURE_AND_RETHROW("") }

   bytes abi_serializer::variant_to_binary( const std::string_view& type, const fc::variant& var, const yield_function_t& yield, bool short_path )const {
      impl::variant_to_binary_context ctx(*this, yield, fc::microseconds{}, type);
      ctx.short_path = short_path;
      return _variant_to_binary(type, var, ctx);
   }

   bytes abi_serializer::variant_to_binary( const std::string_view& type, const fc::variant& var, const fc::microseconds& max_action_data_serialization_time, bool short_path ) const {
      impl::variant_to_binary_context ctx(*this, create_depth_yield_function(), max_action_data_serialization_time, type);
      ctx.short_path = short_path;
      return _variant_to_binary(type, var, ctx);
   }

   void  abi_serializer::variant_to_binary( const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds, const yield_function_t& yield, bool short_path )const {
      impl::variant_to_binary_context ctx(*this, yield, fc::microseconds{}, type);
      ctx.short_path = short_path;
      _variant_to_binary(type, var, ds, ctx);
   }

   void  abi_serializer::variant_to_binary( const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds, const fc::microseconds& max_action_data_serialization_time, bool short_path ) const {
      impl::variant_to_binary_context ctx(*this, create_depth_yield_function(), max_action_data_serialization_time, type);
      ctx.short_path = short_path;
      _variant_to_binary(type, var, ds, ctx);
   }

   type_name abi_serializer::get_action_type(name action)const {
      auto itr = actions.find(action);
      if( itr != actions.end() ) return itr->second;
      return type_name();
   }
   type_name abi_serializer::get_table_type(const string_view& table)const {
      auto itr = tables.find(table);
      if( itr != tables.end() ) return itr->second;
      return type_name();
   }

   type_name abi_serializer::get_action_result_type(name action_result)const {
      auto itr = action_results.find(action_result);
      if( itr != action_results.end() ) return itr->second;
      return type_name();
   }

   std::optional<string> abi_serializer::get_error_message( uint64_t error_code )const {
      auto itr = error_messages.find( error_code );
      if( itr == error_messages.end() )
         return std::optional<string>();

      return itr->second;
   }

   namespace impl {

      fc::scoped_exit<std::function<void()>> abi_traverse_context::enter_scope() {
         std::function<void()> callback = [this](){ --recursion_depth; };

         ++recursion_depth;
         yield( recursion_depth );

         return {std::move(callback)};
      }

      void abi_traverse_context_with_path::set_path_root( const std::string_view& type ) {
         auto rtype = abis.resolve_type(type);

         if( abis.is_array(rtype) ) {
            root_of_path =  array_type_path_root{};
         } else {
            auto itr1 = abis.structs.find(rtype);
            if( itr1 != abis.structs.end() ) {
               root_of_path = struct_type_path_root{ .struct_itr = itr1 };
            } else {
               auto itr2 = abis.variants.find(rtype);
               if( itr2 != abis.variants.end() ) {
                  root_of_path = variant_type_path_root{ .variant_itr = itr2 };
               }
            }
         }
      }

      fc::scoped_exit<std::function<void()>> abi_traverse_context_with_path::push_to_path( const path_item& item ) {
         std::function<void()> callback = [this](){
            SYS_ASSERT( path.size() > 0, abi_exception,
                        "invariant failure in variant_to_binary_context: path is empty on scope exit" );
            path.pop_back();
         };

         path.push_back( item );

         return {std::move(callback)};
      }

      void abi_traverse_context_with_path::set_array_index_of_path_back( uint32_t i ) {
         SYS_ASSERT( path.size() > 0, abi_exception, "path is empty" );

         auto& b = path.back();

         SYS_ASSERT( std::holds_alternative<array_index_path_item>(b), abi_exception, "trying to set array index without first pushing new array index item" );

         std::get<array_index_path_item>(b).array_index = i;
      }

      void abi_traverse_context_with_path::hint_array_type_if_in_array() {
         if( path.size() == 0 || !std::holds_alternative<array_index_path_item>(path.back()) )
            return;

         std::get<array_index_path_item>(path.back()).type_hint = array_type_path_root{};
      }

      void abi_traverse_context_with_path::hint_struct_type_if_in_array( const map<type_name, struct_def>::const_iterator& itr ) {
         if( path.size() == 0 || !std::holds_alternative<array_index_path_item>(path.back()) )
            return;

         std::get<array_index_path_item>(path.back()).type_hint = struct_type_path_root{ .struct_itr = itr };
      }

      void abi_traverse_context_with_path::hint_variant_type_if_in_array( const map<type_name, variant_def>::const_iterator& itr ) {
         if( path.size() == 0 || !std::holds_alternative<array_index_path_item>(path.back()) )
            return;

         std::get<array_index_path_item>(path.back()).type_hint = variant_type_path_root{ .variant_itr = itr };
      }

      constexpr size_t const_strlen( const char* str )
      {
          return (*str == 0) ? 0 : const_strlen(str + 1) + 1;
      }

      void output_name( std::ostream& s, const string_view& str, bool shorten, size_t max_length = 64 ) {
         constexpr size_t min_num_characters_at_ends = 4;
         constexpr size_t preferred_num_tail_end_characters = 6;
         constexpr const char* fill_in = "...";

         static_assert( min_num_characters_at_ends <= preferred_num_tail_end_characters,
                        "preferred number of tail end characters cannot be less than the imposed absolute minimum" );

         constexpr size_t fill_in_length = const_strlen( fill_in );
         constexpr size_t min_length = fill_in_length + 2*min_num_characters_at_ends;
         constexpr size_t preferred_min_length = fill_in_length + 2*preferred_num_tail_end_characters;

         max_length = std::max( max_length, min_length );

         if( !shorten || str.size() <= max_length ) {
            s << str;
            return;
         }

         size_t actual_num_tail_end_characters = preferred_num_tail_end_characters;
         if( max_length < preferred_min_length ) {
            actual_num_tail_end_characters = min_num_characters_at_ends + (max_length - min_length)/2;
         }

         s.write( str.data(), max_length - fill_in_length - actual_num_tail_end_characters );
         s.write( fill_in, fill_in_length );
         s.write( str.data() + (str.size() - actual_num_tail_end_characters), actual_num_tail_end_characters );
      }

      struct generate_path_string_visitor {
         using result_type = void;

         generate_path_string_visitor( bool shorten_names, bool track_only )
         : shorten_names(shorten_names), track_only( track_only )
         {}

         std::stringstream s;
         bool              shorten_names = false;
         bool              track_only     = false;
         path_item         last_path_item;

         void add_dot() {
            s << ".";
         }

         void operator()( const empty_path_item& item ) {
         }

         void operator()( const array_index_path_item& item ) {
            if( track_only ) {
               last_path_item = item;
               return;
            }

            s << "[" << item.array_index << "]";
         }

         void operator()( const field_path_item& item ) {
            if( track_only ) {
               last_path_item = item;
               return;
            }

            const auto& str = item.parent_struct_itr->second.fields.at(item.field_ordinal).name;
            output_name( s, str, shorten_names );
         }

         void operator()( const variant_path_item& item ) {
            if( track_only ) {
               last_path_item = item;
               return;
            }

            s << "<variant(" << item.variant_ordinal << ")=";
            const auto& str = item.variant_itr->second.types.at(item.variant_ordinal);
            output_name( s, str, shorten_names );
            s << ">";
         }

         void operator()( const empty_path_root& item ) {
         }

         void operator()( const array_type_path_root& item ) {
            s << "ARRAY";
         }

         void operator()( const struct_type_path_root& item ) {
            const auto& str = item.struct_itr->first;
            output_name( s, str, shorten_names );
         }

         void operator()( const variant_type_path_root& item ) {
            const auto& str = item.variant_itr->first;
            output_name( s, str, shorten_names );
         }
      };

      struct path_item_type_visitor {
         using result_type = void;

         path_item_type_visitor( std::stringstream& s, bool shorten_names )
         : s(s), shorten_names(shorten_names)
         {}

         std::stringstream& s;
         bool               shorten_names = false;

         void operator()( const empty_path_item& item ) {
         }

         void operator()( const array_index_path_item& item ) {
            const auto& th = item.type_hint;
            if( std::holds_alternative<struct_type_path_root>(th) ) {
               const auto& str = std::get<struct_type_path_root>(th).struct_itr->first;
               output_name( s, str, shorten_names );
            } else if( std::holds_alternative<variant_type_path_root>(th) ) {
               const auto& str = std::get<variant_type_path_root>(th).variant_itr->first;
               output_name( s, str, shorten_names );
            } else if( std::holds_alternative<array_type_path_root>(th) ) {
               s << "ARRAY";
            } else {
               s << "UNKNOWN";
            }
         }

         void operator()( const field_path_item& item ) {
            const auto& str = item.parent_struct_itr->second.fields.at(item.field_ordinal).type;
            output_name( s, str, shorten_names );
         }

         void operator()( const variant_path_item& item ) {
            const auto& str = item.variant_itr->second.types.at(item.variant_ordinal);
            output_name( s, str, shorten_names );
         }
      };

      string abi_traverse_context_with_path::get_path_string()const {
         bool full_path = !short_path;
         bool shorten_names = short_path;

         generate_path_string_visitor visitor(shorten_names, !full_path);
         if( full_path )
            std::visit( visitor, root_of_path );
         for( size_t i = 0, n = path.size(); i < n; ++i ) {
            if( full_path && !std::holds_alternative<array_index_path_item>(path[i]) )
               visitor.add_dot();

            std::visit( visitor, path[i] );

         }

         if( !full_path ) {
            if( std::holds_alternative<empty_path_item>(visitor.last_path_item) ) {
               std::visit( visitor, root_of_path );
            } else {
               path_item_type_visitor vis2(visitor.s, shorten_names);
               std::visit(vis2, visitor.last_path_item);
            }
         }

         return visitor.s.str();
      }

      string abi_traverse_context_with_path::maybe_shorten( const std::string_view& str ) {
         if( !short_path )
            return std::string(str);

         std::stringstream s;
         output_name( s, str, true );
         return s.str();
      }

      string limit_size( const std::string_view& str ) {
         std::stringstream s;
         output_name( s, str, false );
         return s.str();
      }

      fc::scoped_exit<std::function<void()>> variant_to_binary_context::disallow_extensions_unless( bool condition ) {
         std::function<void()> callback = [old_allow_extensions=allow_extensions, this](){
            allow_extensions = old_allow_extensions;
         };

         if( !condition ) {
            allow_extensions = false;
         }

         return {std::move(callback)};
      }
   }

} // namespace sysio::chain

namespace fc {

void to_variant(const sysio::chain::abi_def& abi, fc::variant& v) {
   fc::mutable_variant_object mvo;
   fc::reflector<sysio::chain::abi_def>::visit(
      fc::to_variant_visitor<sysio::chain::abi_def>(mvo, abi)
   );
   // protobuf_types: omit when empty, otherwise replace string with JSON object
   if( abi.protobuf_types.value.empty() ) {
      mvo.erase("protobuf_types");
   } else {
      mvo["protobuf_types"] = fc::json::from_string(abi.protobuf_types.value);
   }
   v = std::move(mvo);
}

void from_variant(const fc::variant& v, sysio::chain::abi_def& abi) {
   // Strip protobuf_types before reflector visit (it's a JSON object but stored as string)
   fc::variant clean_v;
   if( v.is_object() && v.get_object().contains("protobuf_types") ) {
      fc::mutable_variant_object mvo(v.get_object());
      auto pt = mvo["protobuf_types"];
      mvo.erase("protobuf_types");
      clean_v = fc::variant(std::move(mvo));
      fc::reflector<sysio::chain::abi_def>::visit(
         fc::from_variant_visitor<sysio::chain::abi_def>(clean_v.get_object(), abi)
      );
      // Handle protobuf_types: accept JSON object or string and stringify it
      if( pt.is_object() ) {
         abi.protobuf_types.value = fc::json::to_string(pt, fc::time_point::maximum());
      } else if( pt.is_string() ) {
         abi.protobuf_types.value = pt.get_string();
      }
   } else {
      fc::reflector<sysio::chain::abi_def>::visit(
         fc::from_variant_visitor<sysio::chain::abi_def>(v.get_object(), abi)
      );
   }
}

} // namespace fc

namespace sysio::chain::impl {

// -- variant_sink ------------------------------------------------------------------------

void variant_sink::begin_object() {
   stack_.emplace_back();
   stack_.back().kind = frame_kind::object;
}

void variant_sink::end_object() {
   FC_ASSERT(!stack_.empty() && stack_.back().kind == frame_kind::object,
             "variant_sink: end_object without matching begin_object");
   auto popped = std::move(stack_.back());
   stack_.pop_back();
   emit_value(fc::variant(std::move(popped.mvo)));
}

void variant_sink::begin_array() {
   stack_.emplace_back();
   stack_.back().kind = frame_kind::array;
}

void variant_sink::end_array() {
   FC_ASSERT(!stack_.empty() && stack_.back().kind == frame_kind::array,
             "variant_sink: end_array without matching begin_array");
   auto popped = std::move(stack_.back());
   stack_.pop_back();
   emit_value(fc::variant(std::move(popped.arr)));
}

void variant_sink::key(std::string_view k) {
   FC_ASSERT(!stack_.empty() && stack_.back().kind == frame_kind::object,
             "variant_sink: key emitted outside an object frame");
   auto& f = stack_.back();
   f.pending_key.assign(k.data(), k.size());
   f.has_pending_key = true;
}

void variant_sink::value_string(std::string_view s) { emit_value(fc::variant(s)); }
void variant_sink::value_int64(int64_t n)           { emit_value(fc::variant(n)); }
void variant_sink::value_uint64(uint64_t n)         { emit_value(fc::variant(n)); }
void variant_sink::value_bool(bool b)               { emit_value(fc::variant(b)); }
void variant_sink::value_null()                     { emit_value(fc::variant()); }

void variant_sink::value_hex(const char* data, size_t size) {
   if (size == 0) {
      emit_value(fc::variant(std::string{}));
      return;
   }
   emit_value(fc::variant(fc::to_hex(data, size)));
}

void variant_sink::raw_value(std::string_view raw_json) {
   emit_value(fc::json::from_string(std::string(raw_json)));
}

bool variant_sink::frame_has_items() const noexcept {
   return !stack_.empty() && stack_.back().item_count > 0;
}

void variant_sink::emit_value(fc::variant v) {
   if (stack_.empty()) {
      result_ = std::move(v);
      return;
   }
   auto& f = stack_.back();
   if (f.kind == frame_kind::object) {
      FC_ASSERT(f.has_pending_key, "variant_sink: value without preceding key in object frame");
      f.mvo(std::move(f.pending_key), std::move(v));
      f.pending_key.clear();
      f.has_pending_key = false;
   } else {
      f.arr.emplace_back(std::move(v));
   }
   ++f.item_count;
}

void variant_sink::unpack_built_in(std::string_view ftype, fc::datastream<const char*>& stream,
                                   bool is_array, bool is_optional, abi_traverse_context& ctx) {
   FC_ASSERT(abi_, "variant_sink::unpack_built_in requires the abi_serializer-bound constructor");
   auto bv = abi_->find_built_in(ftype);
   FC_ASSERT(bv, "variant_sink::unpack_built_in: '{}' is not a built-in", std::string(ftype));
   emit_value(bv->first(stream, is_array, is_optional, ctx.get_yield_function()));
}

void variant_sink::unpack_protobuf(std::string_view ftype, fc::datastream<const char*>& stream) {
   FC_ASSERT(abi_, "variant_sink::unpack_protobuf requires the abi_serializer-bound constructor");
   emit_value(abi_->pb_binary_to_variant(ftype, stream));
}

void variant_sink::unpack_action_data(const abi_serializer& abi, std::string_view type, const bytes& data,
                                      abi_traverse_context& ctx, bool short_path) {
   action_data_to_variant_context inner(abi, ctx, type);
   inner.short_path = short_path;
   emit_value(abi._binary_to_variant(type, data, inner));
}

bool variant_sink::unpack_action_data_field(std::string_view key_name, const abi_serializer& abi,
                                            std::string_view type, const bytes& data,
                                            abi_traverse_context& ctx, bool short_path) {
   // _binary_to_variant builds the variant before we touch the sink; if it throws
   // pending_key was set by key() but never committed by emit_value, so we just
   // clear it and the surviving frame is left as if neither call happened.
   try {
      key(key_name);
      action_data_to_variant_context inner(abi, ctx, type);
      inner.short_path = short_path;
      emit_value(abi._binary_to_variant(type, data, inner));
      return true;
   } catch(...) {
      if (!stack_.empty()) {
         auto& f = stack_.back();
         f.pending_key.clear();
         f.has_pending_key = false;
      }
      return false;
   }
}

// -- stream_sink -------------------------------------------------------------------------

void stream_sink::begin_object() {
   w_.begin_object();
   frame_items_.emplace_back(0);
}

void stream_sink::end_object() {
   w_.end_object();
   FC_ASSERT(!frame_items_.empty(), "stream_sink: end_object without matching begin_object");
   frame_items_.pop_back();
   on_value_emitted();
}

void stream_sink::begin_array() {
   w_.begin_array();
   frame_items_.emplace_back(0);
}

void stream_sink::end_array() {
   w_.end_array();
   FC_ASSERT(!frame_items_.empty(), "stream_sink: end_array without matching begin_array");
   frame_items_.pop_back();
   on_value_emitted();
}

void stream_sink::key(std::string_view k) { w_.key(k); }

void stream_sink::value_string(std::string_view s) { w_.value_string(s); on_value_emitted(); }
// 64-bit integers route through the guarded fc::to_json_stream overloads (not the writer's
// raw value_* emitters) so magnitudes past json_integer_quote_magnitude emit quoted, exactly
// as variant_sink::value_int64's fc::variant(n) renders through fc::json::to_string.  ABI
// built-in int64/uint64 fields already stream through the guarded generic in
// get_built_in_stream_unpacks; this pins the same quoting for the sink interface itself, so
// direct sink callers (the bytes size field today, any future caller) cannot diverge
// between the two sinks.
void stream_sink::value_int64(int64_t n)           { fc::to_json_stream(n, w_);  on_value_emitted(); }
void stream_sink::value_uint64(uint64_t n)         { fc::to_json_stream(n, w_); on_value_emitted(); }
void stream_sink::value_bool(bool b)               { w_.value_bool(b);   on_value_emitted(); }
void stream_sink::value_null()                     { w_.value_null();    on_value_emitted(); }

void stream_sink::value_hex(const char* data, size_t size) {
   w_.value_hex(data, size);
   on_value_emitted();
}

void stream_sink::raw_value(std::string_view raw_json) {
   w_.raw_value(raw_json);
   on_value_emitted();
}

void stream_sink::value_variant(const fc::variant& v) {
   fc::to_json_stream(v, w_);
   on_value_emitted();
}

bool stream_sink::frame_has_items() const noexcept {
   return !frame_items_.empty() && frame_items_.back() > 0;
}

void stream_sink::on_value_emitted() noexcept {
   if (!frame_items_.empty()) ++frame_items_.back();
}

void stream_sink::unpack_built_in(std::string_view ftype, fc::datastream<const char*>& stream,
                                  bool is_array, bool is_optional, abi_traverse_context& ctx) {
   const auto& dispatch = get_built_in_stream_unpacks();
   auto it = dispatch.find(ftype);
   if( it != dispatch.end() ) {
      it->second(stream, is_array, is_optional, ctx.get_yield_function(), w_);
      on_value_emitted();
      return;
   }
   // Variant-side may carry user overrides via add_specialized_unpack_pack that the
   // streaming dispatch does not mirror today.  Fall back to the variant unpack +
   // to_json_stream(variant, w) bridge so those overrides remain effective.
   FC_ASSERT(abi_, "stream_sink::unpack_built_in requires the abi_serializer-bound constructor");
   auto bv = abi_->find_built_in(ftype);
   FC_ASSERT(bv, "stream_sink::unpack_built_in: '{}' is not a built-in", std::string(ftype));
   auto v = bv->first(stream, is_array, is_optional, ctx.get_yield_function());
   value_variant(v);
}

void stream_sink::unpack_protobuf(std::string_view ftype, fc::datastream<const char*>& stream) {
   // MessageToJsonString already produces JSON; splice it directly into the writer.
   FC_ASSERT(abi_, "stream_sink::unpack_protobuf requires the abi_serializer-bound constructor");
   raw_value(abi_->pb_binary_to_json_string(ftype, stream));
}

void stream_sink::unpack_action_data(const abi_serializer& abi, std::string_view type, const bytes& data,
                                     abi_traverse_context& ctx, bool short_path) {
   action_data_to_variant_context inner(abi, ctx, type);
   inner.short_path = short_path;
   fc::datastream<const char*> ds(data.data(), data.size());
   abi._binary_walk(type, ds, *this, inner);
}

bool stream_sink::unpack_action_data_field(std::string_view key_name, const abi_serializer& abi,
                                           std::string_view type, const bytes& data,
                                           abi_traverse_context& ctx, bool short_path) {
   // Snapshot writer + frame_items_ before the key emit; on throw we rewind both
   // so the buffer / frame stack are byte-identical to the pre-call state.  The
   // walker may have opened nested object frames before throwing, so resizing
   // frame_items_ back to its saved depth is required to keep it in lockstep
   // with the json_writer's internal stack.
   const auto writer_cp = w_.checkpoint();
   const size_t fi_save = frame_items_.size();
   const uint32_t items_save = frame_items_.empty() ? 0 : frame_items_.back();
   try {
      w_.key(key_name);
      action_data_to_variant_context inner(abi, ctx, type);
      inner.short_path = short_path;
      fc::datastream<const char*> ds(data.data(), data.size());
      abi._binary_walk(type, ds, *this, inner);
      on_value_emitted();
      return true;
   } catch(...) {
      w_.rewind(writer_cp);
      frame_items_.resize(fi_save);
      if (!frame_items_.empty()) frame_items_.back() = items_save;
      return false;
   }
}

} // namespace sysio::chain::impl

// -- _binary_walk<Sink> et al ------------------------------------------------------------

namespace sysio::chain {

template<typename Sink>
void abi_serializer::_unpack_enum( const enum_def& e_def, fc::datastream<const char*>& stream,
                                   Sink& sink, impl::binary_to_variant_context& ctx )const
{
   auto btype = built_in_types.find(e_def.type);
   SYS_ASSERT( btype != built_in_types.end(), invalid_type_inside_abi,
               "Enum has unknown underlying type '{}'", impl::limit_size(e_def.type) );
   // Read the underlying integer through the variant-side functor.  Both sinks emit the
   // same shape (string name on hit, raw integer on miss) — the parity is structural.
   auto int_var = btype->second.first(stream, false, false, ctx.get_yield_function());
   auto int_val = int_var.as_int64();
   for( const auto& ev : e_def.values ) {
      if( ev.value == int_val ) {
         sink.value_string(ev.name);
         return;
      }
   }
   // Unknown value -- emit as the underlying integer.  Both sinks take by const-ref
   // and copy internally; the int_var temporary lives until end of expression.
   sink.value_variant(int_var);
}

template<typename Sink>
void abi_serializer::_binary_walk_struct_fields(
      const map<type_name, struct_def, std::less<>>::const_iterator& s_itr,
      fc::datastream<const char*>& stream,
      Sink& sink, impl::binary_to_variant_context& ctx )const
{
   const auto& st = s_itr->second;
   if( st.base != type_name() ) {
      auto base_itr = structs.find(resolve_type(st.base));
      SYS_ASSERT( base_itr != structs.end(), invalid_type_inside_abi,
                  "Unknown base type {}", impl::limit_size(st.base) );
      _binary_walk_struct_fields(base_itr, stream, sink, ctx);
   }
   bool encountered_extension = false;
   for( uint32_t i = 0; i < st.fields.size(); ++i ) {
      const auto& field = st.fields[i];
      bool extension = field.type.ends_with("$");
      encountered_extension |= extension;
      if( !stream.remaining() ) {
         if( extension ) {
            continue;
         }
         if( encountered_extension ) {
            SYS_THROW( abi_exception, "Encountered field '{}' without binary extension designation while processing struct '{}'",
                       ctx.maybe_shorten(field.name), ctx.get_path_string() );
         }
         SYS_THROW( unpack_exception, "Stream unexpectedly ended; unable to unpack field '{}' of struct '{}'",
                    ctx.maybe_shorten(field.name), ctx.get_path_string() );
      }
      auto h1 = ctx.push_to_path( impl::field_path_item{ .parent_struct_itr = s_itr, .field_ordinal = i } );
      auto field_type = resolve_type( extension ? _remove_bin_extension(field.type) : field.type );
      sink.key(field.name);
      if( ctx.is_logging() && field_type == "bytes" ) {
         // Logging-bytes special case: variant path replaces the hex string with
         // {size: N, hex|trimmed_hex: ...}.  Read raw bytes here and emit the
         // sub-object directly so the streaming sink doesn't need to retroactively
         // rewrite a previously-emitted value.
         std::vector<char> data;
         try {
            fc::raw::unpack(stream, data);
         } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack 'bytes' for field '{}' of struct '{}'",
                                   ctx.maybe_shorten(field.name), ctx.get_path_string() )
         sink.begin_object();
         sink.key("size");
         sink.value_uint64(data.size());
         if( data.size() > impl::hex_log_max_size ) {
            sink.key("trimmed_hex");
            sink.value_hex(data.data(), impl::hex_log_max_size);
         } else {
            sink.key("hex");
            sink.value_hex(data.data(), data.size());
         }
         sink.end_object();
      } else {
         _binary_walk(field_type, stream, sink, ctx);
      }
   }
}

template<typename Sink>
void abi_serializer::_binary_walk( const std::string_view& type, fc::datastream<const char*>& stream,
                                   Sink& sink, impl::binary_to_variant_context& ctx )const
{
   auto h = ctx.enter_scope();
   auto rtype = resolve_type(type);
   auto ftype = fundamental_type(rtype);
   auto fixed_array_sz = is_szarray(rtype);

   auto walk_array = [&](fc::unsigned_int::base_uint sz) {
      ctx.hint_array_type_if_in_array();
      sink.begin_array();
      auto h1 = ctx.push_to_path( impl::array_index_path_item{} );
      for( fc::unsigned_int::base_uint i = 0; i < sz; ++i ) {
         ctx.set_array_index_of_path_back(i);
         _binary_walk(ftype, stream, sink, ctx);
      }
      sink.end_array();
   };

   if( fixed_array_sz ) {
      walk_array(*fixed_array_sz);
      return;
   }

   if( auto btype = built_in_types.find(ftype); btype != built_in_types.end() ) {
      try {
         sink.unpack_built_in(ftype, stream, is_array(rtype), is_optional(rtype), ctx);
         return;
      } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack {} type '{}' while processing '{}'",
                                is_array(rtype) ? "array of built-in" : is_optional(rtype) ? "optional of built-in" : "built-in",
                                impl::limit_size(ftype), ctx.get_path_string() )
   }

   if( is_protobuf_type(ftype) ) {
      try {
         sink.unpack_protobuf(ftype, stream);
         return;
      } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack protobuf type '{}' while processing '{}'",
                                impl::limit_size(ftype), ctx.get_path_string() )
   }

   if( is_array(rtype) ) {
      fc::unsigned_int size;
      try {
         fc::raw::unpack(stream, size);
      } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack size of array '{}'", ctx.get_path_string() )
      walk_array(size.value);
      return;
   }

   if( is_optional(rtype) ) {
      char flag;
      try {
         fc::raw::unpack(stream, flag);
      } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack presence flag of optional '{}'", ctx.get_path_string() )
      if( flag ) {
         _binary_walk(ftype, stream, sink, ctx);
      } else {
         sink.value_null();
      }
      return;
   }

   if( auto e_itr = enums.find(rtype); e_itr != enums.end() ) {
      _unpack_enum(e_itr->second, stream, sink, ctx);
      return;
   }

   if( auto v_itr = variants.find(rtype); v_itr != variants.end() ) {
      ctx.hint_variant_type_if_in_array( v_itr );
      fc::unsigned_int select;
      try {
         fc::raw::unpack(stream, select);
      } SYS_RETHROW_EXCEPTIONS( unpack_exception, "Unable to unpack tag of variant '{}'", ctx.get_path_string() )
      SYS_ASSERT( (size_t)select < v_itr->second.types.size(), unpack_exception,
                  "Unpacked invalid tag ({}) for variant '{}'", select.value, ctx.get_path_string() );
      auto h1 = ctx.push_to_path( impl::variant_path_item{ .variant_itr = v_itr, .variant_ordinal = static_cast<uint32_t>(select) } );
      sink.begin_array();
      sink.value_string(v_itr->second.types[select]);
      _binary_walk(v_itr->second.types[select], stream, sink, ctx);
      sink.end_array();
      return;
   }

   auto s_itr = structs.find(rtype);
   SYS_ASSERT( s_itr != structs.end(), invalid_type_inside_abi, "Unknown type {}", ctx.maybe_shorten(rtype) );
   ctx.hint_struct_type_if_in_array( s_itr );
   sink.begin_object();
   _binary_walk_struct_fields(s_itr, stream, sink, ctx);
   const bool emitted_any = sink.frame_has_items();
   sink.end_object();
   SYS_ASSERT( emitted_any, unpack_exception, "Unable to unpack '{}' from stream", ctx.get_path_string() );
}

// Explicit instantiations for the two sinks defined in abi_sinks.hpp.  Keeps the
// member-template definitions out of headers without sacrificing the link surface.
template void abi_serializer::_binary_walk<impl::variant_sink>(
      const std::string_view&, fc::datastream<const char*>&, impl::variant_sink&,
      impl::binary_to_variant_context&) const;
template void abi_serializer::_binary_walk<impl::stream_sink>(
      const std::string_view&, fc::datastream<const char*>&, impl::stream_sink&,
      impl::binary_to_variant_context&) const;

template void abi_serializer::_binary_walk_struct_fields<impl::variant_sink>(
      const map<type_name, struct_def, std::less<>>::const_iterator&,
      fc::datastream<const char*>&, impl::variant_sink&, impl::binary_to_variant_context&) const;
template void abi_serializer::_binary_walk_struct_fields<impl::stream_sink>(
      const map<type_name, struct_def, std::less<>>::const_iterator&,
      fc::datastream<const char*>&, impl::stream_sink&, impl::binary_to_variant_context&) const;

template void abi_serializer::_unpack_enum<impl::variant_sink>(
      const enum_def&, fc::datastream<const char*>&, impl::variant_sink&,
      impl::binary_to_variant_context&) const;
template void abi_serializer::_unpack_enum<impl::stream_sink>(
      const enum_def&, fc::datastream<const char*>&, impl::stream_sink&,
      impl::binary_to_variant_context&) const;

} // namespace sysio::chain

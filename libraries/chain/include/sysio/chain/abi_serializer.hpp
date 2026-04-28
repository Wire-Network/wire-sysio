#pragma once
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/abi_sinks.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <utility>
#include <fc/variant_object.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/time.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_stream_fwd.hpp>

namespace google::protobuf {
   class DescriptorPool;
   class DynamicMessageFactory;
   class Descriptor;
}

namespace sysio::chain {

using std::map;
using std::string;
using std::function;
using std::pair;
using namespace fc;

namespace impl {
   struct abi_from_variant;
   struct abi_to_variant;

   struct abi_traverse_context;
   struct abi_traverse_context_with_path;
   struct binary_to_variant_context;
   struct variant_to_binary_context;
   struct action_data_to_variant_context;

   class variant_sink;
   class stream_sink;
}

/**
 *  Describes the binary representation message and table contents so that it can
 *  be converted to and from JSON.
 */
struct abi_serializer {

   /// passed recursion_depth on each invocation
   using yield_function_t = fc::optional_delegate<void(size_t)>;

   abi_serializer(){ configure_built_in_types(); }
   abi_serializer( abi_def abi, const yield_function_t& yield );
   [[deprecated("use the overload with yield_function_t[=create_yield_function(max_serialization_time)]")]]
   abi_serializer( const abi_def& abi, const fc::microseconds& max_serialization_time );
   void set_abi( abi_def abi, const yield_function_t& yield );
   [[deprecated("use the overload with yield_function_t[=create_yield_function(max_serialization_time)]")]]
   void set_abi(const abi_def& abi, const fc::microseconds& max_serialization_time);

   /// @return string_view of `t` or internal string type
   std::string_view resolve_type(const std::string_view& t)const;
   bool      is_array(const std::string_view& type)const;
   std::optional<fc::unsigned_int> is_szarray(const std::string_view& type)const;
   bool      is_optional(const std::string_view& type)const;
   bool      is_type( const std::string_view& type, const yield_function_t& yield )const;
   bool      is_type(const std::string_view& type, const fc::microseconds& max_serialization_time)const;
   bool      is_builtin_type(const std::string_view& type)const;
   bool      is_integer(const std::string_view& type) const;
   int       get_integer_size(const std::string_view& type) const;
   bool      is_struct(const std::string_view& type)const;
   bool      is_enum(const std::string_view& type)const;

   /// @return string_view of `type`
   std::string_view fundamental_type(const std::string_view& type)const;

   const struct_def& get_struct(const std::string_view& type)const;

   type_name get_action_type(name action)const;
   type_name get_table_type(const std::string_view& table)const;
   type_name get_action_result_type(name action_result)const;

   std::optional<string>  get_error_message( uint64_t error_code )const;

   fc::variant binary_to_variant( const std::string_view& type, const bytes& binary, const yield_function_t& yield, bool short_path = false )const;
   fc::variant binary_to_variant( const std::string_view& type, const bytes& binary, const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;
   fc::variant binary_to_variant( const std::string_view& type, fc::datastream<const char*>& binary, const yield_function_t& yield, bool short_path = false )const;
   fc::variant binary_to_variant( const std::string_view& type, fc::datastream<const char*>& binary, const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;

   /// Streaming counterpart to `binary_to_variant`.  Decodes the binary blob and
   /// emits its JSON representation directly into `w` without constructing a
   /// `fc::variant` tree.  The output is byte-identical to
   /// `fc::json::to_string(binary_to_variant(...))` for all built-in types and
   /// any reflected user struct that has a corresponding `to_json_stream` overload
   /// (which `FC_REFLECT` provides automatically).
   void binary_to_json_stream( const std::string_view& type, const bytes& binary, fc::json_writer& w,
                               const yield_function_t& yield, bool short_path = false )const;
   void binary_to_json_stream( const std::string_view& type, const bytes& binary, fc::json_writer& w,
                               const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;
   void binary_to_json_stream( const std::string_view& type, fc::datastream<const char*>& binary, fc::json_writer& w,
                               const yield_function_t& yield, bool short_path = false )const;
   void binary_to_json_stream( const std::string_view& type, fc::datastream<const char*>& binary, fc::json_writer& w,
                               const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;

   bytes       variant_to_binary( const std::string_view& type, const fc::variant& var, const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;
   bytes       variant_to_binary( const std::string_view& type, const fc::variant& var, const yield_function_t& yield, bool short_path = false )const;
   void        variant_to_binary( const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds, const fc::microseconds& max_action_data_serialization_time, bool short_path = false )const;
   void        variant_to_binary( const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds, const yield_function_t& yield, bool short_path = false )const;

   template<typename T, typename Resolver>
   static void to_variant( const T& o, fc::variant& vo, const Resolver& resolver, const yield_function_t& yield );
   template<typename T, typename Resolver>
   static void to_variant( const T& o, fc::variant& vo, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time );

   template<typename T, typename Resolver>
   static void to_log_variant( const T& o, fc::variant& vo, const Resolver& resolver, const yield_function_t& yield );
   template<typename T, typename Resolver>
   static void to_log_variant( const T& o, fc::variant& vo, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time );

   /// Streaming counterpart to `to_variant`: emits the same JSON shape directly into
   /// `w` without building an intermediate `fc::variant` tree.  Reflected types route
   /// through the same `impl::abi_to_variant` machinery, which is sink-templated -- the
   /// only difference between this path and `to_variant` is the sink instance passed in.
   template<typename T, typename Resolver>
   static void to_json_stream( const T& o, fc::json_writer& w, const Resolver& resolver, const yield_function_t& yield );
   template<typename T, typename Resolver>
   static void to_json_stream( const T& o, fc::json_writer& w, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time );

   template<typename T, typename Resolver>
   static void from_variant( const fc::variant& v, T& o, const Resolver& resolver, const yield_function_t& yield );
   template<typename T, typename Resolver>
   static void from_variant( const fc::variant& v, T& o, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time );

   template<typename Vec>
   static bool is_empty_abi(const Vec& abi_vec)
   {
      return abi_vec.size() <= 4;
   }

   template<typename Vec>
   static bool to_abi(const Vec& abi_vec, abi_def& abi)
   {
      if( !is_empty_abi(abi_vec) ) { /// 4 == packsize of empty Abi
         fc::datastream<const char*> ds( abi_vec.data(), abi_vec.size() );
         fc::raw::unpack( ds, abi );
         return true;
      }
      return false;
   }

   typedef std::function<fc::variant(fc::datastream<const char*>&, bool, bool, const abi_serializer::yield_function_t&)>  unpack_function;
   typedef std::function<void(const fc::variant&, fc::datastream<char*>&, bool, bool, const abi_serializer::yield_function_t&)>  pack_function;

   void add_specialized_unpack_pack( const string& name, std::pair<abi_serializer::unpack_function, abi_serializer::pack_function> unpack_pack );

   /// Lookup helper used by the streaming walker sinks.  Returns a pointer to the
   /// (unpack, pack) entry registered for `type`, or `nullptr` if it is not a
   /// built-in.  Const view; the underlying map is shared with `binary_to_variant`
   /// dispatch so any `add_specialized_unpack_pack` overrides are reflected.
   const std::pair<unpack_function, pack_function>* find_built_in(std::string_view type) const noexcept;

   static constexpr size_t max_recursion_depth = 32; // arbitrary depth to prevent infinite recursion

   // create standard yield function that checks for max_serialization_time and max_recursion_depth.
   // restricts serialization time from creation of yield function until serialization is complete.
   // now() deadline captured at time of this call
   static yield_function_t create_yield_function(const fc::microseconds& max_serialization_time) {
      fc::time_point deadline = fc::time_point::now().safe_add(max_serialization_time);
      return [max_serialization_time, deadline](size_t recursion_depth) {
         SYS_ASSERT( recursion_depth < max_recursion_depth, abi_recursion_depth_exception,
                     "recursive definition, max_recursion_depth {} ", max_recursion_depth );

         SYS_ASSERT( fc::time_point::now() < deadline, abi_serialization_deadline_exception,
                     "serialization time limit {}us exceeded", max_serialization_time );
      };
   }

   static yield_function_t create_depth_yield_function() {
      return [](size_t recursion_depth) {
         SYS_ASSERT( recursion_depth < max_recursion_depth, abi_recursion_depth_exception,
                     "recursive definition, max_recursion_depth {} ", max_recursion_depth );
      };
   }

private:

   map<type_name, type_name, std::less<>>     typedefs;
   map<type_name, struct_def, std::less<>>    structs;
   map<name,type_name>                        actions;
   map<string, type_name, std::less<>>        tables;
   map<uint64_t, string>                      error_messages;
   map<type_name, variant_def, std::less<>>   variants;
   map<type_name, enum_def, std::less<>>      enums;
   map<name,type_name>                        action_results;

   map<type_name, pair<unpack_function, pack_function>, std::less<>> built_in_types;
   void configure_built_in_types();

   // Protobuf support — shared_ptr keeps abi_serializer copyable.
   // These are never modified after set_abi(), so sharing is safe across threads.
   std::shared_ptr<google::protobuf::DescriptorPool>        pb_pool;
   std::shared_ptr<google::protobuf::DynamicMessageFactory> pb_factory;

   bool is_protobuf_type(const std::string_view& type) const;
   const google::protobuf::Descriptor* get_pb_descriptor(const std::string_view& type) const;
   fc::variant pb_binary_to_variant(const std::string_view& type, fc::datastream<const char*>& stream) const;
   /// Streaming protobuf bridge: decodes the protobuf blob and returns the JSON
   /// string produced by `MessageToJsonString` (which `pb_binary_to_variant`
   /// otherwise round-trips through `fc::json::from_string`).  Used by
   /// `stream_sink::unpack_protobuf` to splice via `raw_value` without building
   /// an intermediate variant tree.
   std::string pb_binary_to_json_string(const std::string_view& type, fc::datastream<const char*>& stream) const;
   void pb_variant_to_binary(const std::string_view& type, const fc::variant& var, fc::datastream<char*>& ds) const;

   fc::variant _binary_to_variant( const std::string_view& type, const bytes& binary, impl::binary_to_variant_context& ctx )const;
   fc::variant _binary_to_variant( const std::string_view& type, fc::datastream<const char*>& binary, impl::binary_to_variant_context& ctx )const;

   /// Templated recursive walker that emits a value of `type` from `stream` into `sink`.
   /// Mirrors the recursion shape of `_binary_to_variant`: fixed-array, built-in,
   /// protobuf, dynamic array, optional, enum, variant, struct.  Two explicit
   /// instantiations (variant_sink, stream_sink) live in `abi_serializer.cpp`.
   template<typename Sink>
   void _binary_walk( const std::string_view& type, fc::datastream<const char*>& stream,
                      Sink& sink, impl::binary_to_variant_context& ctx )const;

   /// Walk struct fields without emitting `begin_object`/`end_object`.  Recurses
   /// into the base struct (if any) so its fields land in the same object frame
   /// the caller already opened.
   template<typename Sink>
   void _binary_walk_struct_fields( const map<type_name, struct_def, std::less<>>::const_iterator& s_itr,
                                    fc::datastream<const char*>& stream,
                                    Sink& sink, impl::binary_to_variant_context& ctx )const;

   /// Read an enum's underlying integer from `stream` and emit either the matching
   /// member name as a string, or the raw integer for unknown values.  Templated
   /// over sink to avoid a `fc::variant` round-trip on the streaming path.
   template<typename Sink>
   void _unpack_enum( const enum_def& e_def, fc::datastream<const char*>& stream,
                      Sink& sink, impl::binary_to_variant_context& ctx )const;

   bytes       _variant_to_binary( const std::string_view& type, const fc::variant& var, impl::variant_to_binary_context& ctx )const;
   void        _variant_to_binary( const std::string_view& type, const fc::variant& var,
                                   fc::datastream<char*>& ds, impl::variant_to_binary_context& ctx )const;

   static std::string_view _remove_bin_extension(const std::string_view& type);
   bool _is_type( const std::string_view& type, impl::abi_traverse_context& ctx )const;

   void validate( impl::abi_traverse_context& ctx )const;

   friend struct impl::abi_from_variant;
   friend struct impl::abi_to_variant;
   friend struct impl::abi_traverse_context_with_path;
   friend class  impl::variant_sink;
   friend class  impl::stream_sink;
};

namespace impl {
   const static size_t hex_log_max_size = 64;
   struct abi_traverse_context {
      abi_traverse_context( abi_serializer::yield_function_t yield, fc::microseconds max_action_data_serialization )
      : yield(std::move( yield ))
      , max_action_serialization_time(max_action_data_serialization)
      {
      }

      void logging() { log = true; } // generate variant for logging
      bool is_logging() const { return log; }

      void check_deadline()const { yield( recursion_depth ); }
      abi_serializer::yield_function_t get_yield_function() { return yield; }

      fc::scoped_exit<std::function<void()>> enter_scope();

   protected:
      abi_serializer::yield_function_t  yield;
      // if set then restricts each individual action data serialization
      fc::microseconds                  max_action_serialization_time;
      size_t                            recursion_depth = 1;
      bool                              log = false;
   };

   struct empty_path_root {};

   struct array_type_path_root {
   };

   struct struct_type_path_root {
      map<type_name, struct_def>::const_iterator  struct_itr;
   };

   struct variant_type_path_root {
      map<type_name, variant_def>::const_iterator variant_itr;
   };

   using path_root = std::variant<empty_path_root, array_type_path_root, struct_type_path_root, variant_type_path_root>;

   struct empty_path_item {};

   struct array_index_path_item {
      path_root                                   type_hint;
      uint32_t                                    array_index = 0;
   };

   struct field_path_item {
      map<type_name, struct_def>::const_iterator  parent_struct_itr;
      uint32_t                                    field_ordinal = 0;
   };

   struct variant_path_item {
      map<type_name, variant_def>::const_iterator variant_itr;
      uint32_t                                    variant_ordinal = 0;
   };

   using path_item = std::variant<empty_path_item, array_index_path_item, field_path_item, variant_path_item>;

   struct abi_traverse_context_with_path : public abi_traverse_context {
      abi_traverse_context_with_path( const abi_serializer& abis, abi_serializer::yield_function_t yield, fc::microseconds max_action_data_serialization_time, const std::string_view& type )
      : abi_traverse_context( std::move( yield ), max_action_data_serialization_time ), abis(abis)
      {
         set_path_root(type);
      }

      abi_traverse_context_with_path( const abi_serializer& abis, const abi_traverse_context& ctx, const std::string_view& type )
      : abi_traverse_context(ctx), abis(abis)
      {
         set_path_root(type);
      }

      void set_path_root( const std::string_view& type );

      fc::scoped_exit<std::function<void()>> push_to_path( const path_item& item );

      void set_array_index_of_path_back( uint32_t i );
      void hint_array_type_if_in_array();
      void hint_struct_type_if_in_array( const map<type_name, struct_def>::const_iterator& itr );
      void hint_variant_type_if_in_array( const map<type_name, variant_def>::const_iterator& itr );

      string get_path_string()const;

      string maybe_shorten( const std::string_view& str );

   protected:
      const abi_serializer&  abis;
      path_root              root_of_path;
      vector<path_item>      path;
   public:
      bool                   short_path = false;
   };

   struct binary_to_variant_context : public abi_traverse_context_with_path {
      using abi_traverse_context_with_path::abi_traverse_context_with_path;
   };

   struct action_data_to_variant_context : public binary_to_variant_context {
      action_data_to_variant_context( const abi_serializer& abis, const abi_traverse_context& ctx, const std::string_view& type )
            : binary_to_variant_context(abis, ctx, type)
      {
         short_path = true; // Just to be safe while avoiding the complexity of threading an override boolean all over the place
         if (max_action_serialization_time.count() > 0) {
            fc::time_point deadline = fc::time_point::now().safe_add(max_action_serialization_time);
            yield = [deadline, y=yield, max=max_action_serialization_time](size_t depth) {
               y(depth); // call provided yield that might include an overall time limit or not
               SYS_ASSERT( fc::time_point::now() < deadline, abi_serialization_deadline_exception,
                           "serialization action data time limit {}us exceeded", max );
            };
         }
      }
   };

   struct variant_to_binary_context : public abi_traverse_context_with_path {
      using abi_traverse_context_with_path::abi_traverse_context_with_path;

      fc::scoped_exit<std::function<void()>> disallow_extensions_unless( bool condition );

      bool extensions_allowed()const { return allow_extensions; }

   protected:
      bool                   allow_extensions = true;
   };

   /// limits the string size to default max_length of output_name
   string limit_size( const std::string_view& str );

   /**
    * Determine if a type contains ABI related info, perhaps deeply nested
    * @tparam T - the type to check
    */
   template<typename T>
   constexpr bool single_type_requires_abi_v() {
      return std::is_base_of<transaction, T>::value ||
             std::is_same<T, packed_transaction>::value ||
             std::is_same<T, transaction_trace>::value ||
             std::is_same<T, transaction_receipt>::value ||
             std::is_same<T, action_trace>::value ||
             std::is_same<T, signed_transaction>::value ||
             std::is_same<T, signed_block>::value ||
             std::is_same<T, action>::value;
   }

   /**
    * Basic constexpr for a type, aliases the basic check directly
    * @tparam T - the type to check
    */
   template<typename T>
   struct type_requires_abi {
      static constexpr bool value() {
         return single_type_requires_abi_v<T>();
      }
   };

   /**
    * specialization that catches common container patterns and checks their contained-type
    * @tparam Container - a templated container type whose first argument is the contained type
    */
   template<template<typename ...> class Container, typename T, typename ...Args >
   struct type_requires_abi<Container<T, Args...>> {
      static constexpr bool value() {
         return single_type_requires_abi_v<T>();
      }
   };

   template<typename T>
   constexpr bool type_requires_abi_v() {
      return type_requires_abi<T>::value();
   }

   /**
    * convenience aliases for creating overload-guards based on whether the type contains ABI related info
    */
   template<typename T>
   using not_require_abi_t = std::enable_if_t<!type_requires_abi_v<T>(), int>;

   template<typename T>
   using require_abi_t = std::enable_if_t<type_requires_abi_v<T>(), int>;

   /// Sink-parameterised emitter for ABI-aware reflected types.  The same template
   /// body drives both `abi_serializer::to_variant<T>` (variant_sink) and
   /// `abi_serializer::to_json_stream<T>` (stream_sink) -- no logic is duplicated
   /// across the two output paths.  Each `add` overload emits one named field at
   /// the current sink frame; nested objects/arrays open a sub-frame.
   struct abi_to_variant {
      // Non-ABI types: bottom out into the sink's generic emit (which is
      // fc::variant(v) for variant_sink and fc::to_json_stream(v, w) for stream_sink).
      template<typename Sink, typename M, typename Resolver, not_require_abi_t<M> = 1>
      static void add( Sink& sink, std::string_view name, const M& v, const Resolver&, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.key(name);
         sink.template emit<M>(v);
      }

      // ABI types: walk through fc::reflector via abi_to_variant_visitor.
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add( Sink& sink, std::string_view name, const M& v, const Resolver& resolver, abi_traverse_context& ctx );

      // vector<M> of ABI-bearing M.
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add( Sink& sink, std::string_view name, const vector<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.key(name);
         sink.begin_array();
         for( const auto& iter : v ) {
            add_value( sink, iter, resolver, ctx );
         }
         sink.end_array();
      }

      // deque<M> of ABI-bearing M.
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add( Sink& sink, std::string_view name, const deque<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.key(name);
         sink.begin_array();
         for( const auto& iter : v ) {
            add_value( sink, iter, resolver, ctx );
         }
         sink.end_array();
      }

      // shared_ptr<M> of ABI-bearing M.  Null pointer is a no-op (matches the
      // pre-Sink behaviour of skipping the field entirely).
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add( Sink& sink, std::string_view name, const std::shared_ptr<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         if( !v ) return;
         sink.key(name);
         add_value( sink, *v, resolver, ctx );
      }

      // Element-position emitter used by container helpers above and by the
      // top-level `to_variant<T>` / `to_json_stream<T>` entry points.  Emits the
      // value at the current (array or top-level) sink position with no leading
      // key.  Specialised for each container shape recognised by `add` so that
      // recursion through nested containers does not bottom-out into the
      // reflector path with a container type as `M`.
      template<typename Sink, typename M, typename Resolver, not_require_abi_t<M> = 1>
      static void add_value( Sink& sink, const M& v, const Resolver&, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.template emit<M>(v);
      }
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add_value( Sink& sink, const M& v, const Resolver& resolver, abi_traverse_context& ctx );

      // Container-shape specialisations: walk through to the element add_value.
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add_value( Sink& sink, const vector<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.begin_array();
         for( const auto& iter : v ) {
            add_value( sink, iter, resolver, ctx );
         }
         sink.end_array();
      }
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add_value( Sink& sink, const deque<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.begin_array();
         for( const auto& iter : v ) {
            add_value( sink, iter, resolver, ctx );
         }
         sink.end_array();
      }
      template<typename Sink, typename M, typename Resolver, require_abi_t<M> = 1>
      static void add_value( Sink& sink, const std::shared_ptr<M>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         if( !v ) { sink.value_null(); return; }
         add_value( sink, *v, resolver, ctx );
      }
      template<typename Sink, typename Resolver, typename... Args>
      static void add_value( Sink& sink, const std::variant<Args...>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         add_static_variant<Sink, Resolver> adder( sink, resolver, ctx );
         std::visit( adder, v );
      }

      // std::variant<...>: dispatch the active alternative through the visitor.
      template<typename Sink, typename Resolver>
      struct add_static_variant
      {
         Sink& sink;
         const Resolver& resolver;
         abi_traverse_context& ctx;
         add_static_variant( Sink& s, const Resolver& r, abi_traverse_context& c ) : sink(s), resolver(r), ctx(c) {}

         typedef void result_type;
         template<typename T> void operator()( T& v ) const
         {
            add_value( sink, v, resolver, ctx );
         }
      };

      template<typename Sink, typename Resolver, typename... Args>
      static void add( Sink& sink, std::string_view name, const std::variant<Args...>& v, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.key(name);
         add_static_variant<Sink, Resolver> adder( sink, resolver, ctx );
         std::visit( adder, v );
      }

      // Logging-mode side-effect for sysio::setcode -- inject `code_hash` next to
      // the existing `data`/`hex_data` fields.  Returns false either way; callers
      // still emit `data` afterwards.  No-op outside logging mode.
      template<typename Sink, typename Resolver>
      static bool add_special_logging( Sink& sink, const action& act, const Resolver& /*resolver*/, abi_traverse_context& ctx ) {
         if( !ctx.is_logging() ) return false;
         try {
            if( act.account == sysio::chain::config::system_account_name && act.name == "setcode"_n ) {
               auto setcode_act = act.data_as<setcode>();
               if( setcode_act.code.size() > 0 ) {
                  fc::sha256 code_hash = fc::sha256::hash(setcode_act.code.data(), (uint32_t) setcode_act.code.size());
                  sink.key("code_hash");
                  sink.template emit<fc::sha256>(code_hash);
               }
               return false; // still want the hex data included
            }
         } catch(...) {} // return false
         return false;
      }

      // Emit `bytes` either as raw hex (non-logging) or as the
      // `{size, hex|trimmed_hex}` log shape.  Used for `data` and `hex_data`
      // fields on `action`.
      template<typename Sink>
      static void emit_action_bytes_field( Sink& sink, std::string_view name, const bytes& data, abi_traverse_context& ctx )
      {
         sink.key(name);
         if( !ctx.is_logging() ) {
            sink.template emit<bytes>(data);
            return;
         }
         sink.begin_object();
         sink.key("size");
         sink.value_uint64(data.size());
         if( data.size() > impl::hex_log_max_size ) {
            sink.key("trimmed_hex");
            sink.template emit<bytes>( bytes(&data[0], &data[0] + impl::hex_log_max_size) );
         } else {
            sink.key("hex");
            sink.template emit<bytes>(data);
         }
         sink.end_object();
      }

      /// Action: matches the FC_REFLECT shape but routes `data` through the
      /// resolver-located ABI to decode action arguments inline.  The body lives
      /// on `add_value` so the top-level `to_variant<action>` entry hits the
      /// special handling rather than the reflector-based generic path.
      template<typename Sink, typename Resolver>
      static void add_value( Sink& sink, const action& act, const Resolver& resolver, abi_traverse_context& ctx )
      {
         static_assert(fc::reflector<action>::total_member_count == 4);
         auto h = ctx.enter_scope();
         sink.begin_object();
         add( sink, "account", act.account, resolver, ctx );
         add( sink, "name", act.name, resolver, ctx );
         add( sink, "authorization", act.authorization, resolver, ctx );

         if( add_special_logging( sink, act, resolver, ctx ) ) {
            sink.end_object();
            return;
         }

         bool data_emitted = false;
         try {
            auto abi_optional = resolver(act.account);
            if (abi_optional) {
               const abi_serializer& abi = *abi_optional;
               auto type = abi.get_action_type(act.name);
               if (!type.empty()) {
                  try {
                     sink.key("data");
                     sink.unpack_action_data(abi, type, act.data, ctx, /*short_path*/ false);
                     data_emitted = true;
                  } catch(...) {
                     // serialization failure -- fall back to hex below
                  }
               }
            }
         } catch(...) { /* fall back to hex below */ }
         if( !data_emitted ) {
            emit_action_bytes_field( sink, "data", act.data, ctx );
         }
         emit_action_bytes_field( sink, "hex_data", act.data, ctx );
         sink.end_object();
      }

      template<typename Sink, typename Resolver>
      static void add( Sink& sink, std::string_view name, const action& act, const Resolver& resolver, abi_traverse_context& ctx )
      {
         sink.key(name);
         add_value( sink, act, resolver, ctx );
      }

      /// action_trace: matches FC_REFLECT shape; routes `return_value` through the
      /// resolver-located ABI when an action_result type is registered.
      template<typename Sink, typename Resolver>
      static void add_value( Sink& sink, const action_trace& act_trace, const Resolver& resolver, abi_traverse_context& ctx )
      {
         static_assert(fc::reflector<action_trace>::total_member_count == 19);
         auto h = ctx.enter_scope();
         sink.begin_object();
         add( sink, "action_ordinal", act_trace.action_ordinal, resolver, ctx );
         add( sink, "creator_action_ordinal", act_trace.creator_action_ordinal, resolver, ctx );
         add( sink, "closest_unnotified_ancestor_action_ordinal", act_trace.closest_unnotified_ancestor_action_ordinal, resolver, ctx );
         add( sink, "receipt", act_trace.receipt, resolver, ctx );
         add( sink, "receiver", act_trace.receiver, resolver, ctx );
         add( sink, "act", act_trace.act, resolver, ctx );
         add( sink, "context_free", act_trace.context_free, resolver, ctx );
         add( sink, "elapsed", act_trace.elapsed, resolver, ctx );
         add( sink, "cpu_usage_us", act_trace.cpu_usage_us, resolver, ctx );
         add( sink, "net_usage", act_trace.net_usage, resolver, ctx );
         add( sink, "console", act_trace.console, resolver, ctx );
         add( sink, "trx_id", act_trace.trx_id, resolver, ctx );
         add( sink, "block_num", act_trace.block_num, resolver, ctx );
         add( sink, "block_time", act_trace.block_time, resolver, ctx );
         add( sink, "producer_block_id", act_trace.producer_block_id, resolver, ctx );
         add( sink, "account_ram_deltas", act_trace.account_ram_deltas, resolver, ctx );
         add( sink, "except", act_trace.except, resolver, ctx );
         add( sink, "error_code", act_trace.error_code, resolver, ctx );
         add( sink, "return_value_hex_data", act_trace.return_value, resolver, ctx );

         try {
            auto abi_optional = resolver(act_trace.act.account);
            if (abi_optional) {
               const abi_serializer& abi = *abi_optional;
               auto type = abi.get_action_result_type(act_trace.act.name);
               if (!type.empty()) {
                  sink.key("return_value_data");
                  sink.unpack_action_data(abi, type, act_trace.return_value, ctx, /*short_path*/ false);
               }
            }
         } catch(...) {}
         sink.end_object();
      }

      template<typename Sink, typename Resolver>
      static void add( Sink& sink, std::string_view name, const action_trace& act_trace, const Resolver& resolver, abi_traverse_context& ctx )
      {
         sink.key(name);
         add_value( sink, act_trace, resolver, ctx );
      }

      /// packed_transaction: extract the inner transaction so consumers see action
      /// data fully decoded rather than a packed-bytes blob.
      template<typename Sink, typename Resolver>
      static void add_value( Sink& sink, const packed_transaction& ptrx, const Resolver& resolver, abi_traverse_context& ctx )
      {
         static_assert(fc::reflector<packed_transaction>::total_member_count == 4);
         auto h = ctx.enter_scope();
         sink.begin_object();
         auto trx = ptrx.get_transaction();
         add( sink, "id", trx.id(), resolver, ctx );
         add( sink, "signatures", ptrx.get_signatures(), resolver, ctx );
         add( sink, "compression", ptrx.get_compression(), resolver, ctx );
         add( sink, "packed_context_free_data", ptrx.get_packed_context_free_data(), resolver, ctx );
         add( sink, "context_free_data", ptrx.get_context_free_data(), resolver, ctx );
         if( !ctx.is_logging() )
            add( sink, "packed_trx", ptrx.get_packed_transaction(), resolver, ctx );
         add( sink, "transaction", trx, resolver, ctx );
         sink.end_object();
      }

      template<typename Sink, typename Resolver>
      static void add( Sink& sink, std::string_view name, const packed_transaction& ptrx, const Resolver& resolver, abi_traverse_context& ctx )
      {
         sink.key(name);
         add_value( sink, ptrx, resolver, ctx );
      }

      /// transaction: per FC_REFLECT but recurses into context_free_actions and
      /// actions through the ABI-aware action overload above.
      template<typename Sink, typename Resolver>
      static void add_value( Sink& sink, const transaction& trx, const Resolver& resolver, abi_traverse_context& ctx )
      {
         static_assert(fc::reflector<transaction>::total_member_count == 9);
         auto h = ctx.enter_scope();
         sink.begin_object();
         add( sink, "expiration", trx.expiration, resolver, ctx );
         add( sink, "ref_block_num", trx.ref_block_num, resolver, ctx );
         add( sink, "ref_block_prefix", trx.ref_block_prefix, resolver, ctx );
         add( sink, "max_net_usage_words", trx.max_net_usage_words, resolver, ctx );
         add( sink, "max_cpu_usage_ms", trx.max_cpu_usage_ms, resolver, ctx );
         add( sink, "delay_sec", trx.delay_sec, resolver, ctx );
         add( sink, "context_free_actions", trx.context_free_actions, resolver, ctx );
         add( sink, "actions", trx.actions, resolver, ctx );
         // No transaction extensions are currently supported.
         sink.end_object();
      }

      template<typename Sink, typename Resolver>
      static void add( Sink& sink, std::string_view name, const transaction& trx, const Resolver& resolver, abi_traverse_context& ctx )
      {
         sink.key(name);
         add_value( sink, trx, resolver, ctx );
      }

      /// Emit the inline (no begin/end_object) field set for `signed_block`.
      /// Exposed so that callers with extra wrapper fields (eg `convert_block`'s
      /// `id` / `block_num` / `ref_block_prefix`) can interleave them with the
      /// reflected-block fields inside a single JSON object.
      template<typename Sink, typename Resolver>
      static void emit_signed_block_body( Sink& sink, const signed_block& block, const Resolver& resolver, abi_traverse_context& ctx )
      {
         static_assert(fc::reflector<signed_block>::total_member_count == 13);
         add( sink, "timestamp", block.timestamp, resolver, ctx );
         add( sink, "producer", block.producer, resolver, ctx );
         add( sink, "previous", block.previous, resolver, ctx );
         add( sink, "transaction_mroot", block.transaction_mroot, resolver, ctx );
         add( sink, "finality_mroot", block.finality_mroot, resolver, ctx );
         add( sink, "qc_claim", block.qc_claim, resolver, ctx );
         add( sink, "new_finalizer_policy_diff", block.new_finalizer_policy_diff, resolver, ctx );
         add( sink, "new_proposer_policy_diff", block.new_proposer_policy_diff, resolver, ctx );

         flat_multimap<uint16_t, block_header_extension> header_exts = block.validate_and_extract_header_extensions();
         if (auto it = header_exts.find(protocol_feature_activation::extension_id()); it != header_exts.end()) {
            const auto& new_protocol_features = std::get<protocol_feature_activation>(it->second).protocol_features;
            sink.key("new_protocol_features");
            sink.begin_array();
            for (auto feature : new_protocol_features) {
               sink.begin_object();
               add( sink, "feature_digest", feature, resolver, ctx );
               sink.end_object();
            }
            sink.end_array();
         }

         add( sink, "producer_signatures", block.producer_signatures, resolver, ctx );
         add( sink, "transactions", block.transactions, resolver, ctx );

         if (block.qc) {
            add( sink, "qc", *block.qc, resolver, ctx );
         }
      }

      template<typename Sink, typename Resolver>
      static void add_value( Sink& sink, const signed_block& block, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         sink.begin_object();
         emit_signed_block_body( sink, block, resolver, ctx );
         sink.end_object();
      }

      template<typename Sink, typename Resolver>
      static void add( Sink& sink, std::string_view name, const signed_block& block, const Resolver& resolver, abi_traverse_context& ctx )
      {
         sink.key(name);
         add_value( sink, block, resolver, ctx );
      }
   };

   /**
    * Reflection visitor that drives `abi_to_variant::add` over each member of T,
    * resolving ABIs for nested ABI-bearing types via the supplied Resolver.  Now
    * sink-templated so the same visitor walks reflected fields for both the
    * variant-building and stream-emitting paths.
    *
    * @tparam Sink     - emit target (variant_sink or stream_sink)
    * @tparam T        - the reflected type whose members are being visited
    * @tparam Resolver - callable with signature (const name&) -> std::optional<abi_serializer>
    */
   template<typename Sink, typename T, typename Resolver>
   class abi_to_variant_visitor
   {
      public:
         abi_to_variant_visitor( Sink& s, const T& v, const Resolver& r, abi_traverse_context& c )
         : _sink(s)
         , _val(v)
         , _resolver(r)
         , _ctx(c)
         {}

         template<typename Member, class Class, Member (Class::*member)>
         void operator()( const char* name )const
         {
            abi_to_variant::add( _sink, name, (_val.*member), _resolver, _ctx );
         }

      private:
         Sink&                 _sink;
         const T&              _val;
         const Resolver&       _resolver;
         abi_traverse_context& _ctx;
   };

   struct abi_from_variant {
      /**
       * template which overloads extract for types which are not relvant to ABI information
       * and can be degraded to the normal ::from_variant(...) processing
       */
      template<typename M, typename Resolver, not_require_abi_t<M> = 1>
      static void extract( const fc::variant& v, M& o, const Resolver&, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         from_variant(v, o);
      }

      /**
       * template which overloads extract for types which contain ABI information in their trees
       * for these types we create new ABI aware visitors
       */
      template<typename M, typename Resolver, require_abi_t<M> = 1>
      static void extract( const fc::variant& v, M& o, const Resolver& resolver, abi_traverse_context& ctx );

      /**
       * template which overloads extract for vectors of types which contain ABI information in their trees
       * for these members we call ::extract in order to trigger further processing
       */
      template<typename M, typename Resolver, require_abi_t<M> = 1>
      static void extract( const fc::variant& v, vector<M>& o, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         const variants& array = v.get_array();
         o.clear();
         o.reserve( array.size() );
         for( auto itr = array.begin(); itr != array.end(); ++itr ) {
            M o_iter;
            extract(*itr, o_iter, resolver, ctx);
            o.emplace_back(std::move(o_iter));
         }
      }

 /**
  * template which overloads extract for deque of types which contain ABI information in their trees
  * for these members we call ::extract in order to trigger further processing
  */
      template<typename M, typename Resolver, require_abi_t<M> = 1>
      static void extract( const fc::variant& v, deque<M>& o, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         const variants& array = v.get_array();
         o.clear();
         for( auto itr = array.begin(); itr != array.end(); ++itr ) {
            M o_iter;
            extract(*itr, o_iter, resolver, ctx);
            o.emplace_back(std::move(o_iter));
         }
      }


      /**
       * template which overloads extract for shared_ptr of types which contain ABI information in their trees
       * for these members we call ::extract in order to trigger further processing
       */
      template<typename M, typename Resolver, require_abi_t<M> = 1>
      static void extract( const fc::variant& v, std::shared_ptr<M>& o, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         const variant_object& vo = v.get_object();
         M obj;
         extract(vo, obj, resolver, ctx);
         o = std::make_shared<M>(obj);
      }

      /**
       * Non templated overload that has priority for the action structure
       * this type has members which must be directly translated by the ABI so it is
       * exploded and processed explicitly
       */
      template<typename Resolver>
      static void extract( const fc::variant& v, action& act, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         const variant_object& vo = v.get_object();
         SYS_ASSERT(vo.contains("account"), packed_transaction_type_exception, "Missing account");
         SYS_ASSERT(vo.contains("name"), packed_transaction_type_exception, "Missing name");
         from_variant(vo["account"], act.account);
         from_variant(vo["name"], act.name);

         if (vo.contains("authorization")) {
            from_variant(vo["authorization"], act.authorization);
         }

         bool valid_empty_data = false;
         if( vo.contains( "data" ) ) {
            const auto& data = vo["data"];
            if( data.is_string() ) {
               from_variant(data, act.data);
               valid_empty_data = act.data.empty();
            } else if ( data.is_object() ) {
               auto abi_optional = resolver(act.account);
               if (abi_optional) {
                  const abi_serializer& abi = *abi_optional;
                  auto type = abi.get_action_type(act.name);
                  if (!type.empty()) {
                     variant_to_binary_context _ctx(abi, ctx, type);
                     _ctx.short_path = true; // Just to be safe while avoiding the complexity of threading an override boolean all over the place
                     act.data = abi._variant_to_binary( type, data, _ctx );
                     valid_empty_data = act.data.empty();
                  }
               }
            }
         }

         if( !valid_empty_data && act.data.empty() ) {
            if( vo.contains( "hex_data" ) ) {
               const auto& data = vo["hex_data"];
               if( data.is_string() ) {
                  from_variant(data, act.data);
               }
            }
         }

         SYS_ASSERT(valid_empty_data || !act.data.empty(), packed_transaction_type_exception,
                    "Failed to deserialize data for {}:{}", act.account, act.name);
      }

      template<typename Resolver>
      static void extract( const fc::variant& v, packed_transaction& ptrx, const Resolver& resolver, abi_traverse_context& ctx )
      {
         auto h = ctx.enter_scope();
         const variant_object& vo = v.get_object();
         SYS_ASSERT(vo.contains("signatures"), packed_transaction_type_exception, "Missing signatures");
         SYS_ASSERT(vo.contains("compression"), packed_transaction_type_exception, "Missing compression");
         std::vector<signature_type> signatures;
         packed_transaction::compression_type compression;
         from_variant(vo["signatures"], signatures);
         from_variant(vo["compression"], compression);

         bytes packed_cfd;
         std::vector<bytes> cfd;
         bool use_packed_cfd = false;
         if( vo.contains("packed_context_free_data") && vo["packed_context_free_data"].is_string() && !vo["packed_context_free_data"].as_string().empty() ) {
            from_variant(vo["packed_context_free_data"], packed_cfd );
            use_packed_cfd = true;
         } else if( vo.contains("context_free_data") ) {
            from_variant(vo["context_free_data"], cfd);
         }

         if( vo.contains("packed_trx") && vo["packed_trx"].is_string() && !vo["packed_trx"].as_string().empty() ) {
            bytes packed_trx;
            from_variant(vo["packed_trx"], packed_trx);
            if( use_packed_cfd ) {
               ptrx = packed_transaction( std::move( packed_trx ), std::move( signatures ), std::move( packed_cfd ), compression );
            } else {
               ptrx = packed_transaction( std::move( packed_trx ), std::move( signatures ), std::move( cfd ), compression );
            }
         } else {
            SYS_ASSERT(vo.contains("transaction"), packed_transaction_type_exception, "Missing transaction");
            if( use_packed_cfd ) {
               transaction trx;
               extract( vo["transaction"], trx, resolver, ctx );
               ptrx = packed_transaction( std::move(trx), std::move(signatures), std::move(packed_cfd), compression );
            } else {
               signed_transaction trx;
               extract( vo["transaction"], trx, resolver, ctx );
               trx.signatures = std::move( signatures );
               trx.context_free_data = std::move(cfd);
               ptrx = packed_transaction( std::move( trx ), compression );
            }
         }
      }
   };

   /**
    * Reflection visitor that uses a resolver to resolve ABIs for nested types
    * this will degrade to the common fc::from_variant as soon as the type no longer contains
    * ABI related info
    *
    * @tparam Reslover - callable with the signature (const name& code_account) -> std::optional<abi_def>
    */
   template<typename T, typename Resolver>
   class abi_from_variant_visitor : public reflector_init_visitor<T>
   {
      public:
         abi_from_variant_visitor( const variant_object& _vo, T& v, const Resolver& _resolver, abi_traverse_context& _ctx )
         : reflector_init_visitor<T>(v)
         ,_vo(_vo)
         ,_resolver(_resolver)
         ,_ctx(_ctx)
         {}

         /**
          * Visit a single member and extract it from the variant object
          * @tparam Member - the member to visit
          * @tparam Class - the class we are traversing
          * @tparam member - pointer to the member
          * @param name - the name of the member
          */
         template<typename Member, class Class, Member (Class::*member)>
         void operator()( const char* name )const
         {
            auto itr = _vo.find(name);
            if( itr != _vo.end() )
               abi_from_variant::extract( itr->value(), const_cast<std::remove_const_t<Member>&>(this->obj.*member), _resolver, _ctx );
         }

      private:
         const variant_object& _vo;
         const Resolver& _resolver;
         abi_traverse_context& _ctx;
   };

   template<typename Sink, typename M, typename Resolver, require_abi_t<M>>
   void abi_to_variant::add( Sink& sink, std::string_view name, const M& v, const Resolver& resolver, abi_traverse_context& ctx )
   {
      auto h = ctx.enter_scope();
      sink.key(name);
      sink.begin_object();
      fc::reflector<M>::visit( impl::abi_to_variant_visitor<Sink, M, Resolver>(sink, v, resolver, ctx) );
      sink.end_object();
   }

   template<typename Sink, typename M, typename Resolver, require_abi_t<M>>
   void abi_to_variant::add_value( Sink& sink, const M& v, const Resolver& resolver, abi_traverse_context& ctx )
   {
      auto h = ctx.enter_scope();
      sink.begin_object();
      fc::reflector<M>::visit( impl::abi_to_variant_visitor<Sink, M, Resolver>(sink, v, resolver, ctx) );
      sink.end_object();
   }

   template<typename M, typename Resolver, require_abi_t<M>>
   void abi_from_variant::extract( const fc::variant& v, M& o, const Resolver& resolver, abi_traverse_context& ctx )
   {
      auto h = ctx.enter_scope();
      const variant_object& vo = v.get_object();
      fc::reflector<M>::visit( abi_from_variant_visitor<M, decltype(resolver)>( vo, o, resolver, ctx ) );
   }
} /// namespace sysio::chain::impl

template<typename T, typename Resolver>
void abi_serializer::to_variant( const T& o, fc::variant& vo, const Resolver& resolver, const yield_function_t& yield ) try {
   impl::variant_sink sink;
   impl::abi_traverse_context ctx( yield, fc::microseconds{} );
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
   vo = std::move(sink).take_result();
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::to_variant( const T& o, fc::variant& vo, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time ) try {
   impl::variant_sink sink;
   impl::abi_traverse_context ctx( create_depth_yield_function(), max_action_data_serialization_time );
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
   vo = std::move(sink).take_result();
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::to_log_variant( const T& o, fc::variant& vo, const Resolver& resolver, const yield_function_t& yield ) try {
   impl::variant_sink sink;
   impl::abi_traverse_context ctx( yield, fc::microseconds{} );
   ctx.logging();
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
   vo = std::move(sink).take_result();
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::to_log_variant( const T& o, fc::variant& vo, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time ) try {
   impl::variant_sink sink;
   impl::abi_traverse_context ctx( create_depth_yield_function(), max_action_data_serialization_time );
   ctx.logging();
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
   vo = std::move(sink).take_result();
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::to_json_stream( const T& o, fc::json_writer& w, const Resolver& resolver, const yield_function_t& yield ) try {
   impl::stream_sink sink(w);
   impl::abi_traverse_context ctx( yield, fc::microseconds{} );
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::to_json_stream( const T& o, fc::json_writer& w, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time ) try {
   impl::stream_sink sink(w);
   impl::abi_traverse_context ctx( create_depth_yield_function(), max_action_data_serialization_time );
   impl::abi_to_variant::add_value(sink, o, resolver, ctx);
} FC_RETHROW_EXCEPTIONS(error, "Failed to serialize: {}", boost::core::demangle( typeid(o).name() ))

template<typename T, typename Resolver>
void abi_serializer::from_variant( const fc::variant& v, T& o, const Resolver& resolver, const yield_function_t& yield ) try {
   impl::abi_traverse_context ctx( yield, fc::microseconds{} );
   impl::abi_from_variant::extract(v, o, resolver, ctx);
} FC_RETHROW_EXCEPTIONS(error, "Failed to deserialize variant {}", fc::json::to_log_string(v))

template<typename T, typename Resolver>
void abi_serializer::from_variant( const fc::variant& v, T& o, const Resolver& resolver, const fc::microseconds& max_action_data_serialization_time ) try {
   impl::abi_traverse_context ctx( create_depth_yield_function(), max_action_data_serialization_time );
   impl::abi_from_variant::extract(v, o, resolver, ctx);
} FC_RETHROW_EXCEPTIONS(error, "Failed to deserialize variant {}", fc::json::to_log_string(v))

using abi_serializer_cache_t = std::unordered_map<account_name, std::optional<abi_serializer>>;
using resolver_fn_t = std::function<std::optional<abi_serializer>(const account_name& name)>;

class abi_resolver {
public:
   explicit abi_resolver(abi_serializer_cache_t&& abi_serializers) :
      abi_serializers(std::move(abi_serializers))
   {}

   std::optional<std::reference_wrapper<const abi_serializer>> operator()(const account_name& account) const {
      auto it = abi_serializers.find(account);
      if (it != abi_serializers.end() && it->second)
         return std::reference_wrapper<const abi_serializer>(*it->second);
      return {};
   };

private:
   abi_serializer_cache_t abi_serializers;
};

class abi_serializer_cache_builder {
public:
   explicit abi_serializer_cache_builder(resolver_fn_t resolver) :
      resolver_(std::move(resolver))
   {
   }

   abi_serializer_cache_builder(const abi_serializer_cache_builder&) = delete;

   abi_serializer_cache_builder&& add_serializers(const chain::signed_block_ptr& block) && {
      for( const auto& receipt: block->transactions ) {
         const auto& t = receipt.trx.get_transaction();
         for( const auto& a: t.actions )
            add_to_cache( a );
         for( const auto& a: t.context_free_actions )
            add_to_cache( a );
      }
      return std::move(*this);
   }

   abi_serializer_cache_builder&& add_serializers(const transaction_trace_ptr& trace_ptr) && {
      for( const auto& trace: trace_ptr->action_traces ) {
         add_to_cache(trace.act);
      }
      return std::move(*this);
   }

   abi_serializer_cache_t&& get() && {
      return std::move(abi_serializers);
   }

private:
   void add_to_cache(const chain::action& a) {
      auto it = abi_serializers.find( a.account );
      if( it == abi_serializers.end() ) {
         try {
            abi_serializers.emplace_hint( it, a.account, resolver_( a.account ) );
         } catch( ... ) {
            // keep behavior of not throwing on invalid abi, will result in hex data
         }
      }
   }

   resolver_fn_t resolver_;
   abi_serializer_cache_t abi_serializers;
};

/*
 * This is equivalent to a resolver, except that everytime the abi_serializer for an account
 * is retrieved, it is stored in an unordered_map, so we won't waste time retrieving it again.
 * This is handy when parsing packed_transactions received in a fc::variant.
 */
class caching_resolver {
public:
   explicit caching_resolver(resolver_fn_t resolver) :
      resolver_(std::move(resolver))
   {
   }

   // make it non-copiable (we should only move it for performance reasons)
   caching_resolver(const caching_resolver&) = delete;
   caching_resolver& operator=(const caching_resolver&) = delete;

   std::optional<std::reference_wrapper<const abi_serializer>> operator()(const account_name& account) const {
      auto it = abi_serializers.find(account);
      if (it != abi_serializers.end()) {
         if (it->second)
            return *it->second;
         return {};
      }
      auto serializer = resolver_(account);
      auto& dest = abi_serializers[account]; // add entry regardless
      if (serializer) {
         // we got a serializer, so move it into the cache
         dest = abi_serializer_cache_t::mapped_type{std::move(*serializer)};
         return *dest; // and return a reference to it
      }
      return {};
   };

private:
   const resolver_fn_t resolver_;
   mutable abi_serializer_cache_t abi_serializers;
};


} // sysio::chain

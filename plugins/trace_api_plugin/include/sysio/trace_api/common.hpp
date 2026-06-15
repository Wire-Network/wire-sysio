#pragma once

#include <fc/utility.hpp>
#include <fc/io/raw.hpp>
#include <tuple>
#include <utility>
#include <sysio/trace_api/data_log.hpp>
#include <sysio/chain/name.hpp>

namespace sysio::trace_api {

   // Compile-time constant for setabi detection so we don't pay a chain::name
   // construction cost on every action.  string_to_name instead of the ""_n
   // literal keeps using-directives out of this widely-included header.  Used by
   // chain_extraction (live capture) and store_provider (startup rebuild of the
   // reversible ABI overlay from recorded traces) - keep the two detection sites
   // in lock-step.
   inline constexpr chain::name setabi_action_name = chain::string_to_name("setabi");

   /**
    * Decode a sysio::setabi action payload into (target account, ABI bytes).
    * Shared by chain_extraction (live capture) and store_provider (startup rebuild
    * of the reversible ABI overlay) so the wire format is interpreted in exactly
    * one place.  Throws fc unpack errors on a malformed payload; callers catch and
    * log with their own context (global_sequence etc.).
    */
   inline std::pair<chain::name, chain::bytes> unpack_setabi_data(const chain::bytes& data) {
      chain::name  target;
      chain::bytes abi_bytes;
      auto ds = fc::datastream<const char*>(data.data(), data.size());
      fc::raw::unpack(ds, target);
      fc::raw::unpack(ds, abi_bytes);
      return { target, std::move(abi_bytes) };
   }
   /**
    * A function used to separate cooperative or external concerns from long running tasks
    * calling code should expect that this can throw yield_exception and gracefully unwind if it does
    * @throws yield_exception if the provided yield needs to terminate the long running process for any reason
    */
   using yield_function = fc::optional_delegate<void()>;

   /**
    * Exceptions
    */
   class yield_exception : public std::runtime_error {
      public:
      explicit yield_exception(const char* what_arg)
      :std::runtime_error(what_arg)
      {}

      explicit yield_exception(const std::string& what_arg)
      :std::runtime_error(what_arg)
      {}
   };

   class bad_data_exception : public std::runtime_error {
   public:
      explicit bad_data_exception(const char* what_arg)
      :std::runtime_error(what_arg)
      {}

      explicit bad_data_exception(const std::string& what_arg)
      :std::runtime_error(what_arg)
      {}
   };

   using exception_with_context = std::tuple<const std::exception_ptr&, char const *, uint64_t, char const *>;
   using exception_handler = fc::optional_delegate<void(const exception_with_context&)>;

   using log_handler = fc::optional_delegate<void(const std::string&)>;

   struct block_trace_v0;
   // optional block trace and irreversibility paired data
   using get_block_t = std::optional<std::tuple<data_log_entry, bool>>;

   using get_block_n = std::optional<uint32_t>;
   /**
    * Normal use case: exception_handler except_handler;
    *   except_handler( MAKE_EXCEPTION_WITH_CONTEXT( std::current_exception() ) );
    */
#define MAKE_EXCEPTION_WITH_CONTEXT(eptr) \
   (sysio::trace_api::exception_with_context((eptr),  __FILE__, __LINE__, __func__))


}

#pragma once
/**
 * @file log_message.hpp
 * @brief Defines types and helper macros necessary for generating log messages.
 */
#include <fc/time.hpp>
#include <fc/variant_object.hpp>
#include <spdlog/fmt/fmt.h>
#include <fmt/ranges.h>

#include <memory>
#include <vector>
#include <string>
#include <concepts>

namespace fc
{
   namespace detail
   { 
       class log_context_impl; 
       class log_message_impl; 
   }

   /**
    * Named scope for log_level enumeration.
    */
   class log_level
   {
      public:
         /**
          * @brief Define's the various log levels for reporting.  
          *
          * Each log level includes all higher levels such that 
          * Debug includes Error, but Error does not include Debug.
          */
         enum values
         {
             all, 
             debug, 
             info, 
             warn, 
             error, 
             off  
         };
         log_level( values v = off ):value(v){}
         explicit log_level( int v ):value( static_cast<values>(v)){}
         operator int()const { return value; }
         std::string to_string()const;
         values value;
   };

   void to_variant( log_level e, variant& v );
   void from_variant( const variant& e, log_level& ll );

   /**
    *  @brief provides information about where and when a log message was generated.
    *  @ingroup AthenaSerializable
    *
    *  @see FC_LOG_CONTEXT
    */
   class log_context 
   {
      public:
        log_context();
        log_context( log_level ll,
                    const char* file, 
                    uint64_t line, 
                    const char* method );
        ~log_context();
        explicit log_context( const variant& v );
        variant to_variant()const;

        std::string   get_file()const;
        uint64_t      get_line_number()const;
        std::string   get_method()const;
        std::string   get_thread_name()const;
        std::string   get_task_name()const;
        std::string   get_host_name()const;
        time_point    get_timestamp()const;
        log_level     get_log_level()const;
        std::string   get_context()const;

        void          append_context( const std::string& c );

        std::string   to_string()const;
      private:
        std::shared_ptr<detail::log_context_impl> my;
   };

   void to_variant( const log_context& l, variant& v );
   void from_variant( const variant& l, log_context& c );

   /**
    *  @brief aggregates a message along with the context and associated meta-information.
    *  @ingroup AthenaSerializable
    *
    *  @note log_message has reference semantics, all copies refer to the same log message
    *  and the message is read-only after construction.
    *
    *  When converted to JSON, log_message has the following form:
    *  @code
    *  {
    *     "context" : { ... },
    *     "format"  : "string with ${keys}",
    *     "data"    : { "keys" : "values" }
    *  }
    *  @endcode
    *
    *  @see FC_LOG_MESSAGE
    */
   class log_message
   {
      public:
         log_message();
         /**
          *  @param ctx - generally provided using the FC_LOG_CONTEXT(LEVEL) macro 
          */
         log_message( log_context ctx, std::string msg );
         ~log_message();

         log_message( const variant& v );
         variant        to_variant()const;
                              
         std::string    get_message()const;
         /**
          * A faster version of get_message which does limited formatting and excludes large variants
          * @return formatted message according to format and variant args
          */
         std::string    get_limited_message()const;
                              
         log_context    get_context()const;

      private:
         std::shared_ptr<detail::log_message_impl> my;
   };

   void    to_variant( const log_message& l, variant& v );
   void    from_variant( const variant& l, log_message& c );

   typedef std::vector<log_message> log_messages;

   template<typename S, typename... T>
   auto format_runtime( const S& form, T&&... args ) {
      try {
         return fmt::format(fmt::runtime(form), std::forward<T>(args)...);
      } catch (...) {
         return std::string("Unable to format: ") + std::string(form);
      }
   }

   // compile time enforcement of fmt::format form
   template<typename S, typename... T>
   fc::log_message get_log_message(fc::log_context ctx, const S& form, T&&... args ) {
      if (sizeof...(args) > 0) {
         try {
            return fc::log_message( std::move(ctx), fmt::format(form, std::forward<T>(args)...) );
         } catch (...) {
            fmt::string_view vform{form};
            return fc::log_message( std::move(ctx), std::string("Unable to format: ") + std::string(vform.data(), vform.size()) );
         }
      }
      fmt::string_view vform{form};
      return fc::log_message( std::move(ctx), std::string(vform.data(), vform.size()) );
   }

} // namespace fc

FC_REFLECT_TYPENAME( fc::log_message );
FC_REFLECT_ENUM(fc::log_level::values, (all)(debug)(info)(warn)(error)(off))
FC_REFLECT(fc::log_level, (value))

#ifndef __func__
#define __func__ __FUNCTION__
#endif

/**
 * @def FC_LOG_CONTEXT(LOG_LEVEL)
 * @brief Automatically captures the File, Line, and Method names and passes them to
 *        the constructor of fc::log_context along with LOG_LEVEL
 * @param LOG_LEVEL - a valid log_level::Enum name.
 */
#define FC_LOG_CONTEXT(LOG_LEVEL) \
   fc::log_context( fc::log_level::LOG_LEVEL, __FILE__, __LINE__, __func__ )

#define FC_FMT(FORMAT, ...) \
   fmt::format( FORMAT, ##__VA_ARGS__ )

#define FC_LOG_MESSAGE(LOG_LEVEL, FORMAT, ...) \
  fc::get_log_message( FC_LOG_CONTEXT(LOG_LEVEL), FMT_STRING(FORMAT) __VA_OPT__(,) __VA_ARGS__ )

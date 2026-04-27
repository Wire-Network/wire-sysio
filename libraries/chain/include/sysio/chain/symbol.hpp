#pragma once
#include <fc/exception/exception.hpp>
#include <fc/serialize_as_string.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/core_symbol.hpp>
#include <string>
#include <functional>

namespace sysio::chain {

      /**
         class symbol represents a token and contains precision and name.
         When encoded as a uint64_t, first byte represents the number of decimals, remaining bytes
         represent token name.
         Name must only include upper case alphabets.
         from_string constructs a symbol from an input a string of the form "4,SYS"
         where the integer represents number of decimals. Number of decimals must be larger than zero.
       */

      static constexpr uint64_t string_to_symbol_c(uint8_t precision, std::string_view str) {
         uint64_t result = 0;
         // No validation is done at compile time
         for (size_t i = 0; i < str.size(); ++i) {
            result |= (uint64_t(str[i]) << (8*(1+i)));
         }

         result |= uint64_t(precision);
         return result;
      }

#define SY(P,X) ::sysio::chain::string_to_symbol_c(P,#X)

      static uint64_t string_to_symbol(uint8_t precision, std::string_view str) {
         try {
            uint64_t result = 0;
            for (size_t i = 0; i < str.size(); ++i) {
               // All characters must be upper case alphabets
               SYS_ASSERT (str[i] >= 'A' && str[i] <= 'Z', symbol_type_exception, "invalid character in symbol name");
               result |= (uint64_t(str[i]) << (8*(i+1)));
            }
            result |= uint64_t(precision);
            return result;
         } FC_CAPTURE_LOG_AND_RETHROW("{}", str)
      }

      struct symbol_code {
         uint64_t value;

         operator uint64_t()const { return value; }

         /// Decode value as a packed-byte ASCII string (low byte first), e.g. 0x535953 -> "SYS".
         /// Mirrors the long form `symbol(value << 8).name()` but without going through symbol's
         /// validating constructor; serialization paths must not throw on already-resident state.
         std::string to_string() const {
            uint64_t v = value;
            std::string result;
            while (v > 0) {
               char c = v & 0xFF;
               result += c;
               v >>= 8;
            }
            return result;
         }

         /// Parse a bare symbol-name string (e.g. "SYS") into a symbol_code.  Routes through
         /// symbol's precision-zero constructor, which validates that all characters are
         /// uppercase letters and length <= 7.
         static symbol_code from_string(std::string_view s);
      };

      class symbol : fc::reflect_init {
         public:

            static constexpr uint8_t max_precision = 18;

            explicit symbol(uint8_t p, std::string_view s): m_value(string_to_symbol(p, s)) {
               SYS_ASSERT(valid(), symbol_type_exception, "invalid symbol: {}", s);
            }
            explicit symbol(uint64_t v = CORE_SYMBOL): m_value(v) {
               SYS_ASSERT(valid(), symbol_type_exception, "invalid symbol: {}", name());
            }
            static symbol from_string(std::string_view from);
            uint64_t value() const { return m_value; }
            bool valid() const
            {
               const auto& s = name();
               return decimals() <= max_precision && valid_name(s);
            }
            static bool valid_name(const string& name)
            {
               return all_of(name.begin(), name.end(), [](char c)->bool { return (c >= 'A' && c <= 'Z'); });
            }

            uint8_t decimals() const { return m_value & 0xFF; }
            uint64_t precision() const
            {
               SYS_ASSERT( decimals() <= max_precision, symbol_type_exception, "precision {} should be <= 18", decimals() );
               uint64_t p10 = 1;
               uint64_t p = decimals();
               while( p > 0  ) {
                  p10 *= 10; --p;
               }
               return p10;
            }
            string name() const { return to_symbol_code().to_string(); }

            symbol_code to_symbol_code()const { return {m_value >> 8}; }

            explicit operator string() const
            {
               uint64_t v = m_value;
               uint8_t p = v & 0xFF;
               string ret = sysio::chain::to_string(p);
               ret += ',';
               ret += name();
               return ret;
            }

            string to_string() const { return string(*this); }
            template <typename DataStream>
            friend DataStream& operator<< (DataStream& ds, const symbol& s)
            {
               return ds << s.to_string();
            }

            void reflector_init()const {
               SYS_ASSERT( decimals() <= max_precision, symbol_type_exception, "precision {} should be <= 18", decimals() );
               SYS_ASSERT( valid_name(name()), symbol_type_exception, "invalid symbol: {}", name() );
            }

         private:
            uint64_t m_value;
            friend struct fc::reflector<symbol>;
      }; // class symbol

      inline symbol_code symbol_code::from_string(std::string_view s) {
         return symbol(0, s).to_symbol_code();
      }

      struct extended_symbol {
         symbol       sym;
         account_name contract;
      };

      inline bool operator== (const symbol& lhs, const symbol& rhs)
      {
         return lhs.value() == rhs.value();
      }
      inline bool operator!= (const symbol& lhs, const symbol& rhs)
      {
         return lhs.value() != rhs.value();
      }
      inline bool operator< (const symbol& lhs, const symbol& rhs)
      {
         return lhs.value() < rhs.value();
      }
      inline bool operator> (const symbol& lhs, const symbol& rhs)
      {
         return lhs.value() > rhs.value();
      }

      inline bool operator== (const extended_symbol& lhs, const extended_symbol& rhs)
      {
         return std::tie(lhs.sym, lhs.contract) == std::tie(rhs.sym, rhs.contract);
      }

      inline bool operator!= (const extended_symbol& lhs, const extended_symbol& rhs)
      {
         return std::tie(lhs.sym, lhs.contract) != std::tie(rhs.sym, rhs.contract);
      }

      inline bool operator< (const extended_symbol& lhs, const extended_symbol& rhs)
      {
         return std::tie(lhs.sym, lhs.contract) < std::tie(rhs.sym, rhs.contract);
      }

      inline bool operator> (const extended_symbol& lhs, const extended_symbol& rhs)
      {
         return std::tie(lhs.sym, lhs.contract) > std::tie(rhs.sym, rhs.contract);
      }
} // namespace sysio::chain

FC_REFLECT(sysio::chain::symbol_code, (value))
FC_REFLECT(sysio::chain::symbol, (m_value))
FC_REFLECT(sysio::chain::extended_symbol, (sym)(contract))
FC_SERIALIZE_AS_STRING(sysio::chain::symbol)
FC_SERIALIZE_AS_STRING(sysio::chain::symbol_code)

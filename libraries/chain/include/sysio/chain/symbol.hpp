#pragma once
#include <fc/exception/exception.hpp>
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

      /// Longest symbol name: one character in each byte of the `uint64_t` encoding above the precision byte.
      inline constexpr size_t max_symbol_name_length = sizeof(uint64_t) - 1;

      /**
       * Encode a symbol at compile time. The characters of the name are not validated. A name longer than
       * `max_symbol_name_length` does not compile.
       */
      static consteval uint64_t string_to_symbol_c(uint8_t precision, std::string_view str) {
         SYS_ASSERT(str.size() <= max_symbol_name_length, symbol_type_exception,
                    "symbol name is longer than {} characters", max_symbol_name_length);
         uint64_t result = 0;
         for (size_t i = 0; i < str.size(); ++i) {
            result |= (uint64_t(str[i]) << (8*(1+i)));
         }

         result |= uint64_t(precision);
         return result;
      }

#define SY(P,X) ::sysio::chain::string_to_symbol_c(P,#X)

      /**
       * Encode a symbol.
       *
       * @throws symbol_type_exception if the name is longer than `max_symbol_name_length` or has a character outside
       *         `A`-`Z`
       */
      static uint64_t string_to_symbol(uint8_t precision, std::string_view str) {
         try {
            SYS_ASSERT(str.size() <= max_symbol_name_length, symbol_type_exception,
                       "symbol name is longer than {} characters", max_symbol_name_length);
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

         /**
          * The symbol's uppercase name, independent of precision.
          *
          * @throws symbol_type_exception if the value is not the code of a symbol: a character outside `A`-`Z`, or
          *         a nonzero top byte, which a symbol's precision leaves no room for
          */
         string name() const;
      };

      class symbol : fc::reflect_init {
         public:

            static constexpr uint8_t max_precision = 18;
            /// Width of the precision field in the low bits of the value; the symbol code is the bits above it.
            static constexpr uint32_t precision_bits = 8;

            explicit symbol(uint8_t p, std::string_view s): m_value(string_to_symbol(p, s)) {
               SYS_ASSERT(valid(), symbol_type_exception, "invalid symbol: {}", s);
            }
            explicit symbol(uint64_t v = CORE_SYMBOL): m_value(v) {
               SYS_ASSERT(valid(), symbol_type_exception, "invalid symbol: {}", name());
            }
            /**
             * Parse a symbol of the form "4,SYS": the precision, a comma, the name.
             *
             * @throws symbol_type_exception if the string is empty or has no comma, the precision is outside
             *         0..`max_precision`, or the name is longer than `max_symbol_name_length` or has a character
             *         outside `A`-`Z`
             * @throws fc::parse_error_exception if the precision is not an integer
             */
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
               SYS_ASSERT( decimals() <= max_precision, symbol_type_exception,
                           "precision {} should be <= {}", decimals(), max_precision );
               uint64_t p10 = 1;
               uint64_t p = decimals();
               while( p > 0  ) {
                  p10 *= 10; --p;
               }
               return p10;
            }
            string name() const
            {
               uint64_t v = m_value;
               v >>= 8;
               string result;
               while (v > 0) {
                  char c = v & 0xFF;
                  result += c;
                  v >>= 8;
               }
               return result;
            }

            symbol_code to_symbol_code()const { return {m_value >> precision_bits}; }

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
               SYS_ASSERT( decimals() <= max_precision, symbol_type_exception,
                           "precision {} should be <= {}", decimals(), max_precision );
               SYS_ASSERT( valid_name(name()), symbol_type_exception, "invalid symbol: {}", name() );
            }

         private:
            uint64_t m_value;
            friend struct fc::reflector<symbol>;
      }; // class symbol

      inline string symbol_code::name() const {
         const uint64_t symbol_value = value << symbol::precision_bits;
         SYS_ASSERT( (symbol_value >> symbol::precision_bits) == value, symbol_type_exception,
                     "symbol code {:#x} is too long", value );
         return symbol(symbol_value).name();
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

namespace fc {
   inline void to_variant(const sysio::chain::symbol& var, fc::variant& vo) { vo = var.to_string(); }
   inline void from_variant(const fc::variant& var, sysio::chain::symbol& vo) {
      vo = sysio::chain::symbol::from_string(var.get_string());
   }
}

namespace fc {
   inline void to_variant(const sysio::chain::symbol_code& var, fc::variant& vo) {
      vo = var.name();
   }
   inline void from_variant(const fc::variant& var, sysio::chain::symbol_code& vo) {
      vo = sysio::chain::symbol(0, var.get_string()).to_symbol_code();
   }
}

FC_REFLECT(sysio::chain::symbol_code, (value))
FC_REFLECT(sysio::chain::symbol, (m_value))
FC_REFLECT(sysio::chain::extended_symbol, (sym)(contract))

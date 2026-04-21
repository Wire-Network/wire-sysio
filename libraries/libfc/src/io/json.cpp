#include <fc/io/json.hpp>
//#include <fc/io/fstream.hpp>
//#include <fc/io/sstream.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/log/logger.hpp>
//#include <utfcpp/utf8.h>
#include <fc/utf8.hpp>
#include <charconv>
#include <iostream>
#include <fstream>
#include <sstream>

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

namespace fc
{
    // forward declarations of provided functions
    template<typename T, json::parse_type parser_type> variant variant_from_stream( T& in, uint32_t max_depth );
    template<typename T> std::string parse_escape( T& in );
    template<typename T> std::string string_from_stream( T& in );
    template<typename T> bool skip_white_space( T& in );
    template<typename T> std::string string_from_token( T& in );
    template<typename T, json::parse_type parser_type> variant_object object_from_stream( T& in, uint32_t max_depth );
    template<typename T, json::parse_type parser_type> variants array_from_stream( T& in, uint32_t max_depth );
    template<typename T, json::parse_type parser_type> variant number_from_stream( T& in );
    template<typename T> variant token_from_stream( T& in );
}

#include <fc/io/json_relaxed.hpp>

namespace
{

   // Max |value| decimal strings used to pick the smallest variant bucket that fits a token.
   // Signed entries use the magnitude of *_MIN (one greater than *_MAX); unsigned entries use *_MAX.
   constexpr std::string_view int256_max_str =
      "57896044618658097711785492504343953926634992332820282019728792003956564819968";
   constexpr std::string_view uint256_max_str =
      "115792089237316195423570985008687907853269984665640564039457584007913129639935";
}

namespace fc
{
   namespace {
      void append_utf8( std::string& out, uint32_t cp )
      {
         if( cp < 0x80 ) {
            out.push_back( static_cast<char>(cp) );
         } else if( cp < 0x800 ) {
            out.push_back( static_cast<char>(0xC0 | (cp >> 6)) );
            out.push_back( static_cast<char>(0x80 | (cp & 0x3F)) );
         } else if( cp < 0x10000 ) {
            out.push_back( static_cast<char>(0xE0 | (cp >> 12)) );
            out.push_back( static_cast<char>(0x80 | ((cp >> 6) & 0x3F)) );
            out.push_back( static_cast<char>(0x80 | (cp & 0x3F)) );
         } else {
            out.push_back( static_cast<char>(0xF0 | (cp >> 18)) );
            out.push_back( static_cast<char>(0x80 | ((cp >> 12) & 0x3F)) );
            out.push_back( static_cast<char>(0x80 | ((cp >> 6) & 0x3F)) );
            out.push_back( static_cast<char>(0x80 | (cp & 0x3F)) );
         }
      }

      template<typename T>
      uint32_t read_hex4( T& in )
      {
         uint32_t v = 0;
         for( int i = 0; i < 4; ++i ) {
            char h = in.get();
            uint32_t d;
            if( h >= '0' && h <= '9' )      d = h - '0';
            else if( h >= 'a' && h <= 'f' ) d = 10 + (h - 'a');
            else if( h >= 'A' && h <= 'F' ) d = 10 + (h - 'A');
            else FC_THROW_EXCEPTION( parse_error_exception, "Invalid hex digit in \\u escape" );
            v = (v << 4) | d;
         }
         return v;
      }

      template<typename T>
      void append_unicode_escape( std::string& out, T& in )
      {
         // called with 'u' still in the stream
         in.get(); // consume 'u'
         uint32_t cp = read_hex4( in );
         if( cp >= 0xD800 && cp <= 0xDBFF ) {
            // high surrogate -- must be followed by \uXXXX low surrogate
            if( in.get() != '\\' || in.get() != 'u' )
               FC_THROW_EXCEPTION( parse_error_exception, "Unpaired high surrogate in \\u escape" );
            uint32_t low = read_hex4( in );
            if( low < 0xDC00 || low > 0xDFFF )
               FC_THROW_EXCEPTION( parse_error_exception, "Invalid low surrogate in \\u escape" );
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
         } else if( cp >= 0xDC00 && cp <= 0xDFFF ) {
            FC_THROW_EXCEPTION( parse_error_exception, "Orphan low surrogate in \\u escape" );
         }
         append_utf8( out, cp );
      }
   } // anonymous namespace

   template<typename T>
   std::string parse_escape( T& in )
   {
      std::string result;
      if( in.peek() != '\\' )
         FC_THROW_EXCEPTION( parse_error_exception, "Expected '\\'" );
      try {
         in.get();
         char c = in.peek();
         switch( c )
         {
            case '"': case '\\': case '/':
                      in.get(); result.push_back( c );    break;
            case 'b': in.get(); result.push_back( '\b' ); break;
            case 'f': in.get(); result.push_back( '\f' ); break;
            case 'n': in.get(); result.push_back( '\n' ); break;
            case 'r': in.get(); result.push_back( '\r' ); break;
            case 't': in.get(); result.push_back( '\t' ); break;
            case 'u': append_unicode_escape( result, in ); break;
            default:
               // Lenient: unknown escape passes through verbatim (non-strict per RFC 8259).
               in.get();
               result.push_back( c );
               break;
         }
         return result;
      } FC_RETHROW_EXCEPTIONS( info, "Stream ended with '\\'" );
   }

   template<typename T>
   bool skip_white_space( T& in )
   {
       bool skipped = false;
       while( true )
       {
          switch( in.peek() )
          {
             case ' ':
             case '\t':
             case '\n':
             case '\r':
                skipped = true;
                in.get();
                break;
             case '\0':
                FC_THROW_EXCEPTION( eof_exception, "unexpected end of file" );
                break;
             default:
                return skipped;
          }
       }
   }

   template<typename T>
   std::string string_from_stream( T& in )
   {
      std::string token;
      try
      {
         char c = in.peek();

         if( c != '"' )
            FC_THROW_EXCEPTION( parse_error_exception, "Expected '\"' but read '{}'", std::string(&c, (&c) + 1) );
         in.get();
         while( !in.eof() )
         {
            switch( c = in.peek() )
            {
               case '\\':
                  token += parse_escape( in );
                  break;
               case 0x04:
                  FC_THROW_EXCEPTION( parse_error_exception, "EOF before closing '\"' in string '{}'", token );
               case '"':
                  in.get();
                  return token;
               default:
                  token += c;
                  in.get();
            }
         }
         FC_THROW_EXCEPTION( parse_error_exception, "EOF before closing '\"' in string '{}'", token );
       } FC_RETHROW_EXCEPTIONS( warn, "while parsing token '{}'", token );
   }

   template<typename T>
   std::string string_from_token( T& in )
   {
      std::string token;
      try
      {
         char c = in.peek();

         while( !in.eof() )
         {
            switch( c = in.peek() )
            {
               case '\\':
                  token += parse_escape( in );
                  break;
               case '\t':
               case ' ':
               case '\n':
                  in.get();
                  return token;
               case '\0':
                  FC_THROW_EXCEPTION( eof_exception, "unexpected end of file" );
               default:
                if( isalnum( c ) || c == '_' || c == '-' || c == '.' || c == ':' || c == '/' )
                {
                  token += c;
                  in.get();
                }
                else return token;
            }
         }
         return token;
      }
      catch( const fc::eof_exception& eof )
      {
         return token;
      }
      catch (const std::ios_base::failure&)
      {
         return token;
      }

      FC_RETHROW_EXCEPTIONS( warn, "while parsing token '{}'", token );
   }

   template<typename T, json::parse_type parser_type>
   variant_object object_from_stream( T& in, uint32_t max_depth )
   {
      mutable_variant_object obj;
      try
      {
         char c = in.peek();
         if( c != '{' )
            FC_THROW_EXCEPTION( parse_error_exception, "Expected '{{', but read '{}'", std::string(&c, &c + 1) );
         in.get();
         while( in.peek() != '}' )
         {
            if( in.peek() == ',' )
            {
               in.get();
               continue;
            }
            if( skip_white_space(in) ) continue;
            std::string key = string_from_stream( in );
            skip_white_space(in);
            if( in.peek() != ':' )
            {
               FC_THROW_EXCEPTION( parse_error_exception, "Expected ':' after key \"{}\"", key );
            }
            in.get();
            auto val = variant_from_stream<T, parser_type>( in, max_depth - 1 );

            obj(std::move(key),std::move(val));
            //skip_white_space(in);
         }
         if( in.peek() == '}' )
         {
            in.get();
            return obj;
         }
         FC_THROW_EXCEPTION( parse_error_exception, "Expected '}}' after {}", fc::json::to_log_string(obj) );
      }
      catch( const fc::eof_exception& e )
      {
         FC_THROW_EXCEPTION( parse_error_exception, "Unexpected EOF: {}", e.to_detail_string() );
      }
      catch( const std::ios_base::failure& e )
      {
         FC_THROW_EXCEPTION( parse_error_exception, "Unexpected EOF: {}", e.what() );
      } FC_RETHROW_EXCEPTIONS( warn, "Error parsing object" );
   }

   template<typename T, json::parse_type parser_type>
   variants array_from_stream( T& in, uint32_t max_depth )
   {
      variants ar;
      try
      {
        if( in.peek() != '[' )
           FC_THROW_EXCEPTION( parse_error_exception, "Expected '['" );
        in.get();
        skip_white_space(in);

        while( in.peek() != ']' )
        {
           if( in.peek() == ',' )
           {
              in.get();
              continue;
           }
           if( skip_white_space(in) ) continue;
           ar.push_back( variant_from_stream<T, parser_type>( in, max_depth - 1) );
           skip_white_space(in);
        }
        if( in.peek() != ']' )
           FC_THROW_EXCEPTION( parse_error_exception, "Expected ']' after parsing {}", fc::json::to_log_string(ar) );

        in.get();
      } FC_RETHROW_EXCEPTIONS( warn, "Attempting to parse array {}", fc::json::to_log_string(ar) );
      return ar;
   }

   template<typename T, json::parse_type parser_type>
   variant number_from_stream( T& in )
   {
      std::string s;

      bool  dot = false;
      bool  neg = false;
      if( in.peek() == '-')
      {
        neg = true;
        s += in.get();
      }
      bool done = false;

      try
      {
        while( !done )
        {
          char c = in.peek();
          switch( c )
          {
              case '.':
                 if (dot)
                    FC_THROW_EXCEPTION(parse_error_exception, "Can't parse a number with two decimal places");
                 dot = true;
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7':
              case '8':
              case '9':
                 s += in.get();
                 break;
              case '\0':
                 FC_THROW_EXCEPTION( eof_exception, "unexpected end of file" );
              default:
                 if( isalnum( c ) )
                 {
                    s += string_from_token( in );
                    return s;
                 }
                done = true;
                break;
          }
        }
      }
      catch (fc::eof_exception&)
      {
      }
      catch (const std::ios_base::failure&)
      {
      }

      const auto no_neg_start = neg ? 1 : 0;
      const auto start = s.find_first_not_of('0', no_neg_start);
      const auto str = (start != std::string::npos)
         ? std::string_view(s).substr(start)
         : std::string_view{};

      // if the string is empty and we dropped zeros
      if (str.empty() && no_neg_start < start)
         return 0u;

      // check for s== ".", "-","-.", since "[-]0*" is checked above
      if (str == "." || str.empty()) // check the obviously wrong things we could have encountered
         FC_THROW_EXCEPTION(parse_error_exception, "Can't parse token \"{}\" as a JSON numeric constant", str);

      if( dot )
         return parser_type == json::parse_type::legacy_parser_with_string_doubles ? variant(s) : variant(to_double(s));

      if( neg ) {
         if (str.length() > int256_max_str.length() ||
             (str.length() == int256_max_str.length() && str > int256_max_str)) {
            FC_THROW_EXCEPTION(parse_error_exception,
                               "Negative numeric token \"{}\" exceeds int256 range", s);
         }

         // using the string with no leading 0s, to avoid the string being assumed to be in octal,
         // since a leading 0 with only digits between 0 and 7 are assumed to be octal
         fc::int256 val256(str);
         val256 *= -1;
         if( val256 >= std::numeric_limits<int64_t>::min() ) {
            return static_cast<int64_t>(val256);
         }

         return val256;
      }

      if (str.length() > uint256_max_str.length() ||
          (str.length() == uint256_max_str.length() && str > uint256_max_str)) {
         FC_THROW_EXCEPTION(parse_error_exception,
                            "Numeric token \"{}\" exceeds uint256 range", s);
      }

      // using the string with no leading 0s, to avoid the string being assumed to be in octal,
      // since a leading 0 with only digits between 0 and 7 are assumed to be octal
      fc::uint256 val256(str);
      if( val256 <= std::numeric_limits<uint64_t>::max() ) {
         return static_cast<uint64_t>(val256);
      }

      return val256;
   }

   template<typename T>
   variant token_from_stream( T& in )
   {
      std::string s;
      bool received_eof = false;
      bool done = false;

      try
      {
        char c;
        while((c = in.peek()) && !done)
        {
           switch( c )
           {
              case 'n':
              case 'u':
              case 'l':
              case 't':
              case 'r':
              case 'e':
              case 'f':
              case 'a':
              case 's':
                 s += in.get();
                 break;
              default:
                 done = true;
                 break;
           }
        }
      }
      catch (fc::eof_exception&)
      {
        received_eof = true;
      }
      catch (const std::ios_base::failure&)
      {
        received_eof = true;
      }

      // we can get here either by processing a delimiter as in "null,"
      // an EOF like "null<EOF>", or an invalid token like "nullZ"
      const std::string& str = s;
      if( str == "null" )
        return variant();
      if( str == "true" )
        return true;
      if( str == "false" )
        return false;
      else
      {
        if (received_eof)
        {
          if (str.empty())
            FC_THROW_EXCEPTION( parse_error_exception, "Unexpected EOF" );
          else
            return str;
        }
        else
        {
          // if we've reached this point, we've either seen a partial
          // token ("tru<EOF>") or something our simple parser couldn't
          // make out ("falfe")
          // A strict JSON parser would signal this as an error, but we
          // will just treat the malformed token as an un-quoted string.
          return str + string_from_token(in);;
        }
      }
   }


   template<typename T, json::parse_type parser_type>
   variant variant_from_stream( T& in, uint32_t max_depth )
   {
      if( max_depth == 0 )
          FC_THROW_EXCEPTION( parse_error_exception, "Too many nested items in JSON input!" );
      skip_white_space(in);
      variant var;
      while( 1 )
      {
         signed char c = in.peek();
         switch( c )
         {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
              in.get();
              continue;
            case '"':
              return string_from_stream( in );
            case '{':
               return object_from_stream<T, parser_type>( in, max_depth - 1 );
            case '[':
              return array_from_stream<T, parser_type>( in, max_depth - 1 );
            case '-':
            case '.':
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
              return number_from_stream<T, parser_type>( in );
            // null, true, false, or 'warning' / string
            case 'n':
            case 't':
            case 'f':
              return token_from_stream( in );
            case 0x04: // ^D end of transmission
            case EOF:
            case '\0':
              FC_THROW_EXCEPTION( eof_exception, "unexpected end of file" );
            default:
              FC_THROW_EXCEPTION( parse_error_exception, "Unexpected char '{}' in \"{}\"", c, string_from_token(in) );
         }
      }
	  return variant();
   }

   variant json::from_string( const std::string& utf8_str, const json::parse_type ptype, const uint32_t max_depth )
   { try {
      using stream_t = boost::iostreams::stream<boost::iostreams::array_source>;
      stream_t in(utf8_str.c_str(), utf8_str.size());
      switch( ptype )
      {
          case parse_type::legacy_parser:
             return variant_from_stream<stream_t, json::parse_type::legacy_parser>( in, max_depth );
          case parse_type::legacy_parser_with_string_doubles:
              return variant_from_stream<stream_t, json::parse_type::legacy_parser_with_string_doubles>( in, max_depth );
          case parse_type::strict_parser:
              return json_relaxed::variant_from_stream<stream_t, true>( in, max_depth );
          case parse_type::relaxed_parser:
              return json_relaxed::variant_from_stream<stream_t, false>( in, max_depth );
          default:
              FC_ASSERT( false, "Unknown JSON parser type {}", static_cast<int>(ptype) );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "{}", utf8_str ) }

   /**
    *  Append escaped JSON string to out.
    *  Convert '\t', '\r', '\n', '\\' and '"'  to "\t\r\n\\\"" if escape_control_chars == true
    *  Convert all other < 32 & 127 ascii to escaped unicode "\u00xx"
    *  Removes invalid utf8 characters
    *  Escapes Control sequence Introducer 0x9b to \u009b
    *  All other characters unmolested.
    */
   bool escape_string( const std::string_view& str, std::string& out, const json::yield_function_t& yield, bool escape_control_chars )
   {
      const auto init_size = str.size();
      const auto start_pos = out.size();
      out.reserve( start_pos + init_size + 13 ); // allow for a few escapes
      size_t i = 0;
      for( auto itr = str.begin(); itr != str.end(); ++i,++itr )
      {
         if( i % json::escape_string_yield_check_count == 0 ) yield( init_size + out.size() );
         switch( *itr )
         {
            case '\x00': out += "\\u0000"; break;
            case '\x01': out += "\\u0001"; break;
            case '\x02': out += "\\u0002"; break;
            case '\x03': out += "\\u0003"; break;
            case '\x04': out += "\\u0004"; break;
            case '\x05': out += "\\u0005"; break;
            case '\x06': out += "\\u0006"; break;
            case '\x07': out += "\\u0007"; break; // \a is not valid JSON
            case '\x08': out += "\\u0008"; break; // \b
         // case '\x09': out += "\\u0009"; break; // \t
         // case '\x0a': out += "\\u000a"; break; // \n
            case '\x0b': out += "\\u000b"; break;
            case '\x0c': out += "\\u000c"; break; // \f
         // case '\x0d': out += "\\u000d"; break; // \r
            case '\x0e': out += "\\u000e"; break;
            case '\x0f': out += "\\u000f"; break;
            case '\x10': out += "\\u0010"; break;
            case '\x11': out += "\\u0011"; break;
            case '\x12': out += "\\u0012"; break;
            case '\x13': out += "\\u0013"; break;
            case '\x14': out += "\\u0014"; break;
            case '\x15': out += "\\u0015"; break;
            case '\x16': out += "\\u0016"; break;
            case '\x17': out += "\\u0017"; break;
            case '\x18': out += "\\u0018"; break;
            case '\x19': out += "\\u0019"; break;
            case '\x1a': out += "\\u001a"; break;
            case '\x1b': out += "\\u001b"; break;
            case '\x1c': out += "\\u001c"; break;
            case '\x1d': out += "\\u001d"; break;
            case '\x1e': out += "\\u001e"; break;
            case '\x1f': out += "\\u001f"; break;

            case '\x7f': out += "\\u007f"; break;

            // if escape_control_chars=true these fall-through to default
            case '\t':        // \x09
               if( escape_control_chars ) {
                  out += "\\t";
                  break;
               }
            case '\n':        // \x0a
               if( escape_control_chars ) {
                  out += "\\n";
                  break;
               }
            case '\r':        // \x0d
               if( escape_control_chars ) {
                  out += "\\r";
                  break;
               }
            case '\\':
               if( escape_control_chars ) {
                  out += "\\\\";
                  break;
               }
            case '\"':
               if( escape_control_chars ) {
                  out += "\\\"";
                  break;
               }
            default:
               out += *itr;
         }
      }

      bool escaped = (out.size() - start_pos) != init_size;

      std::string_view appended( out.data() + start_pos, out.size() - start_pos );
      if( !is_valid_utf8( appended ) ) {
         std::string pruned = prune_invalid_utf8( appended );
         out.resize( start_pos );
         out += pruned;
         escaped = true;
      }

      return escaped;
   }

   std::string escape_string( const std::string_view& str, const json::yield_function_t& yield, bool escape_control_chars )
   {
      std::string r;
      escape_string( str, r, yield, escape_control_chars );
      return r;
   }

   // --- std::string-based serialization (indent=0 for compact, indent>0 for pretty) ---

   void to_stream( std::string& os, const variants& a, const json::yield_function_t& yield, uint8_t indent, int level );
   void to_stream( std::string& os, const variant_object& o, const json::yield_function_t& yield, uint8_t indent, int level );
   void to_stream( std::string& os, const variant& v, const json::yield_function_t& yield, uint8_t indent, int level );

   void to_stream( std::string& os, const variants& a, const json::yield_function_t& yield, const uint8_t indent, const int level )
   {
      yield(os.size());
      os += '[';
      if( indent && !a.empty() )
         os += '\n';
      auto itr = a.begin();

      while( itr != a.end() )
      {
         if( indent )
            os.append( (level + 1) * indent, ' ' );
         to_stream( os, *itr, yield, indent, level + 1 );
         ++itr;
         if( itr != a.end() )
            os += ',';
         if( indent )
            os += '\n';
      }
      if( indent && !a.empty() )
         os.append( level * indent, ' ' );
      os += ']';
   }

   void to_stream( std::string& os, const variant_object& o, const json::yield_function_t& yield, const uint8_t indent, const int level )
   {
       yield(os.size());
       os += '{';
       if( indent && o.size() )
          os += '\n';
       auto itr = o.begin();

       while( itr != o.end() )
       {
          if( indent )
             os.append( (level + 1) * indent, ' ' );
          os += '"';
          escape_string( itr->key(), os, yield );
          os += '"';
          os += indent ? ": " : ":";
          to_stream( os, itr->value(), yield, indent, level + 1 );
          ++itr;
          if( itr != o.end() )
             os += ',';
          if( indent )
             os += '\n';
       }
       if( indent && o.size() )
          os.append( level * indent, ' ' );
       os += '}';
   }

   void to_stream( std::string& os, const variant& v, const json::yield_function_t& yield, const uint8_t indent, const int level )
   {
      yield(os.size());
      switch( v.get_type() )
      {
         case variant::null_type:
              os += "null";
              return;
         case variant::int64_type:
         {
              int64_t i = v.as_int64();
              constexpr int64_t max_value(0xffffffff);
              if( i > max_value || i < -max_value ) {
                 os += '"';
                 os += v.as_string();
                 os += '"';
              } else {
                 char buf[21];
                 auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i);
                 os.append(buf, ptr);
              }
              return;
         }
         case variant::uint64_type:
         {
              uint64_t i = v.as_uint64();
              if( i > 0xffffffff ) {
                 os += '"';
                 os += v.as_string();
                 os += '"';
              } else {
                 char buf[21];
                 auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i);
                 os.append(buf, ptr);
              }
              return;
         }
         case variant::int128_type:
         case variant::uint128_type:
         case variant::int256_type:
         case variant::uint256_type: {
            os += '"';
            os += v.as_string();
            os += '"';
            return;
         }

         case variant::double_type:
              os += '"';
              os += v.as_string();
              os += '"';
              return;
         case variant::bool_type:
              os += v.as_bool() ? "true" : "false";
              return;
         case variant::string_type:
              os += '"';
              escape_string( v.get_string(), os, yield );
              os += '"';
              return;
         case variant::blob_type:
              os += '"';
              escape_string( v.as_string(), os, yield );
              os += '"';
              return;
         case variant::array_type:
              to_stream( os, v.get_array(), yield, indent, level );
              return;
         case variant::object_type:
              to_stream( os, v.get_object(), yield, indent, level );
              return;
         default:
            FC_THROW_EXCEPTION( fc::invalid_arg_exception, "Unsupported variant type: {}", std::to_string( v.get_type() ) );
      }
   }

   std::string json::to_string( const variant& v, const json::yield_function_t& yield )
   {
      std::string s;
      fc::to_stream( s, v, yield, 0, 0 );
      yield(s.size());
      return s;
   }

   std::string json::to_pretty_string( const variant& v, const json::yield_function_t& yield ) {
      std::string s;
      fc::to_stream( s, v, yield, 2, 0 );
      yield(s.size());
      return s;
   }

   bool json::save_to_file( const variant& v, const std::filesystem::path& fi, const bool pretty )
   {
      auto str = pretty ? json::to_pretty_string( v, fc::time_point::maximum(), max_length_limit )
                        : json::to_string( v, fc::time_point::maximum() );
      std::ofstream o(fi.generic_string().c_str());
      o.write( str.c_str(), str.size() );
      return o.good();
   }
   variant json::from_file( const std::filesystem::path& p, const json::parse_type ptype, const uint32_t max_depth )
   {
      std::ifstream bi( p.string(), std::ios::binary );
      switch( ptype )
      {
          case json::parse_type::legacy_parser:
             return variant_from_stream<std::ifstream, json::parse_type::legacy_parser>( bi, max_depth );
          case json::parse_type::legacy_parser_with_string_doubles:
              return variant_from_stream<std::ifstream, json::parse_type::legacy_parser_with_string_doubles>( bi, max_depth );
          case json::parse_type::strict_parser:
              return json_relaxed::variant_from_stream<std::ifstream, true>( bi, max_depth );
          case json::parse_type::relaxed_parser:
              return json_relaxed::variant_from_stream<std::ifstream, false>( bi, max_depth );
          default:
              FC_ASSERT( false, "Unknown JSON parser type {}", static_cast<int>(ptype) );
      }
   }

   bool json::is_valid( const std::string& utf8_str, const json::parse_type ptype, const uint32_t max_depth )
   {
      if( utf8_str.size() == 0 ) return false;
      std::stringstream in( utf8_str );
      switch( ptype )
      {
          case json::parse_type::legacy_parser:
             variant_from_stream<std::stringstream, json::parse_type::legacy_parser>( in, max_depth );
              break;
          case json::parse_type::legacy_parser_with_string_doubles:
             variant_from_stream<std::stringstream, json::parse_type::legacy_parser_with_string_doubles>( in, max_depth );
              break;
          case json::parse_type::strict_parser:
             json_relaxed::variant_from_stream<std::stringstream, true>( in, max_depth );
              break;
          case json::parse_type::relaxed_parser:
             json_relaxed::variant_from_stream<std::stringstream, false>( in, max_depth );
              break;
          default:
              FC_ASSERT( false, "Unknown JSON parser type {}", static_cast<int>(ptype) );
      }
      try { in.peek(); } catch ( const eof_exception& e ) { return true; }
      return false;
   }

} // fc

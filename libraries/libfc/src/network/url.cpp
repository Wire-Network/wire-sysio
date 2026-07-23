#include <fc/network/url.hpp>
#include <fc/string.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>

namespace fc
{
  namespace detail
  {
    class url_impl
    {
      public:
         /** Parse an authority while preserving bracketed IPv6 hosts and optional credentials. */
         void parse( const std::string& s )
         {
           const auto scheme_end = s.find("://");
           FC_ASSERT(scheme_end != std::string::npos && scheme_end != 0,
                     "Unable to parse URL scheme");
           _proto = s.substr(0, scheme_end);

           const auto authority_start = scheme_end + 3;
           const auto authority_end = s.find_first_of("/?", authority_start);
           std::string_view authority(s.data() + authority_start,
                                      (authority_end == std::string::npos ? s.size() : authority_end) -
                                         authority_start);
           const bool authority_is_safe =
              std::none_of(authority.begin(), authority.end(),
                           [](unsigned char character) {
                              return character <= 0x20 || character == 0x7f;
                           });
           FC_ASSERT(authority_is_safe,
                     "Unable to parse URL authority");
           if (const auto userinfo_end = authority.rfind('@');
               userinfo_end != std::string_view::npos) {
              const auto userinfo = authority.substr(0, userinfo_end);
              const auto separator = userinfo.find(':');
              _user = std::string(userinfo.substr(0, separator));
              if (separator != std::string_view::npos)
                 _pass = std::string(userinfo.substr(separator + 1));
              authority.remove_prefix(userinfo_end + 1);
           }

           std::string_view port_text;
           if (!authority.empty() && authority.front() == '[') {
              const auto bracket = authority.find(']');
              FC_ASSERT(bracket != std::string_view::npos,
                        "Unable to parse bracketed IPv6 host in URL");
              _host = std::string(authority.substr(1, bracket - 1));
              const auto suffix = authority.substr(bracket + 1);
              FC_ASSERT(suffix.empty() || suffix.front() == ':',
                        "Unable to parse URL authority after bracketed IPv6 host");
              if (!suffix.empty())
                 port_text = suffix.substr(1);
           } else {
              const auto separator = authority.rfind(':');
              if (separator != std::string_view::npos &&
                  authority.find(':') == separator) {
                 _host = std::string(authority.substr(0, separator));
                 port_text = authority.substr(separator + 1);
              } else {
                 _host = std::string(authority);
              }
           }
           if (!port_text.empty()) {
              try {
                 const auto port = to_uint64(std::string(port_text));
                 FC_ASSERT(port <= std::numeric_limits<uint16_t>::max(),
                           "URL port is out of range");
                 _port = static_cast<uint16_t>(port);
              } catch (...) {
                 FC_THROW_EXCEPTION(parse_error_exception,
                                    "Unable to parse port field in URL");
              }
           }

           const auto path_start =
              authority_end == std::string::npos ? s.size() : authority_end;
           const auto query_start = s.find('?', path_start);
           const auto path_end =
              query_start == std::string::npos ? s.size() : query_start;
           const auto path_text = s.substr(path_start, path_end - path_start);
#ifdef WIN32
           // On windows, a URL like file:///c:/autoexec.bat would result in _lpath = c:/autoexec.bat
           // which is what we really want (it's already an absolute path)
           if (!stricmp(_proto.c_str(), "file"))
              _path = path_text;
           else
              _path = path_text.empty() ? std::filesystem::path("/")
                                        : std::filesystem::path(path_text);
#else
           // On unix, a URL like file:///etc/rc.local would result in _lpath = etc/rc.local
           // but we really want to make it the absolute path /etc/rc.local
           _path = path_text.empty() ? std::filesystem::path("/")
                                     : std::filesystem::path(path_text);
#endif
           if (query_start != std::string::npos)
              _query = s.substr(query_start + 1);
         }

         std::string               _proto;
         ostring                   _host;
         ostring                   _user;
         ostring                   _pass;
         opath                     _path;
         ostring                   _query;
         ovariant_object           _args;
         std::optional<uint16_t>   _port;
    };
  }

  void to_variant( const url& u, fc::variant& v )
  {
    v = std::string(u);
  }
  void from_variant( const fc::variant& v, url& u )
  {
    u  = url( v.as_string() );
  }

  url::operator std::string()const
  {
      std::stringstream ss;
      ss<<my->_proto<<"://";
      if( my->_user ) {
        ss << *my->_user;
        if( my->_pass ) {
          ss<<":"<<*my->_pass;
        }
        ss<<"@";
      }
      if( my->_host ) {
        if (my->_host->find(':') != std::string::npos)
          ss<<"["<<*my->_host<<"]";
        else
          ss<<*my->_host;
      }
      if( my->_port ) ss<<":"<<*my->_port;
      if( my->_path ) ss<<my->_path->generic_string();
      if( my->_query ) ss<<"?"<<*my->_query;
    //  if( my->_args ) ss<<"?"<<*my->_args;
      return ss.str();
  }

  url::url( const std::string& u )
  :my( std::make_shared<detail::url_impl>() )
  {
    my->parse(u);
  }

  std::shared_ptr<detail::url_impl> get_null_url()
  {
    static auto u = std::make_shared<detail::url_impl>();
    return u;
  }

  url::url()
  :my( get_null_url() )
  { }

  url::url( const url& u )
  :my(u.my){}

  url::url( url&& u )
  :my( std::move(u.my) )
  {
    u.my = get_null_url();
  }

  url::url( const std::string& proto, const ostring& host, const ostring& user, const ostring& pass,
            const opath& path, const ostring& query, const ovariant_object& args, const std::optional<uint16_t>& port)
     :my( std::make_shared<detail::url_impl>() )
   {
      my->_proto = proto;
      my->_host = host;
      my->_user = user;
      my->_pass = pass;
      my->_path = path;
      my->_query = query;
      my->_args = args;
      my->_port = port;
   }

  url::~url(){}

  url& url::operator=(const url& u )
  {
     my = u.my;
     return *this;
  }

  url& url::operator=(url&& u )
  {
     if( this != &u )
     {
        my = std::move(u.my);
        u.my= get_null_url();
     }
     return *this;
  }

  std::string               url::proto()const
  {
    return my->_proto;
  }
  ostring                   url::host()const
  {
    return my->_host;
  }
  ostring                   url::user()const
  {
    return my->_user;
  }
  ostring                   url::pass()const
  {
    return my->_pass;
  }
  opath                     url::path()const
  {
    return my->_path;
  }
  ostring                   url::query()const
  {
    return my->_query;
  }
  ovariant_object           url::args()const
  {
    return my->_args;
  }
  std::optional<uint16_t>   url::port()const
  {
    return my->_port;
  }



}

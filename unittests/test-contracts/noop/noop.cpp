#include "noop.hpp"

using namespace sysio;

void noop::anyaction( name                       from,
                      const ignore<std::string>& type,
                      const ignore<std::string>& data )
{
   require_auth( from );
}

void noop::transfer( name                       from,
                     const ignore<name>&        to,
                     const ignore<asset>&       quantity,
                     const ignore<std::string>& memo )
{
   require_auth( from );
}

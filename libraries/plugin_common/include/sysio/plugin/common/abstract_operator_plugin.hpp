#pragma once

#include <appbase/application.hpp>

namespace sysio::plugin {

// operator_plugin_base: abstract base class that extends appbase::plugin
// This is a CRTP-style base to be used as: class my_plugin : public sysio::operator_plugin_base<my_plugin> { ... };
// It remains abstract by declaring a pure virtual marker method, which derived operator plugins should implement.

template <typename Impl>
class abstract_operator_plugin : public appbase::plugin<Impl> {
public:
   using appbase::plugin<Impl>::plugin; // inherit constructors
   virtual ~abstract_operator_plugin() = default;

   // Marker method to keep this base abstract. Implement in derived operator plugins if needed.
   virtual const char* operator_kind() const = 0;
};

} // namespace sysio

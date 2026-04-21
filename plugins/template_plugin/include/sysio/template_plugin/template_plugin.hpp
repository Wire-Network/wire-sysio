#pragma once
#include <sysio/chain/application.hpp>

namespace sysio {

using namespace appbase;

/// Template plugin — starting point for new plugins.
/// See also: `npx plop create-cxx-plugin`
class template_plugin : public appbase::plugin<template_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES()

   template_plugin();
   virtual ~template_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace sysio

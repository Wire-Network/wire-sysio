#include <sysio/template_plugin/template_plugin.hpp>

namespace sysio {
   static auto _template_plugin = application::register_plugin<template_plugin>();

class template_plugin_impl {
   public:
};

template_plugin::template_plugin():my(new template_plugin_impl()){}
template_plugin::~template_plugin(){}

void template_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
         ("option-name", bpo::value<string>()->default_value("default value"),
          "Option Description")
         ;
}

void template_plugin::plugin_initialize(const variables_map& options) {
   try {
      if( options.count( "option-name" )) {
         // Handle the option
      }
   }
   FC_LOG_AND_RETHROW()
}

void template_plugin::plugin_startup() {
   // Make the magic happen
}

void template_plugin::plugin_shutdown() {
   // OK, that's enough magic
}

}

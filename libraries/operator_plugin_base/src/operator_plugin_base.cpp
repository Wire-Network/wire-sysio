#include <fc/log/logger.hpp>

#include <sysio/plugin/operator_plugin_base.hpp>

namespace sysio::plugin {

  namespace {
    inline fc::logger& logger() {
      static fc::logger log{ "operator_plugin_base" };
      return log;
    }
  }
}
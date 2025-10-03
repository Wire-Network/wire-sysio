#include <fc/log/logger.hpp>

#include <sysio/operator_plugin_base/operator_plugin_base.hpp>

namespace sysio {

  namespace {
    inline fc::logger& logger() {
      static fc::logger log{ "operator_plugin_base" };
      return log;
    }
  }
}
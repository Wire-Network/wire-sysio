#include <fc/log/logger.hpp>

#include <sysio/plugin/common/abstract_operator_plugin.hpp>

namespace sysio::plugin {

  namespace {
    inline fc::logger& logger() {
      static fc::logger log{ "operator_plugin_base" };
      return log;
    }
  }
}
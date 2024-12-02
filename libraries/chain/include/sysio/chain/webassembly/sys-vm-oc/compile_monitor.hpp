#pragma once

#include <sysio/chain/webassembly/sys-vm-oc/config.hpp>

#include <boost/asio/local/datagram_protocol.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/ipc_helpers.hpp>

namespace sysio { namespace chain { namespace sysvmoc {

wrapped_fd get_connection_to_compile_monitor(int cache_fd);

}}}
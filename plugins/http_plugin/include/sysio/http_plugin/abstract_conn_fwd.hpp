#pragma once

#include <memory>

namespace sysio { namespace detail {
struct abstract_conn;
using abstract_conn_ptr = std::shared_ptr<abstract_conn>;
}} // namespace sysio::detail

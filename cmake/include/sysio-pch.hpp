// sysio-pch.hpp — precompiled header for fc and sysio_chain targets.
//
// This file is consumed by target_precompile_headers() in
// cmake/precompiled-headers.cmake.  Both targets share a single source
// file; headers that are only available in the chain target's include
// path are guarded with __has_include so the fc PCH compilation
// silently skips them.
//
// Header selection criteria:
//   - Frequently included across many TUs in the target
//   - Rarely or never modified (Boost/STL are ideal)
//   - Heavy parse cost (boost/multiprecision, boost/asio, boost/beast)
//   - Project headers only when stable and included in >25% of TUs
//
// Maintenance notes:
//   - boost/range headers are intentionally excluded because they
//     conflict with <ranges> (ADL ambiguity: boost::range::find vs
//     std::ranges::find).  Prefer std::ranges in new code.
//   - Adding a header here that changes frequently will hurt
//     incremental builds — any PCH header change forces recompilation
//     of every TU in the target.

#pragma once

// =====================================================================
// C++ standard library
// =====================================================================
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// =====================================================================
// Boost: multiprecision
// =====================================================================
#include <boost/multiprecision/cpp_int.hpp>

// =====================================================================
// Boost: multi_index
// =====================================================================
#include <boost/multi_index_container.hpp>
#include <boost/multi_index_container_fwd.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/global_fun.hpp>
#if __has_include(<boost/multi_index/random_access_index.hpp>)
#include <boost/multi_index/random_access_index.hpp>
#endif

// =====================================================================
// Boost: iostreams
// =====================================================================
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/pipeline.hpp>
#include <boost/iostreams/positioning.hpp>

// =====================================================================
// Boost: asio
// =====================================================================
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/random_access_file.hpp>
#include <boost/asio/local/datagram_protocol.hpp>

// =====================================================================
// Boost: interprocess
// =====================================================================
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/exceptions.hpp>

// =====================================================================
// Boost: beast
// =====================================================================
#include <boost/beast.hpp>

// =====================================================================
// Boost: preprocessor (used by fc::reflect macros)
// =====================================================================
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/enum.hpp>
#include <boost/preprocessor/seq/size.hpp>
#include <boost/preprocessor/seq/seq.hpp>
#include <boost/preprocessor/stringize.hpp>

// =====================================================================
// Boost: signals2
// =====================================================================
#include <boost/signals2/signal.hpp>

// =====================================================================
// Boost: misc utilities
// =====================================================================
#include <boost/lexical_cast.hpp>
#include <boost/core/typeinfo.hpp>
#include <boost/core/demangle.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/scoped_array.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/crc.hpp>
#include <boost/pool/singleton_pool.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/hana/string.hpp>
#include <boost/hana/equal.hpp>
#include <boost/rational.hpp>
#include <boost/tuple/tuple_io.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/assign/list_of.hpp>

// =====================================================================
// fc project headers (stable foundation, included across most TUs)
// =====================================================================
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/datastream.hpp>
#include <fc/string.hpp>
#include <fc/fwd_impl.hpp>
#include <fc/time.hpp>
#include <fc/bitutil.hpp>

// =====================================================================
// chain project headers (only available when building sysio_chain)
// =====================================================================
#if __has_include(<sysio/chain/exceptions.hpp>)
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <sysio/chain/resource_limits.hpp>
#endif

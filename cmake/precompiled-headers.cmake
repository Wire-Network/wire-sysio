# precompiled-headers.cmake
#
# Precompiled header (PCH) support for fc and sysio_chain targets.
# Toggle with -DENABLE_PCH=ON|OFF (default ON).
#
# Benchmarked on fc + sysio_chain clean build (12-core, clang-18, Debug):
#   Without PCH: 2m 55s wall / 3014s CPU
#   With PCH:    1m 36s wall / 1129s CPU  (45% wall-time reduction)
#
# Header selection criteria:
#   - Frequently included across many TUs in the target
#   - Rarely or never modified (Boost/STL are ideal — zero local edits)
#   - Heavy parse cost (boost/multiprecision, boost/asio, boost/beast)
#   - Project headers only when stable and included in >25% of TUs
#
# Maintenance notes:
#   - boost/range headers are intentionally excluded from the chain PCH
#     because they conflict with <ranges> (ADL ambiguity between
#     boost::range::find and std::ranges::find). Prefer std::ranges in
#     new code; see authority_checker.hpp and parallel_markers.hpp.
#   - The chain target mixes C and C++ sources. The C file
#     (gs_seg_helpers.c) must be excluded via SKIP_PRECOMPILE_HEADERS
#     since this PCH is C++ only.
#   - Adding a header here that changes frequently will hurt incremental
#     builds — any PCH header change forces recompilation of every TU
#     in the target.

option(ENABLE_PCH "Enable precompiled headers for faster builds" ON)

# ---------------------------------------------------------------------------
# fc (foundation library, ~70 TUs)
# ---------------------------------------------------------------------------
function(configure_fc_pch target)
   if(NOT ENABLE_PCH)
      return()
   endif()

   target_precompile_headers(${target} PRIVATE
      # --- C++ standard library ---
      <algorithm>
      <fstream>
      <iostream>
      <limits>
      <memory>
      <ranges>
      <sstream>
      <string>
      <string_view>
      <vector>

      # --- Boost: multiprecision (single heaviest header; used by variant, datastream, raw, int256) ---
      <boost/multiprecision/cpp_int.hpp>

      # --- Boost: multi_index (used by tracked_storage, various indices) ---
      <boost/multi_index_container.hpp>
      <boost/multi_index_container_fwd.hpp>
      <boost/multi_index/ordered_index.hpp>
      <boost/multi_index/hashed_index.hpp>
      <boost/multi_index/member.hpp>
      <boost/multi_index/mem_fun.hpp>
      <boost/multi_index/composite_key.hpp>
      <boost/multi_index/global_fun.hpp>

      # --- Boost: iostreams (used by json.cpp, zlib.cpp, random_access_file) ---
      <boost/iostreams/filtering_stream.hpp>
      <boost/iostreams/device/back_inserter.hpp>
      <boost/iostreams/device/array.hpp>
      <boost/iostreams/device/mapped_file.hpp>
      <boost/iostreams/stream.hpp>
      <boost/iostreams/filter/zlib.hpp>
      <boost/iostreams/categories.hpp>
      <boost/iostreams/pipeline.hpp>
      <boost/iostreams/positioning.hpp>

      # --- Boost: asio (used by listener, message_buffer, mock_time) ---
      <boost/asio/ip/tcp.hpp>
      <boost/asio/deadline_timer.hpp>
      <boost/asio/random_access_file.hpp>

      # --- Boost: interprocess (used by exception.hpp, cfile.hpp, random_access_file) ---
      <boost/interprocess/file_mapping.hpp>
      <boost/interprocess/mapped_region.hpp>
      <boost/interprocess/exceptions.hpp>

      # --- Boost: beast (used by random_access_file.hpp) ---
      <boost/beast.hpp>

      # --- Boost: preprocessor (used by fc::reflect macros) ---
      <boost/preprocessor/seq/for_each.hpp>
      <boost/preprocessor/seq/enum.hpp>
      <boost/preprocessor/seq/size.hpp>
      <boost/preprocessor/seq/seq.hpp>
      <boost/preprocessor/stringize.hpp>

      # --- Boost: misc utilities ---
      <boost/lexical_cast.hpp>
      <boost/core/typeinfo.hpp>
      <boost/core/demangle.hpp>
      <boost/exception/diagnostic_information.hpp>
      <boost/scoped_array.hpp>
      <boost/dll/runtime_symbol_info.hpp>
      <boost/crc.hpp>
      <boost/pool/singleton_pool.hpp>

      # --- fc project headers (stable foundation, included in 15-35 of ~70 TUs) ---
      <fc/exception/exception.hpp>
      <fc/log/logger.hpp>
      <fc/variant.hpp>
      <fc/variant_object.hpp>
      <fc/reflect/reflect.hpp>
      <fc/crypto/hex.hpp>
      <fc/io/json.hpp>
      <fc/io/raw.hpp>
      <fc/io/datastream.hpp>
      <fc/string.hpp>
      <fc/fwd_impl.hpp>
   )
endfunction()

# ---------------------------------------------------------------------------
# sysio_chain (blockchain core, ~50 C++ TUs + 1 C file + 1 .s file)
# ---------------------------------------------------------------------------
function(configure_chain_pch target)
   if(NOT ENABLE_PCH)
      return()
   endif()

   # Exclude C source from C++ PCH (would fail on #include <algorithm> etc.)
   set_source_files_properties(
      webassembly/runtimes/sys-vm-oc/gs_seg_helpers.c
      DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON
   )

   target_precompile_headers(${target} PRIVATE
      # --- C++ standard library ---
      <algorithm>
      <memory>
      <mutex>
      <ranges>
      <string>
      <vector>

      # --- Boost: multiprecision ---
      <boost/multiprecision/cpp_int.hpp>

      # --- Boost: multi_index (used by fork_database, vote_processor, subjective_billing, snapshot_scheduler) ---
      <boost/multi_index_container.hpp>
      <boost/multi_index/ordered_index.hpp>
      <boost/multi_index/hashed_index.hpp>
      <boost/multi_index/member.hpp>
      <boost/multi_index/mem_fun.hpp>
      <boost/multi_index/composite_key.hpp>
      <boost/multi_index/global_fun.hpp>
      <boost/multi_index/random_access_index.hpp>

      # --- Boost: asio (used by thread_utils, transaction_metadata, wasm_interface, platform_timer) ---
      <boost/asio.hpp>
      <boost/asio/io_context.hpp>
      <boost/asio/thread_pool.hpp>
      <boost/asio/post.hpp>
      <boost/asio/use_future.hpp>
      <boost/asio/local/datagram_protocol.hpp>

      # --- Boost: iostreams (used by transaction.cpp, zlib compression) ---
      <boost/iostreams/filtering_stream.hpp>
      <boost/iostreams/device/back_inserter.hpp>
      <boost/iostreams/filter/zlib.hpp>

      # --- Boost: signals2 (used by controller.hpp) ---
      <boost/signals2/signal.hpp>

      # --- Boost: misc utilities ---
      <boost/container/flat_set.hpp>
      <boost/unordered/unordered_flat_map.hpp>
      <boost/core/demangle.hpp>
      <boost/core/typeinfo.hpp>
      <boost/core/ignore_unused.hpp>
      <boost/algorithm/string.hpp>
      # NOTE: boost/range intentionally excluded — conflicts with <ranges>
      # (ADL ambiguity: boost::range::find vs std::ranges::find).
      # Use std::find / std::ranges in new code instead.
      <boost/hana/string.hpp>
      <boost/hana/equal.hpp>
      <boost/rational.hpp>
      <boost/tuple/tuple_io.hpp>
      <boost/property_tree/ptree.hpp>
      <boost/property_tree/json_parser.hpp>
      <boost/date_time/posix_time/posix_time.hpp>
      <boost/accumulators/accumulators.hpp>
      <boost/accumulators/statistics/stats.hpp>
      <boost/assign/list_of.hpp>

      # --- fc project headers (stable, used across most chain TUs) ---
      <fc/exception/exception.hpp>
      <fc/io/json.hpp>
      <fc/io/raw.hpp>
      <fc/variant.hpp>
      <fc/time.hpp>
      <fc/bitutil.hpp>
      <fc/reflect/reflect.hpp>

      # --- chain project headers (stable, used in 8-28 of ~50 TUs) ---
      <sysio/chain/exceptions.hpp>
      <sysio/chain/apply_context.hpp>
      <sysio/chain/controller.hpp>
      <sysio/chain/account_object.hpp>
      <sysio/chain/global_property_object.hpp>
      <sysio/chain/transaction_context.hpp>
      <sysio/chain/authorization_manager.hpp>
      <sysio/chain/deep_mind.hpp>
      <sysio/chain/resource_limits.hpp>
   )
endfunction()

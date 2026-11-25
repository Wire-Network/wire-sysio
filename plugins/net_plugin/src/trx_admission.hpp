#pragma once

#include <fc/io/datastream.hpp>
#include <fc/io/raw.hpp>
#include <fc/scoped_exit.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/transaction.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace sysio::net_utils {

inline constexpr uint32_t def_max_trx_in_progress_size = 100u * 1024u * 1024u; // 100 MiB

/// Consume the body of a nonduplicate transaction on the connection strand. Completion callbacks can release bytes
/// during decoding, so read the balance again before passing the verified transaction and its estimate to the handler.
template <typename Stream, typename Drop, typename IdMismatch, typename Admit>
void process_trx_message_body(fc::bounded_datastream<Stream>& frame, const chain::transaction_id_type& wire_id,
                              const std::atomic<uint32_t>& in_progress, Drop&& drop, IdMismatch&& id_mismatch,
                              Admit&& admit) {
   const uint32_t before_decode = in_progress.load();
   if (before_decode >= def_max_trx_in_progress_size) {
      frame.skip(frame.remaining());
      drop(before_decode, chain::packed_transaction_ptr{});
      return;
   }

   auto trx = std::make_shared<chain::packed_transaction>();
   fc::raw::unpack(frame, *trx);
   frame.skip(frame.remaining());
   if (trx->id() != wire_id) {
      id_mismatch(trx->id());
      return;
   }

   const uint32_t in_progress_size = in_progress.load();
   const size_t trx_size = trx->get_estimated_size();
   if (in_progress_size >= def_max_trx_in_progress_size || trx_size > def_max_trx_in_progress_size - in_progress_size) {
      drop(in_progress_size, trx);
      return;
   }
   admit(trx, static_cast<uint32_t>(trx_size));
}

/// The connection strand charges the admitted size. A one-shot completion or destruction of an
/// unsubmitted callback releases it, including when construction or submission throws.
template <typename Connection, typename Completion>
chain::next_function<chain::transaction_trace_ptr> track_trx_in_progress(Connection& conn, uint32_t trx_size,
                                                                       Completion&& complete) {
   auto release = fc::make_scoped_exit([weak = conn.weak_from_this(), trx_size] {
      if (auto connection = weak.lock()) {
         connection->trx_in_progress_size -= trx_size;
      }
   });
   conn.trx_in_progress_size += trx_size;
   return [release = std::move(release), complete = std::forward<Completion>(complete)](
             const chain::next_function_variant<chain::transaction_trace_ptr>& result) mutable {
      auto release_on_exit = std::move(release);
      complete(result);
   };
}

} // namespace sysio::net_utils

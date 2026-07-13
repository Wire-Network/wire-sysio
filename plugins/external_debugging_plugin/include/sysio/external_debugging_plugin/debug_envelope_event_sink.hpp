#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sysio/batch_operator_plugin/debug_envelope_event.hpp>

namespace fc::parallel {
template <typename T>
class worker_task_queue;
}

namespace sysio::external_debugging {

/** Bounded asynchronous sink for batch-operator debug-envelope events. */
class debug_envelope_event_sink {
public:
   using event_type = opp::debugging::DebugEnvelopeEvent;
   using callback_type = std::function<void(event_type&)>;

   /** Create a running sink with a fixed pending-envelope capacity. */
   debug_envelope_event_sink(std::size_t pending_capacity, callback_type callback);

   /** Stop and join the worker before releasing queue storage. */
   ~debug_envelope_event_sink();

   debug_envelope_event_sink(const debug_envelope_event_sink&) = delete;
   debug_envelope_event_sink& operator=(const debug_envelope_event_sink&) = delete;
   debug_envelope_event_sink(debug_envelope_event_sink&&) = delete;
   debug_envelope_event_sink& operator=(debug_envelope_event_sink&&) = delete;

   /** Admit an event or record an explicit drop when the queue is unavailable. */
   void enqueue(const event_type& event);

   /** Stop accepting work and join the active callback. */
   void stop();

   /** Return the number of events rejected by bounded admission or shutdown. */
   uint64_t dropped_envelopes() const;

   /** Return the number of admitted events waiting behind the active callback. */
   std::size_t pending_envelopes() const;

   /** Return whether the queue can still dispatch callbacks. */
   bool running() const;

private:
   using queue_type = fc::parallel::worker_task_queue<event_type>;

   std::size_t _pending_capacity;
   std::shared_ptr<queue_type> _queue;
   std::atomic<uint64_t> _dropped_envelopes{0};
   std::once_flag _stop_once;
};

using debug_envelope_slot = std::function<void(const debug_envelope_event_sink::event_type&)>;

/** Build a signal slot that retains sink ownership through concurrent disconnect/shutdown. */
debug_envelope_slot make_debug_envelope_slot(std::shared_ptr<debug_envelope_event_sink> sink);

} // namespace sysio::external_debugging

//
// Created by jglanz on 1/25/2024.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace sysio::utils {

  template <typename... Args>
  class event_emitter {
    public:

      using subscribed_fn = std::function<void(Args...)>;

      struct subscription {
        const int key;
        const subscribed_fn fn;

        subscription(const int key, const subscribed_fn& fn) : key(key),
                                                               fn(fn) {
        }

        friend bool operator==(const subscription& lhs, const subscription& rhs) {
          return lhs.key == rhs.key;
        }

        friend bool operator!=(const subscription& lhs, const subscription& rhs) {
          return !(lhs == rhs);
        }

        // subscription(const subscription& other) = delete;
        // subscription(subscription&& other) noexcept = delete;

      };

      using unsubscribe_fn = std::function<void()>;
      using on_subscribe_change_fn = std::function<void(event_emitter*)>;

      using subscriptions = std::vector<subscription>;

      event_emitter() = default;

      virtual ~event_emitter() = default;

      std::vector<subscribed_fn> get_subscriptions() {
        std::scoped_lock lock(_subscription_mutex);
        std::vector<subscribed_fn> fns{};
        std::transform(
          _subscriptions.begin(),
          _subscriptions.end(),
          std::back_inserter(fns),
          [](auto& holder) {
            return holder.fn;
          }
        );

        return fns;
      }

      unsubscribe_fn subscribe(const subscribed_fn& listener) {
        std::scoped_lock lock(_subscription_mutex);
        int key = _next_key++;
        _subscriptions.emplace_back(key, listener);

        if (_on_subscribe) {
          _on_subscribe.value()(this);
        }

        return [key, this] {
          this->unsubscribe(key);
        };
      }

      void unsubscribe(auto key) {
        std::scoped_lock lock(_subscription_mutex);
        // TODO: Add deleter to cleanup
        // std::erase_if(_subscriptions,[key] (auto& holder) {
        //     return key == holder.key;
        // });

        if (_on_unsubscribe) {
          _on_unsubscribe.value()(this);
        }
      }

      void publish(Args... args) {
        std::scoped_lock lock(_subscription_mutex);
        for (auto& holder : _subscriptions) {
          holder.fn(args...);
        }
      }

      void set_on_subscribe(on_subscribe_change_fn fn) {
        _on_subscribe = fn;
      }

      void set_on_unsubscribe(on_subscribe_change_fn fn) {
        _on_unsubscribe = fn;
      }

    private:

      std::recursive_mutex _subscription_mutex{};
      subscriptions _subscriptions{};
      std::atomic_int _next_key{0};

      std::optional<on_subscribe_change_fn> _on_subscribe{};
      std::optional<on_subscribe_change_fn> _on_unsubscribe{};
  };

} // namespace sysio::cron_plugin::utils

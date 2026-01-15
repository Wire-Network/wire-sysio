#pragma once

#include <map>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace fc {
template <class K, class V, class Compare = std::less<K>>
class threadsafe_map {
public:
   using map_type = std::map<K, V, Compare>;

private:
   mutable std::shared_mutex _mtx;
   map_type _map;

public:
   threadsafe_map() = default;

   // ---------- Read view ----------
   class read_view {
      const map_type* _map = nullptr;
      std::shared_lock<std::shared_mutex> _lock;

   public:
      read_view(const map_type& map, std::shared_mutex& mtx)
         : _map(&map)
         , _lock(mtx) {}

      ~read_view() = default;
      read_view(const read_view&) = delete;
      read_view& operator=(const read_view&) = delete;
      read_view(read_view&&) noexcept = default;
      read_view& operator=(read_view&&) noexcept = default;

      // Read-only API
      bool contains(const K& key) const { return _map->contains(key); }

      const V& at(const K& key) const {
         auto it = _map->find(key);
         if (it == _map->end()) {
            throw std::out_of_range("threadsafe_map::read_view::at: key not found");
         }
         return it->second;
      }

      const V& get(const K& key) const { return at(key); }

      V get_copy(const K& key) const {
         return get(key);
      }

      // Read-only access operator: does NOT insert.
      // Throws std::out_of_range if key is missing.
      const V& operator[](const K& key) const {
         return at(key);
      }

      const map_type& map() const { return *_map; }

      auto begin() const { return _map->cbegin(); }
      auto end() const { return _map->cend(); }
      std::size_t size() const { return _map->size(); }
      bool empty() const { return _map->empty(); }
   };

   // ---------- Write view ----------
   class write_view {
      map_type* _map = nullptr;
      std::unique_lock<std::shared_mutex> _lock;

   public:
      write_view(map_type& map, std::shared_mutex& mtx)
         : _map(&map)
         , _lock(mtx) {}

      ~write_view() = default;
      write_view(const write_view&) = delete;
      write_view& operator=(const write_view&) = delete;
      write_view(write_view&&) noexcept = default;
      write_view& operator=(write_view&&) noexcept = default;

      bool contains(const K& key) const { return _map->contains(key); }

      const V& at(const K& key) const {
         auto it = _map->find(key);
         if (it == _map->end()) {
            throw std::out_of_range("threadsafe_map::read_view::at: key not found");
         }
         return it->second;
      }

      const V& get(const K& key) const { return at(key); }

      V get_copy(const K& key) const {
         return get(key);
      }
      // Mutating access operator: inserts default value if missing.
      V& operator[](const K& key) { return (*_map)[key]; }

      template <class... Args>
      auto try_emplace(const K& key, Args&&... args) {
         return _map->try_emplace(key, std::forward<Args>(args)...);
      }

      bool erase(const K& key) { return _map->erase(key) != 0; }

      void clear() { _map->clear(); }

      template <class Fn>
      decltype(auto) with_lock(Fn&& fn) {
         return std::forward<Fn>(fn)(*_map);
      }

      map_type& map() { return *_map; }
   };

   // ---------- Accessors ----------
   [[nodiscard]] read_view readable() const { return read_view{_map, const_cast<std::shared_mutex&>(_mtx)}; }

   [[nodiscard]] write_view writeable() { return write_view{_map, _mtx}; }
};
} // namespace fc
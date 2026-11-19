#include <sysio/chain_plugin/abi_serializer_cache.hpp>

#include <boost/core/typeinfo.hpp>

#include <algorithm>
#include <utility>

namespace sysio::chain_apis {

using chain::abi_def;
using chain::abi_serializer;

abi_serializer_cache::abi_serializer_cache(size_t max_bytes)
   : _max_bytes(max_bytes) {}

std::shared_ptr<const abi_serializer> abi_serializer_cache::get(std::span<const char> abi,
                                                                const fc::microseconds& max_build_time) {
   return get(abi, max_build_time, fc::time_point::maximum());
}

std::shared_ptr<const abi_serializer> abi_serializer_cache::get(std::span<const char> abi,
                                                                const fc::microseconds& max_build_time,
                                                                const fc::time_point& deadline) {
   if (abi_serializer::is_empty_abi(abi))
      return {};
   const fc::sha256       digest = fc::sha256::hash(abi.data(), abi.size());
   const fc::microseconds budget = build_budget(max_build_time, deadline, fc::time_point::now());
   {
      std::unique_lock lock(_mtx);
      const auto itr = _index.find(digest);
      if (itr != _index.end() && !must_rebuild(itr->second->second, budget)) {
         _lru.splice(_lru.begin(), _lru, itr->second);
         const entry cached = itr->second->second;
         lock.unlock();
         return result_of(cached);
      }
      SYS_ASSERT(budget > fc::microseconds(0), chain::abi_serialization_deadline_exception,
                 "serialization time limit {}us exceeded", budget.count());
      ++_builds;
   }
   return result_of(store(digest, build(abi, budget)));
}

fc::microseconds abi_serializer_cache::build_budget(const fc::microseconds& max_build_time,
                                                    const fc::time_point& deadline, const fc::time_point& now) {
   if (deadline == fc::time_point::maximum())
      return max_build_time;
   if (deadline <= now)
      return fc::microseconds(0);
   return std::min(max_build_time, deadline - now);
}

size_t abi_serializer_cache::size() const {
   std::lock_guard lock(_mtx);
   return _index.size();
}

size_t abi_serializer_cache::charged_bytes() const {
   std::lock_guard lock(_mtx);
   return _charged_bytes;
}

uint64_t abi_serializer_cache::builds() const {
   std::lock_guard lock(_mtx);
   return _builds;
}

abi_serializer_cache::entry abi_serializer_cache::build(std::span<const char> abi,
                                                       const fc::microseconds& max_build_time) {
   entry outcome;
   outcome.build_budget = max_build_time;
   outcome.charge       = abi.size() + entry_overhead_bytes;
   try {
      const abi_serializer::yield_function_t yield = abi_serializer::create_yield_function(max_build_time);
      abi_def def;
      abi_serializer::to_abi(abi, def);
      yield(0); // unpacking counts against the budget
      outcome.serializer = std::make_shared<const abi_serializer>(std::move(def), yield);
   } catch (const chain::abi_serialization_deadline_exception& e) {
      outcome.error     = e.dynamic_copy_exception();
      outcome.timed_out = true;
   } catch (const fc::exception& e) {
      outcome.error = e.dynamic_copy_exception();
   } catch (const std::exception& e) {
      outcome.error = fc::exception(FC_LOG_MESSAGE(warn, "rethrow: {}", e.what()), fc::std_exception_code,
                                    BOOST_CORE_TYPEID(e).name(), e.what()).dynamic_copy_exception();
   } catch (...) {
      outcome.error = fc::exception(FC_LOG_MESSAGE(warn, "unknown exception building ABI serializer"))
                         .dynamic_copy_exception();
   }
   if (outcome.error)
      outcome.failed_at = fc::time_point::now();
   return outcome;
}

bool abi_serializer_cache::retry_due(const entry& cached, const fc::time_point& now) {
   if (!cached.timed_out)
      return false;
   fc::time_point retry_at = cached.failed_at;
   retry_at.safe_add(timed_out_retry_interval);
   return now >= retry_at;
}

bool abi_serializer_cache::must_rebuild(const entry& cached, const fc::microseconds& max_build_time) {
   return cached.timed_out &&
          (max_build_time > cached.build_budget || retry_due(cached, fc::time_point::now()));
}

bool abi_serializer_cache::replaces(const entry& outcome, const entry& existing, const fc::time_point& now) {
   if (!existing.timed_out)
      return false; // a serializer, or a failure no budget changes
   return !outcome.timed_out || outcome.build_budget >= existing.build_budget || retry_due(existing, now);
}

std::shared_ptr<const abi_serializer> abi_serializer_cache::result_of(const entry& outcome) {
   if (outcome.error)
      outcome.error->rethrow(); // throws a copy, so concurrent callers never share one exception
   return outcome.serializer;
}

abi_serializer_cache::entry abi_serializer_cache::store(const fc::sha256& digest, const entry& outcome) {
   std::lock_guard lock(_mtx);
   if (const auto itr = _index.find(digest); itr != _index.end()) {
      const entry& existing = itr->second->second;
      if (!replaces(outcome, existing, fc::time_point::now())) {
         _lru.splice(_lru.begin(), _lru, itr->second);
         return existing;
      }
      _charged_bytes -= existing.charge;
      _lru.erase(itr->second);
      _index.erase(itr);
   }
   if (outcome.charge > _max_bytes)
      return outcome;
   _charged_bytes += outcome.charge;
   _lru.emplace_front(digest, outcome);
   _index.emplace(digest, _lru.begin());
   while (_charged_bytes > _max_bytes) {
      _charged_bytes -= _lru.back().second.charge;
      _index.erase(_lru.back().first);
      _lru.pop_back();
   }
   return outcome;
}

abi_serializer_cache& shared_abi_serializer_cache() {
   static abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   return cache;
}

} // namespace sysio::chain_apis

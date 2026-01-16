#include <sysio/services/cron_service_manager.hpp>

namespace sysio::services {

cron_service_manager::~cron_service_manager() {
   stop_all();
}

cron_service_ptr cron_service_manager::create(const std::string& name,
                                              const std::optional<cron_service::options>& options) {
   auto wv = _services.writeable();
   if (wv.contains(name)) {
      return nullptr;
   }

   cron_service::options opts = options.value_or(cron_service::options{});
   opts.name = name;
   auto service = cron_service::create(opts);
   wv.try_emplace(name, service);
   return service;
}

cron_service_ptr cron_service_manager::get_or_create(const std::string& name,
                                                     const std::optional<cron_service::options>& options) {
   auto wv = _services.writeable();
   auto it = wv.map().find(name);
   if (it != wv.map().end()) {
      return it->second;
   }

   cron_service::options opts = options.value_or(cron_service::options{});
   opts.name = name;
   auto service = cron_service::create(opts);
   wv.try_emplace(name, service);
   return service;
}

bool cron_service_manager::contains(const std::string& name) const {
   return _services.readable().contains(name);
}

cron_service_ptr cron_service_manager::get(const std::string& name) const {
   auto rv = _services.readable();
   auto it = rv.map().find(name);
   if (it != rv.map().end()) {
      return it->second;
   }
   return nullptr;
}

bool cron_service_manager::stop(const std::string& name) {
   cron_service_ptr service;
   {
      auto wv = _services.writeable();
      auto it = wv.map().find(name);
      if (it == wv.map().end()) {
         return false;
      }
      service = std::move(it->second);
      wv.map().erase(it);
   }
   // Service destructor handles cleanup (calls stop() internally)
   return true;
}

void cron_service_manager::stop_all() {
   std::vector<cron_service_ptr> to_stop;
   {
      auto wv = _services.writeable();
      for (auto& [_, service] : wv.map()) {
         to_stop.push_back(std::move(service));
      }
      wv.clear();
   }
   // Services cleaned up when vector goes out of scope
}

cron_service_ptr cron_service_manager::start(const std::string& name,
                                             const std::optional<cron_service::options>& options) {
   return create(name, options);
}

void cron_service_manager::start_all() {
   // No-op: cron_service instances auto-start on creation.
   // This method exists for API symmetry with stop_all().
}

const std::vector<cron_service_ptr> cron_service_manager::all() const {
   auto rv = _services.readable();
   return rv.values();
}

const std::vector<std::string> cron_service_manager::all_names() const {
   auto rv = _services.readable();
   return rv.keys();
}

std::size_t cron_service_manager::size() const {
   return _services.readable().size();
}

bool cron_service_manager::empty() const {
   return !_services.readable().size();
}

} // namespace sysio::cron_plugin::services

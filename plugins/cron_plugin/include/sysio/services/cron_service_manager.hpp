#pragma once

#include <fc-lite/threadsafe_map.hpp>
#include <functional>
#include <optional>
#include <string>
#include <sysio/services/cron_service.hpp>
#include <vector>

namespace sysio::services {

class cron_service_manager;
using cron_service_manager_ptr = std::shared_ptr<cron_service_manager>;

/**
 * Thread-safe manager for multiple cron_service instances.
 *
 * Provides lifecycle management (create, start, stop) and lookup operations
 * for named cron_service instances. All operations are thread-safe.
 */
class cron_service_manager : public std::enable_shared_from_this<cron_service_manager> {
public:
   using factory_fn = std::function<cron_service_ptr(const std::string& name)>;

   cron_service_manager() = default;
   ~cron_service_manager();

   cron_service_manager(const cron_service_manager&) = delete;
   cron_service_manager& operator=(const cron_service_manager&) = delete;
   cron_service_manager(cron_service_manager&&) = delete;
   cron_service_manager& operator=(cron_service_manager&&) = delete;

   /**
    * Create a new cron_service with the given name and options.
    * @param name Unique identifier for the service
    * @param options Optional configuration for the cron_service
    * @return The created cron_service, or nullptr if name already exists
    */
   cron_service_ptr create(const std::string& name, const std::optional<cron_service::options>& options = std::nullopt);

   /**
    * Get an existing service or create a new one if it doesn't exist.
    * @param name Unique identifier for the service
    * @param options Optional configuration (only used if creating new)
    * @return The existing or newly created cron_service
    */
   cron_service_ptr get_or_create(const std::string& name,
                                  const std::optional<cron_service::options>& options = std::nullopt);

   /**
    * Check if a service with the given name exists.
    * @param name Service identifier to check
    * @return true if the service exists
    */
   [[nodiscard]] bool contains(const std::string& name) const;

   /**
    * Get a service by name.
    * @param name Service identifier
    * @return The service, or nullptr if not found
    */
   [[nodiscard]] cron_service_ptr get(const std::string& name) const;

   /**
    * Remove and stop a service.
    * @param name Service identifier to stop
    * @return true if the service was found and stopped
    */
   bool stop(const std::string& name);

   /**
    * Stop and remove all services.
    */
   void stop_all();

   /**
    * Start a previously stopped service (re-creates it).
    * @param name Service identifier
    * @param options Optional configuration for the new service
    * @return The newly created service, or nullptr if name already exists
    * @note This creates a new service instance; it does not resume a stopped one.
    */
   cron_service_ptr start(const std::string& name, const std::optional<cron_service::options>& options = std::nullopt);

   /**
    * Restart all services that were previously registered.
    * @note This is a no-op; services auto-start on creation. Use stop_all() + create() pattern.
    */
   void start_all();

   /**
    * Get all managed services.
    * @return Vector of all cron_service instances (snapshot)
    */
   [[nodiscard]] const std::vector<cron_service_ptr> all() const;

   /**
    * Get names of all managed services.
    * @return Vector of service names (snapshot)
    */
   [[nodiscard]] const std::vector<std::string> all_names() const;

   /**
    * Get the number of managed services.
    * @return Count of services
    */
   [[nodiscard]] std::size_t size() const;

   /**
    * Check if no services are managed.
    * @return true if empty
    */
   [[nodiscard]] bool empty() const;

private:
   fc::threadsafe_map<std::string, cron_service_ptr> _services;
};

} // namespace sysio::cron_plugin::services

#include <fc/log/logger.hpp>
#include <sysio/cron_plugin.hpp>

namespace sysio {

namespace {
constexpr auto option_cron_threads = "cron-threads";
}

class cron_plugin_impl {
public:
   cron_plugin_impl() = default;
   ~cron_plugin_impl() = default;

   void initialize(const cron_service::options& options) {
      _cron_service = std::make_unique<opservices::cron_service>(options);
   }

   bool start() {
      return cron_service().start();
   }

   void stop() { cron_service().stop(); }

   opservices::cron_service& cron_service() {
      FC_ASSERT(_cron_service, "cron_service not initialized");
      return *_cron_service;
   }

private:
   std::unique_ptr<opservices::cron_service> _cron_service{nullptr};
};

static auto _cron_plugin = application::register_plugin<cron_plugin>();

cron_plugin::cron_plugin()
   : my(std::make_unique<cron_plugin_impl>()) {}

void cron_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()(option_cron_threads, boost::program_options::value<std::size_t>()->default_value(1),
                     "# of worker threads to use for cron job processing");
}

void cron_plugin::plugin_initialize(const variables_map& options) {
   cron_service::options cron_service_opts{
      .name = "cron_plugin", .num_threads = options.at(option_cron_threads).as<std::size_t>(), .autostart = false};

   my->initialize(cron_service_opts);
}

void cron_plugin::plugin_startup() {
   ilog("Starting cron plugin");
   my->start();
}

void cron_plugin::plugin_shutdown() {
   ilog("Shutdown cron plugin");
   my->stop();
}

services::cron_service& cron_plugin::cron_service() {
   return my->cron_service();
}

void cron_plugin::update_job_metadata(cron_service::job_id_t id, const cron_service::job_metadata_t& metadata) {}
std::vector<services::cron_service::job_id_t>
cron_plugin::list_jobs(const std::vector<cron_service::job_query_t>& queries) {
   return my->cron_service().list(queries);
}
void cron_plugin::cancel_job(cron_service::job_id_t id) {
   my->cron_service().cancel(id);
}
void cron_plugin::cancel_all_jobs() {
   my->cron_service().cancel_all();
}

cron_service::job_id_t cron_plugin::add_job(const services::cron_schedule& sched, cron_service::job_fn_t fn,
                                            const std::optional<cron_service::job_metadata_t>& metadata) {
   return cron_service().add(sched, fn, metadata);
}
} // namespace sysio

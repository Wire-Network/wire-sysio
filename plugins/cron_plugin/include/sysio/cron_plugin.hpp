#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/services/cron_service_manager.hpp>
#include <sysio/utils/event_emitter.hpp>

namespace sysio {

namespace opservices = sysio::services;
using opservices::cron_service;
using opservices::cron_service_manager;
using opservices::cron_service_manager_ptr;
using opservices::cron_service_ptr;

using utils::event_emitter;


class cron_plugin : public appbase::plugin<cron_plugin> {
public:

   APPBASE_PLUGIN_REQUIRES()
   cron_plugin();
   virtual ~cron_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   opservices::cron_service& cron_service();

   /**
    * Add a new scheduled job.
    *
    * @param sched cron_schedule
    * @param fn job function to execute
    * @param metadata optional job metadata
    * @return job_id_t
    */
   cron_service::job_id_t add_job(const services::cron_schedule& sched, cron_service::job_fn_t fn, const std::optional<cron_service::job_metadata_t>& metadata = std::nullopt);

   /**
    * Update job metadata
    *
    * @param id to update
    * @param metadata to set
    */
   void update_job_metadata(cron_service::job_id_t id, const cron_service::job_metadata_t& metadata);

   std::vector<cron_service::job_id_t> list_jobs(const std::vector<cron_service::job_query_t>& queries = {});

   void cancel_job(cron_service::job_id_t id);

   void cancel_all_jobs();
private:
   std::unique_ptr<class cron_plugin_impl> my;
};



} // namespace sysio

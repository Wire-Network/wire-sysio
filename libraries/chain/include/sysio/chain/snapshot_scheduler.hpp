#pragma once

#include <sysio/chain/pending_snapshot.hpp>

#include <fc/crypto/blake3.hpp>
#include <fc/log/log_message.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/types.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <limits>

namespace sysio::chain {

namespace bmi = boost::multi_index;
namespace fs = std::filesystem;

class snapshot_scheduler {
public:
   template<typename T>
   using next_function = sysio::chain::next_function<T>;

   struct snapshot_information {
      chain::block_id_type head_block_id;
      uint32_t head_block_num;
      fc::time_point head_block_time;
      uint32_t version;
      std::string snapshot_name;
      fc::crypto::blake3 root_hash;
   };

   struct snapshot_request_information {
      uint32_t block_spacing = 0;
      uint32_t start_block_num = 0;
      uint32_t end_block_num = std::numeric_limits<uint32_t>::max();
      std::string snapshot_description = "";
   };

   // this struct used to hold request params in api call
   // it is differentiate between 0 and empty values
   struct snapshot_request_params {
      std::optional<uint32_t> block_spacing;
      std::optional<uint32_t> start_block_num;
      std::optional<uint32_t> end_block_num;
      std::optional<std::string> snapshot_description;
   };

   struct snapshot_request_id_information {
      uint32_t snapshot_request_id = 0;
   };

   struct snapshot_schedule_result : public snapshot_request_id_information, public snapshot_request_information {
   };

   /**
    * A request's completion callback, held so that everything able to answer the caller shares one
    * consumable copy.
    *
    * A next_function may be invoked exactly once across all of its copies, yet more than one place
    * can reach a caller: the handler stored on a pending snapshot, another handler if the request
    * runs again after a fork, and the removal path when the request is cancelled or expires. Each
    * holds this shared_ptr rather than its own copy of the callback and moves the callback out of it
    * before use, so whichever gets there first is the one and only answer and the others find the
    * slot empty. Null means the request was scheduled without a caller to answer.
    */
   using request_callback = std::shared_ptr<next_function<snapshot_information>>;

   struct snapshot_schedule_information : public snapshot_request_id_information, public snapshot_request_information {
      std::vector<snapshot_information> pending_snapshots;
      request_callback caller; // not serialized
   };

   struct get_snapshot_requests_result {
      std::vector<snapshot_schedule_information> snapshot_requests;
   };

   struct by_height;

   using pending_snapshot_index = bmi::multi_index_container<
         pending_snapshot<snapshot_information>,
         indexed_by<
               bmi::hashed_unique<tag<by_id>, BOOST_MULTI_INDEX_MEMBER(pending_snapshot<snapshot_information>, block_id_type, block_id)>,
               bmi::ordered_non_unique<tag<by_height>, BOOST_MULTI_INDEX_CONST_MEM_FUN(pending_snapshot<snapshot_information>, uint32_t, get_height)>>>;

   class snapshot_db_json {
   public:
      snapshot_db_json() = default;
      ~snapshot_db_json() = default;

      void set_path(std::filesystem::path path) {
         db_path = std::move(path);
      }

      std::filesystem::path get_json_path() const {
         return db_path / "snapshot-schedule.json";
      }

      const snapshot_db_json& operator>>(std::vector<snapshot_schedule_information>& sr) {
         boost::property_tree::ptree root;

         try {
            std::ifstream file(get_json_path().string());
            file.exceptions(std::istream::failbit | std::istream::eofbit);
            boost::property_tree::read_json(file, root);

            // parse ptree
            for(boost::property_tree::ptree::value_type& req: root.get_child("snapshot_requests")) {
               snapshot_schedule_information ssi;
               ssi.snapshot_request_id = req.second.get<uint32_t>("snapshot_request_id");
               ssi.snapshot_description = req.second.get<std::string>("snapshot_description");
               ssi.block_spacing = req.second.get<uint32_t>("block_spacing");
               ssi.start_block_num = req.second.get<uint32_t>("start_block_num");
               ssi.end_block_num = req.second.get<uint32_t>("end_block_num");
               sr.push_back(ssi);
            }
         } catch(std::ifstream::failure& e) {
            elog("unable to restore snapshots schedule from filesystem {}, details: {}",
                 get_json_path().string(), e.what());
         }

         return *this;
      }

      const snapshot_db_json& operator<<(const std::vector<snapshot_schedule_information>& sr) const {
         boost::property_tree::ptree root;
         boost::property_tree::ptree node_srs;

         for(const auto& key: sr) {
            boost::property_tree::ptree node;
            node.put("snapshot_request_id", key.snapshot_request_id);
            node.put("snapshot_description", key.snapshot_description);
            node.put("block_spacing", key.block_spacing);
            node.put("start_block_num", key.start_block_num);
            node.put("end_block_num", key.end_block_num);
            node_srs.push_back(std::make_pair("", node));
         }

         root.push_back(std::make_pair("snapshot_requests", node_srs));

         try {
            std::ofstream file(get_json_path().string());
            file.exceptions(std::istream::failbit | std::istream::eofbit);
            boost::property_tree::write_json(file, root);
         } catch(std::ofstream::failure& e) {
            elog("unable to store snapshots schedule to filesystem to {}, details: {}",
                 get_json_path().string(), e.what());
         }

         return *this;
      }

   private:
      fs::path db_path;
   };

private:
   struct by_snapshot_id;
   struct by_snapshot_value;
   struct as_vector;

   using snapshot_requests = bmi::multi_index_container<
         snapshot_schedule_information,
         indexed_by<
               bmi::hashed_unique<tag<by_snapshot_id>, BOOST_MULTI_INDEX_MEMBER(snapshot_request_id_information, uint32_t, snapshot_request_id)>,
               bmi::random_access<tag<as_vector>>,
               bmi::ordered_unique<tag<by_snapshot_value>,
                                   composite_key<snapshot_schedule_information,
                                                 BOOST_MULTI_INDEX_MEMBER(snapshot_request_information, uint32_t, block_spacing),
                                                 BOOST_MULTI_INDEX_MEMBER(snapshot_request_information, uint32_t, start_block_num),
                                                 BOOST_MULTI_INDEX_MEMBER(snapshot_request_information, uint32_t, end_block_num)>>>>;
   snapshot_requests _snapshot_requests;
   snapshot_db_json _snapshot_db;
   pending_snapshot_index _pending_snapshot_index;

   uint32_t _snapshot_id = 0;
   uint32_t _inflight_sid = 0;

   // path to write the snapshots to
   fs::path _snapshots_dir;

   std::vector<std::function<void(const snapshot_information&)>> _snapshot_finalized_cbs;

   /**
    * Invoke every registered snapshot-finalized callback with @p si, logging and swallowing
    * callback exceptions so a misbehaving subscriber cannot affect other subscribers or the
    * snapshot pipeline.
    *
    * Must be called exactly once per finalized snapshot: from create_snapshot() when the
    * chain runs in irreversible read mode (the snapshot is final immediately), or from
    * on_irreversible_block() when a pending snapshot's block becomes irreversible.
    */
   void notify_snapshot_finalized(const snapshot_information& si);

   /**
    * Take the completion callback out of @p caller if one is still waiting there.
    *
    * Consuming the slot is what marks a caller answered, and it is visible to every other holder of
    * the same request_callback -- see that alias for why answering has to be exclusive. Callers of
    * this function own the answer and must deliver it.
    *
    * @param caller the request's shared callback slot; emptied when it still held a callback
    * @return the callback to invoke, or an empty next_function if the caller has already been
    *         answered or never existed
    */
   static next_function<snapshot_information> take_pending_answer(const request_callback& caller);

   /**
    * Erase request @p sri, resolving a completion callback it still carries.
    *
    * The single removal path for scheduled requests: every way a request can leave the container
    * -- expiry from unschedule_snapshot_requests(), explicit cancellation through
    * unschedule_snapshot() -- goes through here, so a caller waiting on a request can never have
    * its callback destroyed instead of answered. If the request's caller is still waiting, it is
    * answered with a snapshot_execution_exception built from @p undelivered_reason; if a snapshot
    * from this request already answered it, or one still in flight gets there first, nothing is
    * reported twice. Callback exceptions are logged and swallowed: this is reached from the
    * irreversible-block path, where a throwing completion callback must not escape into block
    * processing.
    *
    * @param sri                 id of the request to remove
    * @param undelivered_reason  message reported to a caller that is still waiting; built at the
    *                            call site so it carries that site's log context
    * @return the removed request
    * @throws snapshot_request_not_found if no request has that id
    */
   snapshot_schedule_result remove_request(uint32_t sri, fc::log_message undelivered_reason);

   void x_serialize() {
      auto& vec = _snapshot_requests.get<as_vector>();
      std::vector<snapshot_schedule_information> sr(vec.begin(), vec.end());
      _snapshot_db << sr;
   };

public:
   snapshot_scheduler() = default;

   using snapshot_finalized_callback_t = std::function<void(const snapshot_information&)>;

   /**
    * Register a callback invoked exactly once for each snapshot that reaches finality --
    * immediately on creation when the chain runs in irreversible read mode, otherwise when
    * the snapshot's block becomes irreversible. Fires for scheduled and on-demand snapshots
    * alike. Callback exceptions are logged and swallowed.
    */
   void add_snapshot_finalized_callback(snapshot_finalized_callback_t cb) {
      _snapshot_finalized_cbs.push_back(std::move(cb));
   }

   // snapshot scheduler listener
   void on_start_block(uint32_t height, chain::controller& chain);

   // to promote pending snapshots
   void on_irreversible_block(const signed_block_ptr& lib, const block_id_type& block_id, const chain::controller& chain);

   // snapshot scheduler handlers
   // schedule a snapshot request; scheduling validation errors are thrown to the caller.
   // next (may be empty) becomes the request's shared callback slot and is resolved at most once:
   // with the snapshot_information (or execution error) of the first snapshot this request delivers,
   // or with a snapshot_execution_exception if the request is removed before delivering one
   snapshot_schedule_result schedule_snapshot(const snapshot_request_information& sri, next_function<snapshot_information> next);

   /**
    * Cancel request @p sri.
    *
    * A request that has not yet delivered a snapshot may still have a caller waiting on it -- an
    * outstanding /v1/producer/create_snapshot is a scheduled request, and is visible through
    * get_snapshot_requests() like any other. Cancelling it resolves that caller with a
    * snapshot_execution_exception rather than destroying its callback.
    *
    * @param sri id of the request to cancel
    * @return the cancelled request
    * @throws snapshot_request_not_found if no request has that id
    */
   snapshot_schedule_result unschedule_snapshot(uint32_t sri);

   /**
    * Remove requests that can no longer produce a snapshot at the given irreversible block height.
    *
    * A one-time request is kept until lib_height passes its start_block_num, because it runs from
    * on_start_block() at start_block_num + 1 and irreversibility never runs ahead of the applied
    * head; its end_block_num, which scheduling allows to equal start_block_num, does not shorten
    * that. A recurring request expires once lib_height reaches its end_block_num.
    *
    * Removal goes through remove_request(), so a request removed while a caller is still waiting on
    * it reports that as an error rather than having its callback destroyed.
    *
    * @param lib_height block number of the last irreversible block
    */
   void unschedule_snapshot_requests(block_num_type lib_height);
   get_snapshot_requests_result get_snapshot_requests();

   /**
    * Find a scheduled request by its recurrence parameters.
    *
    * @param block_spacing   recurrence interval in blocks (0 for a one-time request)
    * @param start_block_num first eligible block height
    * @param end_block_num   last eligible block height
    * @return the matching request id, or std::nullopt when no such request is scheduled
    */
   std::optional<uint32_t> find_snapshot_request(uint32_t block_spacing, uint32_t start_block_num, uint32_t end_block_num) const;

   // initialize with storage
   void set_db_path(fs::path db_path);

   // set snapshot path
   void set_snapshots_path(fs::path sn_path);

   // add pending snapshot info to inflight snapshot request
   void add_pending_snapshot_info(const snapshot_information& si);

   // execute snapshot request srid; caller (may be null) is the request's shared callback slot, and
   // the snapshot answers whoever is still waiting there in addition to the scheduler's own
   // bookkeeping handler
   void execute_snapshot(uint32_t srid, chain::controller& chain, request_callback caller);

   // former producer_plugin snapshot fn
   void create_snapshot(next_function<snapshot_information> next, chain::controller& chain);
};


}// namespace sysio::chain

FC_REFLECT(sysio::chain::snapshot_scheduler::snapshot_information, (head_block_id) (head_block_num) (head_block_time) (version) (snapshot_name) (root_hash))
FC_REFLECT(sysio::chain::snapshot_scheduler::snapshot_request_information, (block_spacing) (start_block_num) (end_block_num) (snapshot_description))
FC_REFLECT(sysio::chain::snapshot_scheduler::snapshot_request_params, (block_spacing) (start_block_num) (end_block_num) (snapshot_description))
FC_REFLECT(sysio::chain::snapshot_scheduler::snapshot_request_id_information, (snapshot_request_id))
FC_REFLECT(sysio::chain::snapshot_scheduler::get_snapshot_requests_result, (snapshot_requests))
FC_REFLECT_DERIVED(sysio::chain::snapshot_scheduler::snapshot_schedule_information, (sysio::chain::snapshot_scheduler::snapshot_request_id_information)(sysio::chain::snapshot_scheduler::snapshot_request_information), (pending_snapshots))
FC_REFLECT_DERIVED(sysio::chain::snapshot_scheduler::snapshot_schedule_result, (sysio::chain::snapshot_scheduler::snapshot_request_id_information)(sysio::chain::snapshot_scheduler::snapshot_request_information), )

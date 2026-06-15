#include <sysio/chain/controller.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/pending_snapshot.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/snapshot_scheduler.hpp>

namespace sysio::chain {

// snapshot_scheduler_listener
void snapshot_scheduler::on_start_block(uint32_t height, chain::controller& chain) {
   // Identify the single request due at this height (one snapshot per height), then execute it AFTER
   // iterating. In irreversible read mode execute_snapshot() creates the snapshot synchronously and
   // unschedules the now-completed request, erasing from _snapshot_requests; doing that while the
   // range-for below iterates the same container would invalidate the loop iterator.
   bool                                found = false;
   uint32_t                            srid  = 0;
   next_function<snapshot_information> next;

   for(const auto& req: _snapshot_requests.get<0>()) {
      // -1 since its called from start block
      bool recurring_snapshot  = req.block_spacing && (height >= req.start_block_num + 1) && (!((height - req.start_block_num - 1) % req.block_spacing));
      bool onetime_snapshot    = (!req.block_spacing) && (height == req.start_block_num + 1);

      if(recurring_snapshot || onetime_snapshot) {
         dlog("snapshot scheduler creating a snapshot from the request [start_block_num:{}, end_block_num={}, block_spacing={}], height={}",
              req.start_block_num, req.end_block_num, req.block_spacing, height);
         srid  = req.snapshot_request_id;
         next  = req.next;
         found = true;
         break;
      }
   }

   if(found)
      execute_snapshot(srid, chain, next);
}

void snapshot_scheduler::on_irreversible_block(const signed_block_ptr& lib, const block_id_type& block_id, const chain::controller& chain) {
   auto& snapshots_by_height = _pending_snapshot_index.get<by_height>();
   uint32_t lib_height = lib->block_num();

   while(!snapshots_by_height.empty() && snapshots_by_height.begin()->get_height() <= lib_height) {
      const auto& pending = snapshots_by_height.begin();
      auto next = pending->next;

      try {
         auto si = pending->finalize(block_id, chain);
         next(snapshot_information{si});
         notify_snapshot_finalized(si);
         // FC_LOG_AND_DROP (not CATCH_AND_CALL) because `next` is a stored request-completion
         // callback invoked here on the block-processing path; a throwing callback must not
         // escape into block production, nor be re-invoked with the exception.
      } FC_LOG_AND_DROP();

      snapshots_by_height.erase(snapshots_by_height.begin());
   }

   unschedule_snapshot_requests(lib_height);
}

void snapshot_scheduler::unschedule_snapshot_requests(block_num_type lib_height) {
   std::vector<uint32_t> unschedule_snapshot_request_ids;
   for(const auto& req: _snapshot_requests.get<0>()) {
      bool marked_for_deletion = (!req.block_spacing && lib_height >= req.start_block_num) || // if one time snapshot executed or scheduled for the past, it should be gone
                                 lib_height >= req.end_block_num;               // any snapshot can expire by end block num (end_block_num can be max value)

      // cleanup - remove expired (or invalid) request
      if(marked_for_deletion) {
         unschedule_snapshot_request_ids.push_back(req.snapshot_request_id);
      }
   }

   for(const auto& i: unschedule_snapshot_request_ids) {
      unschedule_snapshot(i);
   }
}

std::optional<uint32_t> snapshot_scheduler::find_snapshot_request(uint32_t block_spacing, uint32_t start_block_num, uint32_t end_block_num) const {
   const auto& snapshot_by_value = _snapshot_requests.get<by_snapshot_value>();
   auto existing = snapshot_by_value.find(std::make_tuple(block_spacing, start_block_num, end_block_num));
   if (existing == snapshot_by_value.end())
      return std::nullopt;
   return existing->snapshot_request_id;
}

snapshot_scheduler::snapshot_schedule_result snapshot_scheduler::schedule_snapshot(const snapshot_request_information& sri, next_function<snapshot_information> next) {
   // validation errors are thrown to the caller; `next` is only stored as the request-completion
   // callback and must not be used to report scheduling errors, since it is invoked again later
   // from the block-start path where a throwing callback has no caller to report to
   SYS_ASSERT(!find_snapshot_request(sri.block_spacing, sri.start_block_num, sri.end_block_num), chain::duplicate_snapshot_request, "Duplicate snapshot request");
   SYS_ASSERT(sri.start_block_num <= sri.end_block_num, chain::invalid_snapshot_request, "End block number should be greater or equal to start block number");
   SYS_ASSERT(sri.start_block_num + sri.block_spacing <= sri.end_block_num, chain::invalid_snapshot_request, "Block spacing exceeds defined by start and end range");

   _snapshot_requests.emplace(snapshot_schedule_information{{_snapshot_id++}, {sri.block_spacing, sri.start_block_num, sri.end_block_num, sri.snapshot_description}, {}, next});
   x_serialize();

   // returning snapshot_schedule_result
   return snapshot_schedule_result{{_snapshot_id - 1}, {sri.block_spacing, sri.start_block_num, sri.end_block_num, sri.snapshot_description}};
}

snapshot_scheduler::snapshot_schedule_result snapshot_scheduler::unschedule_snapshot(uint32_t sri) {
   auto& snapshot_by_id = _snapshot_requests.get<by_snapshot_id>();
   auto existing = snapshot_by_id.find(sri);
   SYS_ASSERT(existing != snapshot_by_id.end(), chain::snapshot_request_not_found, "Snapshot request not found");

   snapshot_schedule_result result{{existing->snapshot_request_id}, {existing->block_spacing, existing->start_block_num, existing->end_block_num, existing->snapshot_description}};
   _snapshot_requests.erase(existing);
   x_serialize();

   // returning snapshot_schedule_result
   return result;
}

snapshot_scheduler::get_snapshot_requests_result snapshot_scheduler::get_snapshot_requests() {
   get_snapshot_requests_result result;
   auto& asvector = _snapshot_requests.get<as_vector>();
   result.snapshot_requests.reserve(asvector.size());
   result.snapshot_requests.insert(result.snapshot_requests.begin(), asvector.begin(), asvector.end());
   return result;
}

void snapshot_scheduler::set_db_path(fs::path db_path) {
   _snapshot_db.set_path(std::move(db_path));
   // init from db
   if(std::filesystem::exists(_snapshot_db.get_json_path())) {
      std::vector<snapshot_schedule_information> sr;
      _snapshot_db >> sr;
      // if db read succeeded, clear/load
      _snapshot_requests.get<by_snapshot_id>().clear();
      for(snapshot_schedule_information& ssi : sr) {
         //fix up Leap v4's "forever" value of 0 to MAX
         if(ssi.end_block_num == 0)
            ssi.end_block_num = std::numeric_limits<uint32_t>::max();
         _snapshot_requests.insert(ssi);
      }
   }
}

void snapshot_scheduler::set_snapshots_path(fs::path sn_path) {
   _snapshots_dir = std::move(sn_path);
}

void snapshot_scheduler::add_pending_snapshot_info(const snapshot_information& si) {
   auto& snapshot_by_id = _snapshot_requests.get<by_snapshot_id>();
   auto snapshot_req = snapshot_by_id.find(_inflight_sid);
   if(snapshot_req != snapshot_by_id.end()) {
      _snapshot_requests.modify(snapshot_req, [&si](auto& p) {
         p.pending_snapshots.emplace_back(si);
      });
   }
}

void snapshot_scheduler::execute_snapshot(uint32_t srid, chain::controller& chain, next_function<snapshot_information> http_next) {
   _inflight_sid = srid;
   auto next = [srid, this, http_next](const chain::next_function_variant<snapshot_information>& result) {
      if (http_next)
         http_next(chain::next_function_variant<snapshot_information>{result}); // copy; next_function::operator() is rvalue-only
      if(std::holds_alternative<fc::exception_ptr>(result)) {
         if (!http_next)
            wlog("Snapshot creation error: {}", std::get<fc::exception_ptr>(result)->to_detail_string());
      } else {
         // success, snapshot finalized
         auto snapshot_info = std::get<snapshot_information>(result);
         auto& snapshot_by_id = _snapshot_requests.get<by_snapshot_id>();
         auto snapshot_req = snapshot_by_id.find(srid);

         if(snapshot_req != snapshot_by_id.end()) {
            _snapshot_requests.modify(snapshot_req, [&](auto& p) {
               auto& pending = p.pending_snapshots;
               auto it = std::remove_if(pending.begin(), pending.end(), [&snapshot_info](const snapshot_information& s) { return s.head_block_num <= snapshot_info.head_block_num; });
               pending.erase(it, pending.end());
            });
         }

         // Deliberately no notify_snapshot_finalized() here: this `next` is one of possibly
         // several handlers chained onto the snapshot's completion, while the finalized
         // callbacks fire once per snapshot from create_snapshot() (irreversible mode) or
         // on_irreversible_block() (pending promotion). Notifying here as well would invoke
         // every subscriber twice for scheduled snapshots.
      }
   };
   create_snapshot(next, chain);
}

void snapshot_scheduler::create_snapshot(next_function<snapshot_information> next, chain::controller& chain) {
   auto head_id = chain.head().id();
   const auto head_block_num = chain.head().block_num();
   const auto head_block_time = chain.head().block_time();
   const auto& snapshot_path = pending_snapshot<snapshot_information>::get_final_path(head_id, _snapshots_dir);
   const auto& temp_path = pending_snapshot<snapshot_information>::get_temp_path(head_id, _snapshots_dir);

   // maintain legacy exception if the snapshot exists
   if(fs::is_regular_file(snapshot_path)) {
      auto ex = snapshot_exists_exception(FC_LOG_MESSAGE(error, "snapshot named {} already exists", _snapshots_dir.string()));
      next(ex.dynamic_copy_exception());
      return;
   }

   fc::crypto::blake3 captured_root_hash;
   auto write_snapshot = [&](const fs::path& p) -> void {
      fs::create_directory(p.parent_path());
      auto writer = std::make_shared<threaded_snapshot_writer>(p);
      chain.write_snapshot(writer);
      writer->finalize();
      captured_root_hash = writer->get_root_hash();
   };

   // If in irreversible mode, create snapshot and return path to snapshot immediately.
   if(chain.get_read_mode() == db_read_mode::IRREVERSIBLE) {
      try {
         ilog("Starting snapshot creation at block {}", head_block_num);
         write_snapshot(temp_path);
         std::error_code ec;
         fs::rename(temp_path, snapshot_path, ec);
         SYS_ASSERT(!ec, snapshot_finalization_exception,
                    "Unable to finalize valid snapshot of block number {}: [code: {}] {}",
                    head_block_num, ec.value(), ec.message());

         ilog("Snapshot creation at block {} complete; snapshot placed at {}", head_block_num, snapshot_path.string());
         snapshot_information si{head_id, head_block_num, head_block_time, chain_snapshot_header::current_version, snapshot_path.generic_string(), captured_root_hash};
         next(snapshot_information{si});
         notify_snapshot_finalized(si);
         // irreversible-mode snapshots are produced directly from a scheduled one-shot request and
         // never reach on_irreversible_block, so clean up the now-completed request here.
         unschedule_snapshot_requests(head_block_num);
      }
      CATCH_AND_CALL(next);
      return;
   }

   // Otherwise, the result will be returned when the snapshot becomes irreversible.

   // determine if this snapshot is already in-flight
   auto& pending_by_id = _pending_snapshot_index.get<by_id>();
   auto existing = pending_by_id.find(head_id);
   if(existing == pending_by_id.end()) { // if a snapshot at this block is already pending, ignore
      const auto& pending_path = pending_snapshot<snapshot_information>::get_pending_path(head_id, _snapshots_dir);

      try {
         ilog("Starting snapshot creation at block {}", head_block_num);
         write_snapshot(temp_path);// create a new pending snapshot

         std::error_code ec;
         fs::rename(temp_path, pending_path, ec);
         SYS_ASSERT(!ec, snapshot_finalization_exception,
                    "Unable to promote temp snapshot {} to pending {} for block number {}: [code: {}] {}",
                    temp_path.generic_string(), pending_path.generic_string(),
                    head_block_num, ec.value(), ec.message());
         ilog("Snapshot creation at block {} complete; snapshot will be available once block {} becomes irreversible", head_block_num, head_id);
         _pending_snapshot_index.emplace(head_id, head_block_time, next, pending_path.generic_string(), snapshot_path.generic_string(), captured_root_hash);
         add_pending_snapshot_info(snapshot_information{head_id, head_block_num, head_block_time, chain_snapshot_header::current_version, pending_path.generic_string(), captured_root_hash});
      }
      CATCH_AND_CALL(next);
   }
}

void snapshot_scheduler::notify_snapshot_finalized(const snapshot_information& si) {
   for(const auto& cb: _snapshot_finalized_cbs) {
      try {
         cb(si);
      } catch (const fc::exception& e) {
         elog("Snapshot finalized callback error: {}", e.to_detail_string());
      } catch (const std::exception& e) {
         elog("Snapshot finalized callback error: {}", e.what());
      }
   }
}

}// namespace sysio::chain

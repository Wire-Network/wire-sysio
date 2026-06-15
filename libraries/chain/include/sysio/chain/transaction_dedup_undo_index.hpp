#pragma once

#include <sysio/chain/transaction_dedup.hpp>

#include <chainbase/chainbase.hpp>

#include <string>
#include <utility>

namespace sysio::chain {

/// Adapts transaction_dedup to chainbase::abstract_index so the database drives its undo lifecycle
/// (add_undo_session / squash / undo / commit / set_revision) in lockstep with the segment indices.
/// Register it with database::add_undo_participant once at startup; thereafter every db.start_undo_session
/// / squash / undo / commit also drives the dedup, so the controller no longer hand-pairs its own
/// dedup undo calls with the database's -- removing the desync surface that caused fork-switch and
/// restart bugs.
///
/// The dedup keeps its rows on the heap, so the database only dispatches lifecycle EVENTS here; the
/// data path (record / is_known / clear_expired) never goes through chainbase, preserving the
/// dedup's O(1) performance and keeping its churn out of the shared segment.
class dedup_undo_index final : public chainbase::abstract_index {
public:
   /// Reserved type id, above any real chainbase object type, so the participant slots into the
   /// database's index map without colliding with a segment index.
   static constexpr uint16_t reserved_type_id = 0xFFFE;

   explicit dedup_undo_index(transaction_dedup& dedup)
   : chainbase::abstract_index(&dedup), _dedup(dedup) {}

   void     set_revision(uint64_t revision) override { _dedup.set_revision(revision); }
   void     add_undo_session() override              { _dedup.add_undo_session(); }
   int64_t  revision() const override                { return _dedup.revision(); }
   void     undo() const override                    { _dedup.undo(); }
   void     squash() const override                  { _dedup.squash(); }
   void     commit(int64_t revision) const override  { _dedup.commit(revision); }
   void     undo_all() const override                { _dedup.undo_all(); }
   uint32_t type_id() const override                 { return reserved_type_id; }
   uint64_t row_count() const override               { return _dedup.size(); }
   size_t   freelist_memory_usage() const override   { return 0; }   // heap-backed, no segment freelist
   const std::string& type_name() const override     { return _type_name; }
   std::pair<uint64_t, uint64_t> undo_stack_revision_range() const override {
      return _dedup.undo_stack_revision_range();
   }
   void     remove_object(int64_t) override          {}   // not used: the dedup has no segment objects

private:
   transaction_dedup& _dedup;
   const std::string  _type_name = "transaction_dedup";
};

} // namespace sysio::chain

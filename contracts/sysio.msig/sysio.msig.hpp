#pragma once

#include <sysio/binary_extension.hpp>
#include <sysio/sysio.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/ignore.hpp>
#include <sysio/transaction.hpp>

namespace sysio {
/**
 * The `sysio.msig` system contract allows for creation of proposed transactions which require authorization from a list of accounts, approval of the proposed transactions by those accounts required to approve it, and finally, it also allows the execution of the approved transactions on the blockchain.
 *
 * In short, the workflow to propose, review, approve and then executed a transaction it can be described by the following:
 * - first you create a transaction json file,
 * - then you submit this proposal to the `sysio.msig` contract, and you also insert the account permissions required to approve this proposal into the command that submits the proposal to the blockchain,
 * - the proposal then gets stored on the blockchain by the `sysio.msig` contract, and is accessible for review and approval to those accounts required to approve it,
 * - after each of the appointed accounts required to approve the proposed transactions reviews and approves it, you can execute the proposed transaction. The `sysio.msig` contract will execute it automatically, but not before validating that the transaction has not expired, it is not cancelled, and it has been signed by all the permissions in the initial proposal's required permission list.
 */
class [[sysio::contract("sysio.msig")]] multisig : public contract {
public:
   using contract::contract;

   /**
    * Propose action, creates a proposal containing one transaction.
    * Allows an account `proposer` to make a proposal `proposal_name` which has `requested`
    * permission levels expected to approve the proposal, and if approved by all expected
    * permission levels then `trx` transaction can we executed by this proposal.
    * The `proposer` account is authorized and the `trx` transaction is verified if it was
    * authorized by the provided keys and permissions, and if the proposal name doesn’t
    * already exist; if all validations pass the `proposal_name` and `trx` trasanction are
    * saved in the proposals table and the `requested` permission levels to the
    * approvals table (for the `proposer` context). Storage changes are billed to `proposer`.
    *
    * @param proposer - The account proposing a transaction
    * @param proposal_name - The name of the proposal (should be unique for proposer)
    * @param requested - Permission levels expected to approve the proposal
    * @param trx - Proposed transaction
    */
   [[sysio::action]]
   void propose(name proposer, name proposal_name,
                std::vector<permission_level> requested, ignore<transaction> trx);
   /**
    * Approve action approves an existing proposal. Allows an account, the owner of `level` permission, to approve a proposal `proposal_name`
    * proposed by `proposer`. If the proposal's requested approval list contains the `level`
    * permission then the `level` permission is moved from internal `requested_approvals` list to
    * internal `provided_approvals` list of the proposal, thus persisting the approval for
    * the `proposal_name` proposal. Storage changes are billed to `proposer`.
    *
    * @param proposer - The account proposing a transaction
    * @param proposal_name - The name of the proposal (should be unique for proposer)
    * @param level - Permission level approving the transaction
    * @param proposal_hash - Transaction's checksum
    */
   [[sysio::action]]
   void approve( name proposer, name proposal_name, permission_level level,
                 const sysio::binary_extension<sysio::checksum256>& proposal_hash );
   /**
    * Unapprove action revokes an existing proposal. This action is the reverse of the `approve` action: if all validations pass
    * the `level` permission is erased from internal `provided_approvals` and added to the internal
    * `requested_approvals` list, and thus un-approve or revoke the proposal.
    *
    * @param proposer - The account proposing a transaction
    * @param proposal_name - The name of the proposal (should be an existing proposal)
    * @param level - Permission level revoking approval for proposal
    */
   [[sysio::action]]
   void unapprove( name proposer, name proposal_name, permission_level level );
   /**
    * Cancel action cancels an existing proposal.
    *
    * @param proposer - The account proposing a transaction
    * @param proposal_name - The name of the proposal (should be an existing proposal)
    * @param canceler - The account cancelling the proposal (only the proposer can cancel an unexpired transaction, and the canceler has to be different than the proposer)
    *
    * Allows the `canceler` account to cancel the `proposal_name` proposal, created by a `proposer`,
    * only after time has expired on the proposed transaction. It removes corresponding entries from
    * internal proptable and from approval (or old approvals) tables as well.
    */
   [[sysio::action]]
   void cancel( name proposer, name proposal_name, name canceler );
   /**
    * Exec action allows an `executer` account to execute a proposal.
    *
    * Preconditions:
    * - `executer` has authorization,
    * - `proposal_name` is found in the proposals table,
    * - all requested approvals are received,
    * - proposed transaction is not expired,
    * - and approval accounts are not found in invalidations table.
    *
    * If all preconditions are met the transaction is executed as a deferred transaction,
    * and the proposal is erased from the proposals table.
    *
    * @param proposer - The account proposing a transaction
    * @param proposal_name - The name of the proposal (should be an existing proposal)
    * @param executer - The account executing the transaction
    */
   [[sysio::action]]
   void exec( name proposer, name proposal_name, name executer );
   /**
    * Invalidate action allows an `account` to invalidate itself, that is, its name is added to
    * the invalidations table and this table will be cross referenced when exec is performed.
    *
    * @param account - The account invalidating the transaction
    */
   [[sysio::action]]
   void invalidate( name account );

   using propose_action = sysio::action_wrapper<"propose"_n, &multisig::propose>;
   using approve_action = sysio::action_wrapper<"approve"_n, &multisig::approve>;
   using unapprove_action = sysio::action_wrapper<"unapprove"_n, &multisig::unapprove>;
   using cancel_action = sysio::action_wrapper<"cancel"_n, &multisig::cancel>;
   using exec_action = sysio::action_wrapper<"exec"_n, &multisig::exec>;
   using invalidate_action = sysio::action_wrapper<"invalidate"_n, &multisig::invalidate>;

   /// Maximum number of bytes of inner-trx data stored in a single `propchunks` row.
   /// Must leave headroom under `max_kv_value_size` (256 KiB default) for the row's other
   /// fields and the KV layer's per-row overhead. 200 KiB is a comfortable safe choice.
   static constexpr size_t proposal_chunk_size = 200 * 1024;

   struct proposal_key {
      uint64_t proposal_name;
      SYSLIB_SERIALIZE(proposal_key, (proposal_name))
   };

   /**
    * The `proposal` row.
    *
    * For small proposals (inner trx ≤ `proposal_chunk_size`) the full serialized inner trx
    * lives in `packed_transaction` and `chunk_count` is `0` (or absent for legacy rows). The
    * on-disk shape is identical to a pre-chunked-storage row plus the three appended
    * `binary_extension` fields, so external tooling that reads `packed_transaction` directly
    * via `get_table_rows` continues to work for the small case unchanged.
    *
    * For large proposals (inner trx > `proposal_chunk_size`) `packed_transaction` is empty
    * and the bytes are split across `chunk_count` rows of the `propchunks` table, keyed by
    * `(proposal_name, chunk_index)`. Tooling must call `getproposal` to retrieve the
    * assembled blob, or read and concatenate the chunk rows itself.
    *
    * `total_size` is the size in bytes of the assembled `packed_transaction` (used by
    * `getproposal` to pre-reserve the output buffer). `trx_hash` is `sha256(packed_transaction)`
    * computed once at propose time and stored so `approve --proposal-hash` does not need to
    * reassemble chunks on every call.
    */
   struct [[sysio::table("proposal"), sysio::contract("sysio.msig")]] proposal {
      name                                                            proposal_name;
      std::vector<char>                                               packed_transaction; // empty when chunked
      sysio::binary_extension< std::optional<time_point> >            earliest_exec_time;
      sysio::binary_extension< uint32_t >                             chunk_count;        // 0 / absent => not chunked
      sysio::binary_extension< uint32_t >                             total_size;         // assembled blob size in bytes
      sysio::binary_extension< sysio::checksum256 >                   trx_hash;           // sha256 of assembled blob

      SYSLIB_SERIALIZE(proposal, (proposal_name)(packed_transaction)(earliest_exec_time)
                                 (chunk_count)(total_size)(trx_hash))
   };
   using proposals = sysio::kv::scoped_table< "proposal"_n, proposal_key, proposal >;

   /// Composite primary key for `propchunks`: 8-byte proposal_name + 4-byte chunk_index.
   /// Scoped by proposer (same scope as the parent `proposal` row), so chunks for a given
   /// proposal sit contiguously in the KV order under their scope.
   struct propchunk_key {
      uint64_t proposal_name;
      uint32_t chunk_index;
      SYSLIB_SERIALIZE(propchunk_key, (proposal_name)(chunk_index))
   };

   /**
    * One chunk of a chunked proposal's serialized inner trx.
    *
    * Chunks are written by `propose` in increasing `chunk_index` order and read back in the
    * same order by `exec`/`get_proposal`. Each chunk's `data` is at most `proposal_chunk_size`
    * bytes; the last chunk may be smaller. The total assembled size is recorded on the parent
    * `proposal` row's `total_size` field so the reader can pre-size the output buffer and
    * verify the assembled length matches what was written.
    */
   struct [[sysio::table("propchunks"), sysio::contract("sysio.msig")]] propchunk {
      name              proposal_name;
      uint32_t          chunk_index;
      std::vector<char> data;

      SYSLIB_SERIALIZE(propchunk, (proposal_name)(chunk_index)(data))
   };
   using propchunks = sysio::kv::scoped_table< "propchunks"_n, propchunk_key, propchunk >;

   /**
    * Read-only `getproposal` action returns the assembled proposal for `(proposer, proposal_name)`.
    *
    * For non-chunked proposals this is a thin wrapper around the `proposal` row. For chunked
    * proposals — those whose serialized inner transaction exceeds `proposal_chunk_size` and is
    * stored split across the `propchunks` table — the action reassembles the full
    * `packed_transaction` blob in WASM linear memory and returns the complete struct so callers
    * never have to know whether the proposal was chunked.
    *
    * Intended to be invoked via `/v1/chain/send_read_only_transaction` so the action's return
    * value is not bounded by `max_action_return_value_size` (the chain skips that check in
    * read-only context). This is the storage-layout-agnostic way for tooling to read proposals.
    *
    * @param proposer - The proposing account that scopes the proposal table
    * @param proposal_name - The name of the proposal to fetch
    * @return The fully assembled proposal struct with `packed_transaction` populated.
    */
   [[sysio::action("getproposal"), sysio::read_only]]
   proposal get_proposal( name proposer, name proposal_name );

   using getproposal_action = sysio::action_wrapper<"getproposal"_n, &multisig::get_proposal>;

   struct old_approval_key {
      uint64_t proposal_name;
      SYSLIB_SERIALIZE(old_approval_key, (proposal_name))
   };

   struct [[sysio::table("approvals"), sysio::contract("sysio.msig")]] old_approvals_info {
      name                            proposal_name;
      std::vector<permission_level>   requested_approvals;
      std::vector<permission_level>   provided_approvals;

      SYSLIB_SERIALIZE(old_approvals_info, (proposal_name)(requested_approvals)(provided_approvals))
   };
   using old_approvals = sysio::kv::scoped_table< "approvals"_n, old_approval_key, old_approvals_info >;

   struct approval {
      permission_level level;
      time_point       time;
   };

   struct approval_key {
      uint64_t proposal_name;
      SYSLIB_SERIALIZE(approval_key, (proposal_name))
   };

   struct [[sysio::table("approvals2"), sysio::contract("sysio.msig")]] approvals_info {
      uint8_t                 version = 1;
      name                    proposal_name;
      //requested approval doesn't need to contain time, but we want requested approval
      //to be of exactly the same size as provided approval, in this case approve/unapprove
      //doesn't change serialized data size. So, we use the same type.
      std::vector<approval>   requested_approvals;
      std::vector<approval>   provided_approvals;

      SYSLIB_SERIALIZE(approvals_info, (version)(proposal_name)(requested_approvals)(provided_approvals))
   };
   using approvals = sysio::kv::scoped_table< "approvals2"_n, approval_key, approvals_info >;

   struct inval_key {
      uint64_t account;
      SYSLIB_SERIALIZE(inval_key, (account))
   };

   struct [[sysio::table("invals"), sysio::contract("sysio.msig")]] invalidation {
      name         account;
      time_point   last_invalidation_time;

      SYSLIB_SERIALIZE(invalidation, (account)(last_invalidation_time))
   };

   using invalidations = sysio::kv::table< "invals"_n, inval_key, invalidation >;
};
} /// namespace sysio

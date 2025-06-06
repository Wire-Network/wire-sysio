#pragma once

#include <sysio/binary_extension.hpp>
#include <sysio/sysio.hpp>
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

   struct [[sysio::table, sysio::contract("sysio.msig")]] proposal {
      name                                                            proposal_name;
      std::vector<char>                                               packed_transaction;
      sysio::binary_extension< std::optional<time_point> >            earliest_exec_time;

      uint64_t primary_key()const { return proposal_name.value; }
   };
   typedef sysio::multi_index< "proposal"_n, proposal > proposals;

   struct [[sysio::table, sysio::contract("sysio.msig")]] old_approvals_info {
      name                            proposal_name;
      std::vector<permission_level>   requested_approvals;
      std::vector<permission_level>   provided_approvals;
      uint64_t primary_key()const { return proposal_name.value; }
   };
   typedef sysio::multi_index< "approvals"_n, old_approvals_info > old_approvals;
   struct approval {
      permission_level level;
      time_point       time;
   };

   struct [[sysio::table, sysio::contract("sysio.msig")]] approvals_info {
      uint8_t                 version = 1;
      name                    proposal_name;
      //requested approval doesn't need to contain time, but we want requested approval
      //to be of exactly the same size as provided approval, in this case approve/unapprove
      //doesn't change serialized data size. So, we use the same type.
      std::vector<approval>   requested_approvals;
      std::vector<approval>   provided_approvals;
      uint64_t primary_key()const { return proposal_name.value; }
   };
   typedef sysio::multi_index< "approvals2"_n, approvals_info > approvals;

   struct [[sysio::table, sysio::contract("sysio.msig")]] invalidation {
      name         account;
      time_point   last_invalidation_time;

      uint64_t primary_key() const { return account.value; }
   };

   typedef sysio::multi_index< "invals"_n, invalidation > invalidations;
};
} /// namespace sysio

#!/usr/bin/env python3

###############################################################
# multisig_review_test
#
# Integration test for `clio multisig review` against the chunked-storage
# sysio.msig contract. The contract splits proposals whose serialized inner
# transaction exceeds ~200 KiB across multiple rows of a `propchunks` table,
# stores an empty `packed_transaction` on the parent `proposal` row, and
# exposes a read-only `getproposal` action that reassembles the blob and
# returns it via /v1/chain/send_read_only_transaction.
#
# `clio multisig review` no longer reads the `proposal` row directly via
# /v1/chain/get_table_rows — it now always invokes `getproposal` over the
# read-only RPC. This test exercises the full end-to-end path:
#
#   1. Stand up a single-node cluster with the new sysio.msig wasm.
#   2. Create alice and bob accounts.
#   3. Build a JSON inner transaction containing two `setcode` actions, each
#      carrying the full sysio.system wasm (~134 KiB). Total ≈ 270 KiB,
#      which forces the contract to chunk the proposal across two rows.
#   4. Submit it via `clio multisig propose_trx`.
#   5. Run `clio multisig review` and verify the JSON output reflects the
#      proposal accurately — same `proposer`/`proposal_name`, two `setcode`
#      actions, and metadata fields (`chunk_count`, `total_size`, `trx_hash`)
#      that prove the proposal actually went through the chunked path.
#   6. Run `clio multisig review --show-approvals` against an unapproved
#      proposal and verify the approvals section parses cleanly.
#
# If clio's review path were still using get_table_rows, step 5 would return
# an empty `packed_transaction` (chunked proposals' parent row carries no
# inline blob) and `unpack<transaction>` would either error or return an
# empty actions list — both of which the assertions below would catch.
###############################################################

import copy
import json
import os
import shlex
import tempfile
from pathlib import Path

from TestHarness import Account, Cluster, ReturnType, TestHelper, Utils, WalletMgr

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"-p","-n","-d","-s","--nodes-file","--seed"
                            ,"--dump-error-details","-v","--leave-running"
                            ,"--keep-logs","--unshared"})

pnodes=args.p
topo=args.s
delay=args.d
# Force at least one non-producer node alongside the producer(s) so we have an
# API node to enable `--read-only-threads` on (the producer node rejects that
# flag with `read-only-threads not allowed on producer node`).
total_nodes = max(pnodes + 1, args.n)
debug=args.v
nodesFile=args.nodes_file
dontLaunch=nodesFile is not None
seed=args.seed
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)

walletMgr=WalletMgr(True)
SYSIO_ACCT_PRIVATE_DEFAULT_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
SYSIO_ACCT_PUBLIC_DEFAULT_KEY  = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

try:
    Print("Stand up cluster (producer + non-producer API node)")
    # The producer node rejects `--read-only-threads`, so we have to put the
    # read-only-trx execution on the non-producer API node (index `pnodes` —
    # the first node beyond the producers). The whole point of this test is
    # that `multisig review` goes through the read-only-trx code path, so we
    # route the review call to the API node further down.
    specificExtraNodeopArgs = {}
    specificExtraNodeopArgs[pnodes]  = " --read-only-write-window-time-us 10000 "
    specificExtraNodeopArgs[pnodes] += " --read-only-read-window-time-us 510000 "
    specificExtraNodeopArgs[pnodes] += " --read-only-threads 1 "
    extraNodeopArgs = " --http-max-response-time-ms 990000 "
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, activateIF=True,
                      topo=topo, delay=delay,
                      specificExtraNodeopArgs=specificExtraNodeopArgs,
                      extraNodeopArgs=extraNodeopArgs) is False:
        errorExit("Failed to stand up sys cluster.")

    Print("Wait for cluster stabilization")
    if not cluster.waitOnClusterBlockNumSync(3):
        errorExit("Cluster never stabilized")

    biosNode = cluster.biosNode
    # Non-producer API node where read-only-trx execution is enabled.
    apiNode  = cluster.getNode(pnodes)

    # ----- Create the two msig signers -----
    alice = Account('alice')
    alice.ownerPublicKey  = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    alice.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(alice, cluster.sysioAccount, stakedDeposit=1000)

    bob = Account('bob')
    bob.ownerPublicKey  = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    bob.activePublicKey = SYSIO_ACCT_PUBLIC_DEFAULT_KEY
    cluster.createAccountAndVerify(bob, cluster.sysioAccount, stakedDeposit=1000)

    # ----- Deploy the freshly built sysio.msig wasm to the sysio.msig account.
    # Cluster.launch() creates the account but does not deploy the contract.
    msigAccount = copy.deepcopy(cluster.sysioAccount)
    msigAccount.name = 'sysio.msig'
    msigContractDir = str(cluster.contractsPath / 'sysio.msig')
    Print(f"Publish sysio.msig from {msigContractDir}")
    trans = biosNode.publishContract(msigAccount, msigContractDir,
                                     'sysio.msig.wasm', 'sysio.msig.abi',
                                     waitForTransBlock=True)
    if trans is None:
        errorExit("Failed to publish sysio.msig contract.")

    # ----- Build a chunked-size inner transaction.
    # Two setcode actions stacked together, each carrying the full sysio.system
    # wasm (~134 KiB), gives a serialized inner trx of ~270 KiB. The contract's
    # chunk threshold is 200 KiB, so this is guaranteed to take the chunked
    # storage path.
    #
    # `multisig propose_trx` now accepts the natural JSON shape (structured
    # `action.data` objects, recursively encoded against each contract's ABI)
    # in addition to the legacy pre-hex form — the same fallback that
    # `clio push transaction` uses. So we just hand it the trx as JSON.
    system_wasm_path = cluster.contractsPath / 'sysio.system' / 'sysio.system.wasm'
    if not system_wasm_path.exists():
        errorExit(f"sysio.system.wasm not found at {system_wasm_path}; build BUILD_SYSTEM_CONTRACTS=ON")

    with open(system_wasm_path, 'rb') as f:
        system_wasm_hex = f.read().hex()
    Print(f"system wasm size = {len(system_wasm_hex) // 2} bytes")

    test_workdir = Path(tempfile.mkdtemp(prefix='msig_review_test_'))

    inner_trx = {
        # Far-future expiration so the proposal never expires during the test run.
        "expiration": "2099-01-01T00:00:00",
        "ref_block_num": 0,
        "ref_block_prefix": 0,
        "max_net_usage_words": 0,
        "max_cpu_usage_ms": 0,
        "delay_sec": 0,
        "context_free_actions": [],
        "actions": [
            {
                "account": "sysio",
                "name": "setcode",
                "authorization": [{"actor": "alice", "permission": "active"}],
                "data": {
                    "account":   "alice",
                    "vmtype":    0,
                    "vmversion": 0,
                    "code":      system_wasm_hex,
                },
            },
            {
                "account": "sysio",
                "name": "setcode",
                "authorization": [{"actor": "bob", "permission": "active"}],
                "data": {
                    "account":   "bob",
                    "vmtype":    0,
                    "vmversion": 0,
                    "code":      system_wasm_hex,
                },
            },
        ],
        "transaction_extensions": [],
    }

    requested_perms = [
        {"actor": "alice", "permission": "active"},
        {"actor": "bob",   "permission": "active"},
    ]

    trx_path  = test_workdir / 'inner_trx.json'
    perm_path = test_workdir / 'requested_perms.json'
    with open(trx_path, 'w') as f:
        json.dump(inner_trx, f)
    with open(perm_path, 'w') as f:
        json.dump(requested_perms, f)

    Print(f"Inner trx JSON written to {trx_path} (size={trx_path.stat().st_size} bytes)")

    # ----- Propose the chunked transaction via clio.
    # `-p alice@sysio.payer -p alice@active` — Wire's ROA layer requires the
    # virtual `sysio.payer` permission whenever a contract bills RAM to the
    # signer, and propose stores the proposal row + chunk rows on the
    # proposer's quota. The chain also enforces that the explicit payer
    # permission must be the FIRST declared authorization on the action, so
    # `sysio.payer` has to precede `active` in the -p list.
    Print("Submit `multisig propose_trx` against the API node")
    propose_cmd = (
        f"multisig propose_trx -j chunkprop "
        f"{shlex.quote(str(perm_path))} "
        f"{shlex.quote(str(trx_path))} "
        f"alice -p alice@sysio.payer -p alice@active"
    )
    # Submit through the API node so that once we wait for the proposal
    # transaction to land in a block, the proposal row is in the API node's
    # state when we issue the read-only review call below. `-j` forces clio
    # to emit a single JSON document so the test harness can parse the result.
    propose_result = apiNode.processClioCmd(propose_cmd, "propose chunked",
                                            silentErrors=False, exitOnError=True)
    assert propose_result is not None, "multisig propose_trx returned None"

    # Wait for the propose transaction to be included in a block on the API
    # node before we issue the read-only review — otherwise getproposal would
    # race the propose's state mutation and assert "proposal not found".
    propose_trans_id = propose_result["transaction_id"]
    assert apiNode.waitForTransactionInBlock(propose_trans_id), \
        f"propose_trx {propose_trans_id} did not land in a block"

    # ----- Review it back via clio. This is the path that switched to using
    # `getproposal` over send_read_only_transaction. Run it against the
    # API node — the producer node would refuse the read-only trx because
    # `--read-only-threads` is only enabled on the API node.
    Print("Run `multisig review` against the API node (read-only-trx path)")
    review_result = apiNode.processClioCmd("multisig review alice chunkprop",
                                           "multisig review",
                                           silentErrors=False, exitOnError=True)
    assert review_result is not None, "multisig review returned None"

    # ---------- Output assertions ----------
    Print("Validate review JSON output")

    assert review_result["proposer"]      == "alice",     f"unexpected proposer: {review_result['proposer']}"
    assert review_result["proposal_name"] == "chunkprop", f"unexpected proposal_name: {review_result['proposal_name']}"
    assert "transaction_id" in review_result,             "transaction_id missing"
    assert "transaction"    in review_result,             "transaction missing"

    # Inner trx must round-trip both setcode actions in order.
    actions = review_result["transaction"]["actions"]
    assert len(actions) == 2, f"expected 2 actions, got {len(actions)}"
    for i, expected_target in enumerate(("alice", "bob")):
        a = actions[i]
        assert a["account"] == "sysio",   f"action[{i}] account is {a['account']}"
        assert a["name"]    == "setcode", f"action[{i}] name is {a['name']}"
        assert a["data"]["account"] == expected_target, \
            f"action[{i}] target is {a['data']['account']}, expected {expected_target}"

    # Chunked-storage metadata: chunk_count > 0 proves the contract took the
    # chunked path, total_size matches the expected packed inner trx, and
    # trx_hash is populated. If clio were silently using a stale get_table_rows
    # path, packed_transaction would be empty and any of these would catch it.
    assert "chunk_count" in review_result, "review missing chunk_count metadata"
    chunk_count = int(review_result["chunk_count"])
    assert chunk_count > 0, f"expected chunk_count > 0, got {chunk_count}"

    assert "total_size" in review_result, "review missing total_size metadata"
    total_size = int(review_result["total_size"])
    assert total_size > 200 * 1024, \
        f"expected total_size > 200 KiB, got {total_size}"

    assert "trx_hash" in review_result and len(review_result["trx_hash"]) == 64, \
        f"trx_hash missing or wrong length: {review_result.get('trx_hash')}"

    # The chunked proposal's `packed_transaction` field on the parent row is
    # empty by design — the bytes live in propchunks. clio's review used to
    # render that field directly. After this PR it must NOT do that any more,
    # because getproposal returns the assembled blob with packed_transaction
    # populated. We confirm by checking that the field is non-empty in the
    # review output (i.e. clio is not echoing the empty inline field).
    assert "packed_transaction" in review_result, "packed_transaction missing in review"
    assert len(review_result["packed_transaction"]) > 200 * 1024 * 2, \
        ("packed_transaction in review is empty or too small — clio may not be "
         "going through the read-only getproposal path")

    # ----- Same call with --show-approvals exercises the approvals branches.
    Print("Run `multisig review --show-approvals` against the API node")
    review_with_approvals = apiNode.processClioCmd(
        "multisig review --show-approvals alice chunkprop",
        "multisig review --show-approvals",
        silentErrors=False, exitOnError=True)
    assert review_with_approvals is not None, "multisig review --show-approvals returned None"

    assert "approvals" in review_with_approvals, "approvals section missing"
    approvals = review_with_approvals["approvals"]
    assert len(approvals) == 2, f"expected 2 approval entries, got {len(approvals)}"
    for ap in approvals:
        # No approve actions submitted yet — both must be unapproved.
        assert ap["status"] == "unapproved", f"unexpected approval status: {ap}"
        assert ap["level"]["permission"] == "active"
        assert ap["level"]["actor"] in ("alice", "bob")

    testSuccessful = True

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

errorCode = 0 if testSuccessful else 1
exit(errorCode)

#!/usr/bin/env python3
"""account_watch_plugin on a relay: one JSON record per line in its own file, through a "%v" pattern sink.

A producer and a relay; only the relay runs the plugin, watching alice. alice transfers to bob, bob transfers to alice,
and the relay's file must hold a start record, heartbeats, exactly one match for alice's transfer (none for bob's,
which only notifies alice), and a stop record once the relay shuts down.
"""

import json
import os
import signal
import time
from pathlib import Path

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys

Print = Utils.Print
errorExit = Utils.errorExit

args = TestHelper.parse_args({"-v", "--dump-error-details", "--leave-running", "--keep-logs", "--unshared"})
Utils.Debug = args.v

RELAY = 1
WATCHED = "alice"
OTHER = "bob"
LOGGER = "account_watch"
RECORD_WAIT_SECONDS = 30

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True)
cluster.setWalletMgr(walletMgr)
testSuccessful = False


def write_logging_config(directory: Path, records_file: Path) -> Path:
    """The harness's logging template plus an account_watch logger that writes bare messages to records_file."""
    with open(Path(__file__).resolve().parent / "TestHarness" / "logging-template.json") as template:
        config = json.load(template)
    config["sinks"].append({
        "name": "account_watch_file",
        "type": "rotating_file_sink",
        "args": {"base_filename": str(records_file), "max_size": 10, "max_files": 1},
        "format": {"type": "pattern", "args": {"pattern": "%v"}},
    })
    config["loggers"].append({"name": LOGGER, "level": "info", "enabled": True, "sinks": ["account_watch_file"]})
    path = directory / "account-watch-logging.json"
    with open(path, "w") as out:
        json.dump(config, out, indent=2)
    return path


def read_records(records_file: Path) -> list:
    """Every line of records_file, each of which must be one JSON object with an event."""
    if not records_file.exists():
        return []
    records = []
    with open(records_file) as f:
        for line in f:
            record = json.loads(line)
            assert "event" in record, f"record without an event: {line}"
            records.append(record)
    return records


def wait_for(records_file: Path, predicate, what: str) -> list:
    """The records once predicate holds for them; fails after RECORD_WAIT_SECONDS."""
    deadline = time.time() + RECORD_WAIT_SECONDS
    while time.time() < deadline:
        records = read_records(records_file)
        if predicate(records):
            return records
        time.sleep(0.5)
    errorExit(f"timed out waiting for {what}; records: {read_records(records_file)}")


try:
    TestHelper.printSystemInfo("BEGIN")
    work_dir = Path(Utils.DataPath)
    work_dir.mkdir(parents=True, exist_ok=True)
    records_file = work_dir / "account_watch.jsonl"
    logconf = write_logging_config(work_dir, records_file)

    relay_args = (f"--plugin sysio::account_watch_plugin --account-watch-include {WATCHED},*::* "
                  f"--account-watch-heartbeat-seconds 1 --logconf {logconf}")
    if cluster.launch(totalNodes=2, pnodes=1, activateIF=True, specificExtraNodeopArgs={RELAY: relay_args}) is False:
        errorExit("Failed to stand up cluster.")

    walletMgr.launch()
    wallet = walletMgr.create("testwallet", [cluster.sysioAccount, cluster.defproduceraAccount])
    accounts = createAccountKeys(2)
    accounts[0].name = WATCHED
    accounts[1].name = OTHER
    producer = cluster.getNode(0)
    relay = cluster.getNode(RELAY)
    for account in accounts:
        walletMgr.importKey(account, wallet)
        relay.createInitializeAccount(account, cluster.sysioAccount, buyRAM=1000000, stakedDeposit=5000000,
                                         waitForTransBlock=True, exitOnError=True)
    alice, bob = accounts

    records = wait_for(records_file, lambda rs: any(r["event"] == "start" for r in rs), "the start record")
    start = next(r for r in records if r["event"] == "start")
    assert start["include"] == [f"{WATCHED}@*,*::*"], start
    assert start["exclude"] == [], start
    assert start["heartbeat_seconds"] == 1, start

    amount = Node.currencyIntToStr(1000, CORE_SYMBOL)
    notified = producer.transferFunds(bob, alice, amount, "bob to alice")
    notified_id = Node.getTransId(notified)
    notified_block = Node.getTransBlockNum(notified)
    sent = producer.transferFunds(alice, bob, amount, "alice to bob")
    sent_id = Node.getTransId(sent)
    assert relay.waitForBlock(Node.getTransBlockNum(sent)), "relay did not reach alice's transfer"

    records = wait_for(records_file, lambda rs: any(r.get("trx_id") == sent_id for r in rs), "alice's transfer")
    matches = [r for r in records if r["event"] == "match" and r["trx_id"] == sent_id]
    assert len(matches) == 1, matches
    match = matches[0]
    # The block the record names holds the transaction.
    block = relay.getBlock(match["block_num"])
    assert any(t["trx"]["id"] == sent_id for t in block["transactions"]), block
    assert match["block_id"] == block["id"], (match, block["id"])
    assert match["contract"] == "sysio.token", match
    assert match["action"] == "transfer", match
    assert match["matched"] == [{"actor": WATCHED, "permission": "active"}], match
    assert match["data"]["quantity"] == amount, match
    assert match["data"]["to"] == OTHER, match
    assert isinstance(match["global_sequence"], str), match
    assert match["lib_num"] <= match["block_num"], match
    # bob's transfer only notified alice: never reported, checked once the relay is well past both transfers.
    later = max(notified_block, match["block_num"]) + 4
    records = wait_for(records_file,
                       lambda rs: any(r["event"] == "heartbeat" and r["block_num"] >= later for r in rs),
                       f"a heartbeat past block {later}")
    assert not [r for r in records if r.get("trx_id") == notified_id], records
    # Every match was signed by alice.
    for record in records:
        if record["event"] == "match":
            assert {"actor": WATCHED, "permission": "active"} in record["matched"], record

    heartbeats = [r for r in records if r["event"] == "heartbeat"]
    assert heartbeats, records
    assert all(h["lib_num"] <= h["block_num"] for h in heartbeats), heartbeats

    relay.kill(signal.SIGTERM)
    assert not relay.verifyAlive(), "relay did not shut down"
    records = read_records(records_file)
    assert records[-1]["event"] == "stop", records[-1]
    assert records[-1]["matches"] == len([r for r in records if r["event"] == "match"]), records[-1]

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exitCode = 0 if testSuccessful else 1
exit(exitCode)

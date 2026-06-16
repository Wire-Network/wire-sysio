#!/usr/bin/env python3
"""Unit tests for PerformanceTestBasic.queryBlockTrxData post-test extraction.

queryBlockTrxData pulls per-transaction resource data from trace_api/get_actions and
block metadata from chain/get_block_info. trace_api/get_actions filters by a single
action name server-side, so the extraction must not assume that the *first* configured
action appears in every generated transaction -- a user-trx config may list several
distinct action names. These tests drive queryBlockTrxData against a fake node that
emulates the real endpoints (including server-side action-name filtering) so they run in
milliseconds with no cluster. The multi-action case (test_multi_action_captures_all_actions)
is a regression test for the bug where filtering on cfgActions[0] silently dropped every
transaction built from the other configured actions.
"""

import sys
import tempfile
from pathlib import Path

# Make TestHarness / PerformanceHarness importable when run from the (build) tests dir,
# matching performance_test_basic.py's own sys.path setup.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PerformanceHarness.performance_test_basic import PerformanceTestBasic
from PerformanceHarness.log_reader import chainData

# A timestamp in the shape chain/get_block_info emits (fc to_iso_string(), no 'Z'); the
# blockData.timestamp setter parses it with strptime("%Y-%m-%dT%H:%M:%S.%f").
BLOCK_TIME = "2026-01-01T00:00:00.500"


class FakeNode:
    """Minimal stand-in for a TestHarness Node that serves the three endpoints
    queryBlockTrxData consumes. get_actions emulates the real plugin's single
    action-name server-side filter so tests exercise the same filtering the
    production endpoint performs."""

    def __init__(self, blocks):
        # blocks: {block_num: {"info": {...}, "actions": [action, ...], "status": str}}
        self.blocks = blocks
        self.calls = []  # (resource, command, payload) for assertions

    def processUrllibRequest(self, resource, command, payload=None, **kwargs):
        payload = dict(payload or {})
        self.calls.append((resource, command, payload))
        if resource == "chain" and command == "get_block_info":
            return {"payload": self.blocks[payload["block_num"]]["info"]}
        if resource == "trace_api" and command == "get_actions":
            actions = self.blocks[payload["block_num_start"]]["actions"]
            # Real endpoint filters by exactly one action name when "action" is supplied.
            if "action" in payload:
                actions = [a for a in actions if a.get("name") == payload["action"]]
            return {"payload": {"actions": actions}}
        if resource == "trace_api" and command == "get_block":
            return {"payload": {"status": self.blocks[payload["block_num"]]["status"]}}
        raise AssertionError(f"unexpected request: {resource}/{command} {payload}")

    def getActionsCall(self, block_num):
        """Return the recorded get_actions payload for the given block (or None)."""
        for resource, command, payload in self.calls:
            if resource == "trace_api" and command == "get_actions" and payload.get("block_num_start") == block_num:
                return payload
        return None

    def calledGetBlock(self, block_num):
        """True iff trace_api/get_block was requested for the given block."""
        return any(r == "trace_api" and c == "get_block" and p.get("block_num") == block_num
                   for r, c, p in self.calls)


def _action(trx_id, block_num, name, account="sysio.token", cpu=10, net=2, status="pending"):
    """Build a get_actions action variant with the fields queryBlockTrxData reads."""
    return {"trx_id": trx_id, "block_num": block_num, "block_time": BLOCK_TIME,
            "name": name, "account": account, "block_status": status,
            "trx_cpu_usage_us": cpu, "trx_net_usage_words": net}


def _block_info(block_num, producer="defproducera"):
    """Build a chain/get_block_info payload."""
    return {"id": f"{block_num:064x}", "block_num": block_num, "producer": producer,
            "timestamp": BLOCK_TIME}


def _make_ptb(user_trx_data_dict=None):
    """Construct a PerformanceTestBasic without its heavy __init__, wired with just the
    state queryBlockTrxData touches."""
    ptb = PerformanceTestBasic.__new__(PerformanceTestBasic)
    ptb.data = chainData()
    if user_trx_data_dict is not None:
        ptb.userTrxDataDict = user_trx_data_dict
    return ptb


def _action_cfg(*names):
    """A userTrxDataDict whose actions carry the given action names."""
    return {"actions": [{"actionName": n, "actionAuthAcct": "testacct1"} for n in names]}


def _run(ptb, node, start, end):
    """Invoke queryBlockTrxData over [start, end] using throwaway artifact files."""
    with tempfile.TemporaryDirectory() as d:
        block_data_path = Path(d) / "blockData.txt"
        block_trx_data_path = Path(d) / "blockTrxData.txt"
        ptb.queryBlockTrxData(node, block_data_path, block_trx_data_path, start, end)


def test_multi_action_captures_all_actions():
    """Regression: with multiple distinct configured action names the server-side filter
    must be dropped so transactions built from any configured action are captured -- not
    just those carrying the first action. Under the old cfgActions[0] logic the newaccount
    transaction (N1) is filtered out server-side and silently lost."""
    node = FakeNode({10: {"info": _block_info(10), "status": "pending", "actions": [
        _action("ob", 10, "onblock", account="sysio"),
        _action("T1", 10, "transfer"),
        _action("N1", 10, "newaccount", account="sysio"),
    ]}})
    ptb = _make_ptb(_action_cfg("transfer", "newaccount"))
    _run(ptb, node, 10, 10)

    assert node.getActionsCall(10) is not None, "get_actions must be queried for the block"
    assert "action" not in node.getActionsCall(10), "multi-action config must NOT push a server-side filter"
    assert set(ptb.data.trxDict.keys()) == {"T1", "N1"}, \
        f"both action types must be captured, got {sorted(ptb.data.trxDict.keys())}"
    assert "ob" not in ptb.data.trxDict, "onblock must be skipped"
    assert ptb.data.blockDict["10"].transactions == 2
    assert ptb.data.blockDict["10"].status == "pending"


def test_single_action_pushes_server_filter():
    """A single-action config keeps the server-side filter fast path: the action name is
    pushed to get_actions and only matching trxs come back."""
    node = FakeNode({10: {"info": _block_info(10), "status": "pending", "actions": [
        _action("ob", 10, "onblock", account="sysio"),
        _action("T1", 10, "transfer"),
        _action("X1", 10, "newaccount", account="sysio"),  # server filter must exclude this
    ]}})
    ptb = _make_ptb(_action_cfg("transfer"))
    _run(ptb, node, 10, 10)

    assert node.getActionsCall(10).get("action") == "transfer", "single-action config must push the filter down"
    assert set(ptb.data.trxDict.keys()) == {"T1"}
    assert ptb.data.blockDict["10"].transactions == 1


def test_repeated_action_name_pushes_server_filter():
    """Several action entries that share one name are still a single-action filter."""
    node = FakeNode({10: {"info": _block_info(10), "status": "pending", "actions": [
        _action("T1", 10, "transfer"),
    ]}})
    ptb = _make_ptb(_action_cfg("transfer", "transfer"))
    _run(ptb, node, 10, 10)

    assert node.getActionsCall(10).get("action") == "transfer"
    assert set(ptb.data.trxDict.keys()) == {"T1"}


def test_multi_action_trx_deduped_once():
    """A single transaction carrying several matched actions is recorded exactly once,
    with parent-transaction cpu/net totals (not double-counted per action)."""
    node = FakeNode({10: {"info": _block_info(10), "status": "pending", "actions": [
        _action("ob", 10, "onblock", account="sysio"),
        _action("T1", 10, "transfer", cpu=42, net=7),
        _action("T1", 10, "newaccount", account="sysio", cpu=42, net=7),  # same trx, 2nd action
    ]}})
    ptb = _make_ptb(_action_cfg("transfer", "newaccount"))
    _run(ptb, node, 10, 10)

    assert set(ptb.data.trxDict.keys()) == {"T1"}
    assert ptb.data.blockDict["10"].transactions == 1
    assert ptb.data.blockDict["10"].cpu == 42, "cpu must be the per-trx total counted once"
    assert ptb.data.blockDict["10"].net == 7, "net must be the per-trx total counted once"


def test_no_user_config_skips_onblock_only():
    """Default path (no userTrxDataFile): no server-side filter, only onblock is skipped,
    every other transaction is captured."""
    node = FakeNode({10: {"info": _block_info(10), "status": "pending", "actions": [
        _action("ob", 10, "onblock", account="sysio"),
        _action("T1", 10, "transfer"),
        _action("T2", 10, "transfer"),
    ]}})
    ptb = _make_ptb()  # userTrxDataDict unset
    _run(ptb, node, 10, 10)

    assert node.getActionsCall(10) is not None
    assert "action" not in node.getActionsCall(10), "no config must not push a server-side filter"
    assert set(ptb.data.trxDict.keys()) == {"T1", "T2"}
    assert ptb.data.blockDict["10"].transactions == 2


def test_status_falls_back_to_get_block_for_onblock_only_block():
    """When the action filter drops every entry in a block (ramp window), block finality
    still comes from trace_api -- via a get_block fallback for that one block."""
    node = FakeNode({10: {"info": _block_info(10), "status": "irreversible", "actions": [
        _action("ob", 10, "onblock", account="sysio"),  # filtered out server-side
    ]}})
    ptb = _make_ptb(_action_cfg("transfer"))
    _run(ptb, node, 10, 10)

    assert ptb.data.blockDict["10"].transactions == 0
    assert node.calledGetBlock(10), "must fall back to trace_api/get_block for status"
    assert ptb.data.blockDict["10"].status == "irreversible"


def main():
    """Run every test_* in this module and report a single pass/fail summary."""
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failures = 0
    for t in tests:
        try:
            t()
            print(f"PASS  {t.__name__}")
        except Exception as e:  # noqa: BLE001 - surface any failure with context
            failures += 1
            print(f"FAIL  {t.__name__}: {type(e).__name__}: {e}")
    print(f"\nqueryBlockTrxData extraction tests: {len(tests) - failures}/{len(tests)} passed")
    if failures:
        print("*** failures detected")
        return 1
    print("*** no errors detected")
    return 0


if __name__ == "__main__":
    sys.exit(main())

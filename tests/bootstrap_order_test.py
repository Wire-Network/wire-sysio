#!/usr/bin/env python3
"""Verify bootstrap completes its writes before giving block production to peers."""

import unittest
from types import SimpleNamespace
from unittest.mock import patch

from TestHarness import Cluster, Utils
from TestHarness.Cluster import PFSetupPolicy
from TestHarness.core_symbol import CORE_SYMBOL


class BootstrapWallet:
    """Provide the wallet operations needed by bootstrap without starting processes."""

    def launch(self):
        """Report an available wallet service."""
        return True

    def create(self, name):
        """Return a wallet identifier for the requested bootstrap wallet."""
        return name

    def importKey(self, account, wallet):
        """Accept the fixture's account keys."""
        return True


class BootstrapNode:
    """Model a producer that cannot relay writes after its schedule is replaced."""

    def __init__(self, name, events, fail_on=None):
        """Create a node with an optional failed mutation and shared event journal."""
        self.producerName = name
        self.nodeId = "bios" if name == "sysio" else 0
        self.port = 0
        self.isProducer = True
        self.killed = False
        self.keys = [SimpleNamespace(blspubkey="fixture-bls-key", blspop="fixture-pop")]
        self.events = events
        self.fail_on = fail_on
        self.handed_over = False
        self.transaction_count = 0

    def mutate(self, event):
        """Execute locally, rejecting all writes once the node must relay them."""
        if self.handed_over:
            raise AssertionError(f"Bootstrap submitted {event} after production handover")
        self.events.append(event)
        if event == self.fail_on:
            return None
        self.transaction_count += 1
        return {
            "transaction_id": str(self.transaction_count),
            "processed": {"except": None, "receipt": {}},
        }

    def publishContract(self, account, contract_dir, wasm_file, abi_file, **kwargs):
        """Record deployment of a contract to its account."""
        return self.mutate(("publish", account.name, wasm_file))

    def activateAllBuiltinProtocolFeature(self):
        """Record protocol-feature activation before bootstrap's contract writes."""
        self.mutate(("activate-features",))

    def createAccount(self, account, *args, **kwargs):
        """Create an account on the local producer."""
        return self.mutate(("create-account", account.name))

    def validateAccounts(self, accounts):
        """Accept account validation after local creation."""

    def setPriv(self, account, *args, **kwargs):
        """Record the privilege grant that originally failed after handover."""
        return self.mutate(("setpriv", account.name))

    def pushMessage(self, account, action, *args, **kwargs):
        """Apply an action and make schedule proposal the final permitted write."""
        transaction = self.mutate(("push", account, action))
        if transaction is not None and action in ("setprodkeys", "setprods"):
            self.handed_over = True
        return transaction is not None, transaction

    def waitForTransactionInBlock(self, transaction_id, *args, **kwargs):
        """Record inclusion checks for locally executed transactions."""
        self.events.append(("wait-transaction", transaction_id))
        return True

    def waitForTransactionsInBlock(self, transaction_ids):
        """Report inclusion of the account-creation batch."""
        return True

    def getHeadBlockNum(self):
        """Return a shared head for the producer synchronization barrier."""
        return 100

    def waitForBlock(self, block_number, **kwargs):
        """Record that the future producer caught up before schedule proposal."""
        self.events.append(("producer-caught-up", block_number))
        return True

    def getInfo(self, **kwargs):
        """Expose handover only after the schedule action has been submitted."""
        self.events.append(("get-info", self.nodeId, self.handed_over))
        return {
            "head_block_num": self.getHeadBlockNum(),
            "head_block_producer": "defproducera" if self.handed_over else self.producerName,
        }

    def waitForTransFinalization(self, transaction_id, **kwargs):
        """Record the instant-finality activation barrier."""
        self.events.append(("wait-finality", transaction_id))
        return True

    def waitForLibToAdvance(self, **kwargs):
        """Report finality progressing while BIOS is still producing."""
        self.events.append(("wait-lib",))
        return True

    def getAccountSysBalanceStr(self, account):
        """Return the issued bootstrap supply."""
        return f"1000000000.0000 {CORE_SYMBOL}"


class BootstrapOrderTest(unittest.TestCase):
    """Exercise the actual bootstrap orchestration without node binaries or sockets."""

    def run_bootstrap(self, *, load_system=True, instant_finality=False,
                      only_bios=False, only_set_producers=False, fail_on=None):
        """Run bootstrap with only a BIOS and producer, deliberately omitting API nodes."""
        events = []
        bios = BootstrapNode("sysio", events, fail_on=fail_on)
        producer = BootstrapNode("defproducera", events)
        cluster = Cluster()
        cluster.biosNode = bios
        cluster.nodes = [producer]
        cluster.walletMgr = BootstrapWallet()
        keys = {
            name: {"name": name, "private": "fixture-private", "public": "fixture-public", "node": node}
            for name, node in (("sysio", "bios"), ("defproducera", 0))
        }
        with patch.object(Cluster, "parseClusterKeys", return_value=keys), \
                patch.object(Utils, "Print"), \
                patch("TestHarness.Cluster.time.sleep", return_value=None):
            result = cluster.bootstrap(
                launcher=None, biosNode=bios, totalNodes=1, prodCount=1,
                totalProducers=1, pfSetupPolicy=PFSetupPolicy.FULL,
                onlyBios=only_bios, onlySetProds=only_set_producers,
                loadSystemContract=load_system, activateIF=instant_finality,
            )
        return result, bios, events

    def test_full_bootstrap_finishes_before_handover(self):
        """Every full-bootstrap mode works without a post-handover relay path."""
        for load_system in (False, True):
            for instant_finality in (False, True):
                with self.subTest(load_system=load_system, instant_finality=instant_finality):
                    result, bios, events = self.run_bootstrap(
                        load_system=load_system, instant_finality=instant_finality)
                    self.assertIs(result, True)
                    self.assertTrue(bios.handed_over)
                    proposal = events.index(("push", "sysio", "setprodkeys"))
                    setup = events[:proposal]
                    for account in ("sysio.token", "sysio.roa", "sysio.authex", "sysio.dclaim", "sysio.opreg"):
                        self.assertIn(("publish", account, f"{account}.wasm"), setup)
                        self.assertIn(("setpriv", account), setup)
                    for account, action in (
                        ("sysio.token", "create"), ("sysio.token", "issue"),
                        ("sysio.token", "transfer"), ("sysio.opreg", "setconfig"),
                        ("sysio.opreg", "regoperator"), ("sysio.roa", "activateroa"),
                        ("sysio.roa", "forcereg"),
                    ):
                        self.assertIn(("push", account, action), setup)
                    self.assertEqual(("push", "sysio", "init") in setup, load_system)
                    self.assertEqual(("push", "sysio", "setemitcfg") in setup, load_system)
                    self.assertEqual(("publish", "sysio", "sysio.system.wasm") in setup, load_system)
                    self.assertEqual(("push", "sysio", "setfinalizer") in setup, instant_finality)
                    if instant_finality:
                        self.assertIn(("wait-lib",), setup)
                    self.assertIn(("producer-caught-up", 100), setup)
                    self.assertIn(("get-info", "bios", True), events[proposal + 1:])

    def test_early_modes_preserve_minimal_bootstrap(self):
        """Early modes still skip token/ROA setup and retain their handover wait semantics."""
        for only_bios, only_set_producers in ((True, False), (False, True), (True, True)):
            with self.subTest(only_bios=only_bios, only_set_producers=only_set_producers):
                result, bios, events = self.run_bootstrap(
                    only_bios=only_bios, only_set_producers=only_set_producers)
                self.assertIs(result, bios)
                self.assertIn(("push", "sysio", "setprodkeys"), events)
                self.assertNotIn(("publish", "sysio.token", "sysio.token.wasm"), events)
                self.assertNotIn(("push", "sysio.roa", "activateroa"), events)
                self.assertNotIn(("push", "sysio", "init"), events)
                self.assertEqual(("get-info", "bios", True) in events, not only_bios)

    def test_failed_setup_does_not_propose_producers(self):
        """An early or late initialization failure leaves production on BIOS."""
        for failure in (
            ("setpriv", "sysio.token"),
            ("publish", "sysio.roa", "sysio.roa.wasm"),
            ("push", "sysio.opreg", "regoperator"),
            ("push", "sysio", "init"),
        ):
            with self.subTest(failure=failure):
                result, bios, events = self.run_bootstrap(fail_on=failure)
                self.assertIsNone(result)
                self.assertIn(failure, events)
                self.assertFalse(bios.handed_over)
                self.assertNotIn(("push", "sysio", "setprodkeys"), events)


if __name__ == "__main__":
    unittest.main()

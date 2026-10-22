#!/usr/bin/env python3
"""Require bootstrap's privilege transaction to enter a block with BIOS isolated."""

from unittest.mock import patch

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr


class DisconnectedBootstrapCluster(Cluster):
    """Interrupt the BIOS relay path at the transaction lost in the CI failure."""

    def bootstrap(self, launcher, biosNode, *args, **kwargs):
        """Disconnect BIOS peers during the first token privilege transaction."""
        original_set_priv = biosNode.setPriv
        self.isolated_transaction = None

        def set_priv_with_disconnected_peers(account, *action_args, **action_kwargs):
            """Prove local inclusion before reconnecting; restore peers even on failure."""
            if account.name != "sysio.token" or self.isolated_transaction is not None:
                return original_set_priv(account, *action_args, **action_kwargs)

            endpoint = self.getBiosP2pEndpoint()
            try:
                for node in self.nodes:
                    node.processUrllibRequest("net", "disconnect", endpoint, exitOnError=True)

                def bios_is_isolated():
                    """Wait for the asynchronous socket closures on BIOS to finish."""
                    connections = biosNode.processUrllibRequest("net", "connections", exitOnError=True)
                    return not any(peer["is_socket_open"] for peer in connections["payload"])

                assert Utils.waitForBool(bios_is_isolated), "BIOS still has a relay connection"
                Utils.Print("Submit sysio.token setpriv with every BIOS peer disconnected")
                transaction = original_set_priv(account, *action_args, **action_kwargs)
                assert transaction is not None, "Isolated bootstrap transaction failed"
                self.isolated_transaction = Node.getTransId(transaction)
                assert biosNode.waitForTransactionInBlock(self.isolated_transaction), (
                    "Bootstrap depended on relaying its transaction after production handover"
                )
                return transaction
            finally:
                for node in self.nodes:
                    node.processUrllibRequest("net", "connect", endpoint, exitOnError=True)

        with patch.object(biosNode, "setPriv", side_effect=set_priv_with_disconnected_peers):
            return super().bootstrap(launcher, biosNode, *args, **kwargs)


def main():
    """Run full system bootstrap with one successor producer and no API-only node."""
    args = TestHelper.parse_args({"--keep-logs", "--dump-error-details", "--leave-running", "-v", "--unshared"})
    Utils.Debug = args.v
    cluster = DisconnectedBootstrapCluster(
        unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
    wallet = WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)
    successful = False
    try:
        cluster.setWalletMgr(wallet)
        assert cluster.launch(
            pnodes=1, totalNodes=1, totalProducers=1, activateIF=False,
            extraNodeopArgs="--plugin sysio::net_api_plugin",
        ), "Bootstrap did not complete after the isolated transaction"
        assert cluster.isolated_transaction is not None, "Disconnect injection was not exercised"
        successor = cluster.getNode(0)
        assert successor.waitForTransactionInBlock(cluster.isolated_transaction), (
            "Successor did not receive the block containing the isolated transaction"
        )
        assert successor.getInfo(exitOnError=True)["head_block_producer"] == successor.producerName, (
            "Production did not hand over after full bootstrap"
        )
        successful = True
    finally:
        TestHelper.shutdown(cluster, wallet, testSuccessful=successful, dumpErrorDetails=args.dump_error_details)


if __name__ == "__main__":
    main()

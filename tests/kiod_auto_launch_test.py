#!/usr/bin/env python3

# This script tests that clio launches kiod automatically when kiod is not
# running yet.

import subprocess


def run_clio_wallet_command(command: str, no_auto_kiod: bool):
    """Run the given clio command and return subprocess.CompletedProcess."""
    args = ['./programs/clio/clio']

    if no_auto_kiod:
        args.append('--no-auto-kiod')

    args += 'wallet', command

    return subprocess.run(args,
                          check=False,
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)


def stop_kiod():
    """Stop the default kiod instance."""
    run_clio_wallet_command('stop', no_auto_kiod=True)


def check_clio_stderr(stderr: bytes, expected_match: bytes):
    if expected_match not in stderr:
        raise RuntimeError("'{}' not found in {}'".format(
            expected_match.decode(), stderr.decode()))


def kiod_auto_launch_test():
    """Test that keos auto-launching works but can be optionally inhibited."""
    stop_kiod()

    # Make sure that when '--no-auto-kiod' is given, kiod is not started by
    # clio.
    completed_process = run_clio_wallet_command('list', no_auto_kiod=True)
    assert completed_process.returncode != 0
    check_clio_stderr(completed_process.stderr, b'Failed http request to kiod')

    # Verify that kiod auto-launching works.
    completed_process = run_clio_wallet_command('list', no_auto_kiod=False)
    if completed_process.returncode != 0:
        raise RuntimeError("Expected that kiod would be started, "
                           "but got an error instead: {}".format(
                               completed_process.stderr.decode()))
    check_clio_stderr(completed_process.stderr, b'launched')


try:
    kiod_auto_launch_test()
finally:
    stop_kiod()

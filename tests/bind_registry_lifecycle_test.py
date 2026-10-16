#!/usr/bin/env python3
"""Exercise canonical exclusions and lease custody through forced owner death."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from bind_registry import SOURCE_ROOT

CHILD = r'''
import json, os, signal, subprocess, sys, time
from pathlib import Path
resistant = subprocess.Popen([sys.executable, '-c',
    'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("ready",flush=True); time.sleep(600)'],
    stdout=subprocess.PIPE, text=True)
assert resistant.stdout.readline().strip() == 'ready'
Path(sys.argv[1] + '.tmp').write_text(json.dumps({'ports':os.environ['SYSIO_TEST_PORTS'],
    'issuer':int(os.environ['SYSIO_TEST_PORT_ISSUER_PID']), 'group':os.getpid()}))
Path(sys.argv[1] + '.tmp').rename(sys.argv[1])
while not Path(sys.argv[2]).exists(): time.sleep(.1)
'''


def group_alive(group):
    states = subprocess.check_output(['ps', '-eo', 'pgid=,stat='], text=True)
    return any(int(fields[0]) == group and not fields[1].startswith('Z')
               for line in states.splitlines() if len(fields := line.split()) == 2)


def wait_for(predicate, timeout):
    deadline = time.monotonic() + timeout
    while not predicate() and time.monotonic() < deadline:
        time.sleep(.1)
    return predicate()


class RegistryLifecycle(unittest.TestCase):
    def test_owner_death_during_partial_allocation(self):
        wrapper = str(Path(__file__).with_name('bind_registry.py'))
        with tempfile.TemporaryDirectory(prefix='sysio-registry-partial-') as scratch:
            root = Path(scratch)
            marker = root / 'test-started'
            preload = root / 'hold-first.cjs'
            # Pause after a REAL canonical resolve has registered its first
            # config, without adding a test-only hook to the custodian itself.
            preload.write_text(
                'const req=require("node:module").createRequire(' +
                json.dumps(str(Path(SOURCE_ROOT) / 'package.json')) + ');\n' +
                'const {BindConfigProvider:P}=req("@wireio/cluster-tool/lib/cjs/config/BindConfigProvider.js");\n' +
                'const resolve=P.resolve; P.resolve=async(...args)=>{await resolve(...args); return new Promise(()=>{});};\n')
            owner = subprocess.Popen([sys.executable, wrapper, sys.executable, '-c',
                'from pathlib import Path; import sys; Path(sys.argv[1]).touch()', str(marker)],
                env={**os.environ, 'NODE_OPTIONS': os.environ.get('NODE_OPTIONS', '') + ' --require=' + str(preload)})
            try:
                issuer = None
                def find_issuer():
                    nonlocal issuer
                    rows = subprocess.check_output(['ps', '-eo', 'pid=,ppid='], text=True)
                    matches = [int(fields[0]) for line in rows.splitlines()
                               if len(fields := line.split()) == 2 and int(fields[1]) == owner.pid]
                    if matches:
                        issuer = matches[0]
                    return issuer is not None
                self.assertTrue(wait_for(find_issuer, 10))
                registry = Path(os.environ.get('WIRE_BIND_REGISTRY_PATH',
                    str(Path(tempfile.gettempdir()) / 'wire-platform-bind-config')))
                file = registry / f'{issuer}.bind-config.json'
                def registered_first_config():
                    try:
                        return len(json.loads(file.read_text())) == 1
                    except (FileNotFoundError, json.JSONDecodeError):
                        return False
                self.assertTrue(wait_for(registered_first_config, 900))
                self.assertEqual(len(json.loads(file.read_text())), 1)
                owner.kill()
                owner.wait(timeout=10)
                self.assertTrue(wait_for(lambda: not file.exists(), 10))
                self.assertFalse(marker.exists())
            finally:
                if owner.poll() is None:
                    owner.terminate()
                    owner.wait(timeout=60)

    def test_concurrent_issuers_and_forced_owner_death(self):
        wrapper = str(Path(__file__).with_name('bind_registry.py'))
        with tempfile.TemporaryDirectory(prefix='sysio-registry-lifecycle-') as scratch:
            paths = [Path(scratch) / str(index) for index in range(2)]
            owners = [subprocess.Popen([sys.executable, wrapper, sys.executable, '-c', CHILD,
                        str(path), str(path) + '.done']) for path in paths]
            messages = []
            try:
                self.assertTrue(wait_for(lambda: all(path.exists() for path in paths), 900))
                messages = [json.loads(path.read_text()) for path in paths]
                claims = [set(map(int, message['ports'].split(','))) for message in messages]
                self.assertEqual([len(claim) for claim in claims], [192, 192])
                self.assertFalse(claims[0] & claims[1])
                registry = Path(os.environ.get('WIRE_BIND_REGISTRY_PATH',
                    str(Path(tempfile.gettempdir()) / 'wire-platform-bind-config')))
                files = [registry / f"{message['issuer']}.bind-config.json" for message in messages]
                for file, claim in zip(files, claims):
                    entries = json.loads(file.read_text())
                    self.assertEqual(len(entries), 3)
                    registered = {port for entry in entries for port in range(
                        entry['solana']['ports']['dynamicRange']['first'],
                        entry['solana']['ports']['dynamicRange']['last'] + 1)}
                    self.assertTrue(claim <= registered)
                    self.assertFalse(claim & set(range(8000, 10001)))
                owners[0].kill()
                owners[0].wait(timeout=10)
                time.sleep(1)
                self.assertTrue(group_alive(messages[0]['group']))
                self.assertTrue(files[0].exists(), 'Lease released before resistant descendant stopped')
                self.assertTrue(wait_for(lambda: not files[0].exists(), 45))
                self.assertFalse(group_alive(messages[0]['group']))
                self.assertTrue(files[1].exists())
                Path(str(paths[1]) + '.done').touch()
                self.assertEqual(owners[1].wait(timeout=60), 0)
                self.assertFalse(group_alive(messages[1]['group']))
                self.assertFalse(files[1].exists())
            finally:
                for owner in owners:
                    if owner.poll() is None:
                        owner.terminate()
                        owner.wait(timeout=60)


if __name__ == '__main__':
    unittest.main()

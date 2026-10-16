#!/usr/bin/env python3
"""Prove canonical native leases can bind 32 concurrent 192-port test groups."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

CHILD = r'''
import json, os, socket, subprocess, sys, time
from pathlib import Path
ports = list(map(int, os.environ['SYSIO_TEST_PORTS'].split(',')))
sockets = []
for port in ports:
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        listener = socket.socket(socket.AF_INET, kind)
        try:
            listener.bind(('127.0.0.1', port))
        except OSError:
            print(f'Failed canonical port {port}, socket kind {kind}', file=sys.stderr, flush=True)
            subprocess.run(['ss', '-H', '-tan', f'sport = :{port}'], check=False)
            raise
        if kind == socket.SOCK_STREAM: listener.listen(1)
        sockets.append(listener)
Path(sys.argv[1] + '.tmp').write_text(json.dumps({'ports':ports,
    'issuer':int(os.environ['SYSIO_TEST_PORT_ISSUER_PID'])}))
Path(sys.argv[1] + '.tmp').rename(sys.argv[1])
while not Path(sys.argv[2]).exists(): time.sleep(.1)
'''


def main():
    count = 32
    wrapper = str(Path(__file__).with_name('bind_registry.py'))
    with tempfile.TemporaryDirectory(prefix='sysio-registry-stress-') as scratch:
        root = Path(scratch)
        release = root / 'release'
        paths = [root / str(index) for index in range(count)]
        owners = [subprocess.Popen([sys.executable, wrapper, sys.executable, '-c', CHILD,
                    str(path), str(release)],
                    env={**os.environ, 'SYSIO_TEST_PORT_OFFSET': str(100 + index * 192)})
                  for index, path in enumerate(paths)]
        files = []
        try:
            deadline = time.monotonic() + 900
            while not all(path.exists() for path in paths):
                if any(owner.poll() is not None for owner in owners):
                    raise AssertionError('A canonical issuer failed before all 32 groups were ready')
                if time.monotonic() >= deadline:
                    raise AssertionError('32 concurrent canonical leases exceeded allocation budget')
                time.sleep(.1)
            messages = [json.loads(path.read_text()) for path in paths]
            claimed = [port for message in messages for port in message['ports']]
            assert len(claimed) == len(set(claimed)) == count * 192
            assert not set(claimed) & set(range(8000, 10001))
            registry = Path(os.environ.get('WIRE_BIND_REGISTRY_PATH',
                str(Path(tempfile.gettempdir()) / 'wire-platform-bind-config')))
            files = [registry / f"{message['issuer']}.bind-config.json" for message in messages]
            for message, file in zip(messages, files):
                entries = json.loads(file.read_text())
                assert len(entries) == 3
                registered = {port for entry in entries for port in range(
                    entry['solana']['ports']['dynamicRange']['first'],
                    entry['solana']['ports']['dynamicRange']['last'] + 1)}
                assert set(message['ports']) <= registered
            release.touch()
            for owner in owners:
                assert owner.wait(timeout=60) == 0
            assert all(not file.exists() for file in files)
            print('32 concurrent groups: 6144 unique registered ports bound over TCP and UDP; all leases cleaned')
        finally:
            release.touch()
            for owner in owners:
                if owner.poll() is None:
                    owner.terminate()
                    owner.wait(timeout=60)


if __name__ == '__main__':
    main()

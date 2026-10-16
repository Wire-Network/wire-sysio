#!/usr/bin/env python3
"""Real-process FIFO, cancellation, and dead-waiter proofs; no ports are used."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from bind_registry import SOURCE_ROOT as SourceRoot

SOURCE_ROOT = Path(SourceRoot)
WORKER = r'''
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const [root, scratch, label, budget] = process.argv.slice(1);
const requireSource = createRequire(path.join(root, 'package.json'));
const {acquire} = require(path.join(root, 'tests/bind_admission.cjs'));
(async()=>{
  try {
    const release = await acquire({requireSource, deadline:Date.now()+Number(budget),
      isStopping:()=>fs.existsSync(path.join(scratch,label+'.cancel'))});
    fs.appendFileSync(path.join(scratch,'order'),label+'\n');
    fs.writeFileSync(path.join(scratch,label+'.ready'),'');
    while(!fs.existsSync(path.join(scratch,label+'.release'))) await new Promise(r=>setTimeout(r,10));
    await release(); await release();
  } catch(error) {
    fs.writeFileSync(path.join(scratch,label+'.error'),error.code??String(error));
    if(!['ADMISSION_CANCELLED','ADMISSION_TIMEOUT'].includes(error.code)) process.exitCode=1;
  }
})().catch(error=>{console.error(error);process.exitCode=1});
'''


class Admission(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sysio-admission-")
        self.root = Path(self.scratch.name)
        self.registry = self.root / "registry"
        self.children = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=10)
        self.scratch.cleanup()

    def wait(self, predicate, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.02)
        self.fail("Native admission fixture deadline exceeded")

    def start(self, label, budget=60000):
        child = subprocess.Popen(["node", "-e", WORKER, str(SOURCE_ROOT),
            str(self.root), label, str(budget)],
            env={**os.environ, "WIRE_BIND_REGISTRY_PATH": str(self.registry)})
        self.children.append(child)
        return child

    def queued(self, child):
        file = self.registry / "native-admission.json"
        return file.exists() and any(entry["pid"] == child.pid for entry in json.loads(file.read_text()))

    def ready(self, label):
        self.wait(lambda: (self.root / (label + ".ready")).exists())

    def release(self, label, child):
        (self.root / (label + ".release")).touch()
        self.assertEqual(child.wait(timeout=10), 0)

    def test_older_waiters_precede_continuous_arrivals(self):
        head = self.start("head")
        self.ready("head")
        names = ["older", "arrival1", "arrival2", "arrival3"]
        workers = []
        for name in names:
            child = self.start(name)
            self.wait(lambda: self.queued(child))
            workers.append(child)
        self.release("head", head)
        self.ready("older")
        late = self.start("late-arrival")
        self.wait(lambda: self.queued(late))
        for name, child in zip(names + ["late-arrival"], workers + [late]):
            self.ready(name)
            self.release(name, child)
        self.assertEqual((self.root / "order").read_text().splitlines(), ["head"] + names + ["late-arrival"])
        self.assertEqual(json.loads((self.registry / "native-admission.json").read_text()), [])

    def test_dead_cancelled_and_timed_out_waiters_do_not_block(self):
        head = self.start("head")
        self.ready("head")
        dead = self.start("dead")
        self.wait(lambda: self.queued(dead))
        dead.kill()
        dead.wait(timeout=10)
        cancelled = self.start("cancelled")
        self.wait(lambda: self.queued(cancelled))
        (self.root / "cancelled.cancel").touch()
        self.assertEqual(cancelled.wait(timeout=10), 0)
        self.assertEqual((self.root / "cancelled.error").read_text(), "ADMISSION_CANCELLED")
        timed = self.start("timed", budget=300)
        self.assertEqual(timed.wait(timeout=10), 0)
        self.assertEqual((self.root / "timed.error").read_text(), "ADMISSION_TIMEOUT")
        survivor = self.start("survivor")
        self.wait(lambda: self.queued(survivor))
        self.release("head", head)
        self.ready("survivor")
        self.release("survivor", survivor)
        self.assertEqual((self.root / "order").read_text().splitlines(), ["head", "survivor"])
        self.assertEqual(json.loads((self.registry / "native-admission.json").read_text()), [])


if __name__ == "__main__":
    unittest.main()

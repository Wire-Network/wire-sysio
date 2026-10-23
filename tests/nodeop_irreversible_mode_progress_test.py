#!/usr/bin/env python3
"""Deterministically test the irreversible-mode all-counter progress wait."""

import unittest

from nodeop_irreversible_mode_progress import waitForHeadLibAndForkDbHeadToAdvance


class IrreversibleModeProgressTest(unittest.TestCase):
    """Exercise progress polling without node processes, clocks, or real sleeps."""

    timeout = 41
    original = (423, 420, 423)

    def test_lib_only_progress_keeps_waiting(self):
        """A LIB-only update is intermediate; a later all-counter update succeeds."""
        samples = iter((self.original, (423, 421, 423), (424, 421, 424)))
        predicateResults = []

        def waitForBool(predicate, timeout):
            """Drive exactly two polls and record whether each satisfies the predicate."""
            self.assertEqual(self.timeout, timeout)
            predicateResults.append(predicate())
            predicateResults.append(predicate())
            return predicateResults[-1]

        lastObserved = waitForHeadLibAndForkDbHeadToAdvance(
            lambda: next(samples), waitForBool, self.timeout)

        self.assertEqual([False, True], predicateResults)
        self.assertEqual((424, 421, 424), lastObserved)

    def test_each_required_counter_must_advance(self):
        """The wait fails with the last sample when any required counter stays fixed."""
        stuckSamples = {
            "head": (423, 421, 424),
            "lib": (424, 420, 424),
            "fork-db head": (424, 421, 423),
        }

        for stuckCounter, finalSample in stuckSamples.items():
            with self.subTest(stuckCounter=stuckCounter):
                samples = iter((self.original, finalSample))

                def waitForBool(predicate, timeout):
                    """Evaluate the final available sample and model deadline expiration."""
                    self.assertEqual(self.timeout, timeout)
                    self.assertFalse(predicate())
                    return False

                with self.assertRaises(AssertionError) as context:
                    waitForHeadLibAndForkDbHeadToAdvance(
                        lambda: next(samples), waitForBool, self.timeout)

                message = str(context.exception)
                self.assertIn("original (423, 420, 423)", message)
                self.assertIn("last observed ({}, {}, {})".format(*finalSample), message)
                self.assertIn("within 41 seconds", message)


if __name__ == "__main__":
    unittest.main()

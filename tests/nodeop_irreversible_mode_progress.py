"""Polling support for the irreversible-mode integration test's progress assertion."""


def waitForHeadLibAndForkDbHeadToAdvance(sampleProgress, waitForBool, timeout):
   """Wait until head, LIB, and fork-db head all strictly exceed one original sample.

   Return the successful final sample. If the deadline expires, raise an assertion that
   includes the original and last coherent samples for diagnosis.
   """
   originalHead, originalLib, originalForkDbHead = sampleProgress()
   lastObserved = (originalHead, originalLib, originalForkDbHead)

   def allRequiredBlockNumsAdvanced():
      """Capture one coherent progress sample and compare every counter to the baseline."""
      nonlocal lastObserved
      lastObserved = sampleProgress()
      head, lib, forkDbHead = lastObserved
      return head > originalHead and lib > originalLib and forkDbHead > originalForkDbHead

   advanced = waitForBool(allRequiredBlockNumsAdvanced, timeout=timeout)
   if not advanced:
      raise AssertionError(
         "Head, LIB, and fork-db head did not all advance within {} seconds; "
         "original ({}, {}, {}), last observed ({}, {}, {})".format(
            timeout, originalHead, originalLib, originalForkDbHead, *lastObserved))
   return lastObserved

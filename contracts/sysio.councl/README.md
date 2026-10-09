# sysio.councl

Council elections fill 21 seats through simultaneous prioritized T1 flights and one shared voting window across frozen T1/T2/T3 owners. See [DESIGN.md](DESIGN.md) for the contract rules and [OPERATIONS.md](OPERATIONS.md) for running an election.

- Vacant T1 owners nominate or replace three-candidate flights concurrently. Candidate-position reservations prevent same-position reuse while allowing different-position reuse.
- Missing flights are generated in seat order from a frozen deterministic round seed.
- Each owner casts one immutable public ballot covering all eligible flights. A T1 owner excludes their own flight; T2/T3 owners have no exclusions or implicit YES credit.
- After closure, each seat evaluates T1, T2, T3 and then A, B, C within each tier. Winners require `floor(2*B/3)+1` YES votes, where B counts that tier's accepted ballots in the round, including a T1 ballot that omits its own flight. All 21 T1 submissions require 15 YES. Zero submissions cannot elect; there is no extra turnout quorum. Already elected candidates are skipped.
- Continuation preserves winners, registration, and electorates, and starts fresh nominations/ballots and counters for every remaining vacancy until all 21 seats are filled. There is no manual assignment fallback. Explicit reset aborts unfinished elections and deletes partial results.
- Generation, round, and finalized flight-set commitments bind signed ballots. Generation and tabulation use resumable ordered cursors.

The candidate pool remains 23–1,000 self-registered accounts with short handles. Rich profiles, stronger randomness, new candidacy policy, and Hub UI integration are separate work.

See [RESOURCE_USAGE.md](RESOURCE_USAGE.md) for the opt-in CPU/RAM measurement, observed maximum-size costs, and measurement limits.

Build through the root ON/ON configuration's `contracts_project` target, synchronize the council ABI/WASM here, and regenerate `SysioContractTypes.ts` with the canonical generator. Run `council_math_tests,sysio_councl_tests` in `contracts_unit_test` with explicit `--sys-vm-jit`, plus SDK compile/serialization checks. Unrelated snapshot fixtures do not change with this council contract.

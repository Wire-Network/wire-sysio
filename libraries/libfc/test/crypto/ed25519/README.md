# Frozen Ed25519 acceptance vectors

`vectors.hpp` contains 253 fixed inputs and expected results. Tests do not download
or generate vectors. Both `test_fc` and the deployed-contract suite consume this
single table, including all 12 wrong-length signatures. The shared verifier checks
signature and public-key lengths before calling libsodium.

## Published sources

- C2SP/Wycheproof, commit `d17cc89fa5c8f4f4d378b5385ebf75da5d0926c9`,
  [`testvectors_v1/ed25519_test.json`](https://github.com/C2SP/wycheproof/blob/d17cc89fa5c8f4f4d378b5385ebf75da5d0926c9/testvectors_v1/ed25519_test.json):
  all 210 cases, named `wycheproof-<tcId>`. The current filename replaces the older
  `eddsa_test.json` naming. Source SHA-256:
  `ea80ffebac085e21cb9d3e903e7fa2b448d96702da5b31f061d0b83b430b74b3`.
  Apache-2.0; see the shared [LICENSE](LICENSE).
- Novi/ed25519-speccheck, commit `65519336fda78a3d016e947df6d82848aca0c9da`,
  [`cases.json`](https://github.com/novifinancial/ed25519-speccheck/blob/65519336fda78a3d016e947df6d82848aca0c9da/cases.json):
  all 12 cases, named `speccheck-<zero-based index>`. Source SHA-256:
  `08e47a36d9aead288664930505584f353fff113ab854f2800db1e4f5b3540450`.
  Apache-2.0; see the shared [LICENSE](LICENSE).

Expected results implement Wire's pinned cofactorless verification policy, not
Wycheproof's broader `acceptable` category. All ordinary `valid`/`invalid` results
agree with Wycheproof. Mixed-order-key cases 160–180 are accepted; small-order-key
cases are rejected. Speccheck case 3 is accepted because the torsion contributions
of its mixed-order `A` and `R` cancel in the full equation; the other 11 fail.

## Constructed cases

Three cases isolate mixed-order behavior using secret scalar `a=17`, nonce `r=23`,
and order-eight torsion point encoded as
`c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a`:

- `mixed-A-cofactorless`: `A=[a]B+T`, `R=[r]B`, `S=r+ka mod L`, with `k mod 8=0`:
  accepted.
- `mixed-A-cofactored-only`: same construction, `k mod 8 != 0`: rejected even
  though the cofactored equation holds.
- `mixed-R-prime-A`: `A=[a]B`, `R=[r]B+T`, `S=r+ka mod L`: rejected.

The remaining 28 `S0-blacklist-*` cases cover every entry in libsodium's seven-entry
small-order blacklist, both sign bits, and both `A` and `R` roles. They include
orders 1/2/4/8 and noncanonical `p`/`p+1` encodings. For each target point, use that
encoding in the named role and its canonical encoding in the other, set `S=0`,
and search one-byte messages in ascending order until `R + [k]A` is the identity.
The underlying group equation holds, so these are structured forgery rejections.
Both `A` and `R` are small-order in each case: these cases jointly cover the policy,
but do not isolate either role's check or prove each blacklist entry is necessary.
The speccheck cases separately isolate small-order `A` and `R` for some encodings.

All fixed-size expected results were independently checked using affine Edwards
arithmetic modulo `p=2^255-19`, SHA-512 challenges reduced modulo `L`, canonical
encoding comparison, and the documented scalar/key/small-order checks. They were
then compared with Wire-Network/libsodium
`abd3612f433b795a6ac31a386a75f8f26a479033`. The table accepts 140 cases and rejects
113, including the 12 length failures. These results are frozen; do not regenerate
expectations from a new dependency merely to make a failing test pass. A sodium
source/port bump requires consensus review, even if its version string is unchanged.

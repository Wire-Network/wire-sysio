# Release Workflow

How a wire-sysio release goes from a version bump to a published GitHub Release
with verified artifacts attached. Artifact contents are documented in
[release-layout.md](release-layout.md); build commands in
[../BUILD.md](../BUILD.md).

## Strategy C — independent per-repo releases

wire-sysio and wire-cdt release **independently**. There is no cross-repo release
train and no shared pipeline: wire-cdt cuts its own release, and a wire-sysio
release **consumes a pinned wire-cdt release asset**.

The properties that follow from that choice:

- **Resolve at prepare, pin at build.** `prepare-release.yaml` resolves the latest
  published wire-cdt release for the channel and writes it into
  `.cicd/defaults.json` (`wirecdt.release`, `wirecdt.channel`). The tag build then
  downloads *exactly* that release's `wire-cdt_<version>_amd64.deb` asset. Re-runs
  are reproducible, the CDT version is reviewable at gate 1, and there is no
  publish-order race at release time. For a one-off experiment against a
  different CDT, `linux_amd64_build.yaml` takes an `override-cdt-release`
  dispatch input; its channel is then derived from the release's own prerelease
  flag instead of being asserted against the pin.
  (`.cicd/defaults.json`'s `wirecdt` block carries exactly the two keys the
  release-asset path reads — `release` and `channel`. The former `target`
  git-ref key had no reader and was removed.)
- **Release assets, not CI artifacts.** CI artifacts expire after 30 days, so an
  old tag could not be rebuilt from them. Release assets are durable.
  Wire-Network/wire-cdt is public, so the download needs no special token — the
  build's own `GITHUB_TOKEN` is passed to `gh` only to lift the unauthenticated
  rate limit. (The `.cicd/platforms` images all bake the GitHub CLI, so the
  download is a plain `gh release download`.)
- **The version's suffix IS the channel.** `-dev` / `-rcN` means prerelease; no
  suffix means stable. One input (`version`) decides everything downstream —
  the release's prerelease flag, and which wire-cdt channel gets pinned.
- **Two human gates.** Gate 1 is reviewing and merging the bump PR. Gate 2 is
  approving the `release` Environment before anything is tagged or created.
- **The system contracts ship as a release asset.** Only tag builds compile them
  (`BUILD_SYSTEM_CONTRACTS=ON` plus `assert-system-contract-build.sh`), and they
  are bundled as `wire-sysio-system-contracts-<version>.tar.gz`.

## Packaged layouts — this repo vs the consumed wire-cdt deb

The two repos package **differently on purpose**. Do not copy wire-cdt's private
home into wire-sysio.

| Artifact | Programs / payload | Notes |
|---|---|---|
| **wire-sysio deb / rpm** | `/usr/bin/nodeop`, `/usr/bin/clio`, `/usr/bin/kiod` — installed **directly**, own names, no private home and no symlink indirection | prefix `/usr`, `COMPONENT base`; plus `/usr/lib/systemd/system/`, `/usr/lib/tmpfiles.d/`, `/etc/logrotate.d/`. Asserted by `tools/packaging/tests/verify-{deb,rpm}.sh`. |
| **wire-sysio tarball** | self-contained tree | see [release-layout.md](release-layout.md) |
| **wire-cdt deb / rpm** (consumed) | self-contained toolchain home at `/usr/lib/cdt`, with only public `cdt-*` / `sysio-*` entry points symlinked into `/usr/bin` | distro-toolchain layout (cf. `/usr/lib/llvm-18`) |

wire-cdt needs the private home for one specific reason: it **bundles its own
LLVM**, so `clang`, `lld`, `llvm-ar` … would collide with the distro's `clang` /
`lld` / `llvm` packages if installed into `/usr/bin`. wire-sysio bundles no such
thing — `nodeop` / `clio` / `kiod` are unique names — so they install straight
into `/usr/bin` and must stay there.

`install-wire-cdt-package.sh` therefore exports `CDT_ROOT=/usr/lib/cdt` (the
toolchain home, not `/usr`): `build-sysio.sh` checks
`$CDT_ROOT/lib/cmake/cdt/cdt-config.cmake` and `cmake/contract-tools.cmake`
builds `$CDT_ROOT/lib/cmake/cdt/CDTWasmToolchain.cmake`, both of which only
resolve under the home.

## The workflows

| Workflow | Trigger | What it does |
|---|---|---|
| `prepare-release.yaml` | `workflow_dispatch(version)` | Validates the version, writes `VERSION_*` into `CMakeLists.txt`, resolves + pins the latest published wire-cdt release for the channel, opens the `release/prep-v<version>` PR. |
| `tag-release.yaml` | `workflow_dispatch(version)`, `environment: release` | Asserts master HEAD carries the version, creates the annotated tag (`POST /git/tags` then `POST /git/refs`), creates the DRAFT release with generated notes, dispatches both platform builds on the tag ref, dispatches `release.yaml`. |
| `linux_amd64_build.yaml` | tag ref (dispatched or pushed) | Installs the pinned wire-cdt release deb, builds with system contracts ON, assembles deb + rpm + portable tgz, bundles the contracts, uploads `wire-sysio-packages-amd64`. |
| `macos_arm64_build.yaml` | tag ref (dispatched or pushed) | Builds, tests, produces the portable tarball, verifies it in `--no-service` mode, uploads `wire-sysio-packages-macos-arm64`. |
| `release.yaml` | `workflow_dispatch(tag)`; `release: published` as a manual fallback | Awaits both tag-ref builds, downloads both artifacts, verifies each one explicitly, attaches assets + checksums to the draft, then flips the draft public. |

### Post-bump

After a **stable** release lands, dispatch `prepare-release.yaml` again with the
next `-dev` version to restore the development suffix on master. It is the same
workflow and the same single input; the PR is labelled `post-bump`.

### Why the builds are dispatched explicitly

A tag created with `GITHUB_TOKEN` fires **no** `push` event, so the tag builds
would never start on their own. `tag-release.yaml` dispatches them on
`refs/tags/<tag>`, which makes `github.ref` inside those runs the tag ref — so the
existing `startsWith(github.ref, 'refs/tags/v')` gates behave exactly as they
would on a human-pushed tag.

`release.yaml`'s build resolver requires `head_branch == <tag>` for the same
reason it accepts both `push` and `workflow_dispatch`: the master-push build sits
at the same commit SHA but builds **without** the system contracts, and matching it
would attach a package set with no contracts bundle.

### Known friction (documented, not fixed)

- A PR opened with `GITHUB_TOKEN` starts its `pull_request` checks in an
  approval-required state — gate 1 includes an "Approve and run" click.
- Branch protection on master must require the packaging checks, or gate 1 is
  decorative.

## Human vs system

```mermaid
flowchart TD
    classDef human fill:#FFE9B8,stroke:#8a6d1a,color:#1a1a1a
    classDef system fill:#DCEBFF,stroke:#1e5aa8,color:#1a1a1a
    classDef gate fill:#FFD9D9,stroke:#a83232,color:#1a1a1a

    H1[/"Human: pick version — its suffix IS the channel<br>(-dev/-rcN = prerelease, none = stable)"/]:::human
    H2[/"Human: dispatch prepare-release (input: version)"/]:::human
    S1["System: edit VERSION_*, resolve+pin latest<br>wire-cdt release (sysio only), open bump PR"]:::system
    G1{{"GATE 1 — Human: 'Approve and run' the PR checks,<br>review (incl. pinned CDT), merge"}}:::gate
    H3[/"Human: dispatch tag-release (input: version)"/]:::human
    G2{{"GATE 2 — Human: approve 'release' Environment"}}:::gate
    S3["System: assert version == master HEAD,<br>create annotated tag + DRAFT release,<br>DISPATCH linux + macos builds on the tag ref"]:::system
    S4["System: tag builds produce packages<br>(sysio: installs pinned wire-cdt release deb,<br>builds + asserts system contracts, bundles them)"]:::system
    S6["System: release.yaml — await tag-ref builds,<br>download, verify each artifact explicitly"]:::system
    S7["System: attach assets + checksums,<br>flip draft to public"]:::system
    H4[/"Human: sanity-check the release page"/]:::human

    H1 --> H2 --> S1 --> G1 --> H3 --> G2 --> S3 --> S4 --> S6 --> S7 --> H4
```

## Release assets

Eight assets, excluding GitHub's auto-added source archives:

| Asset | Produced by |
|---|---|
| `wire-sysio_<version>_amd64.deb` | linux tag build (`cpack -G DEB`, base component) |
| `wire-sysio-dev_<version>_amd64.deb` | linux tag build (dev component) |
| `wire-sysio-<version>-x86_64.rpm` | linux tag build (`cpack -G RPM`, base component) |
| `wire-sysio-dev-<version>-x86_64.rpm` | linux tag build (dev component) |
| `wire-sysio-<version>-x86_64.tar.gz` | linux tag build (`package-tgz`) |
| `wire-sysio-<version>-macos-arm64.tar.gz` | macOS tag build (`package-tgz`) |
| `wire-sysio-system-contracts-<version>.tar.gz` | linux tag build (`sysio.*/{*.wasm,*.abi}`) |
| `wire-sysio-<version>-checksums.txt` | `release.yaml` |

The `-macos-arm64` suffix comes from `WIRE_ARCH_TAG` in `cmake/package.cmake`;
Linux artifact names are unchanged. The contracts bundle is deliberately named so
it can never be glob-matched as a platform tarball — every verification in
`release.yaml` names its artifact explicitly for the same reason.

## Verification

`release.yaml` runs the packaging suite against each artifact by name:

- `verify-tgz.sh` on the Linux tarball (asserts the systemd unit, tmpfiles.d
  fragment and logrotate policy are present).
- `verify-tgz.sh --no-service` on the macOS tarball (asserts the binaries and
  licenses are present AND that the service files are **absent** — those
  `install()` rules are gated on `NOT APPLE`).
- `verify-deb.sh` / `verify-rpm.sh` on the base + dev packages.
- A contracts-bundle check: the tarball must contain both `sysio.*.wasm` and
  `sysio.*.abi` entries.

The same suite runs as the `verify-packages` job on every build, so a packaging
regression fails CI long before a release is cut.

## Failure and rollback

- `release.yaml` is idempotent for a tag (`gh release upload --clobber`).
  Recovery from a failed asset run is: fix, then re-dispatch `release.yaml` with
  the same tag. **Never re-tag.**
- Wrong tag or wrong content: delete the draft release and the tag, then cut the
  next `-rcN`.
- Abandoned drafts are surfaced at the next release's prepare step; deleting them
  is a deliberate human action.

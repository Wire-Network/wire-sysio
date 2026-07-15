# Release Workflow

How a wire-sysio release goes from a version bump to a published GitHub
Release with verified artifacts attached. Artifact contents are documented in
[release-layout.md](release-layout.md); build commands in
[../BUILD.md](../BUILD.md).

## Overview

1. **Version bump** — a PR to `master` updates `VERSION_MAJOR/MINOR/PATCH/SUFFIX`
   in `CMakeLists.txt`. The version embedded in every artifact comes from here;
   the release workflow fails fast if it doesn't match the tag.
2. **Tag** — a maintainer pushes `v<version>`. The tag build
   (`linux_amd64_build.yaml`) installs the Wire CDT package, builds the system
   contracts from source (and asserts it), assembles the full package set
   (base + dev deb/rpm, portable tgz) on the `ubuntu24` platform, and the
   `verify-packages` job runs the packaging suite (S1-S6, including the
   systemd-in-container service-start gate) against the produced artifacts.
3. **Publish** — a maintainer publishes the GitHub Release for the tag. The
   `Release Actions` workflow (`release.yaml`) locates the successful tag
   build, downloads its `wire-sysio-packages-amd64` artifact, guards
   tag↔version consistency, re-verifies the packages, attaches them plus a
   sha256 checksums file to the release, and refreshes the
   `wire-sysio-experimental-binaries` ghcr image.

For pre-merge validation, `release.yaml` also exposes `workflow_dispatch`
with a `tag` input, which runs the same verify-and-attach path against an
existing tag + release without waiting for a `release: published` event
(those events always execute the workflow file from the default branch).

## Activity

```mermaid
flowchart TD
    A[PR: bump VERSION_* in CMakeLists.txt] --> B[Merge to master]
    B --> C[Push tag v&lt;version&gt;]
    C --> D[Tag build: linux_amd64_build.yaml]
    D --> D1[Install Wire CDT package]
    D1 --> D2[Build + test, system contracts ON]
    D2 --> D3[assert-system-contract-build]
    D3 --> D4[Assemble packages: deb + rpm + tgz]
    D4 --> D5[Upload wire-sysio-packages-amd64]
    D5 --> E{verify-packages: S1-S6}
    E -- fail --> X1[Fix and re-tag]
    E -- pass --> F[Maintainer publishes GitHub Release]
    F --> G[Release Actions: release.yaml]
    G --> G1[Resolve successful tag build run]
    G1 --> G2{Tag matches artifact version?}
    G2 -- no --> X2[Fail: bump CMakeLists VERSION_* first]
    G2 -- yes --> G3[Re-verify packages S1-S6]
    G3 --> G4[Attach artifacts + sha256 checksums]
    G4 --> G5[Push ghcr experimental-binaries image]
    G5 --> H([Release live with verified artifacts])
```

## Sequence

```mermaid
sequenceDiagram
    actor M as Maintainer
    participant GH as GitHub
    participant B as Build workflow<br/>(linux_amd64_build)
    participant V as verify-packages job
    participant R as Release Actions<br/>(release.yaml)
    participant REG as ghcr.io

    M->>GH: merge version-bump PR (CMakeLists VERSION_*)
    M->>GH: push tag v<version>
    GH->>B: trigger (push: tags v*)
    B->>B: install Wire CDT, build + test,<br/>system contracts ON + assert
    B->>B: cpack DEB/RPM + package-tgz (ubuntu24)
    B->>GH: upload artifact wire-sysio-packages-amd64
    GH->>V: run after build
    V->>GH: download artifact
    V->>V: S1 tgz - S2 deb - S3 rpm - S5 lint<br/>(S6 systemd service-start inside S2/S3)
    V-->>GH: all-passing gate
    M->>GH: publish Release for tag
    GH->>R: trigger (release: published)
    R->>GH: resolve successful tag-build run (gh api)
    R->>GH: download wire-sysio-packages-amd64
    R->>R: guard: tag == artifact version
    R->>R: re-verify packages (S1-S6)
    R->>GH: gh release upload artifacts + checksums
    R->>REG: push wire-sysio-experimental-binaries:v<version>
```

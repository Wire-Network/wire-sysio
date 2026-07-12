# Release Artifact Layout

wire-sysio ships three artifact types per release, all produced by CPack from
one build tree (see `BUILD.md` → Packaging). Version shown as `<v>` (e.g.
`1.0.0-dev`; dpkg/rpm metadata uses the tilde form `1.0.0~dev` so pre-release
suffixes sort before the final release).

| Artifact | Name | Install prefix |
|---|---|---|
| Portable tarball | `wire-sysio-<v>-<arch>.tar.gz` | none — self-contained tree |
| Debian/Ubuntu | `wire-sysio_<v>_<debarch>.deb` + `wire-sysio-dev_<v>_<debarch>.deb` | `/usr` |
| RHEL/Fedora | `wire-sysio-<v>-<arch>.rpm` + `wire-sysio-dev-<v>-<arch>.rpm` | `/usr` |

## Portable tarball (`.tar.gz`)

Runtime payload only (the base component), rooted at a plain `wire-sysio/`
directory so that extraction into `/opt` yields `/opt/wire-sysio/bin/nodeop`:

```
wire-sysio/
├── bin/
│   ├── clio
│   ├── cranker-example
│   ├── kiod
│   ├── nodeop
│   ├── outpost_ethereum_client_tool
│   ├── outpost_solana_client_tool
│   ├── sys-util
│   └── trace_api_util
├── etc/
│   └── logrotate.d/
│       └── wire-sysio-nodeop          # reference logrotate policy
├── lib/
│   ├── systemd/system/
│   │   └── wire-sysio-nodeop.service  # reference unit (paths assume /usr)
│   └── tmpfiles.d/
│       └── wire-sysio.conf            # reference state-dir definitions
└── share/
    └── licenses/
        └── sysio/                     # third-party license texts
```

The `etc/` and `lib/` entries are inert reference copies of the system
integration files; nothing is registered by extracting the tarball.

## System packages (`.deb` / `.rpm`)

### `wire-sysio` (base, runtime)

```
/usr/bin/{clio,cranker-example,kiod,nodeop,outpost_ethereum_client_tool,
          outpost_solana_client_tool,sys-util,trace_api_util}
/usr/lib/systemd/system/wire-sysio-nodeop.service
/usr/lib/tmpfiles.d/wire-sysio.conf
/usr/share/licenses/sysio/*
/etc/logrotate.d/wire-sysio-nodeop      # conffile / %config(noreplace)
```

Service behavior on install: the DEB asks (debconf) whether to register the
service and whether to enable it — enabling also starts it (`systemctl
enable --now`); noninteractive installs default to yes/yes. The RPM registers,
enables, and starts on fresh install (no prompting facility). Upgrades never
restart a running node; removal stops/disables the unit but **never deletes**
`/var/lib/wire/sysio` or `/var/log/wire/sysio`.

Runtime directories (created by `systemd-tmpfiles` at install and every boot,
matching the unit's command line — nodeop's internal defaults are unchanged):

| Purpose | Path |
|---|---|
| Config (`--config-dir`) | `/etc/wire/sysio` |
| Data (`--data-dir`) | `/var/lib/wire/sysio/data` |
| Logs (unit `append:`) | `/var/log/wire/sysio` (rotated 5 x 1G, copytruncate) |

### `wire-sysio-dev` (headers/libraries/testing; depends on exact base version)

```
/usr/include/{appbase,chainbase,fc,fc-lite,wasm-jit}/...
/usr/include/sysio/{chain,testing,...}/...   # incl. generated OPP *.pb.h
/usr/include/sysio.version.hpp
/usr/lib/{libfc.a,libfc-lite*.a,libappbase.a,libchainbase.a,
          libsysio_chain.a,libsysio_testing.a,
          libWASM.a,libWAST.a,libLogging.a,libIR.a}
/usr/lib/cmake/sysio/{sysio-config.cmake,SysioTester.cmake,SysioCheckVersion.cmake}
/usr/lib/python3/dist-packages/TestHarness -> ../../../share/sysio_testing/tests/TestHarness
/usr/share/sysio_testing/
├── bin/                                # test-runner copies of the binaries
├── libraries/testing/contracts/        # testing contract artifacts
├── tests/TestHarness/                  # python integration-test framework
└── unittests/system-test-contracts/
```

Not shipped in either package: plugin static libraries (internal, linked
whole-archive into nodeop) and the legacy `spring_testing` maintainer scripts
(superseded by payload-shipped symlinks/copies).

## Verification

Every artifact is gated by `tools/packaging/tests/` (see `BUILD.md`):
layout (S1), payload/control/container install (S2/S3), install-manifest
regression (S4), static lint (S5), and a systemd-in-container runtime stage
(S6) proving the packaged service enables, starts, and stays running.

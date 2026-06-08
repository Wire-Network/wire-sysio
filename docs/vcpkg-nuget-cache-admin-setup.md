# GitHub Packages vcpkg NuGet Cache Admin Setup

This document is for a GitHub organization or repository admin who can create
organization-level Actions secrets for `Wire-Network`.

## Purpose

`wire-sysio` is being updated to use GitHub Packages as a NuGet-backed vcpkg
binary cache. This allows CI and developers to reuse expensive vcpkg builds,
especially packages such as LLVM, instead of rebuilding them from source.

The current GitHub Packages feed already contains private vcpkg cache packages
created by `Wire-Network/wire-cdt`, for example:

```text
llvm_x64-linux-release
abseil_x64-linux-release
protobuf_x64-linux-release
vcpkg-cmake_x64-linux-release
```

Those package records are private and associated with `wire-cdt`. The default
`GITHUB_TOKEN` created for `wire-sysio` workflow runs is scoped to
`wire-sysio`, so it cannot read or write private packages associated with
`wire-cdt` unless package access is granted separately.

The requested setup is to add a shared classic PAT for the vcpkg NuGet cache.
This keeps the workflow simple and avoids having to manually grant
`wire-sysio` access to every individual vcpkg package.

## Requested Admin Action

Please create one GitHub classic personal access token and add two
organization-level Actions secrets under `Wire-Network`.

The two secrets are:

```text
VCPKG_NUGET_TOKEN
VCPKG_NUGET_USER
```

Only `VCPKG_NUGET_TOKEN` is a token. `VCPKG_NUGET_USER` is the GitHub login of
the account that owns the token.

For example, if the token is created by a bot account named `wire-build-bot`:

```text
VCPKG_NUGET_TOKEN = ghp_...
VCPKG_NUGET_USER  = wire-build-bot
```

## Token Requirements

Create a classic PAT from a GitHub user or bot account that has access to the
`Wire-Network` organization packages.

Required token scopes:

```text
read:packages
write:packages
```

If GitHub refuses access to private package metadata or private repositories
with only package scopes, add:

```text
repo
```

Using a dedicated bot/service account is preferred over using a human user's
primary account. If a human-owned PAT is used, please set an expiration and a
rotation reminder.

## Add Organization Secrets With GitHub CLI

Create organization-level secrets restricted to the repositories that should use
the shared vcpkg NuGet cache:

```bash
gh secret set VCPKG_NUGET_TOKEN \
  --org Wire-Network \
  --repos wire-sysio,wire-cdt \
  --body 'PASTE_CLASSIC_PAT_HERE'

gh secret set VCPKG_NUGET_USER \
  --org Wire-Network \
  --repos wire-sysio,wire-cdt \
  --body 'PAT_OWNER_GITHUB_LOGIN'
```

The `--repos wire-sysio,wire-cdt` form creates organization secrets with
selected repository access. The secrets are centrally managed by the
`Wire-Network` organization, but only those selected repositories can use them.

## Add Organization Secrets With GitHub UI

1. Open `https://github.com/organizations/Wire-Network/settings/secrets/actions`.
2. Add a new organization secret named `VCPKG_NUGET_TOKEN`.
3. Set its value to the classic PAT.
4. Restrict repository access to selected repositories.
5. Select both `wire-sysio` and `wire-cdt`.
6. Add a new organization secret named `VCPKG_NUGET_USER`.
7. Set its value to the GitHub login that created the PAT.
8. Restrict repository access to selected repositories.
9. Select both `wire-sysio` and `wire-cdt`.



## Verification

After secrets are added, rerun the vcpkg NuGet cache PR workflow.

A successful restore should show log lines similar to:

```text
Restored N package(s) from NuGet
```

where `N` is greater than zero when matching cached packages exist.

A successful publish should no longer show:

```text
Pushing NuGet to "https://nuget.pkg.github.com/Wire-Network/index.json" failed.
Your request could not be authenticated by the GitHub Packages service.
Forbidden https://nuget.pkg.github.com/Wire-Network/download/...
```

If the workflow still fails with `Forbidden`, please confirm:

1. The PAT is classic, not fine-grained.
2. The PAT has `read:packages` and `write:packages`.
3. The PAT owner has access to the `Wire-Network` package namespace.
4. `VCPKG_NUGET_USER` exactly matches the PAT owner's GitHub login.
5. The secrets are available to GitHub Actions for `Wire-Network/wire-sysio`.

## Security Notes

The PAT should be treated as a package publishing credential for the
`Wire-Network` GitHub Packages namespace.

Recommended safeguards:

1. Use a dedicated bot/service account when possible.
2. Grant only the scopes required for the package cache.
3. Store the PAT only as a GitHub Actions secret.
4. Restrict organization-level secrets to `wire-sysio` and `wire-cdt` rather
   than exposing them to every repository.
5. Rotate the token on a schedule and whenever membership or bot-account access
   changes.

Fork pull requests should not receive this secret. The workflow should continue
to use a fork-safe mode for fork PRs so untrusted code cannot access package
publishing credentials.

## Related GitHub Documentation

- GitHub Packages NuGet registry:
  https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-nuget-registry
- GitHub Packages access control:
  https://docs.github.com/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility

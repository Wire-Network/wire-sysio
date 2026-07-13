#!/usr/bin/env bash
set -euo pipefail

# Generate protobuf bundles for OPP using the repo-local wire-protobuf-bundler
# workspace. This script intentionally uses Bash so CI does not need a mutable
# package repository just to install fish before publishing OPP artifacts.

usage() {
   echo "Usage: generate-opp-bundles.sh [--publish]" >&2
}

publish=false
for arg in "$@"; do
   case "$arg" in
      --publish)
         publish=true
         ;;
      -h|--help)
         usage
         exit 0
         ;;
      *)
         echo "Unknown argument: $arg" >&2
         usage
         exit 1
         ;;
   esac
done

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
tools_root="$(cd -- "$script_dir/.." && pwd -P)"
repo_root="$(cd -- "$tools_root/../../.." && pwd -P)"

# WIRE CodeArtifact cargo registry (provisioned by wire-infra
# ArtifactManagementStack). The generated wire-opp-solana-models crate is
# published to the PUBLIC repository — consumers (wire-solana et al) read it
# anonymously via https://public.crates.pkgs.wire-dev.com with no AWS auth.
# The registry name "wire" must match CARGO_REGISTRY_NAME in
# protobuf-bundler/src/constants.ts. Publishing still authenticates against
# the native CodeArtifact endpoint (GitHub OIDC publisher role).
codeartifact_domain="wire-pkg"
codeartifact_domain_owner="619322354039"
codeartifact_region="us-east-1"
cargo_registry_repository="wire-pkg-rust-public"
cargo_registry_index="sparse+https://${codeartifact_domain}-${codeartifact_domain_owner}.d.codeartifact.${codeartifact_region}.amazonaws.com/cargo/${cargo_registry_repository}/"
# Minimum cargo with sparse-registry + registry-token auth support.
cargo_minimum_version="1.74.0"

# Publishing needs cargo (crate publish), the AWS CLI (CodeArtifact token),
# and AWS credentials (GitHub OIDC role in CI, an SSO profile locally). The
# validate path stays credential- and tool-free beyond node/pnpm.
if [[ "$publish" == true ]]; then
   if ! command -v cargo >/dev/null 2>&1; then
      echo "Error: cargo is required to publish the wire-opp-solana-models crate but is not in PATH." >&2
      exit 1
   fi
   cargo_version="$(cargo --version | awk '{print $2}')"
   if [[ "$(printf '%s\n' "$cargo_minimum_version" "$cargo_version" | sort -V | head -1)" != "$cargo_minimum_version" ]]; then
      echo "Error: cargo >= $cargo_minimum_version is required for sparse registry auth (found $cargo_version)." >&2
      exit 1
   fi
   if ! command -v aws >/dev/null 2>&1; then
      echo "Error: the AWS CLI is required to mint the CodeArtifact publish token but is not in PATH." >&2
      exit 1
   fi

   echo "Fetching CodeArtifact authorization token for cargo publish..."
   cargo_registry_token="$(aws codeartifact get-authorization-token \
      --domain "$codeartifact_domain" \
      --domain-owner "$codeartifact_domain_owner" \
      --region "$codeartifact_region" \
      --query authorizationToken \
      --output text)"
   if [[ -z "$cargo_registry_token" ]]; then
      echo "Error: failed to obtain a CodeArtifact authorization token." >&2
      exit 1
   fi
   # Keep the token out of GitHub Actions logs; the marker is emitted only in CI.
   if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
      echo "::add-mask::${cargo_registry_token}"
   fi
   export CARGO_REGISTRIES_WIRE_INDEX="$cargo_registry_index"
   export CARGO_REGISTRIES_WIRE_TOKEN="$cargo_registry_token"
   # Authenticated alternative registries refuse to operate without an
   # explicit credential provider; cargo:token reads the _TOKEN env var above.
   export CARGO_REGISTRIES_WIRE_CREDENTIAL_PROVIDER="cargo:token"
fi
repo_proto_src_path="$repo_root/libraries/opp/proto"
repo_output_path="$repo_root/build/opp"
output_paths=("$repo_output_path")

if [[ -d "$repo_root/../wire-opp" ]]; then
   output_paths+=("$(cd -- "$repo_root/../wire-opp" && pwd -P)")
fi

pushd "$tools_root" >/dev/null
pnpm install --frozen-lockfile --ignore-scripts

# Match the workspace package directories so scoped npm package names do not
# bypass the OPP tool build on pnpm 10.
pnpm --filter "./proto*" dist

(
   cd protoc-gen-solidity
   pnpm link --global
)
(
   cd protoc-gen-solana
   pnpm link --global
)
(
   cd protobuf-bundler
   pnpm link --global
)
popd >/dev/null

if ! command -v wire-protobuf-bundler >/dev/null 2>&1; then
   echo "Error: wire-protobuf-bundler is not installed or not in PATH." >&2
   exit 1
fi

cmd=(
   wire-protobuf-bundler
   --repo "file://$repo_proto_src_path"
)

for output_path in "${output_paths[@]}"; do
   cmd+=(--output "$output_path")
done

if [[ "$publish" == true ]]; then
   cmd+=(--publish)
fi

echo "Running wire-protobuf-bundler for all targets..."
echo "  repo root: $repo_root"
printf "  outputs:  "
printf " %q" "${output_paths[@]}"
printf "\n"

cd "$repo_root"
printf "Executing command:"
printf " %q" "${cmd[@]}"
printf "\n"
"${cmd[@]}"

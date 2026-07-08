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

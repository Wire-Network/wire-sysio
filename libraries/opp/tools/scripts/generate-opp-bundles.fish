#!/usr/bin/env fish

# generate-opp-bundles.fish
# Generates protobuf bundles for OPP using wire-protobuf-bundler.

set -l script_dir (status dirname)
set -l tools_root (realpath "$script_dir/..")
set -l repo_root (realpath "$tools_root/../../..")
set -l repo_proto_src_path "$repo_root/libraries/opp/proto"

set -l repo_output_path1 "$repo_root/build/opp/"


# --- Defaults ---
set -l publish false

# --- Parse arguments ---
for arg in $argv
   switch $arg
      case '--publish'
         set publish true
      case '*'
         echo "Unknown argument: $arg" >&2
         echo "Usage: generate-opp-bundles.fish [--publish]" >&2
         exit 1
   end
end

# --- Setup & build tools ---
pushd $tools_root
pnpm install; or exit 1
pnpm --filter "proto*" dist; or exit 1
cd protoc-gen-solidity && pnpm link --global && cd ..; or exit 1
cd protoc-gen-solana && pnpm link --global && cd ..; or exit 1
cd protobuf-bundler && pnpm link --global && cd ..; or exit 1
popd

if not which wire-protobuf-bundler &> /dev/null
   echo "Error: wire-protobuf-bundler is not installed or not in PATH." >&2
   exit 1
end

# --- Build command ---
set -l cmd wire-protobuf-bundler \
   --repo "file://$repo_proto_src_path" \
   --output "$repo_output_path1"



# --- Run ---
echo "Running wire-protobuf-bundler for all targets..."
echo "  repo root: $repo_root"
echo "  output:    $repo_output_path1"


if test "$publish" = true
   set -a cmd --publish
end

cd $repo_root; or exit 1
echo "Executing command: $cmd"
$cmd

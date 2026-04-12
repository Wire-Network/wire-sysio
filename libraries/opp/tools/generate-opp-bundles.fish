#!/usr/bin/env fish

# generate-opp-bundles.fish
# Generates protobuf bundles for OPP using wire-protobuf-bundler.

set -l script_dir (status dirname)
set -l repo_root (realpath "$script_dir/../../..")
set -l repo_proto_src_path "$repo_root/libraries/opp/proto"

set -l repo_output_path1 "$repo_root/build/opp/"
set -l repo_output_path2 (realpath "$repo_root/../wire-opp/")

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

# --- Check required tools on PATH ---
set -l required_tools wire-protobuf-bundler protoc-gen-solidity protoc-gen-solana
set -l missing_tools

for tool in $required_tools
   if not command -q $tool
      set -a missing_tools $tool
   end
end

if test (count $missing_tools) -gt 0
   echo "Error: The following required tools are not found on PATH:" >&2
   for tool in $missing_tools
      echo "  - $tool" >&2
   end
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
if test -d "$repo_output_path2"
    set -a cmd --output "$repo_output_path2/"
    echo "  repo opp root: $repo_output_path2"
end

if test "$publish" = true
   set -a cmd --publish
end

cd $repo_root; or exit 1
echo "Executing command: $cmd"
$cmd

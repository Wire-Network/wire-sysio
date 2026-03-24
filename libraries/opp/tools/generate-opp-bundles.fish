#!/usr/bin/env fish

# generate-opp-bundles.fish
# Generates protobuf bundles for OPP using wire-protobuf-bundler.

set -l script_dir (status dirname)
set -l repo_root (realpath "$script_dir/../../..")

# --- Defaults ---
set -l target ""
set -l publish false

# --- Parse arguments ---
for arg in $argv
   switch $arg
      case '--target=*'
         set target (string replace -- '--target=' '' $arg)
      case '--publish'
         set publish true
      case '*'
         echo "Unknown argument: $arg" >&2
         echo "Usage: generate-opp-bundles.fish --target=[solidity|solana] [--publish]" >&2
         exit 1
   end
end

# --- Validate target ---
if test -z "$target"
   echo "Error: --target is required. Must be 'solidity' or 'solana'." >&2
   exit 1
end

if not contains -- $target solidity solana
   echo "Error: --target must be 'solidity' or 'solana'. Got: '$target'" >&2
   exit 1
end

# --- Validate --publish is only used with solidity ---
if test "$publish" = true; and test "$target" != solidity
   echo "Error: --publish is only supported for --target=solidity at the moment." >&2
   exit 1
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
   --repo "file://$repo_root/libraries/opp/proto" \
   --target $target \
   --output "$repo_root/build/opp/$target/" \
   --package-name "@wireio/opp-$target-models"

if test "$publish" = true
   set -a cmd --publish
end

# --- Run ---
echo "Running wire-protobuf-bundler for target '$target'..."
echo "  repo root: $repo_root"
echo "  output:    $repo_root/build/opp/$target/"

cd $repo_root; or exit 1
$cmd

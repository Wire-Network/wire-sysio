#!/usr/bin/env bash
# Generate/check the native parser. CMake supplies the vcpkg-installed JAR and system JVM.
# Usage: ANTLR_JAR=/path/antlr-4.13.2-complete.jar [ANTLR_JAVA=/usr/bin/java] ./generate-parser.sh generate|check
set -euo pipefail
readonly antlr_version=4.13.2
readonly antlr_sha256=eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76
readonly plugin_dir="$(dirname "$(dirname "$(realpath -- "${BASH_SOURCE[0]}")")")"
readonly mode="${1:-check}"
[[ $# -le 1 && ( "$mode" == generate || "$mode" == check ) ]] || { echo 'Usage: generate-parser.sh generate|check' >&2; exit 2; }
: "${ANTLR_JAR:?Set ANTLR_JAR to the official antlr-4.13.2-complete.jar}"
readonly jar="$(realpath -- "$ANTLR_JAR")"
readonly java="${ANTLR_JAVA:-java}"
command -v "$java" >/dev/null || { echo 'Java runtime required; on Ubuntu 24.04 install openjdk-21-jre-headless from the default Ubuntu repositories.' >&2; exit 1; }
printf '%s  %s\n' "$antlr_sha256" "$jar" | sha256sum --check --strict
version_output="$("$java" -jar "$jar")"
[[ "$version_output" == *"Version $antlr_version"* ]] || { echo 'ANTLR version mismatch' >&2; exit 1; }
output_dir="$(mktemp -d)"
trap 'rm -rf -- "$output_dir"' EXIT
cd "$plugin_dir/grammar"
"$java" -jar "$jar" -Dlanguage=Cpp -package sysio::query_engine_plugin -visitor -no-listener -Xexact-output-dir -o "$output_dir" WireQuery.g4
mkdir -p "$plugin_dir/generated"
for component in Lexer Parser Visitor BaseVisitor; do
   for suffix in h cpp; do
      file="WireQuery${component}.${suffix}"
      if [[ "$mode" == generate ]]; then
         cp -- "$output_dir/$file" "$plugin_dir/generated/$file"
      else
         diff -u -- "$plugin_dir/generated/$file" "$output_dir/$file"
      fi
   done
done
echo "ANTLR $antlr_version parser $mode passed"

#!/usr/bin/env bash
# Resolve the workflow run that built a given TAG REF, for each named workflow.
#
# Usage: resolve-build-run.sh <tag> <target-sha> <workflow.yaml>...
#
# Emits one `<workflow-basename-sans-.yaml>-run-id=<id>` line per workflow on
# STDOUT, shaped to be redirected straight into "$GITHUB_OUTPUT". EVERY progress,
# diagnostic and error line goes to STDERR instead, so that redirect can never
# swallow the log or corrupt the outputs file.
#
# Environment:
#   GH_TOKEN           (required) auth for `gh api`
#   GITHUB_REPOSITORY  (required) owner/repo whose runs are queried
#   RESOLVE_ATTEMPTS   poll attempts before giving up (default 330)
#   RESOLVE_INTERVAL   seconds between attempts (default 60)
#
# The default 330 x 60s is deliberately longer than a full cold build: the asset
# workflow is dispatched at the SAME moment as the platform builds, so it must
# outlast the whole build, not merely the artifact upload that follows one. Kept
# under the 6h GitHub-hosted job ceiling.
#
# This file is kept BYTE-IDENTICAL with wire-sysio/.github/scripts/resolve-build-run.sh
# (`diff` across the two repos is the check): both repos run the same Strategy-C
# release path, and the resolver's failure modes are subtle enough that a forked
# copy would drift silently. Repo-specific bits -- which workflows to wait for,
# and how TARGET_SHA is derived -- stay at the call sites, which is exactly why
# they are arguments here.
set -euo pipefail

readonly default_attempts=330
readonly default_interval=60

die() {
   echo "::error::$*" >&2
   exit 1
}

[[ $# -ge 3 ]] || die "usage: resolve-build-run.sh <tag> <target-sha> <workflow.yaml>..."

tag="$1"
target_sha="$2"
shift 2
workflows=("$@")

[[ -n "${GH_TOKEN:-}" ]] || die "GH_TOKEN is not set"
[[ -n "${GITHUB_REPOSITORY:-}" ]] || die "GITHUB_REPOSITORY is not set"

attempts="${RESOLVE_ATTEMPTS:-$default_attempts}"
interval="${RESOLVE_INTERVAL:-$default_interval}"

# First attempt only (see below): dump the candidate runs the API actually
# returned before the filter is applied.
log_candidates=1

# Resolve ONE workflow's successful run AT THE TAG REF.
#
# The run must be a build OF THE TAG REF, not merely of the same commit: a
# master-push build sits at the identical SHA but does not carry the tag-only
# packaging behaviour, so matching it would attach the wrong artifact set.
# head_branch is the tag name for both a tag push and a workflow_dispatch on the
# tag ref -- tag-release.yaml uses the latter, because a GITHUB_TOKEN-created tag
# fires no push event -- so both events are accepted and head_branch is the
# discriminator.
resolve_run() {
   local workflow="$1" runs_json
   runs_json="$(gh api "repos/${GITHUB_REPOSITORY}/actions/workflows/${workflow}/runs?head_sha=${target_sha}&per_page=50")"
   if [[ "$log_candidates" == "1" ]]; then
      # A head_branch-semantics mismatch is the classic resolver failure. Dumping
      # the candidates on attempt ONE makes it visible in minute one instead of
      # after 5.5 hours of silent polling.
      {
         echo "--- ${workflow} candidate runs at ${target_sha} (want head_branch == ${tag}) ---"
         jq -r '.workflow_runs | length | "candidates: \(.)"' <<<"$runs_json"
         jq -c '.workflow_runs[] | {id, head_branch, event, status, conclusion}' <<<"$runs_json"
      } >&2
   fi
   jq -r --arg tag "$tag" '
      [.workflow_runs[]
        | select(.status == "completed" and .conclusion == "success")
        | select(.event == "push" or .event == "workflow_dispatch")
        | select(.head_branch == $tag)]
      | .[0].id // empty' <<<"$runs_json"
}

declare -A run_ids=()
for workflow in "${workflows[@]}"; do
   run_ids["$workflow"]=""
done

# ALL requested workflows are polled every iteration, and a resolved one is never
# re-queried: waiting for them in sequence would spend the whole budget on the
# first workflow while the second sat finished and unclaimed.
for ((attempt = 1; attempt <= attempts; attempt++)); do
   pending=0
   for workflow in "${workflows[@]}"; do
      [[ -n "${run_ids[$workflow]}" ]] || run_ids["$workflow"]="$(resolve_run "$workflow")"
      [[ -n "${run_ids[$workflow]}" ]] || pending=1
   done
   log_candidates=0
   if [[ "$pending" -eq 0 ]]; then
      break
   fi
   status=""
   for workflow in "${workflows[@]}"; do
      status+=" ${workflow%.yaml}=${run_ids[$workflow]:-pending}"
   done
   echo "Waiting for tag-ref builds of ${tag} (${target_sha}):${status} (attempt ${attempt}/${attempts})" >&2
   sleep "$interval"
done

# Emitted only after EVERY workflow resolved: a partial output set would let the
# caller download one platform's artifacts and publish an incomplete release.
for workflow in "${workflows[@]}"; do
   [[ -n "${run_ids[$workflow]}" ]] \
      || die "No successful ${workflow} run found for ${tag} (${target_sha})"
done
for workflow in "${workflows[@]}"; do
   echo "${workflow%.yaml}-run-id=${run_ids[$workflow]}"
done

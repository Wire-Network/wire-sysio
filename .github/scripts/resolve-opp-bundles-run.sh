#!/usr/bin/env bash
# Resolve the opp-bundles.yaml run that published a given TAG's OPP model
# bundles, discriminated by the tag-suffixed published-versions artifact.
#
# Usage: resolve-opp-bundles-run.sh <tag>
#
# Emits `opp-bundles-run-id=<id>` on STDOUT, shaped to be redirected straight
# into "$GITHUB_OUTPUT". EVERY progress, diagnostic and error line goes to
# STDERR instead, so that redirect can never swallow the log or corrupt the
# outputs file. (Same conventions as resolve-build-run.sh — which stays
# byte-locked with wire-cdt's copy and therefore is NOT extended for this.)
#
# tag-release.yaml dispatches opp-bundles.yaml ON the tag ref, so the run's
# head_branch is the tag name — the same discriminator resolve-build-run.sh
# uses. The run must ALSO carry the `opp-published-versions-<tag>` artifact:
# the artifact name encodes the run's own ref (opp-bundles.yaml uploads it as
# opp-published-versions-${GITHUB_REF_NAME}), so its presence proves the run
# published THIS tag's bundles and its versions file is the one to record.
#
# Environment:
#   GH_TOKEN           (required) auth for `gh api`
#   GITHUB_REPOSITORY  (required) owner/repo whose runs are queried
#   RESOLVE_ATTEMPTS   poll attempts before giving up (default 60)
#   RESOLVE_INTERVAL   seconds between attempts (default 30)
#
# The default 60 x 30s (30 min) is far smaller than resolve-build-run.sh's
# budget: the opp-bundles run takes ~10 minutes and is dispatched at the same
# moment as the platform builds, which release.yaml waits out FIRST — by the
# time this resolver runs, the opp run has usually been finished for hours.
set -euo pipefail

readonly default_attempts=60
readonly default_interval=30
readonly workflow="opp-bundles.yaml"

die() {
   echo "::error::$*" >&2
   exit 1
}

[[ $# -eq 1 ]] || die "usage: resolve-opp-bundles-run.sh <tag>"

tag="$1"

[[ -n "${GH_TOKEN:-}" ]] || die "GH_TOKEN is not set"
[[ -n "${GITHUB_REPOSITORY:-}" ]] || die "GITHUB_REPOSITORY is not set"

attempts="${RESOLVE_ATTEMPTS:-$default_attempts}"
interval="${RESOLVE_INTERVAL:-$default_interval}"
artifact_name="opp-published-versions-${tag}"

# First attempt only: dump the candidate runs the API actually returned before
# the filter is applied, so a head_branch-semantics mismatch is visible in
# minute one instead of after the whole polling budget.
log_candidates=1

# Resolve the successful opp-bundles run AT THE TAG REF carrying the
# tag-suffixed published-versions artifact.
resolve_run() {
   local runs_json candidate_ids candidate artifact_count
   runs_json="$(gh api "repos/${GITHUB_REPOSITORY}/actions/workflows/${workflow}/runs?event=workflow_dispatch&branch=${tag}&per_page=50")"
   if [[ "$log_candidates" == "1" ]]; then
      {
         echo "--- ${workflow} candidate runs at head_branch ${tag} (want artifact ${artifact_name}) ---"
         jq -r '.workflow_runs | length | "candidates: \(.)"' <<<"$runs_json"
         jq -c '.workflow_runs[] | {id, head_branch, event, status, conclusion}' <<<"$runs_json"
      } >&2
   fi
   candidate_ids="$(
      jq -r '
         [.workflow_runs[]
           | select(.status == "completed" and .conclusion == "success")]
         | .[].id' <<<"$runs_json"
   )"
   # Newest first (the API's default order): the newest successful run's
   # artifact carries the versions actually published last.
   for candidate in $candidate_ids; do
      # `gh api --jq` takes a bare jq EXPRESSION and no jq flags (`--arg` would be
      # consumed as the expression and the rest as positionals, erroring with
      # "accepts 1 arg(s)"); pipe through real jq to bind the artifact name.
      artifact_count="$(
         gh api "repos/${GITHUB_REPOSITORY}/actions/runs/${candidate}/artifacts" |
            jq -r --arg name "$artifact_name" '[.artifacts[] | select(.name == $name)] | length'
      )"
      if [[ "$artifact_count" -gt 0 ]]; then
         echo "$candidate"
         return 0
      fi
      echo "Run ${candidate} succeeded but carries no ${artifact_name} artifact; skipping" >&2
   done
   echo ""
}

run_id=""
for ((attempt = 1; attempt <= attempts; attempt++)); do
   run_id="$(resolve_run)"
   log_candidates=0
   if [[ -n "$run_id" ]]; then
      break
   fi
   echo "Waiting for the ${workflow} run of ${tag} carrying ${artifact_name} (attempt ${attempt}/${attempts})" >&2
   sleep "$interval"
done

[[ -n "$run_id" ]] \
   || die "No successful ${workflow} run with artifact ${artifact_name} found for ${tag}. Dispatch it with: gh workflow run ${workflow} --ref ${tag} -f channel=<dev|stable>"

echo "opp-bundles-run-id=${run_id}"

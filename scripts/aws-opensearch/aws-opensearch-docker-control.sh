#!/usr/bin/env bash
# Manage the local OpenSearch + Dashboards Compose stack.
set -euo pipefail

scriptPath="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
composeFile="$scriptPath/aws-opensearch-docker-compose.yaml"

usage() {
  cat <<EOF
Usage: aws-opensearch-docker-control.sh <start|stop|destroy> [-f|--follow]
  start    Create/start both containers; reuse the existing data volume.
  stop     Stop both containers, keeping the containers and data.
  destroy  Remove both containers and their network, keeping the data.
  -f       Follow both container logs after start; ignored by stop/destroy.
           Ctrl-C stops following logs and leaves the containers running.

UI: http://localhost:5601; API: http://localhost:9200 (security disabled).
Host ports: 5601, 9200, 9300, 9600, 9650, published on all interfaces.
Compose file: $composeFile
Persistent data volume: aws-opensearch-data.
OPENSEARCH_VERSION selects the version of both images (default: 3.8.0).
OPENSEARCH_IMAGE / OPENSEARCH_DASHBOARDS_IMAGE override individual images;
use matching versions when overriding images.
EOF
}

followLogs=false
showHelp=false
positionalArgs=()
while (( $# > 0 )); do
  case "$1" in
    -f|--follow) followLogs=true ;;
    -h|--help) showHelp=true ;;
    --)
      shift
      positionalArgs+=("$@")
      break
      ;;
    -*)
      printf 'Error: unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
    *) positionalArgs+=("$1") ;;
  esac
  shift
done

if (( ${#positionalArgs[@]} > 1 )); then
  usage >&2
  exit 2
fi
if [[ "$showHelp" == true ]]; then
  usage
  exit 0
fi
if (( ${#positionalArgs[@]} != 1 )); then
  usage >&2
  exit 2
fi

action="${positionalArgs[0]}"
case "$action" in
  start|stop|destroy) ;;
  *) usage >&2; exit 2 ;;
esac

if ! command -v docker >/dev/null 2>&1; then
  echo 'Error: Docker is not installed or is not on PATH.' >&2
  exit 1
fi
docker compose version >/dev/null

if [[ ! -f "$composeFile" ]]; then
  printf 'Error: Compose file not found: %s\n' "$composeFile" >&2
  exit 1
fi

# Fix the project and file so this works from any working directory.
composeArgs=(--project-name aws-opensearch --file "$composeFile")
case "$action" in
  start)
    docker compose "${composeArgs[@]}" config --quiet

    # This external volume survives every lifecycle action, including destroy.
    docker volume create aws-opensearch-data >/dev/null
    docker compose "${composeArgs[@]}" up --detach

    printf '%s\n' \
      'UI:  http://localhost:5601 (no login)' \
      'API: http://localhost:9200 (no credentials or TLS)' \
      'Ports 5601, 9200, 9300, 9600, and 9650 are published on all host interfaces.' \
      'Startup can take a minute; Docker health status reports readiness.'

    if [[ "$followLogs" == true ]]; then
      exec docker compose "${composeArgs[@]}" logs --follow --tail 100
    fi

    printf '\ndocker compose --project-name aws-opensearch --file %q logs --follow --tail 100\n' "$composeFile"
    ;;
  stop)
    docker compose "${composeArgs[@]}" stop
    echo 'Stopped OpenSearch and Dashboards; containers and data are preserved.'
    ;;
  destroy)
    docker compose "${composeArgs[@]}" down
    echo 'Removed OpenSearch and Dashboards containers and network; aws-opensearch-data is preserved.'
    ;;
esac

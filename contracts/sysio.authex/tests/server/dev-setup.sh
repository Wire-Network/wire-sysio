#!/usr/bin/env bash
set -euo pipefail
set -x

export KIOD_ROOT="$HOME/.config/wire/kiod"
export WALLET_DIR="$KIOD_ROOT/data"
export SECRETS_DIR="$KIOD_ROOT/secrets"

ACCT_AUTHEX="sysio.authex"
ACCT_NODE_OWNER_1="node1"
DEFAULT_PUBLIC_KEY="SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"

PUBLIC_KEY=""
PUB_K1=""
BUILD_DIR=""
PRIVATE_KEY=""
LINK_USERNAME=""
NODEOP_URL="http://localhost:8888"

usage() {
   echo "Usage: $0 -B <build-dir> [--url <nodeop-url>] [--private-key <private-key>] [--link-username <link-username>]"
   echo ""
   echo "  -B <build-dir>              Required. Path to build folder (absolute or relative to repo root)."
   echo "  --url <nodeop-url>          Optional. URL of the node operator."
   echo "  --private-key <private-key>  Optional. K1 private key to derive the public key from."
   echo "  --link-username <link-username>  Optional. Username to link."
   exit 1
}

while [[ $# -gt 0 ]]; do
   case "$1" in
      -B)
         [[ $# -lt 2 ]] && usage
         BUILD_DIR="$2"
         shift 2
         ;;
      --private-key)
         [[ $# -lt 2 ]] && usage
         PRIVATE_KEY="$2"
         shift 2
         ;;
       --link-username)
         [[ $# -lt 2 ]] && usage
         LINK_USERNAME="$2"
         shift 2
         ;;
       --url)
         [[ $# -lt 2 ]] && usage
         NODEOP_URL="$2"
         shift 2
         ;;
      *)
         echo "Unknown option: $1"
         usage
         ;;
   esac
done

export NODEOP_URL

[[ -z "$BUILD_DIR" ]] && { echo "Error: -B <build-dir> is required."; usage; }

# Resolve build dir to absolute path
if [[ "$BUILD_DIR" != /* ]]; then
   SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
   REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
   BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

CLIO="$BUILD_DIR/programs/clio/clio"
if [[ ! -x "$CLIO" ]]; then
   echo "Error: clio not found at $CLIO"
   exit 1
fi

die() {
    echo "$1"
    exit 255
}

run() {
    eval "$@" || die "An error occurred while executing: $*"
}

run-clio() {
    ${CLIO} --wallet-dir "$WALLET_DIR" -u "$NODEOP_URL" $@ || die "An error occurred while executing: $*"
}


# Derive public key from private key, or use defaults
if [[ -n "$PRIVATE_KEY" ]]; then
   CONVERT_OUTPUT=$("${CLIO}" convert k1_private_key --to-console --private-key "$PRIVATE_KEY")
   PUBLIC_KEY=$(echo "$CONVERT_OUTPUT" | sed -n '2s/^Public key: //p')
   if [[ -z "$PUBLIC_KEY" ]]; then
      echo "Error: failed to extract public key from clio output:"
      echo "$CONVERT_OUTPUT"
      exit 1
   fi
else
  PUBLIC_KEY="$DEFAULT_PUBLIC_KEY"
fi

# Now convert all pub key formats from public key
CONVERT_OUTPUT=$(run-clio convert k1_public_key "${PUBLIC_KEY}")
PUB_K1=$(echo "$CONVERT_OUTPUT" | sed -n '2s/^Public key: //p')

# CREATE ACCOUNTS
create_account() {
   local name="$1"
   run-clio system newaccount sysio "$name" "$PUBLIC_KEY" "$PUBLIC_KEY" -p sysio@active
}

create_account_if_not_exists() {
   local name="$1"
   if ! run-clio get account "$name" > /dev/null 2>&1; then
      create_account "$name"
   else
      echo "Account $name already exists, skipping creation."
   fi
}

create_account_if_not_exists "${ACCT_NODE_OWNER_1}"
create_account_if_not_exists "${LINK_USERNAME}"

${CLIO} push action sysio.roa forcereg '["'"${ACCT_NODE_OWNER_1}"'", 1]' -p sysio.roa@active || die "forcereg failed"
${CLIO} push action sysio.roa addpolicy '["'"${LINK_USERNAME}"'", "'"${ACCT_NODE_OWNER_1}"'", "200.0100 SYS", "0.0000 SYS", "200.1000 SYS", 0, 0]' -p "${ACCT_NODE_OWNER_1}@active" || die "addpolicy failed"

echo "Setup complete. Account ${LINK_USERNAME} is ready to use with authex. The users private key is the same as the one provided: ${PRIVATE_KEY}"

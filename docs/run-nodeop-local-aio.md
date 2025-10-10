# Run NodeOp locally All-In-One (AIO)

## Quick start

> NOTE: The files in the `<target-root>` directory are overwritten AND you must have `fish` installed.

The script file being used is [../scripts/](../scripts/dev-chain-run-nodeop.fish)

```shell
# USAGE 
# ./scripts/dev-chain-run-nodeop.fish <cmake-build-root> <target-root>

# EXAMPLE (my target root is /opt/wire-network/chain-001) 
./scripts/dev-chain-run-nodeop.fish \
  $PWD/build/debug \
  /opt/wire-network/chain-001
```


## Detailed breakdown

Below is a script that does the following
- Creates a fresh config, wallet, genesis & keys in `TARGET_ROOT`
- Launches `kiod` to ensure the correct wallet is used
- Starts `nodeop` with the correct config

> NOTE: This script is for local development only. 
> The script runs in FISH shell only.

 


```shell
# FISH SHELL
# SETUP
# Add build bin folder to PATH
fish_add_path <cmake-build-root>/bin

# Make sure to stop any running kiod processes 
# before `chain-configure` is run.
pkill -9 kiod || true

# Any directory works here
export TARGET_ROOT=<target-root>
export CONFIG_ROOT=$TARGET_ROOT/config
# Only template currently is `aio` (all-in-one)
export CONFIG_TEMPLATE=aio

# GENERATE KEYS & CONFIG
# NOTE: This will overwrite any existing files in $TARGET_ROOT
clio chain-configure \
  --target=$TARGET_ROOT \
  --template=$CONFIG_TEMPLATE \
  --overwrite

# GET THE WALLET PASSWORD & KEYS
export WALLET_PW=(cat $TARGET_ROOT/secrets/sysio_wallet_pw.txt)
export SYS_PUBLIC_KEY=(cat $TARGET_ROOT/secrets/sysio_key.txt | grep Public | grep -Eo '(SYS.*)')
export SYS_PRIVATE_KEY=(cat $TARGET_ROOT/secrets/sysio_key.txt | grep Private | grep -oP 'Private\\skey:\\s\K([a-zA-Z0-9]+)$')

# START KIOD TO ENSURE THE CORRECT WALLET IS USED
pkill -9 kiod || true
sleep 1
echo "Starting kiod with wallet dir $TARGET_ROOT/wallet. Logs at $LOGS_ROOT/kiod.log"
kiod --wallet-dir $TARGET_ROOT/wallet &> $LOGS_ROOT/kiod.log &
sleep 1

# UNLOCK THE WALLET
clio wallet unlock --name default --password $WALLET_PW

# START THE NODE
nodeop --config-dir $CONFIG_ROOT \
  --data-dir "$TARGET_ROOT/data" \
  --genesis-json "$CONFIG_ROOT/genesis.json" \
  --contracts-console \
  --signature-provider "$SYS_PUBLIC_KEY=KEY:$SYS_PRIVATE_KEY"
```
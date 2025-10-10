#!/usr/bin/env fish

if not test (count $argv) -eq 2
		echo "Usage: dev-chain-run-nodeop.fish <cmake-build-root> <target-root>"
		exit 1
end

set CMAKE_BUILD_ROOT $argv[1]
set TARGET_ROOT $argv[2]
set CONFIG_ROOT $TARGET_ROOT/config
set LOGS_ROOT $TARGET_ROOT/logs

if not test -d $CMAKE_BUILD_ROOT/bin
		echo "Error: CMake build bin not found at $CMAKE_BUILD_ROOT/bin"
		exit 1
end


set REQ_FILES $TARGET_ROOT/secrets/sysio_wallet_pw.txt \
	$TARGET_ROOT/wallet/default.wallet \
	$TARGET_ROOT/secrets/sysio_key.txt \
	$CONFIG_ROOT/genesis.json \
	$CONFIG_ROOT/config.ini

for f in $REQ_FILES
	if not test -f $f
		echo "Error: Required Files are:
      $REQ_FILES

      $f not found
		"
		exit 1
	end
end

mkdir -p $LOGS_ROOT

# Add build bin folder to PATH
fish_add_path $CMAKE_BUILD_ROOT/bin


# START/STOP KIOD TO ENSURE THE CORRECT WALLET IS USED
pkill -9 kiod || true
sleep 1
echo "Starting kiod with wallet dir $TARGET_ROOT/wallet. Logs at $LOGS_ROOT/kiod.log"
kiod --wallet-dir $TARGET_ROOT/wallet &> $LOGS_ROOT/kiod.log &
sleep 1

# GET THE WALLET PASSWORD & KEYS
set WALLET_PW (cat $TARGET_ROOT/secrets/sysio_wallet_pw.txt)
set SYS_PUBLIC_KEY (cat $TARGET_ROOT/secrets/sysio_key.txt | grep Public | grep -Eo '(SYS.*)')
set SYS_PRIVATE_KEY (cat $TARGET_ROOT/secrets/sysio_key.txt | grep Private | grep -oP 'Private\\skey:\\s\K([a-zA-Z0-9]+)$')

# UNLOCK THE WALLET
clio wallet unlock --name default --password $WALLET_PW

# START THE NODE
nodeop --config-dir $CONFIG_ROOT \
  --data-dir "$TARGET_ROOT/data" \
  --genesis-json "$CONFIG_ROOT/genesis.json" \
  --contracts-console \
  --signature-provider "$SYS_PUBLIC_KEY=KEY:$SYS_PRIVATE_KEY"
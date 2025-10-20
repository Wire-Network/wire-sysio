#!/usr/bin/env fish

# Usage:
#   dev-chain-run-nodeop.fish <cmake-build-root> <target-root> [<run-prefix>]
#
# When is provided, this script will generate two CLion run configurations
# into .run/ using the provided <run-prefix>:
#   1) <run-prefix>-clio-unlock-wallet
#   2) <run-prefix>-nodeop-producer (configured to run the clio unlock before launch)

if test (count $argv) -lt 2
    echo "Usage: dev-chain-run-nodeop.fish <cmake-build-root> <target-root> [--generate-run <run-prefix>]"
    exit 1
end

set CMAKE_BUILD_ROOT $argv[1]
set TARGET_ROOT $argv[2]
set CONFIG_ROOT $TARGET_ROOT/config
set LOGS_ROOT $TARGET_ROOT/logs

set GENERATE_RUN 0
set RUN_PREFIX ""

# Check for <run-prefix>
if test (count $argv) -ge 3
    if test (count $argv) -gt 3
        echo "Error: <run-prefix> is the final available positional argument"
        exit 1
    end

    set GENERATE_RUN 1
    set RUN_PREFIX $argv[3]
end

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

# Add build bin folder to PATH
fish_add_path $CMAKE_BUILD_ROOT/bin

# GET THE WALLET PASSWORD & KEYS
set WALLET_PW (cat $TARGET_ROOT/secrets/sysio_wallet_pw.txt)
set SYS_PUBLIC_KEY (cat $TARGET_ROOT/secrets/sysio_key.txt | grep Public | grep -Eo '(SYS.*)')
set SYS_PRIVATE_KEY (cat $TARGET_ROOT/secrets/sysio_key.txt | grep Private | grep -oP 'Private\skey:\s\K([a-zA-Z0-9]+)$')

if test $GENERATE_RUN -eq 1
    # Generate CLion run configuration XMLs under .run/
    set RUN_DIR ".run"
    mkdir -p $RUN_DIR

    set CLIO_NAME "$RUN_PREFIX-clio-unlock-wallet"
    set NODEOP_NAME "$RUN_PREFIX-nodeop-producer"

    # Active CLion profile name (from workspace context); adjust if needed
    set CLION_PROFILE ""

    set PROJECT_NAME "wire-sysio"

    # Construct PROGRAM_PARAMS for both configs
    set CLIO_PARAMS "wallet unlock --name default --password $WALLET_PW"

    set NODEOP_PARAMS "--config-dir $CONFIG_ROOT --data-dir $TARGET_ROOT/data --genesis-json $CONFIG_ROOT/genesis.json --contracts-console --signature-provider '$SYS_PUBLIC_KEY=KEY:$SYS_PRIVATE_KEY'"

    # Write clio run config
    set CLIO_FILE "$RUN_DIR/$CLIO_NAME.run.xml"
    printf '<component name="ProjectRunConfigurationManager">\n' > $CLIO_FILE
    printf '  <configuration default="false" name="%s" type="CMakeRunConfiguration" folderName="chains" factoryName="Application" PROGRAM_PARAMS="%s" REDIRECT_INPUT="false" ELEVATE="false" USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="true" WORKING_DIR="file://$ProjectFileDir$" PASS_PARENT_ENVS_2="true" PROJECT_NAME="%s" TARGET_NAME="clio" CONFIG_NAME="%s" RUN_TARGET_PROJECT_NAME="%s" RUN_TARGET_NAME="clio">\n' $CLIO_NAME $CLIO_PARAMS $PROJECT_NAME $CLION_PROFILE $PROJECT_NAME >> $CLIO_FILE
    printf '    <method v="2">\n' >> $CLIO_FILE
    printf '      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask" enabled="true" />\n' >> $CLIO_FILE
    printf '    </method>\n' >> $CLIO_FILE
    printf '  </configuration>\n' >> $CLIO_FILE
    printf '</component>\n' >> $CLIO_FILE

    # Write nodeop run config, attempting to add a before-run task to run the clio config
    set NODEOP_FILE "$RUN_DIR/$NODEOP_NAME.run.xml"
    printf '<component name="ProjectRunConfigurationManager">\n' > $NODEOP_FILE
    printf '  <configuration default="false" name="%s" type="CMakeRunConfiguration" folderName="chains" factoryName="Application" PROGRAM_PARAMS="%s" REDIRECT_INPUT="false" ELEVATE="false" USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="true" WORKING_DIR="file://$ProjectFileDir$" PASS_PARENT_ENVS_2="true" PROJECT_NAME="%s" TARGET_NAME="nodeop" CONFIG_NAME="%s" RUN_TARGET_PROJECT_NAME="%s" RUN_TARGET_NAME="nodeop">\n' $NODEOP_NAME $NODEOP_PARAMS $PROJECT_NAME $CLION_PROFILE $PROJECT_NAME >> $NODEOP_FILE
    printf '    <method v="2">\n' >> $NODEOP_FILE
    # Build before run
    printf '      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask" enabled="true" />\n' >> $NODEOP_FILE
    # Attempt to reference the other run configuration before run
    printf '      <option name="RunConfigurationTask" enabled="true" run_configuration_name="%s" run_configuration_type="CMakeRunConfiguration"/>\n' $CLIO_NAME >> $NODEOP_FILE
    printf '    </method>\n' >> $NODEOP_FILE
    printf '  </configuration>\n' >> $NODEOP_FILE
    printf '</component>\n' >> $NODEOP_FILE

    echo "Generated run configurations:" >&2
    echo "  $CLIO_FILE" >&2
    echo "  $NODEOP_FILE" >&2
    exit 0
end

# Normal execution path: perform kiod/clio/nodeop orchestration
mkdir -p $LOGS_ROOT

# START/STOP KIOD TO ENSURE THE CORRECT WALLET IS USED
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
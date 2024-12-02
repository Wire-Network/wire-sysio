#!/bin/sh
#
# EOS Mechanics actions script.
#

API_URL="http://127.0.0.1:8888"
WALLET_URL="http://127.0.0.1:8900/"
WALLET_NAME="eosmechanics"
WALLET_PASS="password to eosmechanics wallet with benchmark permission key"
PATH="$PATH:/usr/local/bin:/usr/local/sysio/bin/"

date
clio -u "$API_URL" --wallet-url "$WALLET_URL" wallet unlock -n "$WALLET_NAME" --password "$WALLET_PASS"
clio -u "$API_URL" --wallet-url "$WALLET_URL" push action eosmechanics cpu '' -p eosmechanics@benchmark -f
#clio -u "$API_URL" --wallet-url "$WALLET_URL" push action eosmechanics ram '' -p eosmechanics@benchmark -f
clio -u "$API_URL" --wallet-url "$WALLET_URL" wallet stop

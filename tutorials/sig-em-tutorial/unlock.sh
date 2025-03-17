#!/bin/bash

# Read the password from the wallet.pass file
PASSWORD=$(cat ../bios-boot-tutorial/wallet.pwd)

# Use the password to unlock the wallet
clio wallet unlock --password "$PASSWORD"
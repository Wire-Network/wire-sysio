#!/usr/bin/env bash

echo "[+] Installing build deps (sudo)…"
sudo apt-get update
sudo apt-get install -y \
 build-essential cmake ninja-build git python3-dev \
 zlib1g-dev libffi-dev libedit-dev libxml2-dev \
 libncurses-dev libtinfo-dev libzstd-dev \
 libssl-dev ccache \
 gcc-10 g++-10

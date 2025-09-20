#! /usr/bin/env bash
set -e

# This cleanup script removes the references to
# `numeric_ublas` from the `accumulators` component

echo "Running post-install script for boost: '${1}' '${2}'"
installPrefix=${1}
ver=${2}

if [ -z "${installPrefix}" ] || [ -z "${ver}" ] || [ ! -d "${installPrefix}" ]; then
    echo "Error: installPrefix or version not provided or installPrefix does not exist."
    exit 1
fi

cmakeLibDir="${installPrefix}/lib/cmake"
cmakeDebugLibDir="${installPrefix}/debug/lib/cmake"

for libDir in "${cmakeLibDir}" "${cmakeDebugLibDir}"; do
    echo "CMake config dir: ${cmakeLibDir}"
    if [ ! -d "${libDir}" ]; then
        echo "Warning: CMake config dir does not exist: ${libDir}"
        continue
    fi

    pushd "${libDir}"
    cd "boost_accumulators-${ver}"
    sed -i '/find_dependency(boost_numeric_ublas .* EXACT)/d' boost_accumulators-config.cmake
    popd

done


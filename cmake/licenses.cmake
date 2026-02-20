# Wire-Sysio license
configure_file(${CMAKE_SOURCE_DIR}/LICENSE.md licenses/sysio/LICENSE.md COPYONLY)

# In-tree dependency licenses
configure_file(${CMAKE_SOURCE_DIR}/libraries/wasm-jit/LICENSE licenses/sysio/LICENSE.wavm COPYONLY)

# vcpkg dependency licenses
# Collect copyright files from all vcpkg-installed packages into the licenses directory.
# vcpkg convention: each package places a copyright file in share/<pkg>/copyright.
set(_vcpkg_share_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

# Packages that use the standard "copyright" filename
set(_vcpkg_copyright_packages
   bls12-381
   bn256
   boost
   boringssl-custom
   bzip2
   catch2
   civetweb
   cli11
   cryptopp
   curl
   ethash
   fmt
   gmp
   gsl-lite
   gtest
   liblzma
   libsodium
   llvm
   magic-enum
   nlohmann-json
   prometheus-cpp
   rapidjson
   secp256k1-internal
   softfloat
   spdlog
   zlib
   zstd
)

foreach(_pkg IN LISTS _vcpkg_copyright_packages)
   set(_src "${_vcpkg_share_dir}/${_pkg}/copyright")
   if(EXISTS "${_src}")
      configure_file("${_src}" "licenses/sysio/LICENSE.${_pkg}" COPYONLY)
   else()
      message(WARNING "License file not found for vcpkg package: ${_pkg} (expected ${_src})")
   endif()
endforeach()

# wire-sys-vm uses LICENSE.md instead of copyright
set(_sys_vm_license "${_vcpkg_share_dir}/wire-sys-vm/LICENSE.md")
if(EXISTS "${_sys_vm_license}")
   configure_file("${_sys_vm_license}" "licenses/sysio/LICENSE.wire-sys-vm" COPYONLY)
else()
   message(WARNING "License file not found for vcpkg package: wire-sys-vm (expected ${_sys_vm_license})")
endif()
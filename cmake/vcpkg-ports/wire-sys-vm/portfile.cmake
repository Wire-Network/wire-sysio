vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Wire-Network/eos-vm
    REF 1589ff7a9890d946b27c9e545944c8e7987a8cab
    PATCHES clang-libcxx.patch
    SHA512 91a54cf7a43086639a3a172e18032d9ec80a7bdc06b44f98a3f91c46919427c00e37324bab70e213c6764bc7bab390774ef3fb80eaa587284d1a86fe6d0ddc06
)

vcpkg_check_features(OUT_FEATURE_OPTIONS OPTIONS
    INVERTED_FEATURES
        softfloat-skip-install ENABLE_SOFTFLOAT_INSTALL
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
    -DENABLE_INSTALL=ON
    -DENABLE_TESTS=OFF
    -DENABLE_SPEC_TESTS=OFF
    ${OPTIONS}
)

vcpkg_cmake_install()

# vcpkg_cmake_config_fixup(CONFIG_PATH "share/${PORT}" PACKAGE_NAME ${PORT})

vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(GLOB_RECURSE debug_contents "${CURRENT_PACKAGES_DIR}/debug/*")
if(NOT debug_contents)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")

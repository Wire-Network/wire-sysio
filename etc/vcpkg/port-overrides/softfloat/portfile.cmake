vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Wire-Network/berkeley-softfloat-3
    REF d2f34016470318b56673c34727c4b9f72a725f34
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DBUILD_TESTING=OFF
        -DCMAKE_INSTALL_FULL_INCLUDEDIR=${CURRENT_PACKAGES_DIR}/include
        -DCMAKE_INSTALL_FULL_LIBDIR=${CURRENT_PACKAGES_DIR}/lib
)

vcpkg_cmake_install()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/softfloatConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/softfloat/softfloatConfig.cmake"
    @ONLY
)

vcpkg_fixup_pkgconfig()

vcpkg_copy_pdbs()
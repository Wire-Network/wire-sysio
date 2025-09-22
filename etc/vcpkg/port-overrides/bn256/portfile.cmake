vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/Wire-Network/bn256.git
  REF 34c90583fba83f3dc385eab3415e61754aa309fc
  SHA512 34c90583fba83f3dc385eab3415e61754aa309fc
)


vcpkg_execute_required_process(
  COMMAND sed -i "/third-party/d" CMakeLists.txt
  WORKING_DIRECTORY ${SOURCE_PATH}
  LOGNAME bn256-remove-thirdparty
)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-10
  -DCMAKE_C_COMPILER=/usr/bin/gcc-10
  -DBN256_ENABLE_TEST=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/cmake/${PORT}")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/lib/cmake/${PORT}")

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/bn256Config.cmake.in"
  "${CURRENT_PACKAGES_DIR}/lib/cmake/${PORT}/bn256Config.cmake"
  @ONLY
)


vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/${PORT}")
# PACKAGE_NAME ${PORT}

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")


#file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
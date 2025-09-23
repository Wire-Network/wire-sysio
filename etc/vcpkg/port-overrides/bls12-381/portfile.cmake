vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/AntelopeIO/bls12-381.git
  REF 6b718d127a2b6095593a85622b4e3f85c3fbbcf6
  SHA512 6b718d127a2b6095593a85622b4e3f85c3fbbcf6
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "share/${PORT}" PACKAGE_NAME ${PORT})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

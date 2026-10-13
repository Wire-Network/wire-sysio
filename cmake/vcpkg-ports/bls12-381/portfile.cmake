vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO AntelopeIO/bls12-381
  REF 6b718d127a2b6095593a85622b4e3f85c3fbbcf6
  PATCHES include-vector.patch
  SHA512 7033734e2f740aeaa268e8bdee0b1a5f0839095fddcc4bedbdfea1ece0ab6dd47008302646ae90a1bec176d64a47967de1fc7a2ac40f8d72bf57e2d3397c3e84
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "share/${PORT}" PACKAGE_NAME ${PORT})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

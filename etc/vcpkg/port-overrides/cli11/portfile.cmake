vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/AntelopeIO/CLI11.git
  REF f325cc66b40cf05e35b457561e0fa1adfedfc973
  HEAD_REF leap-util-blocklog-refactor
)

vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  -DCLI11_SINGLE_FILE=ON
  -DCLI11_BUILD_TESTS=OFF
  -DCLI11_BUILD_EXAMPLES=OFF
  -DCLI11_BUILD_DOCS=OFF
)

vcpkg_cmake_install()
if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/cmake/CLI11")
  vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CLI11)
elseif(EXISTS "${CURRENT_PACKAGES_DIR}/share/CLI11")
  vcpkg_cmake_config_fixup(CONFIG_PATH share/CLI11)
else()
  message(NOTICE "[CLI11] Could not find expected CMake config path for fixup; package config files may be missing.")
endif()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug)

file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
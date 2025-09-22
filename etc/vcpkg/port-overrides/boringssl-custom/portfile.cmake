set(BORINGSSL_GIT_URL "https://github.com/Wire-Network/boringssl-custom.git")
set(BORINGSSL_COMMIT 15500a39d874c39aa8e8adf7367a9c6a52753b60)

# FIND GIT EXECUTABLE
find_program(GIT git REQUIRED)
find_program(GO go REQUIRED)
find_program(PY3 python3 REQUIRED)

# SETUP CLONE DIRECTORY
set(BORINGSSL_CLONE_DIR "${DOWNLOADS}/boringssl-src-${BORINGSSL_COMMIT}")

# CHECK IF CLONE ALREADY EXISTS, OTHERWISE CLONE IT
if(NOT EXISTS "${BORINGSSL_CLONE_DIR}/.fetched")
  message(STATUS "[boringssl] Cloning super-project (with submodules) @ ${BORINGSSL_COMMIT}")
  file(REMOVE_RECURSE "${BORINGSSL_CLONE_DIR}")
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" clone --recurse-submodules --progress "${BORINGSSL_GIT_URL}" "${BORINGSSL_CLONE_DIR}"
    WORKING_DIRECTORY "${DOWNLOADS}"
    LOGNAME boringssl-git-clone
  )
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" checkout ${BORINGSSL_COMMIT}
    WORKING_DIRECTORY "${BORINGSSL_CLONE_DIR}"
    LOGNAME boringssl-git-checkout
  )
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" submodule update --init --recursive --jobs ${VCPKG_CONCURRENCY}
    WORKING_DIRECTORY "${BORINGSSL_CLONE_DIR}"
    LOGNAME boringssl-git-submodule-update
  )
  file(WRITE "${BORINGSSL_CLONE_DIR}/.fetched" "commit=${BORINGSSL_COMMIT}\n")
else()
  message(STATUS "[boringssl] Using cached clone in ${BORINGSSL_CLONE_DIR}")
endif()

# COPY TO BUILDTREES WORKING SOURCE DIRECTORY
set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/boringssl-${BORINGSSL_COMMIT}")
if(EXISTS "${SOURCE_PATH}")
  file(REMOVE_RECURSE "${SOURCE_PATH}")
endif()
file(MAKE_DIRECTORY "${SOURCE_PATH}/src")
file(COPY "${BORINGSSL_CLONE_DIR}/" DESTINATION "${SOURCE_PATH}/src/")

vcpkg_execute_required_process(
  COMMAND ${PY3} ./src/util/generate_build_files.py cmake
  WORKING_DIRECTORY ${SOURCE_PATH}
  LOGNAME boringssl-custom-generate-build-files
)

file(READ "${CMAKE_CURRENT_LIST_DIR}/boringssl-customCMakeLists.txt.cmake" BORINGSSL_CUSTOM_CMAKELISTS_TXT)
file(APPEND "${SOURCE_PATH}/CMakeLists.txt" "${BORINGSSL_CUSTOM_CMAKELISTS_TXT}")

# NOW CONFIGURE AND INSTALL USING CMAKE
vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  -DCMAKE_C_COMPILER=/usr/bin/gcc-10
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-10
  -DOPENSSL_EXPORT=1
  -DBORINGSSL_IMPLEMENTATION=1
)

# INSTALL
vcpkg_cmake_install()

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/boringssl-customConfig.cmake.in"
  "${CURRENT_PACKAGES_DIR}/share/boringssl-custom/boringssl-customConfig.cmake"
  @ONLY
)

# CMAKE CONFIG FIXUP FOR ``boringssl-custom``
#if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/cmake/boringssl-custom")
#  vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/boringssl-custom)
#elseif(EXISTS "${CURRENT_PACKAGES_DIR}/share/boringssl-custom")
#  vcpkg_cmake_config_fixup(CONFIG_PATH share/boringssl-custom)
#else()
#  message(NOTICE "[boringssl] Could not find expected CMake config path for fixup; package config files may be missing.")
#endif()

## COPY LICENSE
#file(INSTALL ${SOURCE_PATH}/LICENSE_1_0.txt DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

# COPY PDB FILES
vcpkg_copy_pdbs()

#file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug)


set(BOOST_GIT_URL "https://github.com/boostorg/boost.git")
set(BOOST_COMMIT 564e2ac16907019696cdaba8a93e3588ec596062)

# FIND GIT EXECUTABLE
find_program(GIT git REQUIRED)

# SETUP CLONE DIRECTORY
set(BOOST_CLONE_DIR "${DOWNLOADS}/boost-src-${BOOST_COMMIT}")

# CHECK IF CLONE ALREADY EXISTS, OTHERWISE CLONE IT
if(NOT EXISTS "${BOOST_CLONE_DIR}/.fetched")
  message(STATUS "[boost] Cloning super-project (with submodules) @ ${BOOST_COMMIT}")
  file(REMOVE_RECURSE "${BOOST_CLONE_DIR}")
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" clone --recurse-submodules --progress "${BOOST_GIT_URL}" "${BOOST_CLONE_DIR}"
    WORKING_DIRECTORY "${DOWNLOADS}"
    LOGNAME boost-git-clone
  )
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" checkout ${BOOST_COMMIT}
    WORKING_DIRECTORY "${BOOST_CLONE_DIR}"
    LOGNAME boost-git-checkout
  )
  vcpkg_execute_required_process(
    ALLOW_IN_DOWNLOAD_MODE
    COMMAND "${GIT}" submodule update --init --recursive --jobs ${VCPKG_CONCURRENCY}
    WORKING_DIRECTORY "${BOOST_CLONE_DIR}"
    LOGNAME boost-git-submodule-update
  )
  file(WRITE "${BOOST_CLONE_DIR}/.fetched" "commit=${BOOST_COMMIT}\n")
else()
  message(STATUS "[boost] Using cached clone in ${BOOST_CLONE_DIR}")
endif()

# COPY TO BUILDTREES WORKING SOURCE DIRECTORY
set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/boost-${BOOST_COMMIT}")
if(EXISTS "${SOURCE_PATH}")
  file(REMOVE_RECURSE "${SOURCE_PATH}")
endif()
file(COPY "${BOOST_CLONE_DIR}/" DESTINATION "${SOURCE_PATH}")

# NOW CONFIGURE AND INSTALL USING CMAKE
vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  -DBoost_USE_MULTITHREADED=ON
  -DBoost_USE_STATIC_LIBS=ON
  -DBOOST_EXCLUDE_LIBRARIES="mysql;cobalt"
  -DCMAKE_C_COMPILER=/usr/bin/gcc-10
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-10
  -DBUILD_TESTING=OFF
)

# INSTALL
vcpkg_cmake_install()

# PERFORM CONFIG FIXUP TO RELOCATE AND ADJUST BOOST CMAKE PACKAGE CONFIGURATION FILES.
if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/cmake/boost")
  vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/boost)
elseif(EXISTS "${CURRENT_PACKAGES_DIR}/share/boost")
  vcpkg_cmake_config_fixup(CONFIG_PATH share/boost)
else()
  message(NOTICE "[boost] Could not find expected CMake config path for fixup; package config files may be missing.")
endif()

# COPY LICENSE
file(INSTALL ${SOURCE_PATH}/LICENSE_1_0.txt DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

# COPY PDB FILES
vcpkg_copy_pdbs()

# POST INSTALL CMAKE CONFIG FILES
# This cleanup script removes the references to
# `numeric_ublas` from the `accumulators` component
vcpkg_execute_required_process(
  COMMAND ${CMAKE_CURRENT_LIST_DIR}/post-install-cmake-config.sh ${CURRENT_PACKAGES_DIR} 1.83.0
  WORKING_DIRECTORY ${CURRENT_PACKAGES_DIR}
  LOGNAME post-install-cmake-boost-files
)

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug)
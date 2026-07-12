# Packaging configuration for wire-sysio. Produces:
#   TGZ -- portable tarball, top-level dir `wire-sysio/` (extract to /opt ->
#          /opt/wire-sysio/bin/nodeop); build via `cpack -G TGZ` (artifact
#          wire-sysio.tar.gz) or the `package-tgz` target (versioned name)
#   DEB -- Debian/Ubuntu package under /usr; debconf prompts control systemd
#          service registration + enable/start
#   RPM -- RHEL/Fedora package under /usr; fresh install registers, enables
#          and starts the service (rpm has no interactive prompt path)
# Per-generator differences (staging mode, packaging prefix, tarball top dir)
# live in cmake/cpack-project-config.cmake.

set(CPACK_GENERATOR "TGZ")
find_program(DPKG_FOUND "dpkg")
find_program(RPMBUILD_FOUND "rpmbuild")
if(DPKG_FOUND)
   list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_FOUND)
   list(APPEND CPACK_GENERATOR "RPM")
endif()

set(CPACK_PACKAGE_VERSION "${VERSION_FULL}")

# rpmbuild rejects '-' in the Version tag, and '~' sorts pre-release suffixes
# before the final release in BOTH dpkg and rpm comparisons -- the correct
# ordering for a "-dev" suffix. Artifact file names keep VERSION_FULL as-is.
string(REPLACE "-" "~" WIRE_PACKAGE_TILDE_VERSION "${VERSION_FULL}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${WIRE_PACKAGE_TILDE_VERSION}")
set(CPACK_RPM_PACKAGE_VERSION "${WIRE_PACKAGE_TILDE_VERSION}")

set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${VERSION_FULL}")
# Artifact names carry no OS tag (wire-sysio-<version>-<arch>.*); os-release is
# still read for the dev-package LLVM dependency probe below.
if(EXISTS /etc/os-release)
   file(READ /etc/os-release OS_RELEASE LIMIT 4096)
endif()

# Fix debian package filename as it should have the format:
# <PackageName>_<VersionNumber>_<DebianArchitecture>.deb

# Find architecture using dpkg
if (DPKG_FOUND)
    execute_process(COMMAND bash -c "${DPKG_FOUND} --print-architecture"
        OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(REGEX REPLACE "^${CMAKE_PROJECT_NAME}-(.*)$" "${CMAKE_PROJECT_NAME}_\\1_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")

string(APPEND CPACK_PACKAGE_FILE_NAME "-${CMAKE_SYSTEM_PROCESSOR}")

# ── Packaged service assets (systemd unit, tmpfiles.d, logrotate policy) ──
# Literal `lib/...` on purpose (NOT ${CMAKE_INSTALL_LIBDIR}): systemd units and
# tmpfiles.d fragments live in /usr/lib/systemd/system and /usr/lib/tmpfiles.d
# on every supported distro -- never lib64/ or lib/<multiarch>/.
install(FILES "${CMAKE_SOURCE_DIR}/tools/packaging/systemd/wire-sysio-nodeop.service"
        DESTINATION lib/systemd/system COMPONENT base)
install(FILES "${CMAKE_SOURCE_DIR}/tools/packaging/systemd/wire-sysio.conf"
        DESTINATION lib/tmpfiles.d COMPONENT base)
# logrotate policies are only read from /etc/logrotate.d -- absolute on purpose.
install(FILES "${CMAKE_SOURCE_DIR}/tools/packaging/logrotate/wire-sysio-nodeop"
        DESTINATION /etc/logrotate.d COMPONENT base)

# Per-generator staging/prefix/top-dir configuration, evaluated at cpack time.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/cpack-project-config.cmake")

# Convenience target: build the portable tarball and rename it to the
# versioned artifact name. Monolithic archives derive BOTH the artifact name
# and the internal top-level directory from CPACK_PACKAGE_FILE_NAME, and the
# top dir must be plain `wire-sysio`, so `cpack -G TGZ` emits
# wire-sysio.tar.gz and this target restores the versioned name.
add_custom_target(package-tgz
   COMMAND "${CMAKE_CPACK_COMMAND}" -G TGZ
   COMMAND "${CMAKE_COMMAND}" -E rename "${CMAKE_BINARY_DIR}/wire-sysio.tar.gz"
           "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.tar.gz"
   WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
   COMMENT "Packaging ${CPACK_PACKAGE_FILE_NAME}.tar.gz (portable tarball)"
   VERBATIM)

set(CPACK_PACKAGE_CONTACT "Wire Network Foundation")
set(CPACK_PACKAGE_VENDOR "Wire Network Foundation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++ implementation of the Antelope protocol")
set(CPACK_COMPONENT_BASE_DESCRIPTION "daemon and CLI tools including ${NODE_EXECUTABLE_NAME}, ${CLI_CLIENT_EXECUTABLE_NAME}, and ${KEY_STORE_EXECUTABLE_NAME}")
set(CPACK_COMPONENT_DEV_DESCRIPTION "headers and libraries for native contract unit testing")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Wire-Network/wire-sysio")

set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "libc6 (>= 2.38), debconf (>= 0.5) | debconf-2.0"
)
set(CPACK_DEBIAN_BASE_PACKAGE_RECOMMENDS "logrotate")

set(CPACK_DEBIAN_BASE_PACKAGE_SECTION "utils")

set(CPACK_DEBIAN_PACKAGE_CONFLICTS "sysio, mandel")
set(CPACK_RPM_PACKAGE_CONFLICTS "sysio, mandel")

# Two packages per format: `wire-sysio` (daemon + CLI tools, component base)
# and `wire-sysio-dev` (headers/libs for native contract unit testing,
# component dev) -- for BOTH the DEB and RPM generators. The portable TGZ
# stays base-only (see cmake/cpack-project-config.cmake).
set(CPACK_COMPONENTS_ALL "base" "dev")

#enable per component packages for .deb; ensure main package is just "sysio", not "sysio-base", and make the dev package have "sysio-dev" at the front not the back
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_BASE_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_DEBIAN_BASE_FILE_NAME "${CPACK_DEBIAN_FILE_NAME}.deb")
string(REGEX REPLACE "^(${CMAKE_PROJECT_NAME})" "\\1-dev" CPACK_DEBIAN_DEV_FILE_NAME "${CPACK_DEBIAN_BASE_FILE_NAME}")

#deb package tooling will be unable to detect deps for the dev package. llvm is tricky since we don't know what package could have been used; try to figure it out
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "${CMAKE_PROJECT_NAME} (= ${WIRE_PACKAGE_TILDE_VERSION}), libgmp-dev, python3, python3-numpy, zlib1g-dev")
find_program(DPKG_QUERY "dpkg-query")
if(DPKG_QUERY AND OS_RELEASE MATCHES "\n?ID=\"?ubuntu" AND LLVM_CMAKE_DIR)
   execute_process(COMMAND "${DPKG_QUERY}" -S "${LLVM_CMAKE_DIR}" COMMAND cut -d: -f1 RESULT_VARIABLE LLVM_PKG_FIND_RESULT OUTPUT_VARIABLE LLVM_PKG_FIND_OUTPUT)
   if(LLVM_PKG_FIND_OUTPUT)
      string(STRIP "${LLVM_PKG_FIND_OUTPUT}" LLVM_PKG_FIND_OUTPUT)
      string(APPEND CPACK_DEBIAN_DEV_PACKAGE_DEPENDS ", ${LLVM_PKG_FIND_OUTPUT}")
   endif()
endif()
# The legacy TestHarness postinst/prerm (scripts/postinst, scripts/prerm) are
# intentionally NOT wired into the dev package: on CMake >= 3.21 the python
# dist-packages TestHarness symlink and the sysio_testing/bin programs are
# already shipped as package payload (see the root CMakeLists.txt install
# rules), and the legacy scripts reference stale spring_testing paths under
# the configure-time prefix, which breaks installation outright.

# Debconf maintainer files for the base package: dpkg requires the scripts to
# be executable (0755); `templates` stays a plain data file.
file(COPY "${CMAKE_SOURCE_DIR}/tools/packaging/debian/templates"
     DESTINATION "${CMAKE_BINARY_DIR}/packaging/debian")
foreach(script config postinst prerm postrm)
   file(COPY "${CMAKE_SOURCE_DIR}/tools/packaging/debian/${script}"
        DESTINATION "${CMAKE_BINARY_DIR}/packaging/debian"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
set(CPACK_DEBIAN_BASE_PACKAGE_CONTROL_EXTRA
    "${CMAKE_BINARY_DIR}/packaging/debian/templates;${CMAKE_BINARY_DIR}/packaging/debian/config;${CMAKE_BINARY_DIR}/packaging/debian/postinst;${CMAKE_BINARY_DIR}/packaging/debian/prerm;${CMAKE_BINARY_DIR}/packaging/debian/postrm")

# ── RPM: component packages (wire-sysio + wire-sysio-dev) ──
set(CPACK_RPM_COMPONENT_INSTALL ON)
# Explicit per-component package + artifact names (the base rpm is plain
# `wire-sysio`, no component suffix; the dev rpm mirrors the deb naming).
set(CPACK_RPM_BASE_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_RPM_DEV_PACKAGE_NAME "${CMAKE_PROJECT_NAME}-dev")
set(CPACK_RPM_BASE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.rpm")
string(REPLACE "${CMAKE_PROJECT_NAME}-" "${CMAKE_PROJECT_NAME}-dev-" CPACK_RPM_DEV_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
set(CPACK_RPM_DEV_FILE_NAME "${CPACK_RPM_DEV_FILE_NAME}.rpm")
set(CPACK_RPM_DEV_PACKAGE_REQUIRES "${CMAKE_PROJECT_NAME} = ${WIRE_PACKAGE_TILDE_VERSION}")

# Service scriptlets belong to the BASE package only -- the dev rpm must not
# touch the service.
set(CPACK_RPM_BASE_POST_INSTALL_SCRIPT_FILE   "${CMAKE_SOURCE_DIR}/tools/packaging/rpm/post.sh")
set(CPACK_RPM_BASE_PRE_UNINSTALL_SCRIPT_FILE  "${CMAKE_SOURCE_DIR}/tools/packaging/rpm/preun.sh")
set(CPACK_RPM_BASE_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/tools/packaging/rpm/postun.sh")
# Never claim ownership of distro-owned directories.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/lib/systemd;/usr/lib/systemd/system;/usr/lib/tmpfiles.d;/etc/logrotate.d;/etc/wire;/usr/share/licenses")
# Both rpms would otherwise package identical /usr/lib/.build-id links for the
# duplicated testing binaries and conflict on co-install.
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none")
set(CPACK_RPM_BASE_USER_FILELIST "%config(noreplace) /etc/logrotate.d/wire-sysio-nodeop")

#since rpm packages aren't component based, make sure description picks up more detailed description instead of just summary
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_COMPONENT_BASE_DESCRIPTION}")

# Relative install destinations + the per-generator staging configuration in
# cpack-project-config.cmake replace the old blanket DESTDIR mechanism:
# DEB/RPM stage plainly under CPACK_PACKAGING_INSTALL_PREFIX=/usr, while the
# TGZ generator flips SET_DESTDIR back on with CPACK_INSTALL_PREFIX=/ so the
# absolute /etc/logrotate.d entry stages inside the archive.
set(CPACK_SET_DESTDIR OFF)
set(CPACK_PACKAGE_RELOCATABLE OFF)

include(CPack)

# CPack per-generator configuration (CPACK_PROJECT_CONFIG_FILE).
# Evaluated by cpack once per generator, after the main CPack config.

if(CPACK_GENERATOR STREQUAL "TGZ")
   # The archive generator is monolithic: left at "ALL" it would stage every
   # install rule (including 'Unspecified' ones), not just the base component.
   # Constrain the install to the base component so the portable tarball ships
   # exactly the runtime payload. DEB and RPM must KEEP "ALL": they use
   # component packaging (CPACK_{DEB,RPM}_COMPONENT_INSTALL) to emit the
   # wire-sysio + wire-sysio-dev package pair, and constraining the field here
   # silently collapses them to a single component package.
   string(REPLACE ";ALL;/" ";base;/" CPACK_INSTALL_CMAKE_PROJECTS "${CPACK_INSTALL_CMAKE_PROJECTS}")
endif()

if(CPACK_GENERATOR STREQUAL "TGZ")
   # Portable tarball: stage via DESTDIR so the absolute /etc/logrotate.d
   # entry lands inside the archive (archive generators reject absolute
   # destinations without DESTDIR). Relative destinations resolve against
   # "/", so there is no /usr layer, and the top-level directory is plain
   # `wire-sysio` -- extracting into /opt yields /opt/wire-sysio/bin/nodeop.
   set(CPACK_SET_DESTDIR ON)
   set(CPACK_INSTALL_PREFIX "/")
   # Monolithic archives derive BOTH the artifact name and the internal
   # top-level directory from CPACK_PACKAGE_FILE_NAME, so `cpack -G TGZ`
   # emits wire-sysio.tar.gz; the `package-tgz` build target renames the
   # artifact to the versioned name afterwards.
   set(CPACK_PACKAGE_FILE_NAME "wire-sysio")
   # Portable tarball stays runtime-only; dev headers/libs are the DEB/RPM
   # wire-sysio-dev packages' concern.
   set(CPACK_COMPONENTS_ALL base)
elseif(CPACK_GENERATOR MATCHES "^(DEB|RPM)$")
   # /usr-prefixed packages. Absolute /etc destinations stage correctly with
   # plain (non-DESTDIR) staging on both dpkg and rpmbuild.
   set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
   set(CPACK_SET_DESTDIR OFF)
endif()

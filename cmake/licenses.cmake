# Wire-Sysio license
configure_file(${CMAKE_SOURCE_DIR}/LICENSE.md licenses/sysio/LICENSE.md COPYONLY)

# In-tree dependency licenses
configure_file(${CMAKE_SOURCE_DIR}/libraries/wasm-jit/LICENSE licenses/sysio/LICENSE.wavm COPYONLY)

# vcpkg dependency licenses
# Collect copyright files from all vcpkg-installed packages into the licenses directory.
# Package names are read programmatically from vcpkg.json (requires CMake 3.19+).
set(_vcpkg_share_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

file(READ "${CMAKE_SOURCE_DIR}/vcpkg.json" _vcpkg_json)
string(JSON _dep_count LENGTH "${_vcpkg_json}" "dependencies")

set(_vcpkg_packages "")
math(EXPR _dep_last "${_dep_count} - 1")
foreach(_i RANGE ${_dep_last})
   string(JSON _dep_type TYPE "${_vcpkg_json}" "dependencies" ${_i})
   if(_dep_type STREQUAL "STRING")
      string(JSON _dep_name GET "${_vcpkg_json}" "dependencies" ${_i})
   else()
      string(JSON _dep_name GET "${_vcpkg_json}" "dependencies" ${_i} "name")
   endif()
   list(APPEND _vcpkg_packages "${_dep_name}")
endforeach()

# Also discover transitive dependencies (e.g. fmt via spdlog) by scanning installed packages
file(GLOB _installed_dirs LIST_DIRECTORIES true "${_vcpkg_share_dir}/*")
foreach(_dir IN LISTS _installed_dirs)
   if(IS_DIRECTORY "${_dir}")
      get_filename_component(_pkg "${_dir}" NAME)
      # Skip vcpkg-internal helper packages
      if(NOT _pkg MATCHES "^vcpkg-")
         list(APPEND _vcpkg_packages "${_pkg}")
      endif()
   endif()
endforeach()
list(REMOVE_DUPLICATES _vcpkg_packages)

# Collect license files, trying common filenames used by vcpkg ports
foreach(_pkg IN LISTS _vcpkg_packages)
   set(_found FALSE)
   foreach(_license_name copyright LICENSE.md LICENSE LICENSE.txt)
      set(_src "${_vcpkg_share_dir}/${_pkg}/${_license_name}")
      if(EXISTS "${_src}")
         configure_file("${_src}" "licenses/sysio/LICENSE.${_pkg}" COPYONLY)
         set(_found TRUE)
         break()
      endif()
   endforeach()
   if(NOT _found)
      message(WARNING "License file not found for vcpkg package: ${_pkg}")
   endif()
endforeach()
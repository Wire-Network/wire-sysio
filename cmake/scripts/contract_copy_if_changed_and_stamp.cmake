# Helper script to copy files only if they changed, then update a stamp file
# Usage: cmake -DSRC1=file1 -DSRC2=file2 -DDST1=dest1 -DDST2=dest2 -DCOUNT=2 -DSTAMP_FILE=stamp.txt -P copy_if_changed_and_stamp.cmake

if(NOT DEFINED COUNT OR NOT DEFINED STAMP_FILE)
  message(FATAL_ERROR "COUNT and STAMP_FILE must be defined")
endif()

# Build lists from numbered variables
set(SRC_FILES "")
set(DST_FILES "")
foreach(i RANGE 1 ${COUNT})
  if(NOT DEFINED SRC${i} OR NOT DEFINED DST${i})
    message(FATAL_ERROR "SRC${i} and DST${i} must be defined")
  endif()
  list(APPEND SRC_FILES "${SRC${i}}")
  list(APPEND DST_FILES "${DST${i}}")
endforeach()

# Check if any source files differ from destination files
set(NEEDS_COPY FALSE)

foreach(SRC_FILE DST_FILE IN ZIP_LISTS SRC_FILES DST_FILES)
  if(NOT EXISTS "${DST_FILE}")
    set(NEEDS_COPY TRUE)
    message(STATUS "Destination ${DST_FILE} does not exist, will copy")
    break()
  endif()

  # Compare file content using MD5 hash
  file(MD5 "${SRC_FILE}" SRC_MD5)
  file(MD5 "${DST_FILE}" DST_MD5)
  if(NOT "${SRC_MD5}" STREQUAL "${DST_MD5}")
    set(NEEDS_COPY TRUE)
    message(STATUS "${SRC_FILE} differs from ${DST_FILE}, will copy")
    break()
  endif()
endforeach()

# Only copy and update stamp if files changed
if(NEEDS_COPY)
  message(STATUS "Copying contracts and updating stamp file")
  foreach(SRC_FILE DST_FILE IN ZIP_LISTS SRC_FILES DST_FILES)
    get_filename_component(DST_DIR "${DST_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST_DIR}")
    configure_file("${SRC_FILE}" "${DST_FILE}" COPYONLY)
    message(STATUS "  Copied ${SRC_FILE} -> ${DST_FILE}")
  endforeach()
  file(TOUCH "${STAMP_FILE}")
  message(STATUS "Updated stamp file: ${STAMP_FILE}")
else()
  message(STATUS "Contracts unchanged, skipping copy")
endif()

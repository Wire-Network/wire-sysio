find_package(protobuf CONFIG REQUIRED)

# protoc is available as a target via vcpkg
set(PROTOC_EXECUTABLE protobuf::protoc)

# -- or locate it explicitly --
# find_program(PROTOC_PROGRAM protoc
#   HINTS "${Protobuf_PROTOC_EXECUTABLE}"
#         "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/protobuf"
# )

project(secp256k1-internal)

include(GNUInstallDirs)

#the secp256k1-internal INTERFACE library is used to define some include paths & compile defs that are needed not just
# for compiling the library (where PRIVATE would have been fine), but also for the unit tests.
#add_library(secp256k1-internal INTERFACE)
add_library(secp256k1-internal STATIC
  src/secp256k1.c
  src/precomputed_ecmult.c
  src/precomputed_ecmult_gen.c
)
#target_include_directories(secp256k1-internal INTERFACE src)

target_compile_definitions(secp256k1-internal
  PRIVATE
  ENABLE_MODULE_RECOVERY=1
  ECMULT_GEN_PREC_BITS=4
  ECMULT_WINDOW_SIZE=15
  SECP256K1_STATIC=1)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
  target_compile_definitions(secp256k1-internal PRIVATE USE_ASM_X86_64=1)
endif()

# secp256k1 produces over 190 "unused-function" warnings like
# warning: ‘secp256k1_fe_normalize_weak’ declared ‘static’ but never defined [-Wunused-f unction]
# As we consider it as a system header and use it verbatim without any modifications,
# just disable the warning to avoid cluttering compile log
target_compile_options(secp256k1-internal PRIVATE -Wno-unused-function)

#add_library(secp256k1-internal STATIC
#  src/secp256k1.c
#  src/precomputed_ecmult.c
#  src/prelrecomputed_ecmult_gen.c
#)

#target_include_directories(secp256k1-internal
#  PUBLIC
#  ${CMAKE_CURRENT_LIST_DIR}/include
#)

#target_link_libraries(secp256k1 PRIVATE secp256k1-internal)

#install( TARGETS secp256k1
#  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
#  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
#  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
#)


file(GLOB HEADERS include/*.h)
install(FILES ${HEADERS} DESTINATION include)

#install(TARGETS secp256k1  EXPORT secp256k1-internal-config
#RUNTIME DESTINATION bin
#ARCHIVE DESTINATION lib
#LIBRARY DESTINATION lib
#)

install(TARGETS secp256k1-internal EXPORT secp256k1-internal-config
RUNTIME DESTINATION bin
ARCHIVE DESTINATION lib
LIBRARY DESTINATION lib
#INCLUDES DESTINATION include
)

install(
EXPORT secp256k1-internal-config
FILE secp256k1-internal-config.cmake
NAMESPACE secp256k1::
DESTINATION share/secp256k1-internal
)

add_executable(secp256k1-bench src/bench.c)
target_link_libraries(secp256k1-bench secp256k1-internal)

add_executable(secp256k1-tests src/tests.c)
target_link_libraries(secp256k1-tests secp256k1-internal)
add_test(secp256k1-tests secp256k1-tests)

add_executable(secp256k1-exhaustive-tests src/tests_exhaustive.c)
target_link_libraries(secp256k1-exhaustive-tests secp256k1-internal)
#add_test(secp256k1-exhaustive-tests secp256k1-exhaustive-tests)

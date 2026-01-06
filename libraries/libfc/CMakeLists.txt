
set(fc_sources
  src/int128.cpp
  src/int256.cpp
  src/variant.cpp
  src/exception.cpp
  src/variant_object.cpp
  src/string.cpp
  src/time.cpp
  src/mock_time.cpp
  src/utf8.cpp
  src/io/datastream.cpp
  src/io/json.cpp
  src/io/varint.cpp
  src/io/fstream.cpp
  src/io/console.cpp
  src/filesystem.cpp
  src/interprocess/file_mapping.cpp
  src/log/log_message.cpp
  src/log/logger.cpp
  src/log/appender.cpp
  src/log/console_appender.cpp
  src/log/dmlog_appender.cpp
  src/log/logger_config.cpp
  src/crypto/_digest_common.cpp
  src/crypto/aes.cpp
  src/crypto/crc.cpp
  src/crypto/city.cpp
  src/crypto/base58.cpp
  src/crypto/bigint.cpp
  src/crypto/hex.cpp
  src/crypto/sha1.cpp
  src/crypto/sha3.cpp
  src/crypto/ripemd160.cpp
  src/crypto/sha256.cpp
  src/crypto/sha224.cpp
  src/crypto/sha512.cpp
  src/crypto/ethereum/ethereum_utils.cpp
  src/crypto/elliptic_common.cpp
  src/crypto/elliptic_impl_priv.cpp
  src/crypto/elliptic_secp256k1.cpp
  src/crypto/elliptic_r1.cpp
  src/crypto/elliptic_em.cpp
  src/crypto/elliptic_webauthn.cpp
  src/crypto/elliptic_ed.cpp
  src/crypto/rand.cpp
  src/crypto/public_key.cpp
  src/crypto/private_key.cpp
  src/crypto/signature.cpp
  src/crypto/signature_provider.cpp
  src/crypto/bls_private_key.cpp
  src/crypto/bls_public_key.cpp
  src/crypto/bls_signature.cpp
  src/crypto/bls_utils.cpp
  src/crypto/modular_arithmetic.cpp
  src/crypto/blake2.cpp
  src/crypto/k1_recover.cpp
  src/network/url.cpp
  src/network/ethereum/ethereum_abi.cpp
  src/network/ethereum/ethereum_client.cpp
  src/network/ethereum/ethereum_rlp_encoder.cpp
  src/network/http/http_client.cpp
  src/network/json_rpc/json_rpc_client.cpp
  src/compress/zlib.cpp
  src/log/gelf_appender.cpp
)

file(GLOB_RECURSE fc_headers src/*.hpp src/*.h include/*.hpp include/*.h)
add_library(fc ${fc_sources} ${fc_headers})

target_include_directories(fc PUBLIC include)

# try and make this very clear that this json parser is intended only for webauthn parsing..
#set_source_files_properties(src/crypto/elliptic_webauthn.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include/fc/crypto/webauthn_json/include")

if(WIN32)
  target_link_libraries(fc PUBLIC ws2_32 mswsock userenv)
elseif(APPLE)
  find_library(security_framework Security REQUIRED)
  find_library(corefoundation_framework CoreFoundation REQUIRED)
  target_link_libraries(fc PUBLIC ${security_framework} ${corefoundation_framework})
endif()

target_link_libraries(
  fc
  PUBLIC
  fc-lite
  Boost::date_time
  Boost::chrono
  Boost::iostreams
  Boost::interprocess
  Boost::multi_index
  Boost::dll
  Boost::multiprecision
  Boost::beast
  Boost::asio
  Boost::thread
  Boost::unit_test_framework
  ethash::keccak
  Threads::Threads
  libsodium::libsodium
  GMP::gmp
  ZLIB::ZLIB
  nlohmann_json::nlohmann_json
  rapidjson
  boringssl::ssl
  boringssl::crypto
  boringssl::decrepit
  gsl-lite::gsl-lite
  secp256k1::secp256k1-internal
  bls12-381::bls12-381
  bn256::bn256
  magic_enum::magic_enum
  ${PLATFORM_SPECIFIC_LIBS}
  ${CMAKE_DL_LIBS}
)

target_compile_definitions(fc PUBLIC -DFC_AVAILABLE=1)

add_subdirectory(test)

install(
  TARGETS fc
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

install(
  DIRECTORY include/fc 
  DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR} 
)



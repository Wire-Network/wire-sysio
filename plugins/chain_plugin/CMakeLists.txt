file(GLOB HEADERS "include/sysio/chain_plugin/*.hpp")
add_library( chain_plugin
             account_query_db.cpp
             trx_finality_status_processing.cpp
             chain_plugin.cpp
             trx_retry_db.cpp
             tracked_votes.cpp
             get_info_db.cpp
             ${HEADERS} )

if(SYSIO_ENABLE_DEVELOPER_OPTIONS)
    message(WARNING "SYSIO Developer Options are enabled; these are NOT supported")
    target_compile_definitions(chain_plugin PUBLIC SYSIO_DEVELOPER)
endif()

target_link_libraries( chain_plugin producer_plugin sysio_chain custom_appbase appbase resource_monitor_plugin
        Boost::bimap Boost::process
        Boost::dll
        Boost::multiprecision )
target_include_directories( chain_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../chain_interface/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/appbase/include" "${CMAKE_CURRENT_SOURCE_DIR}/../resource_monitor_plugin/include")

add_subdirectory( test )

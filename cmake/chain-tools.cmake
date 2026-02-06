include(CMakeParseArguments)

macro(chain_target TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCE_FILES" ${ARGN})
    message(NOTICE "Creating unittest target (${TARGET}) with sources: ${ARG_SOURCE_FILES}")
    add_executable(${TARGET} ${ARG_SOURCE_FILES})

    target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(${TARGET}
            PRIVATE
            -Wl,${whole_archive_flag}
            batch_operator_plugin
            chain_api_plugin
            db_size_api_plugin
            net_api_plugin
            net_plugin
            cron_plugin
            producer_api_plugin
            prometheus_plugin
            resource_monitor_plugin
            state_history_plugin
            signature_provider_manager_plugin
            outpost_client_plugin
            outpost_ethereum_client_plugin
            outpost_solana_client_plugin
            test_control_api_plugin
            test_control_plugin
            trace_api_plugin
            chain_plugin
            appbase
            -Wl,${no_whole_archive_flag}
            ${PLUGIN_DEFAULT_DEPENDENCIES}

            http_client_plugin
            http_plugin
            net_plugin
            producer_plugin
            sysio_chain
            sysio_chain_wrap
            version

            gsl-lite::gsl-lite
            CLI11::CLI11

            ${CMAKE_DL_LIBS}
            ${PLATFORM_SPECIFIC_LIBS}


            -Wl,${build_id_flag}
    )

    copy_bin(${TARGET})
    install(
            TARGETS ${TARGET}
            RUNTIME
            DESTINATION bin
    )

endmacro()
include(CMakeParseArguments)

macro(chain_target TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCE_FILES" ${ARGN})
    message(NOTICE "Creating unittest target (${TARGET}) with sources: ${ARG_SOURCE_FILES}")
    add_executable(${TARGET} ${ARG_SOURCE_FILES})

    target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

    if (UNIX)
        if (APPLE)
            set(whole_archive_flag "-force_load")
            set(no_whole_archive_flag "")
            set(build_id_flag "")
        else ()
            set(whole_archive_flag "--whole-archive")
            set(no_whole_archive_flag "--no-whole-archive")
            set(build_id_flag "--build-id")
        endif ()
    else ()
        set(whole_archive_flag "--whole-archive")
        set(no_whole_archive_flag "--no-whole-archive")
        set(build_id_flag "")
    endif ()

    target_link_libraries(${TARGET}
            PRIVATE
            -Wl,${whole_archive_flag}
            batch_operator_plugin
            chain_api_plugin
            db_size_api_plugin
            net_api_plugin
            net_plugin
            operator_plugin
            producer_api_plugin
            prometheus_plugin
            resource_monitor_plugin
            state_history_plugin
            test_control_api_plugin
            test_control_plugin
            trace_api_plugin
            chain_plugin
            appbase
            boringssl::crypto
            boringssl::decrepit
            boringssl::ssl
            ${PLUGIN_DEFAULT_DEPENDENCIES}
            -Wl,${no_whole_archive_flag}

            http_client_plugin
            http_plugin
            net_plugin
            producer_plugin
            sysio_chain
            sysio_chain_wrap
            version
            wallet_api_plugin
            wallet_plugin


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
configure_file(__init__.py . COPYONLY)
configure_file(BasePluginArgs.py . COPYONLY)
configure_file(plugin_args_emission_test.py . COPYONLY)

set(GEN_FILES
        ChainPluginArgs.py
        HttpPluginArgs.py
        NetPluginArgs.py
        ProducerPluginArgs.py
        ResourceMonitorPluginArgs.py
        SignatureProviderManagerPluginArgs.py
        StateHistoryPluginArgs.py
        TraceApiPluginArgs.py)

add_custom_command(COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_nodeop_plugin_args_class_files.py
                   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                   DEPENDS ${NODE_EXECUTABLE_NAME}
                   OUTPUT ${GEN_FILES})

add_custom_target(GenNodeopPluginArgs ALL
                  DEPENDS ${GEN_FILES}
                  COMMENT "Generating NodeopPluginArgs files")

add_p_test(NAME plugin_args_emission_test
           COMMAND tests/PerformanceHarness/NodeopPluginArgs/plugin_args_emission_test.py)

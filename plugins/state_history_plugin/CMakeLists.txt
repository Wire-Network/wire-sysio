
plugin_target(
        state_history_plugin
        LIBRARIES
        state_history
        chain_plugin
        sysio_chain
        appbase
)

if( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11 )
   target_compile_options( state_history_plugin PRIVATE "-fcoroutines" )
endif()

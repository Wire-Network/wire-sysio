set(contract_name sysio.uwrit)
bootstrap_contract(${contract_name})

if(BUILD_SYSTEM_CONTRACTS)
    find_cdt_magic_enum()
    file(GLOB_RECURSE SOURCES src/*.cpp)
    file(GLOB_RECURSE HEADERS include/*.hpp)
    add_contract(${contract_name} ${contract_name} ${SOURCES})
    set(targets ${contract_name})

    if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
        list(APPEND targets ${contract_name}_native)
        add_native_contract(
                TARGET ${contract_name}_native
                SOURCES ${SOURCES}
                INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
                CONTRACT_CLASS "sysio::uwrit"
                HEADERS ${HEADERS}
                ABI_FILE ${CMAKE_BINARY_DIR}/contracts/${contract_name}/${contract_name}.abi
        )
    endif()

    foreach(target ${targets})
        if(NOT TARGET ${target})
            message(WARNING "Target ${target} not found, skipping include directory setup")
            continue()
        endif()

        target_include_directories(${target}
                PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../../libraries/opp/generated-cdt>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/libfc-lite/include>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.epoch/include>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include>
        )

        target_link_libraries(${target}
                INTERFACE
                magic_enum::magic_enum
        )
    endforeach()
endif()

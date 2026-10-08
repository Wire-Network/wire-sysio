set(contract_name sysio.andon)
bootstrap_contract(${contract_name})

if(BUILD_SYSTEM_CONTRACTS)
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
                CONTRACT_CLASS "sysio::andon"
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
        )

    endforeach()
endif()

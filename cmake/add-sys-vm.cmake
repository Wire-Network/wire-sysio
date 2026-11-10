# Add the pinned VM without changing Sysio's test, tool, compiler, or installation settings.
function(add_sys_vm_submodule SOURCE_DIRECTORY BINARY_DIRECTORY)
   if(NOT EXISTS "${SOURCE_DIRECTORY}/CMakeLists.txt")
      message(FATAL_ERROR "Missing sys-vm submodule. Run: git submodule update --init libraries/sys-vm")
   endif()

   # The subproject predates CMP0077; normal variables keep its options local to this function.
   set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
   set(ENABLE_TESTS OFF)
   set(ENABLE_TOOLS OFF)
   set(ENABLE_CCACHE OFF)
   set(ENABLE_PROFILE OFF)
   set(ENABLE_GPERFTOOLS OFF)
   set(ENABLE_ADDRESS_SANITIZER OFF)
   set(ENABLE_UNDEFINED_BEHAVIOR_SANITIZER OFF)
   set(ENABLE_INSTALL ON)
   set(ENABLE_SOFTFLOAT ON)
   set(ENABLE_SOFTFLOAT_INSTALL OFF)
   set(FULL_DEBUG_BUILD OFF)
   set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "${SYSVM_INSTALL_COMPONENT}")

   # cmake_dependent_option writes the shared cache even with CMP0077 NEW. Preserve the parent choices.
   set(dependent_options ENABLE_SPEC_TESTS ENABLE_FUZZ_TESTS
                         ENABLE_ADDRESS_SANITIZER ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
   foreach(option IN LISTS dependent_options)
      set(${option}_was_cached FALSE)
      if(DEFINED CACHE{${option}})
         set(${option}_was_cached TRUE)
         foreach(property VALUE TYPE HELPSTRING)
            get_property(${option}_${property} CACHE ${option} PROPERTY ${property})
         endforeach()
      endif()
   endforeach()

   # Both projects have a dependencies.cmake; resolve the VM's modules in its own source tree first.
   list(PREPEND CMAKE_MODULE_PATH "${SOURCE_DIRECTORY}/cmake")
   add_subdirectory("${SOURCE_DIRECTORY}" "${BINARY_DIRECTORY}")

   foreach(option IN LISTS dependent_options)
      if(${option}_was_cached)
         set(${option} "${${option}_VALUE}" CACHE ${${option}_TYPE} "${${option}_HELPSTRING}" FORCE)
      else()
         unset(${option} CACHE)
      endif()
   endforeach()

   # The standalone VM relies on global include paths; expose its headers to build and SDK consumers.
   target_include_directories(sys-vm SYSTEM INTERFACE
      "$<BUILD_INTERFACE:${SOURCE_DIRECTORY}/include>"
      "$<INSTALL_INTERFACE:include>")
   target_compile_features(sys-vm INTERFACE cxx_std_20)
   add_library(sys-vm::sys-vm ALIAS sys-vm)
endfunction()

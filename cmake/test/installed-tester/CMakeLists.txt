cmake_minimum_required(VERSION 3.19)
project(wire_installed_tester_probe LANGUAGES CXX)
# Configure with the installed llvm-toolchain.cmake before enabling C++.
find_package(sysio CONFIG REQUIRED)
add_sysio_test(installed_tester_probe main.cpp)

# Host tool: builds on Windows or Linux with any C++11 compiler.
#   cmake -S utils/pspconv -B build-pspconv && cmake --build build-pspconv --config Release
cmake_minimum_required(VERSION 3.8)
project(pspconv CXX C)
set(LIBRW_PLATFORM "NULL" CACHE STRING "" FORCE)
set(LIBRW_TOOLS OFF CACHE BOOL "" FORCE)
set(LIBRW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../vendor/librw librw)
add_executable(pspconv pspconv.cpp)
target_link_libraries(pspconv PRIVATE librw::librw)
set_target_properties(pspconv PROPERTIES CXX_STANDARD 11)

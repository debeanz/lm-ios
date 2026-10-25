# Host build of the recompiler alone (lmrecompile), for cross builds (iOS, docs/IOS.md): the
# main project's own lmrecompile is built for the target and cannot run on the build machine.
#   cmake -S tools/recomp/host -B build/host-tools -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/host-tools
# then configure the cross build with -DLM_HOST_RECOMPILE=<build/host-tools/lmrecompile>.
cmake_minimum_required(VERSION 3.25)
project(lmrecompile_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_root ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(lmrecompile ${_root}/tools/recomp/lmrecompile.cpp ${_root}/src/recompiler/recompiler.cpp)
target_include_directories(lmrecompile PRIVATE ${_root}/src)

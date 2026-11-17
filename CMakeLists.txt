cmake_minimum_required(VERSION 3.20)
project(claude-coreutils VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# CachyOS / Arch: optimise for the machine unless asked otherwise.
option(CU_NATIVE "Build with -march=native (not for redistributable packages)" OFF)
option(CU_LTO "Link time optimisation" ON)
set(CU_MARCH "" CACHE STRING "Explicit -march (CachyOS: x86-64-v3 or x86-64-v4)")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)

add_executable(claude-coreutils
    src/main.cpp
    src/common.cpp
    src/arch.cpp
    src/cmd_archchroot.cpp
    src/cmd_fetch.cpp
    src/cmd_files.cpp
    src/cmd_copy.cpp
    src/cmd_misc.cpp
    src/cmd_list.cpp
    src/cmd_text1.cpp
    src/cmd_text2.cpp
    src/cmd_sum.cpp
    src/cmd_system.cpp
)
target_compile_definitions(claude-coreutils PRIVATE CU_VERSION="${PROJECT_VERSION}" _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_compile_options(claude-coreutils PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(claude-coreutils PRIVATE OpenSSL::Crypto)

if(CU_NATIVE)
    target_compile_options(claude-coreutils PRIVATE -O3 -march=native)
elseif(CU_MARCH)
    target_compile_options(claude-coreutils PRIVATE -O3 -march=${CU_MARCH})
endif()
if(CU_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET claude-coreutils PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

include(GNUInstallDirs)
install(TARGETS claude-coreutils RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES completions/claude-coreutils.fish DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)
install(FILES completions/claude-coreutils.bash RENAME claude-coreutils DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions)
install(FILES completions/_claude-coreutils DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)

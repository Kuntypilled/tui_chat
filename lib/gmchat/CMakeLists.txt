# Project
cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(libgmchat LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)

# Ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

# Library
add_library(gmchat ${LIBRARY_LINKING_TYPE}
  src/gmchat.cpp
  src/gmchat.h
  src/gmclient.cpp
  src/gmclient.h
  src/gmhelper.cpp
  src/gmhelper.h
)
install(TARGETS gmchat DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Common
target_common_config(gmchat)

# Headers
target_include_directories(gmchat PRIVATE "../common/src")
target_include_directories(gmchat PRIVATE "../ncutil/src")

# JSON parsing uses the RapidJSON copy bundled with cereal in ncutil
target_include_directories(gmchat SYSTEM PRIVATE "../ncutil/ext/cereal/include")

# Compiler flags
set_target_properties(gmchat PROPERTIES COMPILE_FLAGS
                      "-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith \
                       -Wcast-qual -Wno-missing-braces -Wswitch-default \
                       -Wunreachable-code -Wundef -Wuninitialized \
                       -Wcast-align")

# Linking
target_link_libraries(gmchat PUBLIC ncutil)

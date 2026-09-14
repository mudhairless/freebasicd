cmake_minimum_required(VERSION 3.16)
project(freebasiclsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LspCpp is vendored as a git submodule pinned to commit 19150d12. Keep its
# build surface minimal: no websockets, examples, tests, or Boost-requiring
# targets. Standalone Asio and C++17 are the defaults.
set(LSPCPP_BUILD_WEBSOCKETS OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_MINIMAL_EXAMPLE OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_PERF_SMOKE OFF CACHE BOOL "" FORCE)
set(LSPCPP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(LSPCPP_USE_CPP17 ON CACHE BOOL "" FORCE)
set(LSPCPP_STANDALONE_ASIO ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/LspCpp)

add_library(freebasiclsp_core STATIC
    src/session.cpp
)
target_link_libraries(freebasiclsp_core PRIVATE lspcpp)

add_executable(freebasiclsp
    src/main.cpp
)
target_link_libraries(freebasiclsp PRIVATE freebasiclsp_core lspcpp)

enable_testing()

add_executable(session_integration
    tests/session_integration.cpp
)
target_include_directories(session_integration PRIVATE
    ${CMAKE_SOURCE_DIR}
    third_party/LspCpp/tests
)
target_link_libraries(session_integration PRIVATE freebasiclsp_core lspcpp)
add_test(NAME session_integration COMMAND session_integration)
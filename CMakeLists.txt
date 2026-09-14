cmake_minimum_required(VERSION 3.14)
project(freebasiclsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(langservercpp CONFIG REQUIRED)

add_executable(freebasiclsp
    src/main.cpp
    src/language.cpp
    src/parser.cpp
)

target_link_libraries(freebasiclsp PRIVATE langservercpp)
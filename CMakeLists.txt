cmake_minimum_required(VERSION 3.15)
set(CMAKE_C_STANDARD 11)
project(wikithing)

add_subdirectory(corecommon)

include_directories(./include ./corecommon/src)

add_custom_target(wiki-headers ALL ${CMAKE_BINARY_DIR}/corecommon/headergen ./src WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_dependencies(wiki-headers headergen)

file(GLOB SRC ./src/*.h ./src/*.c)

add_executable(wikithing ${SRC})
add_dependencies(wikithing wiki-headers corecommon)

include_directories(/usr/local/include)
find_library(LIBEVENT libevent.a)

target_link_libraries(wikithing PUBLIC corecommon ${LIBEVENT})
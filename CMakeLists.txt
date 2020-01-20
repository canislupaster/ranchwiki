cmake_minimum_required(VERSION 3.15)
set(CMAKE_C_STANDARD 11)
project(wikithing)

add_subdirectory(corecommon)

include_directories(./include ./corecommon/src)

file(GLOB SRC ./src/*.h ./src/*.c)

add_executable(wikithing ${SRC})
add_dependencies(wikithing corecommon)

include_directories(/usr/local/include)
find_library(LIBEVENT libevent.a)

target_link_libraries(wikithing PUBLIC corecommon ${LIBEVENT})
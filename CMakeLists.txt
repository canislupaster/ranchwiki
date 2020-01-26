cmake_minimum_required(VERSION 3.15)
set(CMAKE_C_STANDARD 11)
project(wikithing)

add_subdirectory(corecommon)

include_directories(./include ./corecommon/src)

file(GLOB SRC ./src/*.c)

add_executable(wikithing ${SRC})
add_dependencies(wikithing corecommon)

if(MSVC)
  target_compile_options(wikithing PRIVATE /W4 /WX)
else()
  target_compile_options(wikithing PRIVATE -Wall -Wextra -pedantic -Wno-newline-eof -Wno-unused-parameter)
endif()

include_directories(/usr/local/include)
find_library(LIBEVENT libevent.a)

target_link_libraries(wikithing PUBLIC corecommon ${LIBEVENT})
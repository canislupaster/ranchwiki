cmake_minimum_required(VERSION 3.15)
set(CMAKE_C_STANDARD 11)
project(ranch)

if (CMAKE_BUILD_TYPE MATCHES Debug)
	add_compile_definitions(BUILD_DEBUG)
endif ()

add_subdirectory(corecommon)

include_directories(./include ./corecommon/src /usr/local/include)

file(GLOB SRC src/*.c src/*.h)
list(REMOVE_ITEM SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/util_main.c)

add_executable(ranch ${SRC} src/main.c)
add_executable(ranch-contained ${SRC} src/container.c)

add_custom_target(genheader_ranch COMMAND headergen ${CMAKE_SOURCE_DIR}/src)
add_dependencies(genheader_ranch headergen)

add_dependencies(ranch corecommon genheader_ranch)
add_dependencies(ranch-contained corecommon genheader_ranch)

find_library(LIBEVENT libevent.a)
find_library(LIBEVENT_PTHREADS libevent_pthreads.a)

find_package(OPENSSL REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

target_link_libraries(ranch PUBLIC corecommon ${LIBEVENT} ${LIBEVENT_PTHREADS} ${OPENSSL_CRYPTO_LIBRARY} Threads::Threads)
target_include_directories(ranch PUBLIC ${OPENSSL_INCLUDE_DIR})

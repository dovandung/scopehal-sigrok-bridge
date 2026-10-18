cmake_minimum_required(VERSION 3.13)
project(scopehal-sigrok-bridge C CXX)

# libsigrok4DSL is compiled from a DSView / ALL-LOGIC source tree:
#   cmake -B build -DALLLOGIC_DIR=/path/to/ALL-LOGIC
include(${PROJECT_SOURCE_DIR}/cmake/libsigrok4DSL.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(WARNINGS "-Wall -Wextra -Wuninitialized -Wshadow -Wpedantic -Wcast-align -Wwrite-strings -Wvla")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WARNINGS}")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address")

if(WIN32)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_USE_MATH_DEFINES -D_POSIX_THREAD_SAFE_FUNCTIONS")
endif()

add_subdirectory("${PROJECT_SOURCE_DIR}/lib/log")
add_subdirectory("${PROJECT_SOURCE_DIR}/lib/scpi-server-tools")
add_subdirectory("${PROJECT_SOURCE_DIR}/lib/xptools")

add_executable(scopehal-sigrok-bridge
	src/main.cpp
	src/srbinding.cpp
	src/server.cpp
	src/SigrokSCPIServer.cpp
	src/Capture.cpp
)

target_include_directories(scopehal-sigrok-bridge PRIVATE lib/)

target_link_libraries(scopehal-sigrok-bridge
	sigrok4DSL
	scpi-server-tools
	xptools
	log
	pthread
)

install(TARGETS scopehal-sigrok-bridge RUNTIME DESTINATION bin)

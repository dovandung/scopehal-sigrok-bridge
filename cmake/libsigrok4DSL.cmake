# Builds libsigrok4DSL as a static library straight from an ALL-LOGIC / DSView
# source checkout. DSView compiles the library into its executable and does not
# install it, so the bridge does the same.
#
#   cmake -DALLLOGIC_DIR=/path/to/ALL-LOGIC ..
#
# Every driver present in the tree is enabled. Drivers with extra dependencies
# are enabled only when those are found (hsdaoh-la needs libhsdaoh).

if(NOT ALLLOGIC_DIR)
	message(FATAL_ERROR "Set -DALLLOGIC_DIR=<path to ALL-LOGIC or DSView checkout>")
endif()
get_filename_component(ALLLOGIC_DIR "${ALLLOGIC_DIR}" ABSOLUTE)
set(SR_DIR "${ALLLOGIC_DIR}/libsigrok4DSL")
if(NOT EXISTS "${SR_DIR}/lib_main.c")
	message(FATAL_ERROR "${SR_DIR}/lib_main.c not found: ALLLOGIC_DIR must point at an ALL-LOGIC/DSView checkout")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SR_DEPS REQUIRED glib-2.0 libusb-1.0 zlib)

# The library's output modules include "../config.h" relative to their own
# directory, so the header has to live in the source tree (same as DSView).
if(NOT EXISTS "${SR_DIR}/config.h")
	set(PACKAGE "libsigrok4DSL")
	set(PACKAGE_VERSION "bridge")
	set(PACKAGE_STRING "libsigrok4DSL bridge")
	configure_file("${SR_DIR}/config.h.in" "${SR_DIR}/config.h")
endif()

file(GLOB SR_SOURCES
	"${SR_DIR}/*.c"
	"${SR_DIR}/input/*.c"
	"${SR_DIR}/output/*.c")
file(GLOB_RECURSE SR_HW_SOURCES "${SR_DIR}/hardware/*.c")
list(FILTER SR_HW_SOURCES EXCLUDE REGEX "/hsdaoh-la/")
list(APPEND SR_SOURCES ${SR_HW_SOURCES}
	"${ALLLOGIC_DIR}/common/minizip/zip.c"
	"${ALLLOGIC_DIR}/common/minizip/unzip.c"
	"${ALLLOGIC_DIR}/common/minizip/ioapi.c"
	"${ALLLOGIC_DIR}/common/log/xlog.c")

set(SR_DEFS
	HAVE_LA_DEMO=1
	HAVE_DSL_DEVICE=1
	HAVE_CH32_DEVICE=1
	HAVE_SLOGIC_DEVICE=1
	HAVE_PXLOGIC_DEVICE=1
	HAVE_ATKLOGIC_DEVICE=1
	HAVE_FX2LAFW_DEVICE=1
	HAVE_CYPRESS_FX3_DEVICE=1)

option(ENABLE_HSDAOH_LA "Build the hsdaoh-la driver (needs libhsdaoh)" ON)
if(ENABLE_HSDAOH_LA AND EXISTS "${SR_DIR}/hardware/hsdaoh-la/hsdaoh_la.c")
	pkg_search_module(HSDAOH libhsdaoh)
	if(HSDAOH_FOUND)
		file(GLOB HSDAOH_SOURCES "${SR_DIR}/hardware/hsdaoh-la/*.c")
		list(APPEND SR_SOURCES ${HSDAOH_SOURCES})
		list(APPEND SR_DEFS HAVE_HSDAOH_LA_DEVICE=1)
	else()
		message(STATUS "libhsdaoh not found: hsdaoh-la driver left out")
	endif()
endif()

add_library(sigrok4DSL STATIC ${SR_SOURCES})
set_target_properties(sigrok4DSL PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(sigrok4DSL PUBLIC ${SR_DEFS})
target_compile_options(sigrok4DSL PRIVATE -w -O2)
target_include_directories(sigrok4DSL PUBLIC
	"${ALLLOGIC_DIR}"
	"${SR_DIR}"
	"${ALLLOGIC_DIR}/common"
	${SR_DEPS_INCLUDE_DIRS}
	${HSDAOH_INCLUDE_DIRS})
target_link_directories(sigrok4DSL PUBLIC ${SR_DEPS_LIBRARY_DIRS} ${HSDAOH_LIBRARY_DIRS})
target_link_libraries(sigrok4DSL PUBLIC ${SR_DEPS_LIBRARIES} ${HSDAOH_LIBRARIES} pthread m)

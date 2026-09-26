cmake_minimum_required(VERSION 3.16)

# rmv-report links against the parser and backend libraries of AMD's
# Radeon Memory Visualizer, but NOT against its Qt frontend. AMD's top-level
# CMakeLists.txt always adds the frontend (there is no option to switch it
# off), and the frontend targets Qt 6.7 - with newer Qt it fails at configure
# time. So instead of patching AMD's tree, this file pulls in only the
# subdirectories that are actually needed and sets the few variables that
# AMD's top-level file would otherwise provide.

project(rmv-report LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(RMV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/radeon_memory_visualizer"
    CACHE PATH "Checkout of GPUOpen-Tools/radeon_memory_visualizer (see scripts/fetch-rmv.sh)")

if (NOT EXISTS "${RMV_SOURCE_DIR}/source/backend/rmt_data_set.h")
    message(FATAL_ERROR
        "Radeon Memory Visualizer sources not found in ${RMV_SOURCE_DIR}.\n"
        "Run scripts/fetch-rmv.sh first, or pass -DRMV_SOURCE_DIR=<path>.")
endif ()
if (NOT EXISTS "${RMV_SOURCE_DIR}/external/rdf/CMakeLists.txt")
    message(FATAL_ERROR
        "The dependencies of the Radeon Memory Visualizer are missing.\n"
        "Run scripts/fetch-rmv.sh (it calls AMD's build/fetch_dependencies.py).")
endif ()

# The same switches AMD's top-level CMakeLists.txt sets before adding rdf and
# system_info_utils.
set(RDF_ENABLE_CXX_BINDINGS ON)
set(RDF_STATIC ON)
set(SYSTEM_INFO_BUILD_RDF_INTERFACES ON)

# Provides devtools_target_options(), which the parser and backend call.
list(APPEND CMAKE_MODULE_PATH "${RMV_SOURCE_DIR}/cmake")
include(dev_tools)

add_definitions(-DJSON_SKIP_UNSUPPORTED_COMPILER_CHECK)
include_directories("${RMV_SOURCE_DIR}/external/")
include_directories("${RMV_SOURCE_DIR}/external/third_party/")
if (UNIX)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif ()

# Second argument = binary directory; required because the sources live
# outside this project's tree.
if (NOT TARGET amdrdf)
    add_subdirectory("${RMV_SOURCE_DIR}/external/rdf" rmv/rdf EXCLUDE_FROM_ALL)
endif ()
if (NOT TARGET system_info)
    add_subdirectory("${RMV_SOURCE_DIR}/external/system_info_utils" rmv/system_info_utils EXCLUDE_FROM_ALL)
endif ()
add_subdirectory("${RMV_SOURCE_DIR}/source/parser" rmv/parser EXCLUDE_FROM_ALL)
add_subdirectory("${RMV_SOURCE_DIR}/source/backend" rmv/backend EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

add_executable(rmv-report src/rmv_report.cpp)
target_include_directories(rmv-report PRIVATE
    "${RMV_SOURCE_DIR}/source/backend"
    "${RMV_SOURCE_DIR}/source/parser"
    "${RMV_SOURCE_DIR}/source")
target_link_libraries(rmv-report PRIVATE RmvBackend RmvParser rdf system_info Threads::Threads)

install(TARGETS rmv-report RUNTIME DESTINATION bin)

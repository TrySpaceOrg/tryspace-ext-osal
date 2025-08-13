######################################################################
#
# CMAKE build recipe for POSIX OSAL implementation
#
######################################################################

# This CMake script generates targets specific to the POSIX implementation

# The basic set of files which are always built
set(POSIX_BASE_SRCLIST
    ../posix/src/os-impl-console.c
    ../posix/src/os-impl-condvar.c
    ../posix/src/os-impl-countsem.c
    ../posix/src/os-impl-dirs.c
    ../posix/src/os-impl-errors.c
    ../posix/src/os-impl-files.c
    ../posix/src/os-impl-filesys.c
    ../posix/src/os-impl-heap.c
    ../posix/src/os-impl-idmap.c
    ../posix/src/os-impl-mutex.c
    ../posix/src/os-impl-queues.c
    src/os-impl-binsem.c
    src/os-impl-common.c
    src/os-impl-tasks.c
    src/os-impl-timebase.c
)


# Use portable blocks for basic I/O
set(POSIX_IMPL_SRCLIST
    ../portable/os-impl-posix-gettime.c
    ../portable/os-impl-console-bsp.c
    ../portable/os-impl-bsd-select.c
    ../portable/os-impl-posix-io.c
    ../portable/os-impl-posix-files.c
    ../portable/os-impl-posix-dirs.c
)

if (OSAL_CONFIG_INCLUDE_SHELL)
    list(APPEND POSIX_IMPL_SRCLIST
       ../posix/src/os-impl-shell.c
    )
else ()
    list(APPEND POSIX_IMPL_SRCLIST
       ../portable/os-impl-no-shell.c
    )
endif ()

# If some form of module loading is configured,
# then build the module loader
if (OSAL_CONFIG_INCLUDE_DYNAMIC_LOADER)
    list(APPEND POSIX_IMPL_SRCLIST
        ../posix/src/os-impl-loader.c
        ../portable/os-impl-posix-dl-loader.c
        ../portable/os-impl-posix-dl-symtab.c
    )
else ()
    list(APPEND POSIX_IMPL_SRCLIST
        src/os-impl-no-module.c
        ../portable/os-impl-no-loader.c
        ../portable/os-impl-no-symtab.c
    )
endif ()

if (OSAL_CONFIG_INCLUDE_NETWORK)
    list(APPEND POSIX_IMPL_SRCLIST
        ../portable/os-impl-bsd-sockets.c   # Use BSD socket layer implementation
        ../portable/os-impl-posix-network.c # Use POSIX-defined hostname/id implementation
    )
else()
    list(APPEND POSIX_IMPL_SRCLIST
        ../portable/os-impl-no-network.c    # non-implemented versions of all network APIs
        ../portable/os-impl-no-sockets.c    # non-implemented versions of all socket APIs
    )
endif ()

# Defines an OBJECT target named "osal_tryspace_impl" with selected source files
add_library(osal_tryspace_impl OBJECT
    ${POSIX_BASE_SRCLIST}
    ${POSIX_IMPL_SRCLIST}
)

# Find ZeroMQ dependency
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZeroMQ REQUIRED libzmq)

  # Find Simulith headers
set(SIMULITH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../simulith" CACHE PATH "Path to Simulith directory")
if(NOT EXISTS "${SIMULITH_DIR}/include/simulith_time.h")
    message(FATAL_ERROR "Could not find ${SIMULITH_DIR}/include/simulith_time.h
    Please set SIMULITH_DIR to the correct path.")
endif()

# Only add Simulith subdirectory if not already added
if(NOT TARGET simulith)
# Set policy to NEW to allow external project linking
cmake_policy(SET CMP0079 NEW)
add_subdirectory(${SIMULITH_DIR} ${CMAKE_BINARY_DIR}/simulith)
endif()

target_include_directories(osal_tryspace_impl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${SIMULITH_DIR}/include
    ../../../../psp/fsw/tryspace/inc
)
target_compile_definitions(osal_public_api INTERFACE
    _POSIX_OS_
)

# Link with simulith for time provider
target_link_libraries(osal_public_api INTERFACE simulith)

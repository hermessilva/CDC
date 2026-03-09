###############################################################################
# DataCrusher - High-Performance PostgreSQL CDC Engine
# Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
# Cross-Platform (Windows/Linux) - C++20 - CMake 3.24+
#
# Windows (VS 2022):
#   cmake -B build -G "Visual Studio 17 2022" -A x64 -DPOSTGRESQL_ROOT="C:/Program Files/PostgreSQL/16"
#   cmake --build build --config Release
#
# Linux:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
###############################################################################
cmake_minimum_required(VERSION 3.20)

project(DataCrusher
    VERSION 1.0.0
    DESCRIPTION "High-Performance PostgreSQL CDC Engine via Logical Decoding"
    LANGUAGES CXX
)

# ===========================================================================
# C++20 Standard - Mandatory
# ===========================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===========================================================================
# Platform / Architecture Tag (for cross-compilation output naming)
# ===========================================================================
if(NOT DEFINED DC_PLATFORM_TAG)
    if(WIN32)
        set(_OS "win")
    else()
        set(_OS "linux")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(_ARCH "arm64")
    else()
        set(_ARCH "x64")
    endif()
    set(DC_PLATFORM_TAG "${_OS}-${_ARCH}")
endif()

# ===========================================================================
# Output Directory
# ===========================================================================
# Override with -DDC_DEPLOY_DIR=<path>  (Build-DataCrusher.ps1 sets this automatically)
if(DEFINED DC_DEPLOY_DIR)
    set(DC_OUTPUT_DIR "${DC_DEPLOY_DIR}")
else()
    set(DC_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../Deploy")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DC_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DC_OUTPUT_DIR}")
foreach(CFG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${DC_OUTPUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} "${DC_OUTPUT_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFG} "${DC_OUTPUT_DIR}")
endforeach()

# ===========================================================================
# Build Options
# ===========================================================================
option(DC_ENABLE_ASAN      "Enable AddressSanitizer (Debug builds)"           OFF)
option(DC_ENABLE_TSAN      "Enable ThreadSanitizer (Debug builds)"            OFF)
option(DC_BUILD_TESTS      "Build unit tests"                                 OFF)
option(DC_STATIC_RUNTIME   "Use static CRT on Windows (/MT, self-contained)"  ON)
option(DC_STATIC_LINK      "Statically link all deps (self-contained binary)" ON)

# ===========================================================================
# Platform Detection
# ===========================================================================
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)  # Windows 10+
    add_definitions(-DNOMINMAX)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

# ===========================================================================
# PostgreSQL / libpq Discovery
# ===========================================================================
# Static (all platforms): DC_STATIC_LINK=ON — link against custom-built static libpq (no SSL)
# Dynamic (Windows):      uses installed PostgreSQL (development mode only)
# Dynamic (Linux):        pkg-config / system libpq
if(WIN32)
    if(DC_STATIC_LINK)
        # Static linking: vcpkg-built static libpq (no SSL, no compression)
        # Build-DataCrusher.ps1 sets POSTGRESQL_STATIC_ROOT automatically via vcpkg
        if(NOT DEFINED POSTGRESQL_STATIC_ROOT)
            message(FATAL_ERROR
                "POSTGRESQL_STATIC_ROOT must be set for Windows static linking.\n"
                "Use Build-DataCrusher.ps1 which sets up vcpkg automatically.")
        endif()
        set(LIBPQ_INCLUDE_DIR "${POSTGRESQL_STATIC_ROOT}/include")
        set(LIBPQ_STATIC_LIB_DIR "${POSTGRESQL_STATIC_ROOT}/lib")
        if(NOT EXISTS "${LIBPQ_INCLUDE_DIR}/libpq-fe.h")
            message(FATAL_ERROR "Static libpq-fe.h not found in ${LIBPQ_INCLUDE_DIR}")
        endif()
    else()
        # Dynamic linking from installed PostgreSQL (development / VS IDE mode)
        if(NOT DEFINED POSTGRESQL_ROOT)
            foreach(_PG_VER 18 17 16 15 14)
                if(EXISTS "C:/Program Files/PostgreSQL/${_PG_VER}")
                    set(POSTGRESQL_ROOT "C:/Program Files/PostgreSQL/${_PG_VER}")
                    break()
                endif()
            endforeach()
        endif()

        if(NOT POSTGRESQL_ROOT)
            message(FATAL_ERROR
                "PostgreSQL not found. Set -DPOSTGRESQL_ROOT=\"C:/Program Files/PostgreSQL/18\" "
                "pointing to your PostgreSQL installation directory."
            )
        endif()

        message(STATUS "PostgreSQL root: ${POSTGRESQL_ROOT}")

        set(LIBPQ_INCLUDE_DIR "${POSTGRESQL_ROOT}/include")
        set(LIBPQ_LIB_DIR     "${POSTGRESQL_ROOT}/lib")
        set(LIBPQ_BIN_DIR     "${POSTGRESQL_ROOT}/bin")

        if(NOT EXISTS "${LIBPQ_INCLUDE_DIR}/libpq-fe.h")
            message(FATAL_ERROR "libpq-fe.h not found in ${LIBPQ_INCLUDE_DIR}")
        endif()

        find_library(LIBPQ_LIBRARY
            NAMES libpq pq
            PATHS "${LIBPQ_LIB_DIR}"
            NO_DEFAULT_PATH
            REQUIRED
        )

        # Runtime DLLs for development (installed PG includes SSL DLLs it depends on)
        file(GLOB LIBPQ_RUNTIME_DLLS
            "${LIBPQ_BIN_DIR}/libpq.dll"
            "${LIBPQ_BIN_DIR}/libssl-*.dll"
            "${LIBPQ_BIN_DIR}/libcrypto-*.dll"
            "${LIBPQ_BIN_DIR}/libiconv-*.dll"
            "${LIBPQ_BIN_DIR}/libintl-*.dll"
            "${LIBPQ_BIN_DIR}/libwinpthread-*.dll"
        )
    endif()
else()
    if(DC_STATIC_LINK)
        # Custom-built libpq (no SSL, no GSSAPI/LDAP) for self-contained static binary
        if(NOT DEFINED POSTGRESQL_STATIC_ROOT)
            set(POSTGRESQL_STATIC_ROOT "/opt/pgsql")
        endif()
        set(LIBPQ_INCLUDE_DIR "${POSTGRESQL_STATIC_ROOT}/include")
        set(LIBPQ_STATIC_LIB_DIR "${POSTGRESQL_STATIC_ROOT}/lib")
        if(NOT EXISTS "${LIBPQ_INCLUDE_DIR}/libpq-fe.h")
            message(FATAL_ERROR "Static libpq-fe.h not found in ${LIBPQ_INCLUDE_DIR}.\n"
                "Build PostgreSQL from source with: ./configure --without-gssapi --without-ldap --without-openssl")
        endif()
    else()
        # Dynamic: use system pkg-config
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBPQ REQUIRED libpq)
    endif()
endif()

# ===========================================================================
# Source Files
# ===========================================================================
set(DC_HEADERS
    src/Constants.h
    src/Platform.h
    src/Logger.h
    src/TypeHandlers.h
    src/ReplicationStream.h
    src/TransactionManager.h
    src/CopyWriter.h
    src/ReconnectionManager.h
    src/LatencyMonitor.h
    src/DataCrusherEngine.h
    src/SignalTable.h
    src/WalMessageParser.h
    src/TenantWorkerConfig.h
    src/TenantWorker.h
    src/WorkerManager.h
)

set(DC_SOURCES
    src/Logger.cpp
    src/TypeHandlers.cpp
    src/ReplicationStream.cpp
    src/TransactionManager.cpp
    src/CopyWriter.cpp
    src/ReconnectionManager.cpp
    src/LatencyMonitor.cpp
    src/DataCrusherEngine.cpp
    src/SignalTable.cpp
    src/WalMessageParser.cpp
    src/TenantWorker.cpp
    src/WorkerManager.cpp
    src/Main.cpp
)

# ===========================================================================
# Executable Target
# ===========================================================================
add_executable(DataCrusher ${DC_SOURCES} ${DC_HEADERS})

set_target_properties(DataCrusher PROPERTIES
    OUTPUT_NAME "DataCrusher.${DC_PLATFORM_TAG}"
)

target_include_directories(DataCrusher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_include_directories(DataCrusher PRIVATE "${LIBPQ_INCLUDE_DIR}")
    if(DC_STATIC_LINK)
        # Library suffix: .lib for MSVC, .a for MinGW (llvm-mingw Windows ARM64 cross-compilation)
        set(_SAVED_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
        if(MSVC)
            set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
        else()
            set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        endif()

        find_library(PQ_STATIC      NAMES pq libpq       PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
        find_library(PGCOMMON_STATIC NAMES pgcommon libpgcommon PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
        find_library(PGPORT_STATIC   NAMES pgport libpgport   PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)

        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_SAVED_SUFFIXES})

        target_link_libraries(DataCrusher PRIVATE
            ${PQ_STATIC} ${PGCOMMON_STATIC} ${PGPORT_STATIC}
            ws2_32 secur32 advapi32 shell32 wldap32 crypt32
        )
        if(NOT MSVC)
            # MinGW (llvm-mingw): pull in libgcc, libstdc++, libwinpthread statically
            target_link_options(DataCrusher PRIVATE -static)
        endif()

        message(STATUS "  Static libpq:       ${PQ_STATIC}")
        message(STATUS "  Static libpgcommon: ${PGCOMMON_STATIC}")
        message(STATUS "  Static libpgport:   ${PGPORT_STATIC}")
    else()
        target_link_libraries(DataCrusher PRIVATE "${LIBPQ_LIBRARY}" ws2_32 secur32)
    endif()
else()
    if(DC_STATIC_LINK)
        set(_SAVED_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

        # libpq, pgcommon, pgport from custom build (no SSL, no GSSAPI/LDAP)
        find_library(PQ_STATIC       NAMES pq       PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
        find_library(PGCOMMON_STATIC  NAMES pgcommon  PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
        find_library(PGPORT_STATIC    NAMES pgport    PATHS ${LIBPQ_STATIC_LIB_DIR} NO_DEFAULT_PATH REQUIRED)

        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_SAVED_SUFFIXES})

        target_include_directories(DataCrusher PRIVATE "${LIBPQ_INCLUDE_DIR}")
        target_link_libraries(DataCrusher PRIVATE
            ${PQ_STATIC} ${PGCOMMON_STATIC} ${PGPORT_STATIC}
            pthread dl
        )
        target_link_options(DataCrusher PRIVATE -static-libstdc++ -static-libgcc)

        message(STATUS "  Static libpq:       ${PQ_STATIC}")
        message(STATUS "  Static libpgcommon: ${PGCOMMON_STATIC}")
        message(STATUS "  Static libpgport:   ${PGPORT_STATIC}")
    else()
        target_include_directories(DataCrusher PRIVATE ${LIBPQ_INCLUDE_DIRS})
        target_link_libraries(DataCrusher PRIVATE ${LIBPQ_LIBRARIES} pthread)
    endif()
endif()

# ===========================================================================
# Compiler Flags
# ===========================================================================
if(MSVC)
    target_compile_options(DataCrusher PRIVATE
        /W4 /WX         # Warnings as errors
        /permissive-    # Standards conformance
        /Zc:__cplusplus # Correct __cplusplus macro
        /utf-8          # Source and execution charset
        /MP             # Multi-processor compilation
        /GS             # Buffer security checks
        /sdl            # Additional security checks
    )
    # Release optimizations
    target_compile_options(DataCrusher PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /GL /GF /Gy>
    )
    target_link_options(DataCrusher PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
    # Static CRT if requested
    if(DC_STATIC_RUNTIME)
        set_property(TARGET DataCrusher PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
else()
    target_compile_options(DataCrusher PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter
        -fstack-protector-strong
        $<$<CONFIG:Release>:-O3 -flto>
        $<$<CONFIG:Debug>:-g -O0>
    )
    target_link_options(DataCrusher PRIVATE
        $<$<CONFIG:Release>:-flto>
    )
    # Sanitizers
    if(DC_ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(DataCrusher PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(DataCrusher PRIVATE -fsanitize=address)
    endif()
    if(DC_ENABLE_TSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(DataCrusher PRIVATE -fsanitize=thread)
        target_link_options(DataCrusher PRIVATE -fsanitize=thread)
    endif()
endif()

# ===========================================================================
# Post-Build: Copy libpq DLLs (Windows)
# ===========================================================================
if(WIN32 AND LIBPQ_RUNTIME_DLLS)
    foreach(_DLL ${LIBPQ_RUNTIME_DLLS})
        add_custom_command(TARGET DataCrusher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_DLL}"
                "$<TARGET_FILE_DIR:DataCrusher>"
            COMMENT "Copying ${_DLL} to output directory"
        )
    endforeach()
endif()

# ===========================================================================
# Install Rules
# ===========================================================================
install(TARGETS DataCrusher RUNTIME DESTINATION bin)
if(WIN32 AND LIBPQ_RUNTIME_DLLS)
    install(FILES ${LIBPQ_RUNTIME_DLLS} DESTINATION bin)
endif()

message(STATUS "")
message(STATUS "=== DataCrusher Build Configuration ===")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  C++ Standard:  C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform:      ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
    if(DC_STATIC_LINK)
        message(STATUS "  PostgreSQL:    ${POSTGRESQL_STATIC_ROOT} (static, no SSL)")
    else()
        message(STATUS "  PostgreSQL:    ${POSTGRESQL_ROOT} (dynamic)")
        message(STATUS "  libpq:         ${LIBPQ_LIBRARY}")
    endif()
elseif(DC_STATIC_LINK)
    message(STATUS "  PostgreSQL:    ${POSTGRESQL_STATIC_ROOT} (static, no SSL)")
endif()
message(STATUS "=======================================")
message(STATUS "")

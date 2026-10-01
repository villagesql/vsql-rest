# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.18)
project(vsql_rest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTENSION_NAME "vsql_rest")

# Find VillageSQL SDK
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(VillageSQL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "VillageSQL include dir: ${VillageSQL_INCLUDE_DIR}")

# OpenSSL — required for TLS and JWT verification
#
# On macOS, Homebrew's OpenSSL is keg-only and FindOpenSSL resolves it to a
# versioned Cellar path (e.g. Cellar/openssl@3/3.6.0). That path is removed
# when Homebrew upgrades OpenSSL, breaking existing build directories. Prefer
# the stable opt/ symlink and drop cached paths that no longer exist.
if(APPLE AND NOT OPENSSL_ROOT_DIR AND NOT DEFINED ENV{OPENSSL_ROOT_DIR})
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix openssl@3
            OUTPUT_VARIABLE _brew_openssl_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _brew_result
            ERROR_QUIET
        )
        if(_brew_result EQUAL 0 AND EXISTS "${_brew_openssl_prefix}")
            set(OPENSSL_ROOT_DIR "${_brew_openssl_prefix}")
        endif()
        unset(_brew_openssl_prefix)
        unset(_brew_result)
    endif()
endif()
foreach(_var OPENSSL_INCLUDE_DIR OPENSSL_CRYPTO_LIBRARY OPENSSL_SSL_LIBRARY)
    if(${_var} AND NOT EXISTS "${${_var}}")
        message(STATUS "Discarding stale cached ${_var}: ${${_var}}")
        unset(${_var} CACHE)
    endif()
endforeach()

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")

# Extension shared library
add_library(vsql_rest SHARED
    src/vsql_rest.cc
    src/http_server.cc
    src/tls.cc
    src/sql_executor.cc
    src/jwt_auth.cc
    src/json_emit.cc
    src/schema_cache.cc
    src/request_queue.cc
    src/third_party/picohttpparser.c
)

# A Status enumerator with no reason phrase in format_http_response() is a
# build failure, not a "999 Unknown" status line at runtime.
target_compile_options(vsql_rest PRIVATE -Werror=switch)

option(VSQL_VALGRIND "Build for running under Valgrind" OFF)

if(VSQL_VALGRIND)
    target_compile_definitions(vsql_rest PRIVATE VSQL_VALGRIND)

    # picohttpparser's header scan uses pcmpestri, which Valgrind cannot decode
    # in either encoding -- SIGILL on the first HTTP request. Drop to its scalar
    # path, which is what non-x86 builds already use.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        set_source_files_properties(src/third_party/picohttpparser.c
            PROPERTIES COMPILE_OPTIONS -mno-sse4.2)
    endif()
endif()

# include-dev must come before include so preview headers take precedence
# over older protocol headers in the stable include tree
target_include_directories(vsql_rest PRIVATE
    "${VillageSQL_PREFIX}/include-dev"
    "${VillageSQLExtensionFramework_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party"
)

target_link_libraries(vsql_rest PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Threads (for std::thread in accept/connection loops)
find_package(Threads REQUIRED)
target_link_libraries(vsql_rest PRIVATE Threads::Threads)

# Build the VEB package
VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET vsql_rest
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

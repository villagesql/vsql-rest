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
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")

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

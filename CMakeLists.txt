cmake_minimum_required(VERSION 3.25)
project(rawimport_pipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT to match vcpkg's x64-windows-static triplet (/MT).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Portable-branch dependencies (v2.1.0-portable):
# SQLite replaces MariaDB entirely — no libmariadb, no external DB server.
find_package(OpenSSL REQUIRED)   # SHA-256 (EVP)
find_package(spdlog REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(rawimport-pipeline
  src/main.cpp
  src/config.cpp
  src/util.cpp
  src/db.cpp
  src/pipeline.cpp
  src/converter.cpp
  src/api.cpp
  src/worker.cpp
  src/metrics.cpp
)

target_include_directories(rawimport-pipeline PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)

target_link_libraries(rawimport-pipeline PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  spdlog::spdlog
  SQLite::SQLite3
)

if(MSVC)
  # Winsock for the REST API listener.
  target_link_libraries(rawimport-pipeline PRIVATE ws2_32)
endif()

# Warnings as errors for the integration build (ORCHESTRATION §7.5).
if(MSVC)
  target_compile_options(rawimport-pipeline PRIVATE /W4)
else()
  target_compile_options(rawimport-pipeline PRIVATE -Wall -Wextra)
endif()

install(TARGETS rawimport-pipeline DESTINATION /usr/local/bin)
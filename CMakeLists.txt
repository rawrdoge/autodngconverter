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

# Dependencies (pinned per ORCHESTRATION_CppRewrite.md §2 / PRD §6.2).
# Package names follow Debian bookworm / vcpkg conventions.
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)

# MariaDB C connector (libmariadb-dev on Debian) provides libmariadb.so directly;
# no CMake config package is shipped, so we link the bare library name.
# The C API header (mysql.h) lives under an include subdir on Debian
# (e.g. /usr/include/mariadb), so locate and add it.
find_library(MARIADB_LIB
  NAMES mariadb libmariadb mariadbclient
  PATH_SUFFIXES mariadb mysql
  REQUIRED)
find_path(MARIADB_INCLUDE NAMES mysql.h PATH_SUFFIXES mariadb mysql REQUIRED)

# LibRaw — CCT analysis RAW decoding (PRD_CCT_Analysis_Native_Cpp_v2.3.0 §4).
# Dual resolution:
#   - vcpkg (Windows): lowercase CONFIG package, reentrant target libraw::raw_r
#   - Debian bookworm: libraw-dev 0.20.x ships NO CMake config -> pkg-config
set(RI_LIBRAW_TARGET "")
find_package(libraw CONFIG QUIET)
if(libraw_FOUND)
  set(RI_LIBRAW_TARGET libraw::raw_r)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)
  set(RI_LIBRAW_TARGET PkgConfig::LIBRAW)
endif()

add_executable(rawimport-pipeline
  src/main.cpp
  src/config.cpp
  src/util.cpp
  src/db.cpp
  src/pipeline.cpp
  src/converter.cpp
  src/rotation.cpp
  src/reconcile.cpp
  src/api.cpp
  src/worker.cpp
  src/metrics.cpp
  src/exiftool_daemon.cpp
  src/cct_analyzer.cpp  # CCT native analysis (PRD v2.3.0)
)

# cxxopts: header-only, vendored at src/vendor/cxxopts.hpp
target_include_directories(rawimport-pipeline PRIVATE src ${MARIADB_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)

target_link_libraries(rawimport-pipeline PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ${MARIADB_LIB}
  ${RI_LIBRAW_TARGET}
)

if(MSVC)
  # libmariadb's SChannel TLS backend, Winsock, shell-path helper and zlib
  # compression need these system/vcpkg libraries.
  find_package(ZLIB REQUIRED)
  target_link_libraries(rawimport-pipeline PRIVATE
    secur32 ws2_32 crypt32 bcrypt advapi32 Shlwapi ZLIB::ZLIB)
endif()

# Warnings as errors for the integration build (ORCHESTRATION §7.5).
if(MSVC)
  target_compile_options(rawimport-pipeline PRIVATE /W4)
else()
  target_compile_options(rawimport-pipeline PRIVATE -Wall -Wextra)
endif()

# Migrations must be bundled into the runtime image (PRD §10 fix).
install(DIRECTORY migrations DESTINATION /app)
install(TARGETS rawimport-pipeline DESTINATION /usr/local/bin)
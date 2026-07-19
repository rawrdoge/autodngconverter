cmake_minimum_required(VERSION 3.25)
project(rawimport_pipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Dependencies (pinned per ORCHESTRATION_CppRewrite.md §2 / PRD §6.2).
# Package names follow Debian bookworm / vcpkg conventions.
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
# MariaDB C connector (libmariadb-dev on Debian) provides libmariadb.so directly;
# no CMake config package is shipped, so we link the bare library name.
# The C API header (mysql.h) lives under an include subdir on Debian
# (e.g. /usr/include/mariadb), so locate and add it.
find_library(MARIADB_LIB mariadb REQUIRED)
find_path(MARIADB_INCLUDE NAMES mysql.h PATH_SUFFIXES mariadb mysql REQUIRED)

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
)

target_include_directories(rawimport-pipeline PRIVATE src ${MARIADB_INCLUDE})

target_link_libraries(rawimport-pipeline PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  cxxopts::cxxopts
  ${MARIADB_LIB}
)

# Warnings as errors for the integration build (ORCHESTRATION §7.5).
target_compile_options(rawimport-pipeline PRIVATE -Wall -Wextra)

# Migrations must be bundled into the runtime image (PRD §10 fix).
install(DIRECTORY migrations DESTINATION /app)
install(TARGETS rawimport-pipeline DESTINATION /usr/local/bin)
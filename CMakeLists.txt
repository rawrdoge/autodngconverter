cmake_minimum_required(VERSION 3.25)
project(rawimport_pipeline VERSION 2.3.0 LANGUAGES CXX)

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

# ============================================================================
# Hexagonal Architecture Layer (domain/ports/adapters + CLI)
# ============================================================================
option(WITH_HEXAGONAL "Build hexagonal architecture layer" ON)
option(BUILD_TESTING "Build unit tests" OFF)

if(WITH_HEXAGONAL OR BUILD_TESTING)
  # Domain types library — header-only, zero external deps.
  add_library(domain_types INTERFACE)
  target_include_directories(domain_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(domain_types INTERFACE cxx_std_20)

  # --------------------------------------------------------------------------
  # Infrastructure adapters (conditional on available deps)
  # --------------------------------------------------------------------------
  set(INFRA_SOURCES "")
  set(INFRA_DEPS "")

  list(APPEND INFRA_SOURCES src/infrastructure/std_filesystem.cpp)
  list(APPEND INFRA_SOURCES src/infrastructure/env_config_provider.cpp)
  list(APPEND INFRA_SOURCES src/infrastructure/prometheus_metrics_sink.cpp)

  # OpenSSL-based hash service
  find_library(OPENSSL_CRYPTO_LIBRARY crypto)
  find_path(OPENSSL_INCLUDE_DIR openssl/evp.h)
  if(OPENSSL_CRYPTO_LIBRARY AND OPENSSL_INCLUDE_DIR)
    list(APPEND INFRA_SOURCES src/infrastructure/openssl_hash_service.cpp)
    list(APPEND INFRA_DEPS ${OPENSSL_CRYPTO_LIBRARY})
    target_include_directories(domain_types INTERFACE ${OPENSSL_INCLUDE_DIR})
    add_compile_definitions(HAVE_OPENSSL)
  endif()

  # Feature flags for optional adapters
  option(WITH_MARIADB     "Enable MariaDB ImportStore adapter"      OFF)
  option(WITH_POSTGRESQL  "Enable PostgreSQL ImportStore adapter"   OFF)
  option(WITH_DNGLAB      "Enable dnglab ConverterEngine adapter"   OFF)
  option(WITH_ADOBE_DNG   "Enable Adobe DNG Converter adapter"      OFF)
  option(WITH_EXIFTOOL    "Enable ExifTool ExifExtractor adapter"   OFF)
  option(WITH_IMMICH      "Enable Immich RotationSink adapter"      OFF)
  option(WITH_SPDLOG      "Enable spdlog LogSink adapter"           OFF)

  if(WITH_MARIADB)
    find_library(MARIADB_LIB NAMES mariadb libmariadb mariadbclient
                 PATH_SUFFIXES mariadb mysql)
    find_path(MARIADB_INCLUDE NAMES mysql.h PATH_SUFFIXES mariadb mysql)
    if(MARIADB_LIB AND MARIADB_INCLUDE)
      add_compile_definitions(WITH_MARIADB)
      list(APPEND INFRA_SOURCES src/infrastructure/mariadb_import_store.cpp)
      list(APPEND INFRA_DEPS ${MARIADB_LIB})
      target_include_directories(domain_types INTERFACE ${MARIADB_INCLUDE})
    else()
      message(WARNING "WITH_MARIADB requested but MariaDB SDK not found")
    endif()
  endif()

  if(WITH_POSTGRESQL)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
      add_compile_definitions(WITH_POSTGRESQL)
      list(APPEND INFRA_SOURCES src/infrastructure/postgresql_import_store.cpp)
      list(APPEND INFRA_DEPS PostgreSQL::Libpq)
    else()
      message(WARNING "WITH_POSTGRESQL requested but PostgreSQL not found")
    endif()
  endif()

  if(WITH_DNGLAB)
    add_compile_definitions(WITH_DNGLAB)
    list(APPEND INFRA_SOURCES src/infrastructure/dnglab_converter_engine.cpp)
  endif()

  # Adobe DNG: binary spawn only, no library needed
  list(APPEND INFRA_SOURCES src/infrastructure/adobe_dng_converter_engine.cpp)
  if(WITH_ADOBE_DNG)
    add_compile_definitions(WITH_ADOBE_DNG)
  endif()

  if(WITH_EXIFTOOL)
    add_compile_definitions(WITH_EXIFTOOL)
    list(APPEND INFRA_SOURCES src/infrastructure/exiftool_exif_extractor.cpp)
    list(APPEND INFRA_SOURCES src/infrastructure/exiftool_daemon_exif_extractor.cpp)
  endif()

  if(WITH_IMMICH)
    add_compile_definitions(WITH_IMMICH)
    list(APPEND INFRA_SOURCES src/infrastructure/immich_rotation_sink.cpp)
    find_package(CURL QUIET)
    if(TARGET CURL::libcurl)
      list(APPEND INFRA_DEPS CURL::libcurl)
    endif()
  endif()

  if(WITH_SPDLOG)
    add_compile_definitions(WITH_SPDLOG)
    list(APPEND INFRA_SOURCES src/infrastructure/spdlog_log_sink.cpp)
    find_package(spdlog QUIET)
    if(TARGET spdlog::spdlog)
      list(APPEND INFRA_DEPS spdlog::spdlog)
    endif()
  endif()

  # Migration infrastructure
  list(APPEND INFRA_SOURCES
    src/infrastructure/migration_options.hpp
    src/infrastructure/migration_runner.cpp
    src/infrastructure/migration_runner.hpp
  )
  if(WITH_MARIADB AND WITH_POSTGRESQL)
    add_compile_definitions(HAVE_MIGRATION)
  endif()

  # Converter factory
  if(WITH_DNGLAB OR WITH_ADOBE_DNG)
    list(APPEND INFRA_SOURCES src/infrastructure/factories/converter_factory.cpp)
  endif()

  if(INFRA_SOURCES)
    add_library(infrastructure STATIC ${INFRA_SOURCES})
    set_target_properties(infrastructure PROPERTIES CXX_STANDARD 20)
    target_include_directories(infrastructure PUBLIC include)
    target_include_directories(infrastructure PRIVATE src)
    if(INFRA_DEPS)
      target_link_libraries(infrastructure PRIVATE ${INFRA_DEPS} m)
    endif()
  endif()

  # Domain library (types impl + use cases)
  add_library(domain STATIC
    src/domain/types.cpp
    src/domain/use_cases.cpp
  )
  set_target_properties(domain PROPERTIES CXX_STANDARD 20)
  target_include_directories(domain PUBLIC include)
  if(INFRA_SOURCES)
    target_link_libraries(domain PUBLIC infrastructure)
  endif()

  # CLI application (hexagonal composition root)
  add_executable(rawimport-cli
    src/cli/main.cpp
  )
  set_target_properties(rawimport-cli PROPERTIES CXX_STANDARD 20)
  target_include_directories(rawimport-cli PRIVATE include src)
  target_link_libraries(rawimport-cli PRIVATE infrastructure domain)
endif()

# ============================================================================
# Tests
# ============================================================================
if(BUILD_TESTING)
  enable_testing()

  # Domain types test — zero external deps.
  add_executable(domain_types_test tests/domain/types_test.cpp)
  set_target_properties(domain_types_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(domain_types_test PRIVATE include src tests)
  add_test(NAME domain_types_tests COMMAND domain_types_test)

  # Port interface test — verifies all port headers compile.
  add_executable(port_interface_test tests/domain/ports_test.cpp)
  set_target_properties(port_interface_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(port_interface_test PRIVATE include tests)
  add_test(NAME port_interface_tests COMMAND port_interface_test)

  # Infrastructure smoke test.
  add_executable(infra_test tests/infrastructure/infra_test.cpp)
  set_target_properties(infra_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(infra_test PRIVATE include src tests)
  add_test(NAME infra_tests COMMAND infra_test)

  # Converter factory tests.
  add_executable(converter_factory_test tests/infrastructure/converter_factory_test.cpp)
  set_target_properties(converter_factory_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(converter_factory_test PRIVATE include src tests)
  if(WITH_DNGLAB OR WITH_ADOBE_DNG)
    target_link_libraries(converter_factory_test PRIVATE infrastructure)
  endif()
  add_test(NAME converter_factory_tests COMMAND converter_factory_test)

  # Migration tests (need both DB drivers).
  add_executable(migration_test tests/migration/migration_test.cpp)
  set_target_properties(migration_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(migration_test PRIVATE include src tests)
  if(WITH_MARIADB AND WITH_POSTGRESQL)
    target_link_libraries(migration_test PRIVATE infrastructure)
  endif()
  add_test(NAME migration_tests COMMAND migration_test)
endif()

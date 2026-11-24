cmake_minimum_required(VERSION 3.25)
project(rawimport_pipeline VERSION 2.3.27 LANGUAGES CXX)

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
  set(INFRA_INCLUDE_DIRS "")

  list(APPEND INFRA_SOURCES src/infrastructure/std_filesystem.cpp)
  list(APPEND INFRA_SOURCES src/infrastructure/env_config_provider.cpp)
  list(APPEND INFRA_SOURCES src/infrastructure/prometheus_metrics_sink.cpp)

  # OpenSSL-based hash service
  find_package(OpenSSL REQUIRED)
  if(OPENSSL_FOUND)
    list(APPEND INFRA_SOURCES src/infrastructure/openssl_hash_service.cpp)
    list(APPEND INFRA_DEPS OpenSSL::SSL OpenSSL::Crypto)
    add_compile_definitions(HAVE_OPENSSL)
    set(HAVE_OPENSSL ON CACHE INTERNAL "OpenSSL found")
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
    if(NOT MARIADB_LIB OR NOT MARIADB_INCLUDE)
      message(FATAL_ERROR "WITH_MARIADB=ON but MariaDB client library/headers not found. Install libmariadb-dev (Debian/Ubuntu) or mariadb-connector-c-devel (RHEL/Fedora), then retry.")
    endif()
    add_compile_definitions(WITH_MARIADB)
    list(APPEND INFRA_SOURCES src/infrastructure/mariadb_import_store.cpp)
    list(APPEND INFRA_DEPS ${MARIADB_LIB})
    target_include_directories(domain_types INTERFACE ${MARIADB_INCLUDE})
  endif()

  if(WITH_POSTGRESQL)
    # Try full PostgreSQL find_package first (requires server headers)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
      add_compile_definitions(WITH_POSTGRESQL)
      list(APPEND INFRA_SOURCES src/infrastructure/postgresql_import_store.cpp)
      list(APPEND INFRA_DEPS PostgreSQL::PostgreSQL)
      set(POSTGRESQL_AVAILABLE TRUE)
    else()
      # Fallback: try pkg-config for libpq (works with libpq-dev only, no server)
      find_package(PkgConfig QUIET)
      if(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_LIBPQ QUIET libpq)
        if(PC_LIBPQ_FOUND)
          add_compile_definitions(WITH_POSTGRESQL)
          list(APPEND INFRA_SOURCES src/infrastructure/postgresql_import_store.cpp)
          add_library(PostgreSQL::Libpq INTERFACE IMPORTED)
          set_target_properties(PostgreSQL::Libpq PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${PC_LIBPQ_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${PC_LIBPQ_LIBRARIES}"
            INTERFACE_LINK_DIRECTORIES "${PC_LIBPQ_LIBRARY_DIRS}"
          )
          list(APPEND INFRA_DEPS PostgreSQL::Libpq)
          message(STATUS "Found libpq via pkg-config: ${PC_LIBPQ_INCLUDE_DIRS}")
          set(POSTGRESQL_AVAILABLE TRUE)
        else()
          message(FATAL_ERROR "WITH_POSTGRESQL=ON but libpq not found via pkg-config. Install libpq-dev (Debian/Ubuntu) or postgresql-devel (RHEL/Fedora).")
        endif()
      else()
        message(FATAL_ERROR "WITH_POSTGRESQL=ON but neither PostgreSQL package nor pkg-config found. Install libpq-dev and pkg-config.")
      endif()
    endif()
  endif()

  if(WITH_DNGLAB)
    find_library(DNGLAB_LIBRARY NAMES dnglab)
    find_path(DNGLAB_INCLUDE_DIR NAMES dnglab/converter.hpp)
    if(DNGLAB_LIBRARY AND DNGLAB_INCLUDE_DIR)
      add_compile_definitions(WITH_DNGLAB)
      list(APPEND INFRA_SOURCES src/infrastructure/dnglab_converter_engine.cpp)
      list(APPEND INFRA_DEPS ${DNGLAB_LIBRARY})
      list(APPEND INFRA_INCLUDE_DIRS ${DNGLAB_INCLUDE_DIR})
    else()
      message(WARNING "WITH_DNGLAB requested but dnglab library not found")
    endif()
  endif()

  if(WITH_ADOBE_DNG)
    add_compile_definitions(WITH_ADOBE_DNG)
    list(APPEND INFRA_SOURCES src/infrastructure/adobe_dng_converter_engine.cpp)
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
    find_package(spdlog REQUIRED)
    add_compile_definitions(WITH_SPDLOG)
    list(APPEND INFRA_SOURCES src/infrastructure/spdlog_log_sink.cpp)
    if(TARGET spdlog::spdlog)
      list(APPEND INFRA_DEPS spdlog::spdlog)
    endif()
  endif()

  # Migration infrastructure (requires both DB libraries)
  if(MARIADB_LIB AND MARIADB_INCLUDE AND POSTGRESQL_AVAILABLE)
    list(APPEND INFRA_SOURCES
      src/infrastructure/migration_options.hpp
      src/infrastructure/migration_runner.cpp
      src/infrastructure/migration_runner.hpp
    )
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
    if(INFRA_INCLUDE_DIRS)
      target_include_directories(infrastructure PUBLIC ${INFRA_INCLUDE_DIRS})
    endif()
    target_include_directories(infrastructure PRIVATE src)
    if(INFRA_DEPS)
      target_link_libraries(infrastructure PRIVATE ${INFRA_DEPS})
      if(UNIX AND NOT APPLE)
        target_link_libraries(infrastructure PRIVATE m)
      endif()
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
    src/cli/migrate_command.cpp
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
  target_link_libraries(infra_test PRIVATE infrastructure)
  if(HAVE_OPENSSL)
    target_link_libraries(infra_test PRIVATE OpenSSL::Crypto)
  endif()
  add_test(NAME infra_tests COMMAND infra_test)

  # Converter factory tests.
  add_executable(converter_factory_test tests/infrastructure/converter_factory_test.cpp)
  set_target_properties(converter_factory_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(converter_factory_test PRIVATE include src tests)
  if(WITH_DNGLAB OR WITH_ADOBE_DNG)
    target_link_libraries(converter_factory_test PRIVATE infrastructure)
  endif()
  add_test(NAME converter_factory_tests COMMAND converter_factory_test)

  # Migration tests (need both DB drivers with libraries found).
  if(MARIADB_LIB AND MARIADB_INCLUDE AND POSTGRESQL_AVAILABLE)
    add_executable(migration_test tests/migration/migration_test.cpp)
    set_target_properties(migration_test PROPERTIES CXX_STANDARD 20)
    target_include_directories(migration_test PRIVATE include src tests)
    target_link_libraries(migration_test PRIVATE infrastructure)
    add_test(NAME migration_tests COMMAND migration_test)
  endif()

  # E2E test with .NRW fixtures (requires DB + converter engine)
  option(BUILD_E2E_TESTS "Build end-to-end tests with NRW fixtures" OFF)
  if(BUILD_E2E_TESTS)
    # Require database backend
    if(NOT WITH_POSTGRESQL AND NOT WITH_MARIADB)
      message(FATAL_ERROR "BUILD_E2E_TESTS requires WITH_POSTGRESQL=ON or WITH_MARIADB=ON")
    endif()
    # Require converter engine
    if(NOT WITH_DNGLAB AND NOT WITH_ADOBE_DNG)
      message(FATAL_ERROR "BUILD_E2E_TESTS requires WITH_DNGLAB=ON or WITH_ADOBE_DNG=ON")
    endif()
    # Require OpenSSL for hashing
    if(NOT HAVE_OPENSSL)
      message(FATAL_ERROR "BUILD_E2E_TESTS requires OpenSSL (HAVE_OPENSSL)")
    endif()
    # Test suite files must exist
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/testsuite/DSCN7027.NRW)
      message(FATAL_ERROR "BUILD_E2E_TESTS requires testsuite fixtures in testsuite/")
    endif()

    add_executable(e2e_nrw_test tests/e2e/nrw_e2e_test.cpp)
      set_target_properties(e2e_nrw_test PROPERTIES CXX_STANDARD 20)
      target_include_directories(e2e_nrw_test PRIVATE include src tests)
      target_compile_definitions(e2e_nrw_test PRIVATE
        $<$<BOOL:${WITH_DNGLAB}>:WITH_DNGLAB=1>
        $<$<BOOL:${WITH_ADOBE_DNG}>:WITH_ADOBE_DNG=1>
        $<$<BOOL:${WITH_EXIFTOOL}>:WITH_EXIFTOOL=1>
        $<$<BOOL:${HAVE_OPENSSL}>:HAVE_OPENSSL=1>
      )
      target_link_libraries(e2e_nrw_test PRIVATE infrastructure domain)
    # Propagate PostgreSQL include dirs (either full find_package or pkg-config fallback)
    if(WITH_POSTGRESQL)
      if(TARGET PostgreSQL::PostgreSQL)
        target_link_libraries(e2e_nrw_test PRIVATE PostgreSQL::PostgreSQL)
      elseif(TARGET PostgreSQL::Libpq)
        target_link_libraries(e2e_nrw_test PRIVATE PostgreSQL::Libpq)
      endif()
    endif()
    if(WITH_SPDLOG)
      target_link_libraries(e2e_nrw_test PRIVATE spdlog::spdlog)
    endif()
    if(UNIX AND NOT APPLE)
      target_link_libraries(e2e_nrw_test PRIVATE m pthread dl)
    endif()
    add_test(NAME e2e_nrw_tests COMMAND e2e_nrw_test)
  endif()
endif()

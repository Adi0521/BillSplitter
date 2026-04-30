cmake_minimum_required(VERSION 3.20)
project(billsplitter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies via FetchContent ──────────────────────────────────────────────
include(FetchContent)

# Crow (header-only HTTP framework)
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
)
FetchContent_MakeAvailable(Crow)

# nlohmann/json (header-only)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ── System dependencies ────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)   # provides PostgreSQL::PostgreSQL
find_package(CURL REQUIRED)

# libpqxx — try pkg-config first, fall back to manual
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PQXX IMPORTED_TARGET libpqxx)
endif()
if(NOT PQXX_FOUND)
    find_library(PQXX_LIB NAMES pqxx REQUIRED)
    find_path(PQXX_INCLUDE_DIR NAMES pqxx/pqxx REQUIRED)
    add_library(PkgConfig::pqxx UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::pqxx PROPERTIES
        IMPORTED_LOCATION "${PQXX_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${PQXX_INCLUDE_DIR}"
    )
endif()

# ── Sources ────────────────────────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
)

add_executable(billsplitter-backend ${SOURCES})

target_include_directories(billsplitter-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(billsplitter-backend PRIVATE
    Crow::Crow
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    PkgConfig::pqxx
    ${PostgreSQL_LIBRARIES}
)

# Compiler warnings
target_compile_options(billsplitter-backend PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

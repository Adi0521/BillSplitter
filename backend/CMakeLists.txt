cmake_minimum_required(VERSION 3.20)
project(billsplitter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake 4 removed compatibility with projects declaring cmake_minimum_required
# below 3.5. Some FetchContent dependencies still do; this floor keeps them
# configurable without patching their CMakeLists.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Homebrew installs openssl@3 and postgresql keg-only, so find_package needs
# explicit hints on macOS or it silently picks the system LibreSSL.
if(APPLE)
    execute_process(COMMAND brew --prefix openssl@3
        OUTPUT_VARIABLE BREW_OPENSSL OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(BREW_OPENSSL)
        set(OPENSSL_ROOT_DIR "${BREW_OPENSSL}")
    endif()
    execute_process(COMMAND brew --prefix libpq
        OUTPUT_VARIABLE BREW_LIBPQ OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(BREW_LIBPQ)
        list(APPEND CMAKE_PREFIX_PATH "${BREW_LIBPQ}")
    endif()
endif()

# ── Dependencies via FetchContent ──────────────────────────────────────────────
include(FetchContent)

# Crow (header-only HTTP framework)
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.3.3
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

# libpqxx — prefer its own CMake config, then pkg-config, then a manual search.
# Each branch defines the same target name (billsplitter::pqxx) so the link line
# below does not need to know which one was taken. Note that pkg_check_modules
# names its imported target after the PREFIX argument (PkgConfig::PQXX), not
# after the module.
add_library(billsplitter::pqxx INTERFACE IMPORTED)

find_package(libpqxx QUIET CONFIG)
if(libpqxx_FOUND)
    target_link_libraries(billsplitter::pqxx INTERFACE libpqxx::pqxx)
    message(STATUS "libpqxx: using CMake config package")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PQXX IMPORTED_TARGET libpqxx)
    endif()
    if(TARGET PkgConfig::PQXX)
        target_link_libraries(billsplitter::pqxx INTERFACE PkgConfig::PQXX)
        message(STATUS "libpqxx: using pkg-config (${PQXX_VERSION})")
    else()
        find_library(PQXX_LIB NAMES pqxx REQUIRED)
        find_path(PQXX_INCLUDE_DIR NAMES pqxx/pqxx REQUIRED)
        target_link_libraries(billsplitter::pqxx INTERFACE "${PQXX_LIB}")
        target_include_directories(billsplitter::pqxx INTERFACE "${PQXX_INCLUDE_DIR}")
        message(STATUS "libpqxx: using manual search (${PQXX_LIB})")
    endif()
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
    billsplitter::pqxx
    ${PostgreSQL_LIBRARIES}
)

# Compiler warnings
target_compile_options(billsplitter-backend PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

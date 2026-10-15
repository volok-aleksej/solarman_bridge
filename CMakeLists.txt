cmake_minimum_required(VERSION 3.20)
project(solarman_bridge
    VERSION 1.2.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CONFUSE           REQUIRED IMPORTED_TARGET libconfuse)
pkg_check_modules(SQLITE3           REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CJSON             REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(LIBEVENT          REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(LIBEVENT_PTHREADS REQUIRED IMPORTED_TARGET libevent_pthreads)
pkg_check_modules(LIBCRYPT          REQUIRED IMPORTED_TARGET libcrypt)

find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

# All production code as one object lib, shared between the main binary and
# the test binary; main.cpp stays out so gtest_main owns the test entry point.
# OBJECT, not STATIC: every object is linked in, so the model files in
# src/models/, which nothing references and which only register themselves,
# are not dropped by the linker.
add_library(solarman_core OBJECT ${CORE_SOURCES} ${CORE_HEADERS})
target_include_directories(solarman_core PUBLIC include)
target_link_libraries(solarman_core PUBLIC
    PkgConfig::CONFUSE
    PkgConfig::SQLITE3
    spdlog::spdlog
    Threads::Threads
)
target_link_libraries(solarman_core PRIVATE
    PkgConfig::CJSON
    PkgConfig::LIBEVENT
    PkgConfig::LIBEVENT_PTHREADS
    PkgConfig::LIBCRYPT
)

add_executable(solarman_bridge src/main.cpp)
target_link_libraries(solarman_bridge PRIVATE solarman_core)

include(GNUInstallDirs)
install(TARGETS solarman_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/solarman_bridge.conf
        RENAME solarman_bridge.conf.template
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/solarman_bridge)
install(DIRECTORY config/www/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/solarman_bridge/www)

enable_testing()
add_subdirectory(tests)

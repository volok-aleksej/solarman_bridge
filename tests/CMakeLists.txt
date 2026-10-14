find_package(GTest REQUIRED)
include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp"
)

add_executable(solarman_tests ${TEST_SOURCES})
target_link_libraries(solarman_tests PRIVATE
    solarman_core
    GTest::gtest_main
)
gtest_discover_tests(solarman_tests)

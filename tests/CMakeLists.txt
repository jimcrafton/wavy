add_executable(wavy_tests
    test_audioProgram.cpp
    test_audioSteps.cpp
    test_version.cpp
)

target_link_libraries(wavy_tests PRIVATE wavy::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wavy_tests)

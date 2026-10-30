add_executable(wavy_tests
    test_audioEngine.cpp
    test_audioEvents.cpp
    test_audioProgram.cpp
    test_audioSteps.cpp
    test_clipPlayer.cpp
    test_version.cpp
)

target_link_libraries(wavy_tests PRIVATE wavy::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wavy_tests)

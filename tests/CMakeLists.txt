find_package(GTest CONFIG REQUIRED)

add_executable(nemo_core_tests
    GraphTests.cpp
    CommandTests.cpp
    PersistenceTests.cpp
    TimingTests.cpp
)
target_link_libraries(nemo_core_tests PRIVATE nemo::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(nemo_core_tests)

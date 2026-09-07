find_package(GTest CONFIG REQUIRED)

add_executable(nemo_core_tests
    TimingTests.cpp
    EvaluationTests.cpp
    CommandTests.cpp
    PersistenceTests.cpp
)
target_link_libraries(nemo_core_tests PRIVATE nemo::core GTest::gtest_main)

# Tests intentionally ignore returned ids when they only need the side effect.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core_tests PRIVATE -Wno-unused-result)
endif()

include(GoogleTest)
gtest_discover_tests(nemo_core_tests)

find_package(GTest CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)

add_executable(nemo_core_tests
    TimingTests.cpp
    EvaluationTests.cpp
    CommandTests.cpp
    PersistenceTests.cpp
)
target_link_libraries(nemo_core_tests PRIVATE nemo::core GTest::gtest_main)

add_executable(nemo_media_tests
    MediaTests.cpp
)
target_link_libraries(nemo_media_tests PRIVATE nemo::media GTest::gtest_main OpenImageIO::OpenImageIO)

# Tests intentionally ignore returned ids when they only need the side effect.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core_tests PRIVATE -Wno-unused-result)
    target_compile_options(nemo_media_tests PRIVATE -Wno-unused-result)
endif()

include(GoogleTest)
gtest_discover_tests(nemo_core_tests)
gtest_discover_tests(nemo_media_tests)

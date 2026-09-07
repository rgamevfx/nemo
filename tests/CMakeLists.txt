find_package(GTest CONFIG REQUIRED)

add_executable(nemo_core_tests
    GraphTests.cpp
    CommandTests.cpp
    PersistenceTests.cpp
    TimingTests.cpp
)
target_link_libraries(nemo_core_tests PRIVATE nemo::core GTest::gtest_main)

if(NEMO_BUILD_GPU)
    # Gpu* tests live beside the core suite so `ctest -R Gpu` and
    # `nemo_core_tests --gtest_filter='Gpu*'` both address them (issue #2).
    target_sources(nemo_core_tests PRIVATE GpuTests.cpp)
    target_link_libraries(nemo_core_tests PRIVATE nemo::gpu)
endif()

# Tests intentionally ignore returned ids when they only need the side effect.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core_tests PRIVATE -Wno-unused-result)
endif()

include(GoogleTest)
gtest_discover_tests(nemo_core_tests)

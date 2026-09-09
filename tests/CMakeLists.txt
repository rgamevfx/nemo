find_package(GTest CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)

add_executable(nemo_core_tests
    TimingTests.cpp
    EvaluationTests.cpp
    ReuseTests.cpp
    CommandTests.cpp
    PersistenceTests.cpp
    ViewerModelTests.cpp
)
target_link_libraries(nemo_core_tests PRIVATE nemo::core GTest::gtest_main)

add_executable(nemo_media_tests
    MediaTests.cpp
)
target_link_libraries(nemo_media_tests PRIVATE nemo::media GTest::gtest_main OpenImageIO::OpenImageIO)
if(NEMO_BUILD_GPU)
    # Gpu* tests live beside the core suite so `ctest -R Gpu` and
    # `nemo_core_tests --gtest_filter='Gpu*'` both address them (issue #2).
    target_sources(nemo_core_tests PRIVATE GpuTests.cpp)
    target_link_libraries(nemo_core_tests PRIVATE nemo::gpu)
endif()

if(NEMO_BUILD_GPU)
    # Color* tests (issue #6): policy serialization plus the OCIO viewing
    # transform with its GPU-native acceptance path (`ctest -R Color`).
    add_executable(nemo_color_tests
        ColorTests.cpp
    )
    target_link_libraries(nemo_color_tests PRIVATE nemo::media nemo::gpu GTest::gtest_main)

    # HwMedia* tests (issue #10): hardware decode interop and the codec
    # sweep harness. Vulkan video decode requires the application device;
    # the mediaConvert SPIR-V comes from the nemo_shaders target.
    add_executable(nemo_hwmedia_tests
        HwMediaTests.cpp
        MediaDecodeTests.cpp
        MediaEncodeTests.cpp
    )
    target_link_libraries(nemo_hwmedia_tests PRIVATE nemo::media nemo::gpu GTest::gtest_main)
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo_hwmedia_tests PRIVATE NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}")
    endif()
endif()

# Tests intentionally ignore returned ids when they only need the side effect.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core_tests PRIVATE -Wno-unused-result)
    target_compile_options(nemo_media_tests PRIVATE -Wno-unused-result)
endif()

include(GoogleTest)
gtest_discover_tests(nemo_core_tests)
gtest_discover_tests(nemo_media_tests)

if(NEMO_BUILD_GPU)
    gtest_discover_tests(nemo_color_tests)
    gtest_discover_tests(nemo_hwmedia_tests)
endif()

# Effect* tests (issue #8): native GPU effect execution over the shared
# plan — Slang/GLSL front ends, CPU-reference tolerances, contract
# violations must fail. Slang SPIR-V comes from the nemo_shaders target.
if(NEMO_BUILD_GPU)
    # Effect* tests (issue #8): native GPU effect execution over the shared
    # plan — Slang/GLSL front ends, CPU-reference tolerances, contract
    # violations must fail. Slang SPIR-V comes from the nemo_shaders target.
    add_executable(nemo_effect_tests
        GpuEffectTests.cpp
    )
    target_link_libraries(nemo_effect_tests PRIVATE nemo::eval nemo::gpu GTest::gtest_main)
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo_effect_tests PRIVATE
            NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}"
            NEMO_SLANG_SRC_DIR="${CMAKE_SOURCE_DIR}/src/nemo/gpu/shaders")
    endif()
    gtest_discover_tests(nemo_effect_tests)
    add_executable(nemo_viewer_tests ViewerTests.cpp)
    target_sources(nemo_viewer_tests PRIVATE InteractiveSchedulerTests.cpp)
    target_link_libraries(nemo_viewer_tests PRIVATE nemo::eval nemo::media GTest::gtest_main)
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo_viewer_tests PRIVATE NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}")
        add_dependencies(nemo_viewer_tests nemo_shaders)
    endif()
    gtest_discover_tests(nemo_viewer_tests)
endif()

add_executable(nemo_workspace_tests WorkspaceTests.cpp)
target_link_libraries(nemo_workspace_tests PRIVATE nemo::workspace GTest::gtest_main)
gtest_discover_tests(nemo_workspace_tests)

if(NEMO_BUILD_UI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2 Test)
    add_executable(nemo_workspace_ui_tests
        WorkspaceDragTests.cpp
        InteractiveUiTests.cpp
        ../apps/nemo-ui/WorkspaceController.cpp
        ../apps/nemo-ui/WorkspaceController.hpp
        ../apps/nemo-ui/ViewerController.cpp
        ../apps/nemo-ui/ViewerController.hpp
        ../apps/nemo-ui/ViewerRuntime.cpp
        ../apps/nemo-ui/ViewerRuntime.hpp
        ../apps/nemo-ui/ViewerItem.cpp
        ../apps/nemo-ui/ViewerItem.hpp
    )
    set_target_properties(nemo_workspace_ui_tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(nemo_workspace_ui_tests PRIVATE
        NEMO_UI_QML_DIR="${PROJECT_SOURCE_DIR}/apps/nemo-ui/qml"
    )
    target_link_libraries(nemo_workspace_ui_tests PRIVATE
        nemo::workspace nemo::eval GTest::gtest Qt6::Quick Qt6::QuickControls2 Qt6::Test
    )
    gtest_discover_tests(nemo_workspace_ui_tests)
endif()

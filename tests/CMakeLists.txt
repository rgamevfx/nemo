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

add_executable(nemo_workspace_tests WorkspaceTests.cpp)
target_link_libraries(nemo_workspace_tests PRIVATE nemo::workspace GTest::gtest_main)
gtest_discover_tests(nemo_workspace_tests)

if(NEMO_BUILD_UI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2 Test)
    add_executable(nemo_workspace_ui_tests
        WorkspaceDragTests.cpp
        ../apps/nemo-ui/WorkspaceController.cpp
        ../apps/nemo-ui/WorkspaceController.hpp
    )
    set_target_properties(nemo_workspace_ui_tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(nemo_workspace_ui_tests PRIVATE
        NEMO_UI_QML_DIR="${PROJECT_SOURCE_DIR}/apps/nemo-ui/qml"
    )
    target_link_libraries(nemo_workspace_ui_tests PRIVATE
        nemo::workspace GTest::gtest Qt6::Quick Qt6::QuickControls2 Qt6::Test
    )
    gtest_discover_tests(nemo_workspace_ui_tests)
endif()

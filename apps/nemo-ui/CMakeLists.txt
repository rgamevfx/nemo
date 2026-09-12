find_package(nlohmann_json CONFIG REQUIRED)

# The workspace model deliberately has no Qt or rendering dependencies.
add_library(nemo_workspace Workspace.cpp)
add_library(nemo::workspace ALIAS nemo_workspace)
target_include_directories(nemo_workspace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nemo_workspace PUBLIC nlohmann_json::nlohmann_json)

if(NEMO_BUILD_UI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2)
    if(UNIX AND NOT APPLE)
        # Native xdg-desktop-portal FileChooser client for Open/Save As/Recover.
        find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    endif()
    qt_standard_project_setup()
    if(NOT NEMO_BUILD_GPU)
        message(FATAL_ERROR "NEMO_BUILD_UI requires NEMO_BUILD_GPU for native viewer presentation")
    endif()
    qt_add_executable(nemo-ui main.cpp PanelContextRouter.cpp PanelContextRouter.hpp
        MediaLibraryModel.cpp MediaLibraryModel.hpp
        NativeFileChooser.cpp NativeFileChooser.hpp
        ParameterEditorRegistry.cpp ParameterEditorRegistry.hpp
        ProjectFileController.cpp ProjectFileController.hpp
        WorkspaceController.cpp WorkspaceController.hpp
        ViewerController.cpp ViewerController.hpp
        ViewerControllerRegistry.cpp ViewerControllerRegistry.hpp
        ViewerRuntime.cpp ViewerRuntime.hpp
        ViewerItem.cpp ViewerItem.hpp GraphItem.cpp GraphItem.hpp TimelineItem.cpp TimelineItem.hpp)
    qt_add_qml_module(nemo-ui
        URI Nemo
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        QML_FILES qml/Main.qml qml/WorkspaceNode.qml qml/Panel.qml qml/DockDrag.qml qml/PanelDragHandler.qml
                  qml/Theme.qml qml/ChromeButton.qml qml/StudioComboBox.qml qml/UnavailablePanel.qml
                  qml/ViewerPanel.qml qml/GraphPanel.qml qml/TimelinePanel.qml qml/ParametersPanel.qml
                  qml/MediaBinPanel.qml
    )
    target_link_libraries(nemo-ui PRIVATE nemo::core nemo::workspace nemo::eval Qt6::Quick Qt6::QuickControls2)
    if(UNIX AND NOT APPLE)
        target_link_libraries(nemo-ui PRIVATE Qt6::DBus)
    elseif(WIN32)
        # Win32 common item dialogs (IFileOpenDialog/IFileSaveDialog).
        target_link_libraries(nemo-ui PRIVATE ole32 shell32 uuid)
    endif()
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo-ui PRIVATE NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}")
        add_dependencies(nemo-ui nemo_shaders)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_workspace PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_workspace PRIVATE -Werror)
    endif()
    if(TARGET nemo-ui)
        target_compile_options(nemo-ui PRIVATE -Wall -Wextra -Wpedantic)
        if(NEMO_WARN_AS_ERROR)
            target_compile_options(nemo-ui PRIVATE -Werror)
        endif()
    endif()
endif()

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
    # One internal target owns the controller/runtime/item implementation the
    # application and its UI tests both need (issue #68). Both consumers link it
    # and inherit the common include directory, Qt/platform dependencies,
    # compile definitions, automoc handling and compiler options; neither
    # re-lists these sources.
    add_library(nemo_ui_shared STATIC
        HistoryController.cpp HistoryController.hpp
        PanelContextRouter.cpp PanelContextRouter.hpp
        AnimationViewModel.cpp AnimationViewModel.hpp
        MediaLibraryModel.cpp MediaLibraryModel.hpp
        NativeFileChooser.cpp NativeFileChooser.hpp
        ParameterEditorRegistry.cpp ParameterEditorRegistry.hpp
        ProjectFileController.cpp ProjectFileController.hpp
        ReadSourceController.cpp ReadSourceController.hpp
        WorkspaceController.cpp WorkspaceController.hpp
        ViewerController.cpp ViewerController.hpp
        ViewerControllerRegistry.cpp ViewerControllerRegistry.hpp
        ViewerRuntime.cpp ViewerRuntime.hpp
        ViewerItem.cpp ViewerItem.hpp GraphItem.cpp GraphItem.hpp TimelineItem.cpp TimelineItem.hpp)
    set_target_properties(nemo_ui_shared PROPERTIES AUTOMOC ON)
    target_include_directories(nemo_ui_shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(nemo_ui_shared PUBLIC
        nemo::core nemo::workspace nemo::eval Qt6::Quick Qt6::QuickControls2)
    if(UNIX AND NOT APPLE)
        target_link_libraries(nemo_ui_shared PUBLIC Qt6::DBus)
    elseif(WIN32)
        # Win32 common item dialogs (IFileOpenDialog/IFileSaveDialog).
        target_link_libraries(nemo_ui_shared PUBLIC ole32 shell32 uuid)
    endif()
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo_ui_shared PUBLIC NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}")
        add_dependencies(nemo_ui_shared nemo_shaders)
    endif()
    # The QML module stays URI `Nemo` with the same files and resource prefix;
    # it is declared on the shared target so the generated type registration and
    # compiled QML resources live with the C++ implementation the application
    # and its UI tests both link.
    qt_add_qml_module(nemo_ui_shared
        URI Nemo
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        QML_FILES qml/Main.qml qml/WorkspaceNode.qml qml/Panel.qml qml/DockDrag.qml qml/PanelDragHandler.qml
                  qml/Theme.qml qml/ChromeButton.qml qml/StudioComboBox.qml qml/UnavailablePanel.qml
                  qml/ViewerPanel.qml qml/GraphPanel.qml qml/TimelinePanel.qml qml/ParametersPanel.qml
                  qml/ReadSourceEditor.qml qml/MediaBinPanel.qml qml/SubnetParameters.qml
                  qml/AnimationPanel.qml qml/AnimationHeaderTools.qml qml/PinButton.qml
                  qml/NumericField.qml qml/ChannelEditor.qml qml/MergeOperationEditor.qml
                  qml/ShuffleEditor.qml
                  qml/KeyIndicator.qml qml/ExposureLabel.qml
                  qml/HistoryMenu.qml qml/HistoryMenuItem.qml
    )

    # The application keeps its own entry point and loads the module's compiled
    # resources at startup. Linking the module's plugin propagates the generated
    # registration and resource objects so static linking cannot drop them.
    qt_add_executable(nemo-ui main.cpp)
    target_link_libraries(nemo-ui PRIVATE nemo_ui_shared nemo_ui_sharedplugin)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_workspace PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_workspace PRIVATE -Werror)
    endif()
    # The shared UI implementation carries the warning options, so both
    # consumers compile it identically instead of only the application.
    if(TARGET nemo_ui_shared)
        target_compile_options(nemo_ui_shared PRIVATE -Wall -Wextra -Wpedantic)
        if(NEMO_WARN_AS_ERROR)
            target_compile_options(nemo_ui_shared PRIVATE -Werror)
        endif()
    endif()
    if(TARGET nemo-ui)
        target_compile_options(nemo-ui PRIVATE -Wall -Wextra -Wpedantic)
        if(NEMO_WARN_AS_ERROR)
            target_compile_options(nemo-ui PRIVATE -Werror)
        endif()
    endif()
endif()

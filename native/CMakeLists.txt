cmake_minimum_required(VERSION 3.21)
project(AiTaskHubNative VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Sql HttpServer Widgets)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(ai-task-hub-native
    src/main.cpp
    src/task_store.cpp
    src/task_store.h
)

qt_add_qml_module(ai-task-hub-native
    URI AiTaskHub
    VERSION 1.0
    SOURCES
        src/circular_image.cpp
        src/circular_image.h
    QML_FILES
        qml/Main.qml
)

target_link_libraries(ai-task-hub-native PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Widgets
)

target_include_directories(ai-task-hub-native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(ai-task-hub-native PROPERTIES
    WIN32_EXECUTABLE TRUE
)

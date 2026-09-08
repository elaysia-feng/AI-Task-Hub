cmake_minimum_required(VERSION 3.20)
project(ai_task_hub_win32 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ai-task-hub-win32 WIN32
    src/main.cpp
    src/json_lite.cpp
    src/task_store.cpp
    src/http_server.cpp
    src/integration_manager.cpp
    third_party/sqlite-amalgamation-3450200/sqlite3.c
)

target_include_directories(ai-task-hub-win32 PRIVATE
    src
    third_party/sqlite-amalgamation-3450200
)

target_compile_definitions(ai-task-hub-win32 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
)

if (MINGW)
    target_compile_options(ai-task-hub-win32 PRIVATE -municode -O2 -ffunction-sections -fdata-sections)
    target_link_options(ai-task-hub-win32 PRIVATE -municode -static -Wl,--gc-sections)
endif()

target_link_libraries(ai-task-hub-win32 PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    shlwapi
    shell32
    ole32
    uuid
    comctl32
    comdlg32
    advapi32
    user32
    gdi32
    ws2_32
)

set_target_properties(ai-task-hub-win32 PROPERTIES OUTPUT_NAME "AI Task Hub Win32")

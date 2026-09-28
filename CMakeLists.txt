cmake_minimum_required(VERSION 3.24)

project(
    LuaIDE
    VERSION 2.0.0
    LANGUAGES C CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "LuaIDE supports Windows only.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ============================================================
# Lua 5.5.1
# ============================================================

FetchContent_Declare(
    lua_source
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(lua_source)

file(
    GLOB LUA_SOURCES
    CONFIGURE_DEPENDS
    "${lua_source_SOURCE_DIR}/src/*.c"
)

# Эти файлы содержат main() либо включают все Lua-файлы повторно.
list(FILTER LUA_SOURCES EXCLUDE REGEX "[/\\\\]lua\\.c$")
list(FILTER LUA_SOURCES EXCLUDE REGEX "[/\\\\]luac\\.c$")
list(FILTER LUA_SOURCES EXCLUDE REGEX "[/\\\\]onelua\\.c$")
list(FILTER LUA_SOURCES EXCLUDE REGEX "[/\\\\]ltests\\.c$")

add_library(lua_runtime STATIC ${LUA_SOURCES})

target_include_directories(
    lua_runtime
    PUBLIC
        "${lua_source_SOURCE_DIR}/src"
)

target_compile_definitions(
    lua_runtime
    PRIVATE
        _CRT_SECURE_NO_WARNINGS
        LUA_USE_WINDOWS
)

# ============================================================
# Dear ImGui, docking branch
# ============================================================

FetchContent_Declare(
    imgui_source
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(imgui_source)

add_library(
    imgui STATIC

    "${imgui_source_SOURCE_DIR}/imgui.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_widgets.cpp"

    "${imgui_source_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_source_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(
    imgui
    PUBLIC
        "${imgui_source_SOURCE_DIR}"
        "${imgui_source_SOURCE_DIR}/backends"
)

target_compile_definitions(
    imgui
    PUBLIC
        IMGUI_DEFINE_MATH_OPERATORS
    PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_link_libraries(
    imgui
    PUBLIC
        d3d11
        dxgi
        user32
        gdi32
        imm32
        dwmapi
)

# ============================================================
# ImGuiColorTextEdit
# ============================================================

FetchContent_Declare(
    text_editor_source
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(text_editor_source)

add_library(
    imgui_text_editor STATIC
    "${text_editor_source_SOURCE_DIR}/TextEditor.cpp"
)

target_include_directories(
    imgui_text_editor
    PUBLIC
        "${text_editor_source_SOURCE_DIR}"
)

target_link_libraries(
    imgui_text_editor
    PUBLIC
        imgui
)

# ============================================================
# LuaIDE
# ============================================================

add_executable(
    LuaIDE WIN32

    src/main.cpp

    src/IdeApplication.hpp
    src/IdeApplication.cpp

    src/LuaRunner.hpp
    src/LuaRunner.cpp
)

target_compile_definitions(
    LuaIDE
    PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0A00
)

target_link_libraries(
    LuaIDE
    PRIVATE
        lua_runtime
        imgui
        imgui_text_editor

        d3d11
        dxgi
        user32
        gdi32
        imm32
        dwmapi
        comdlg32
        shell32
        ole32
)

if(MSVC)
    target_compile_options(
        LuaIDE
        PRIVATE
            /W4
            /utf-8
            /permissive-
            /EHsc
    )

    target_compile_options(lua_runtime PRIVATE /W0 /utf-8)
    target_compile_options(imgui PRIVATE /W0 /utf-8)
    target_compile_options(imgui_text_editor PRIVATE /W0 /utf-8)
endif()

set_target_properties(
    LuaIDE
    PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:LuaIDE>"
)

set_property(
    DIRECTORY
    PROPERTY VS_STARTUP_PROJECT LuaIDE
)
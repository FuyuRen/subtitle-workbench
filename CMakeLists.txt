cmake_minimum_required(VERSION 3.25)
project(subtitle-workbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# --- imgui ---
add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    vendor/imgui
    vendor/imgui/backends
)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler)

# --- swb core library ---
add_library(swb STATIC
    src/swb/config.cpp
    src/swb/output.cpp
    src/swb/source_subtitle_resolution.cpp
    src/swb/task.cpp
    src/swb/cli.cpp
    src/swb/http.cpp
    src/swb/process.cpp
    src/swb/workspace.cpp
    src/swb/ytdlp.cpp
    src/swb/audio.cpp
    src/swb/segment.cpp
    src/swb/srt.cpp
    src/swb/translate.cpp
    src/swb/transcribe.cpp
)
target_include_directories(swb PUBLIC src)
target_link_libraries(swb PUBLIC imgui winhttp)

# --- ui library ---
add_library(swb_ui STATIC
    src/swb/app.cpp
    src/swb/windows_task_feedback.cpp
)
target_include_directories(swb_ui PUBLIC src)
target_link_libraries(swb_ui PUBLIC swb)

# --- executable ---
add_executable(subtitle-workbench WIN32 src/main.cpp)
target_link_libraries(subtitle-workbench PRIVATE swb_ui shell32 ole32 uuid)

# --- tests ---
option(SWB_BUILD_TESTS "Build unit tests" ON)
if(SWB_BUILD_TESTS)
    enable_testing()
    add_executable(swb_tests
        tests/test_main.cpp
        tests/test_config.cpp
        tests/test_output.cpp
        tests/test_audio.cpp
        tests/test_segment.cpp
        tests/test_srt.cpp
        tests/test_task.cpp
        tests/test_translate.cpp
        tests/test_transcribe.cpp
        tests/test_ytdlp.cpp
        tests/test_workspace.cpp
        tests/test_cli.cpp
    )
    target_include_directories(swb_tests PRIVATE tests)
    target_link_libraries(swb_tests PRIVATE swb)
    add_test(NAME swb_tests COMMAND swb_tests)
endif()

# --- install/package ---
install(TARGETS subtitle-workbench RUNTIME DESTINATION .)
install(FILES ${CMAKE_SOURCE_DIR}/config.ini.example DESTINATION .)

option(SWB_BUNDLE_EXTERNAL_TOOLS "Bundle ffmpeg, ffprobe and yt-dlp into the distribution package when found" OFF)
if(SWB_BUNDLE_EXTERNAL_TOOLS)
    find_program(SWB_FFMPEG_EXECUTABLE NAMES ffmpeg.exe ffmpeg DOC "Path to ffmpeg executable for distribution package")
    if(SWB_FFMPEG_EXECUTABLE)
        install(PROGRAMS "${SWB_FFMPEG_EXECUTABLE}" DESTINATION vendor/bin)
    else()
        message(WARNING "ffmpeg executable not found. The distribution package will not include ffmpeg.exe.")
    endif()

    if(SWB_FFMPEG_EXECUTABLE)
        get_filename_component(SWB_FFMPEG_DIRECTORY "${SWB_FFMPEG_EXECUTABLE}" DIRECTORY)
        find_program(
            SWB_FFPROBE_EXECUTABLE
            NAMES ffprobe.exe ffprobe
            HINTS "${SWB_FFMPEG_DIRECTORY}"
            DOC "Path to ffprobe executable for distribution package")
    else()
        find_program(SWB_FFPROBE_EXECUTABLE NAMES ffprobe.exe ffprobe DOC "Path to ffprobe executable for distribution package")
    endif()
    if(SWB_FFPROBE_EXECUTABLE)
        install(PROGRAMS "${SWB_FFPROBE_EXECUTABLE}" DESTINATION vendor/bin)
    else()
        message(WARNING "ffprobe executable not found. The distribution package will not include ffprobe.exe.")
    endif()

    find_program(SWB_YTDLP_EXECUTABLE NAMES yt-dlp.exe yt-dlp DOC "Path to yt-dlp executable for distribution package")
    if(SWB_YTDLP_EXECUTABLE)
        install(PROGRAMS "${SWB_YTDLP_EXECUTABLE}" DESTINATION vendor/bin)
    else()
        message(WARNING "yt-dlp executable not found. The distribution package will not include yt-dlp.exe.")
    endif()
endif()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)

set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-release-windows-x64")
include(CPack)

# menu.dll: the loader's menu (ImGui over CS2 via a D3D11 Present hook). Build:
#   cmake -S . -B build -A x64
#   cmake --build build --config Release        -> build/bin/menu.dll, build/bin/preview.exe
cmake_minimum_required(VERSION 3.21)
project(menu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one file to ship
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui minhook)

add_library(alm_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(alm_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
add_library(alm_minhook STATIC ${minhook_SOURCE_DIR}/src/buffer.c ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c ${minhook_SOURCE_DIR}/src/hde/hde32.c ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(alm_minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(menu SHARED src/main.cpp src/Hooks.cpp src/Menu.cpp)
target_compile_definitions(menu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(menu PRIVATE /utf-8 /W4 /permissive- /wd4100)
target_link_libraries(menu PRIVATE alm_imgui alm_minhook d3d11 dxgi)

# Offline look + logic check: WARP device renders every page to build/preview_*.bmp
add_executable(preview tools/preview.cpp src/Menu.cpp)
target_include_directories(preview PRIVATE src)
target_compile_definitions(preview PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(preview PRIVATE alm_imgui d3d11 dxgi)

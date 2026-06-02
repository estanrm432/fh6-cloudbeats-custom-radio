cmake_minimum_required(VERSION 3.20)
project(HorizonCloudBeatsApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(_WIN32_WINNT=0x0A00 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  add_compile_options(/utf-8 /EHsc /W3)
  add_compile_options("$<$<CONFIG:Release>:/O2>")
endif()

set(WV2 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")

# WIN32 => /SUBSYSTEM:WINDOWS (no console window).
add_executable(HorizonCloudBeats WIN32 main.cpp app.rc)

target_include_directories(HorizonCloudBeats PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${WV2}/include"
)
target_link_directories(HorizonCloudBeats PRIVATE "${WV2}/x64")

# WebView2LoaderStatic links the loader into the .exe (no extra DLL to ship);
# the WebView2 *runtime* is preinstalled on Windows 10/11 via Edge.
target_link_libraries(HorizonCloudBeats PRIVATE
  WebView2LoaderStatic
  ole32 oleaut32 user32 gdi32 gdiplus shell32 advapi32 version shlwapi ws2_32 dwmapi
)

# We embed our own manifest via app.rc — stop the linker generating a second
# one (otherwise CVT1100 "duplicate resource: MANIFEST").
if(MSVC)
  target_link_options(HorizonCloudBeats PRIVATE /MANIFEST:NO)
endif()

set_target_properties(HorizonCloudBeats PROPERTIES OUTPUT_NAME "HorizonCloudBeats")

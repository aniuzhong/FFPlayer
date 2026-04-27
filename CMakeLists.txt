cmake_minimum_required(VERSION 3.16)
project(FFPlayer C CXX)

set(CMAKE_C_STANDARD 99)
include(FetchContent)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Root directory for bundled third-party packages")
set(FETCHCONTENT_UPDATES_DISCONNECTED OFF CACHE BOOL
  "Allow FetchContent git dependencies to check updates on configure")
set(FFMPEG_PREBUILT_ROOT "${THIRD_PARTY_DIR}/ffmpeg-8.1-full_build-shared" CACHE PATH
    "gyan FFmpeg tree (include/, lib/, bin/)")
set(FFMPEG_LIB_DIR "${FFMPEG_PREBUILT_ROOT}/lib" CACHE PATH "Directory containing FFmpeg import libraries (.lib)")

# SDL2 MSVC dev zip unpacks to e.g. SDL2-2.32.10/ (override if you use another layout under third_party).
set(FFPLAY_SDL2_ROOT_DIRNAME "SDL2-2.32.10" CACHE STRING
  "Folder name under THIRD_PARTY_DIR containing SDL2 (cmake/sdl2-config.cmake, lib/x64, ...)")
set(_ffplayer_sdl2_root "${THIRD_PARTY_DIR}/${FFPLAY_SDL2_ROOT_DIRNAME}")

set(FFPLAY_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FFPLAY_GEN_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(DownloadPrebuiltDeps)

if(NOT SDL2_DIR AND EXISTS "${_ffplayer_sdl2_root}/cmake/sdl2-config.cmake")
  set(SDL2_DIR "${_ffplayer_sdl2_root}/cmake" CACHE PATH "Directory that provides SDL2Config / sdl2-config.cmake")
endif()

include(FFmpegGyan)
ffplay_generate_config()

function(ffplay_find_ffmpeg_lib out_var lib_name)
  unset(_cand CACHE)
  unset(_cand)
  find_library(_cand
    NAMES "${lib_name}" "lib${lib_name}" "lib${lib_name}.dll.a" "${lib_name}.lib" "lib${lib_name}.lib"
    PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH)
  if(NOT _cand)
    find_library(_cand NAMES "${lib_name}" "lib${lib_name}" "lib${lib_name}.dll.a" "${lib_name}.lib")
  endif()
  if(NOT _cand)
    message(FATAL_ERROR "FFmpeg library '${lib_name}' not found (FFMPEG_LIB_DIR='${FFMPEG_LIB_DIR}').")
  endif()
  set("${out_var}" "${_cand}" PARENT_SCOPE)
endfunction()

function(ffplay_resolve_sdl2_dll out_var)
  set(_sdl_dll "")
  get_target_property(_sdl2_loc SDL2::SDL2 IMPORTED_LOCATION)
  if(_sdl2_loc AND NOT _sdl2_loc STREQUAL "NOTFOUND" AND _sdl2_loc MATCHES "\\.[Dd][Ll][Ll]$" AND EXISTS "${_sdl2_loc}")
    set(_sdl_dll "${_sdl2_loc}")
  else()
    get_target_property(_sdl2_impl SDL2::SDL2 IMPORTED_IMPLIB)
    if(_sdl2_impl AND NOT _sdl2_impl STREQUAL "NOTFOUND" AND EXISTS "${_sdl2_impl}")
      string(REGEX REPLACE "\\.[Ll][Ii][Bb]$" ".dll" _sdl2_try "${_sdl2_impl}")
      if(EXISTS "${_sdl2_try}")
        set(_sdl_dll "${_sdl2_try}")
      endif()
    endif()
  endif()
  if(NOT _sdl_dll)
    foreach(_c IN ITEMS "${_ffplayer_sdl2_root}/lib/x64/SDL2.dll" "${_ffplayer_sdl2_root}/lib/x86/SDL2.dll")
      if(NOT _sdl_dll AND EXISTS "${_c}")
        set(_sdl_dll "${_c}")
      endif()
    endforeach()
  endif()
  set("${out_var}" "${_sdl_dll}" PARENT_SCOPE)
endfunction()

function(ffplay_copy_runtime_dll target_name dll_path out_dir)
  add_custom_command(TARGET "${target_name}" POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll_path}" "${out_dir}/"
    VERBATIM)
endfunction()

find_package(SDL2 CONFIG QUIET)
if(NOT TARGET SDL2::SDL2)
  find_path(SDL2_MANUAL_INCLUDE_DIR NAMES SDL.h
    HINTS "${_ffplayer_sdl2_root}/include" "$ENV{SDL2_DIR}/include" "$ENV{SDL2DIR}/include"
    PATH_SUFFIXES SDL2 include/SDL2)
  find_library(SDL2_MANUAL_LIBRARY NAMES SDL2 SDL2-static
    HINTS "${_ffplayer_sdl2_root}/lib/x64" "${_ffplayer_sdl2_root}/lib/x86"
      "$ENV{SDL2_DIR}/lib" "$ENV{SDL2DIR}/lib" "$ENV{SDL2_DIR}/lib/x64")
  if(SDL2_MANUAL_INCLUDE_DIR AND SDL2_MANUAL_LIBRARY)
    add_library(SDL2::SDL2 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
      IMPORTED_LOCATION "${SDL2_MANUAL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${SDL2_MANUAL_INCLUDE_DIR}")
  endif()
endif()
if(NOT TARGET SDL2::SDL2)
  message(FATAL_ERROR
    "SDL2::SDL2 not found. Expected SDL2 under third_party/${FFPLAY_SDL2_ROOT_DIRNAME} "
    "(lib/x64/SDL2.lib and SDL2.dll), or set SDL2_DIR to the directory that contains sdl2-config.cmake. "
    "With FFPLAY_FETCH_PREBUILT_DEPS=ON, CMake can download the zip from FFPLAY_SDL2_DEV_ZIP_URL.")
endif()

set(_ffplay_target ffplay)
add_executable(${_ffplay_target}
  ffplay-cli/ffplay.c
  ffplay-cli/cmdutils.c
  ffplay-cli/opt_common.c)
target_include_directories(${_ffplay_target} PRIVATE
  "${FFPLAY_GEN_DIR}"
  "${CMAKE_SOURCE_DIR}/ffplay-cli/shim/include"
  "${CMAKE_SOURCE_DIR}/ffplay-cli"
  "${FFMPEG_PREBUILT_ROOT}/include")

set_target_properties(${_ffplay_target} PROPERTIES CXX_STANDARD 17)

if(MSVC)
  target_compile_options(${_ffplay_target} PRIVATE /utf-8)
endif()

set(_ffmpeg_libnames avutil avcodec avformat avfilter avdevice swscale swresample)
foreach(_lib IN LISTS _ffmpeg_libnames)
  ffplay_find_ffmpeg_lib(_ff_lib "${_lib}")
  target_link_libraries(${_ffplay_target} PRIVATE "${_ff_lib}")
endforeach()

target_link_libraries(${_ffplay_target} PRIVATE SDL2::SDL2)

if(WIN32)
  target_link_libraries(${_ffplay_target} PRIVATE shell32 comdlg32)
  # Copy runtime DLLs next to the exe so the process does not exit before main() with no console output.
  set(_out "$<TARGET_FILE_DIR:${_ffplay_target}>")
  ffplay_resolve_sdl2_dll(_sdl_dll)
  if(_sdl_dll)
    ffplay_copy_runtime_dll("${_ffplay_target}" "${_sdl_dll}" "${_out}")
  else()
    message(WARNING "SDL2.dll not found; ffplay.exe may fail to start (expected under lib/x64 or lib/x86).")
  endif()

  if(EXISTS "${FFMPEG_PREBUILT_ROOT}/bin")
    file(GLOB _gyan_dlls LIST_DIRECTORIES false "${FFMPEG_PREBUILT_ROOT}/bin/*.dll")
    foreach(_d IN LISTS _gyan_dlls)
      ffplay_copy_runtime_dll("${_ffplay_target}" "${_d}" "${_out}")
    endforeach()
  else()
    message(WARNING
      "Directory ${FFMPEG_PREBUILT_ROOT}/bin not found; FFmpeg DLLs were not copied. "
      "Add gyan bin to PATH or copy DLLs next to ffplay.exe before running.")
  endif()
endif()

set(_ffplay_gui_target ffplay-gui)
add_executable(${_ffplay_gui_target} WIN32
  ffplay-gui/main.cpp
  ffplay-gui/application.cpp
  ffplay-gui/audio_device.c
  ffplay-gui/audio_pipeline.c
  ffplay-gui/audio_visualizer.c
  ffplay-gui/ffplayer.c
  ffplay-gui/audio_thread.c
  ffplay-gui/video_thread.c
  ffplay-gui/video_renderer.c
  ffplay-gui/subtitle_thread.c
  ffplay-gui/demuxer.c
  ffplay-gui/read_thread.c
  ffplay-gui/clock.c
  ffplay-gui/av_sync.c
  ffplay-gui/filter.c
  ffplay-gui/stream.c
  ffplay-gui/packet_queue.c
  ffplay-gui/frame_queue.c
  ffplay-gui/decoder.c)

set(_ffplay_gui_default_icon "ffplay-gui/icon/icons8-ffmpeg-48.ico")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_ffplay_gui_default_icon}")
  set(_ffplay_gui_default_icon "")
endif()
set(FFPLAY_GUI_ICON "${_ffplay_gui_default_icon}" CACHE FILEPATH
  "Path to a .ico file embedded into ffplay-gui.exe on Windows")
if(WIN32 AND FFPLAY_GUI_ICON)
  get_filename_component(_ffplay_gui_icon_abs "${FFPLAY_GUI_ICON}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  if(EXISTS "${_ffplay_gui_icon_abs}")
    string(REPLACE "/" "\\\\" _ffplay_gui_icon_rc_path "${_ffplay_gui_icon_abs}")
    set(_ffplay_gui_icon_rc "${FFPLAY_GEN_DIR}/ffplay-gui-icon.rc")
    file(WRITE "${_ffplay_gui_icon_rc}" "IDI_APP_ICON ICON \"${_ffplay_gui_icon_rc_path}\"\n")
    target_sources(${_ffplay_gui_target} PRIVATE "${_ffplay_gui_icon_rc}")
  else()
    message(WARNING
      "FFPLAY_GUI_ICON was set to '${FFPLAY_GUI_ICON}', but resolved file "
      "'${_ffplay_gui_icon_abs}' does not exist. Icon will not be embedded.")
  endif()
endif()

set_target_properties(${_ffplay_gui_target} PROPERTIES
  OUTPUT_NAME "ffplay-gui"
  CXX_STANDARD 20)
target_include_directories(${_ffplay_gui_target} PRIVATE
  "${FFPLAY_GEN_DIR}"
  "${CMAKE_SOURCE_DIR}/src/shim/include"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/ffplay-gui"
  "${FFMPEG_PREBUILT_ROOT}/include")

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)
FetchContent_GetProperties(imgui SOURCE_DIR IMGUI_BUILD_DIR)
if(NOT IMGUI_BUILD_DIR OR NOT EXISTS "${IMGUI_BUILD_DIR}/imgui.h")
  message(FATAL_ERROR "Failed to fetch imgui sources from '${IMGUI_BUILD_DIR}'.")
endif()

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.x
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

target_sources(${_ffplay_gui_target} PRIVATE
  "${IMGUI_BUILD_DIR}/imgui.cpp"
  "${IMGUI_BUILD_DIR}/imgui_draw.cpp"
  "${IMGUI_BUILD_DIR}/imgui_tables.cpp"
  "${IMGUI_BUILD_DIR}/imgui_widgets.cpp"
  "${IMGUI_BUILD_DIR}/imgui_demo.cpp"
  "${IMGUI_BUILD_DIR}/backends/imgui_impl_sdl2.cpp"
  "${IMGUI_BUILD_DIR}/backends/imgui_impl_sdlrenderer2.cpp")
target_include_directories(${_ffplay_gui_target} PRIVATE
  "${IMGUI_BUILD_DIR}"
  "${IMGUI_BUILD_DIR}/backends")

if(MSVC)
  target_compile_options(${_ffplay_gui_target} PRIVATE /utf-8)
endif()

foreach(_lib IN LISTS _ffmpeg_libnames)
  ffplay_find_ffmpeg_lib(_ff_lib "${_lib}")
  target_link_libraries(${_ffplay_gui_target} PRIVATE "${_ff_lib}")
endforeach()

target_link_libraries(${_ffplay_gui_target} PRIVATE SDL2::SDL2 spdlog::spdlog)

if(WIN32)
  target_link_libraries(${_ffplay_gui_target} PRIVATE shell32 comdlg32)
  set(_out_gui "$<TARGET_FILE_DIR:${_ffplay_gui_target}>")
  ffplay_resolve_sdl2_dll(_sdl_dll_gui)
  if(_sdl_dll_gui)
    ffplay_copy_runtime_dll("${_ffplay_gui_target}" "${_sdl_dll_gui}" "${_out_gui}")
  else()
    message(WARNING "SDL2.dll not found; ffplay-gui.exe may fail to start (expected under lib/x64 or lib/x86).")
  endif()

  if(EXISTS "${FFMPEG_PREBUILT_ROOT}/bin")
    file(GLOB _gyan_dlls_gui LIST_DIRECTORIES false "${FFMPEG_PREBUILT_ROOT}/bin/*.dll")
    foreach(_d IN LISTS _gyan_dlls_gui)
      ffplay_copy_runtime_dll("${_ffplay_gui_target}" "${_d}" "${_out_gui}")
    endforeach()
  endif()
endif()

# ======================================================================
# ffplay-gui-d3d11 — D3D11 rendering backend (Win32 only)
# ======================================================================
if(WIN32)
  set(_ffplay_gui_d3d11_target ffplay-gui-d3d11)
  add_executable(${_ffplay_gui_d3d11_target} WIN32
    ffplay-gui/main.cpp
    ffplay-gui/application_d3d11.cpp
    ffplay-gui/audio_device.c
    ffplay-gui/audio_pipeline.c
    ffplay-gui/audio_visualizer.c
    ffplay-gui/ffplayer.c
    ffplay-gui/audio_thread.c
    ffplay-gui/video_thread.c
    ffplay-gui/video_renderer_d3d11.c
    ffplay-gui/subtitle_thread.c
    ffplay-gui/demuxer.c
    ffplay-gui/read_thread.c
    ffplay-gui/clock.c
    ffplay-gui/av_sync.c
    ffplay-gui/filter.c
    ffplay-gui/stream.c
    ffplay-gui/packet_queue.c
    ffplay-gui/frame_queue.c
    ffplay-gui/decoder.c)

  set_target_properties(${_ffplay_gui_d3d11_target} PROPERTIES
    OUTPUT_NAME "ffplay-gui-d3d11"
    CXX_STANDARD 20)
  target_compile_definitions(${_ffplay_gui_d3d11_target} PRIVATE FFPLAYER_USE_D3D11)
  target_include_directories(${_ffplay_gui_d3d11_target} PRIVATE
    "${FFPLAY_GEN_DIR}"
    "${CMAKE_SOURCE_DIR}/src/shim/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/ffplay-gui"
    "${FFMPEG_PREBUILT_ROOT}/include")

  # ImGui sources — Win32 + DX11 backends
  target_sources(${_ffplay_gui_d3d11_target} PRIVATE
    "${IMGUI_BUILD_DIR}/imgui.cpp"
    "${IMGUI_BUILD_DIR}/imgui_draw.cpp"
    "${IMGUI_BUILD_DIR}/imgui_tables.cpp"
    "${IMGUI_BUILD_DIR}/imgui_widgets.cpp"
    "${IMGUI_BUILD_DIR}/imgui_demo.cpp"
    "${IMGUI_BUILD_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_BUILD_DIR}/backends/imgui_impl_dx11.cpp")
  target_include_directories(${_ffplay_gui_d3d11_target} PRIVATE
    "${IMGUI_BUILD_DIR}"
    "${IMGUI_BUILD_DIR}/backends")

  if(MSVC)
    target_compile_options(${_ffplay_gui_d3d11_target} PRIVATE /utf-8)
  endif()

  foreach(_lib IN LISTS _ffmpeg_libnames)
    ffplay_find_ffmpeg_lib(_ff_lib "${_lib}")
    target_link_libraries(${_ffplay_gui_d3d11_target} PRIVATE "${_ff_lib}")
  endforeach()

  # SDL2 for audio + timer; D3D11 / DXGI / D3DCompiler for rendering
  target_link_libraries(${_ffplay_gui_d3d11_target} PRIVATE
    SDL2::SDL2 spdlog::spdlog
    d3d11 dxgi dxguid d3dcompiler shell32 comdlg32)

  # Icon
  if(FFPLAY_GUI_ICON)
    get_filename_component(_d3d11_icon_abs "${FFPLAY_GUI_ICON}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(EXISTS "${_d3d11_icon_abs}")
      string(REPLACE "/" "\\\\" _d3d11_icon_rc_path "${_d3d11_icon_abs}")
      set(_d3d11_icon_rc "${FFPLAY_GEN_DIR}/ffplay-gui-d3d11-icon.rc")
      file(WRITE "${_d3d11_icon_rc}" "IDI_APP_ICON ICON \"${_d3d11_icon_rc_path}\"\n")
      target_sources(${_ffplay_gui_d3d11_target} PRIVATE "${_d3d11_icon_rc}")
    endif()
  endif()

  # Copy runtime DLLs
  set(_out_d3d11 "$<TARGET_FILE_DIR:${_ffplay_gui_d3d11_target}>")
  ffplay_resolve_sdl2_dll(_sdl_dll_d3d11)
  if(_sdl_dll_d3d11)
    ffplay_copy_runtime_dll("${_ffplay_gui_d3d11_target}" "${_sdl_dll_d3d11}" "${_out_d3d11}")
  endif()
  if(EXISTS "${FFMPEG_PREBUILT_ROOT}/bin")
    file(GLOB _gyan_dlls_d3d11 LIST_DIRECTORIES false "${FFMPEG_PREBUILT_ROOT}/bin/*.dll")
    foreach(_d IN LISTS _gyan_dlls_d3d11)
      ffplay_copy_runtime_dll("${_ffplay_gui_d3d11_target}" "${_d}" "${_out_d3d11}")
    endforeach()
  endif()
endif()

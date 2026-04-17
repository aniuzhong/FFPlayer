# Optional: download gyan FFmpeg (.zip) and SDL2 MSVC dev (.zip) into
# THIRD_PARTY_DIR when the expected folder layout is missing. Extraction uses `cmake -E tar xf` (no 7-Zip).

option(FFPLAY_FETCH_PREBUILT_DEPS
  "Download and extract FFmpeg / SDL2 into third_party when required files are missing"
  ON)

set(FFPLAY_FFMPEG_PREBUILT_URL
  "https://github.com/GyanD/codexffmpeg/releases/download/8.1/ffmpeg-8.1-full_build-shared.zip"
  CACHE STRING "URL for gyan FFmpeg 8.1 full_build-shared archive (.zip)")
set(FFPLAY_SDL2_DEV_ZIP_URL
  "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip"
  CACHE STRING "URL for SDL2 MSVC development package (.zip)")

function(ffplay_deps_cache_dir out_var)
  set("${out_var}" "${THIRD_PARTY_DIR}/.cache" PARENT_SCOPE)
endfunction()

function(ffplay_ffmpeg_tree_ok root out_var)
  if(EXISTS "${root}/include/libavutil/avutil.h" AND EXISTS "${root}/lib")
    set("${out_var}" 1 PARENT_SCOPE)
  else()
    set("${out_var}" 0 PARENT_SCOPE)
  endif()
endfunction()

function(ffplay_sdl2_tree_ok root out_var)
  if(EXISTS "${root}/cmake/sdl2-config.cmake")
    set("${out_var}" 1 PARENT_SCOPE)
  else()
    set("${out_var}" 0 PARENT_SCOPE)
  endif()
endfunction()

function(ffplay_download_file url destfile human_msg)
  get_filename_component(_parent "${destfile}" DIRECTORY)
  file(MAKE_DIRECTORY "${_parent}")
  message(STATUS "${human_msg}: ${url}")
  file(DOWNLOAD "${url}" "${destfile}" SHOW_PROGRESS TLS_VERIFY ON STATUS _st)
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    list(GET _st 1 _err)
    message(FATAL_ERROR "Download failed (code ${_code}): ${_err}")
  endif()
endfunction()

function(ffplay_extract_zip zipfile dest_parent)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${zipfile}"
    WORKING_DIRECTORY "${dest_parent}"
    RESULT_VARIABLE _r)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "Extract failed (exit ${_r}) for zip:\n  ${zipfile}\n"
      "  (CMake 'cmake -E tar xf'; working dir: ${dest_parent})")
  endif()
endfunction()

function(ffplay_ensure_cached_archive out_var cache_dir archive_url human_msg)
  get_filename_component(_zip_name "${archive_url}" NAME)
  set(_archive "${cache_dir}/${_zip_name}")
  if(NOT EXISTS "${_archive}")
    ffplay_download_file("${archive_url}" "${_archive}" "${human_msg}")
  endif()
  set("${out_var}" "${_archive}" PARENT_SCOPE)
endfunction()

function(ffplay_extract_archive_to_third_party archive_path dep_name)
  message(STATUS "Extracting ${dep_name} zip into ${THIRD_PARTY_DIR}")
  ffplay_extract_zip("${archive_path}" "${THIRD_PARTY_DIR}")
endfunction()

function(ffplay_ensure_prebuilt_deps)
  if(NOT FFPLAY_FETCH_PREBUILT_DEPS)
    return()
  endif()

  ffplay_deps_cache_dir(_cache)
  file(MAKE_DIRECTORY "${_cache}")

  ffplay_ffmpeg_tree_ok("${FFMPEG_PREBUILT_ROOT}" _ok_ff)
  if(NOT _ok_ff)
    ffplay_ensure_cached_archive(_ff_arc "${_cache}" "${FFPLAY_FFMPEG_PREBUILT_URL}"
      "Downloading FFmpeg prebuilt (gyan full_build-shared)")
    ffplay_extract_archive_to_third_party("${_ff_arc}" "FFmpeg")
    ffplay_ffmpeg_tree_ok("${FFMPEG_PREBUILT_ROOT}" _ok_ff2)
    if(NOT _ok_ff2)
      message(FATAL_ERROR
        "After extract, expected FFmpeg tree not found at:\n  FFMPEG_PREBUILT_ROOT='${FFMPEG_PREBUILT_ROOT}'\n"
        "  (expect include/libavutil/avutil.h and lib/)")
    endif()
  endif()

  if(NOT DEFINED _ffplayer_sdl2_root OR _ffplayer_sdl2_root STREQUAL "")
    message(FATAL_ERROR "DownloadPrebuiltDeps.cmake: _ffplayer_sdl2_root must be set by CMakeLists.txt")
  endif()

  ffplay_sdl2_tree_ok("${_ffplayer_sdl2_root}" _ok_sdl)
  if(NOT _ok_sdl)
    ffplay_ensure_cached_archive(_sdl_arc "${_cache}" "${FFPLAY_SDL2_DEV_ZIP_URL}"
      "Downloading SDL2 MSVC development zip")
    ffplay_extract_archive_to_third_party("${_sdl_arc}" "SDL2")
    ffplay_sdl2_tree_ok("${_ffplayer_sdl2_root}" _ok_sdl2)
    if(NOT _ok_sdl2)
      message(FATAL_ERROR
        "After extract, SDL2 tree not found at:\n  ${_ffplayer_sdl2_root}\n"
        "  Expected cmake/sdl2-config.cmake. Set FFPLAY_SDL2_ROOT_DIRNAME to match the zip top folder.")
    endif()
  endif()

endfunction()

ffplay_ensure_prebuilt_deps()

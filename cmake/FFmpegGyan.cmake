# Configure-time probes for the gyan prebuilt tree: FFMPEG_VERSION, configuration string,
# and CC_IDENT for generated config.h. Prefer parsing bin/ffplay -version so the string
# matches the DLLs' embedded configuration (see opt_common.c).
# CMAKE_CURRENT_LIST_DIR is wrong inside function() bodies, so cache the module dir at include time.
set(FFMPEG_GYAN_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(ffplay_c_escape out_var text)
  string(REPLACE "\\" "\\\\" _t "${text}")
  string(REPLACE "\"" "\\\"" _t "${_t}")
  string(REPLACE "\r" "" _t "${_t}")
  string(REPLACE "\n" " " _t "${_t}")
  set("${out_var}" "${_t}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_gyan_merge_stdio out_var stdout_var stderr_var)
  set(_m "${${stdout_var}}")
  if(_m STREQUAL "")
    set(_m "${${stderr_var}}")
  endif()
  string(REPLACE "\r" "" _m "${_m}")
  set("${out_var}" "${_m}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_gyan_first_path out_var)
  set(_p "")
  foreach(_c ${ARGN})
    if(_p STREQUAL "" AND EXISTS "${_c}")
      set(_p "${_c}")
    endif()
  endforeach()
  set("${out_var}" "${_p}" PARENT_SCOPE)
endfunction()

function(ffplay_generate_config)
  set(_pre "${FFMPEG_PREBUILT_ROOT}")
  _ffmpeg_gyan_first_path(_ff "${_pre}/bin/ffmpeg.exe" "${_pre}/bin/ffmpeg")
  _ffmpeg_gyan_first_path(_ffplay_ref "${_pre}/bin/ffplay.exe" "${_pre}/bin/ffplay")

  set(FFMPEG_VERSION_STR "unknown")
  set(FFMPEG_CONFIGURATION_ESC "(ffplay: set FFMPEG_PREBUILT_ROOT to gyan tree with bin/ffplay and bin/ffmpeg)")
  set(CC_IDENT_ESC "unknown")

  set(_verh "${_pre}/include/libavutil/ffversion.h")
  if(EXISTS "${_verh}")
    file(STRINGS "${_verh}" _ver_line REGEX "define FFMPEG_VERSION")
    if(_ver_line MATCHES "define FFMPEG_VERSION \"([^\"]+)\"")
      set(FFMPEG_VERSION_STR "${CMAKE_MATCH_1}")
    endif()
  endif()

  string(TIMESTAMP CONFIG_THIS_YEAR "%Y")

  set(_cfg "")
  set(_cc_from_ref "")

  if(NOT _ffplay_ref STREQUAL "")
    execute_process(
      COMMAND "${_ffplay_ref}" -hide_banner -version
      OUTPUT_VARIABLE _pv_out
      ERROR_VARIABLE _pv_err
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )
    _ffmpeg_gyan_merge_stdio(_pv _pv_out _pv_err)
    string(REGEX MATCH "configuration: ([^\n]*)" _unused_cfg "${_pv}")
    if(CMAKE_MATCH_1)
      set(_cfg "${CMAKE_MATCH_1}")
      string(STRIP "${_cfg}" _cfg)
    endif()
    string(REGEX MATCH "built with ([^\n]*)" _unused_cc "${_pv}")
    if(CMAKE_MATCH_1)
      set(_cc_from_ref "${CMAKE_MATCH_1}")
      string(STRIP "${_cc_from_ref}" _cc_from_ref)
    endif()
  endif()

  if(_cfg STREQUAL "" AND NOT _ff STREQUAL "")
    execute_process(
      COMMAND "${_ff}" -hide_banner -buildconf
      OUTPUT_VARIABLE _bc_out
      ERROR_VARIABLE _bc_err
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )
    _ffmpeg_gyan_merge_stdio(_bc _bc_out _bc_err)
    string(REGEX REPLACE "[\t ]*configuration:[\t ]*" "" _bc "${_bc}")
    string(REGEX REPLACE "[\t ]*Exiting with exit code[\t ]*[0-9]+[\t ]*\$" "" _bc "${_bc}")
    string(STRIP "${_bc}" _bc)
    string(REGEX REPLACE "[\t ]+" " " _cfg "${_bc}")
    string(STRIP "${_cfg}" _cfg)
  endif()

  if(NOT _cfg STREQUAL "")
    ffplay_c_escape(FFMPEG_CONFIGURATION_ESC "${_cfg}")
  endif()

  if(NOT _cc_from_ref STREQUAL "")
    ffplay_c_escape(CC_IDENT_ESC "${_cc_from_ref}")
  elseif(NOT _ff STREQUAL "")
    execute_process(
      COMMAND "${_ff}" -hide_banner -version
      OUTPUT_VARIABLE _ver_out
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE "\n" ";" _ver_lines "${_ver_out}")
    set(_cc_done 0)
    foreach(_ln ${_ver_lines})
      if(NOT _cc_done AND _ln MATCHES "^built with (.+)$")
        ffplay_c_escape(CC_IDENT_ESC "${CMAKE_MATCH_1}")
        set(_cc_done 1)
      endif()
    endforeach()
  else()
    message(WARNING
      "Neither ${_pre}/bin/ffplay(.exe) nor ffmpeg(.exe) found; configuration / CC_IDENT may be incomplete.")
  endif()

  configure_file(
    "${FFMPEG_GYAN_MODULE_DIR}/config.h.in"
    "${FFPLAY_GEN_DIR}/config.h"
    @ONLY
  )
  configure_file(
    "${FFMPEG_GYAN_MODULE_DIR}/config_components.h.in"
    "${FFPLAY_GEN_DIR}/config_components.h"
    COPYONLY
  )
endfunction()

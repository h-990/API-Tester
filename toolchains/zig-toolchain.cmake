if(NOT DEFINED ZIG_SYSTEM_NAME OR ZIG_SYSTEM_NAME STREQUAL "")
  if(DEFINED ENV{ZIG_SYSTEM_NAME})
    set(ZIG_SYSTEM_NAME "$ENV{ZIG_SYSTEM_NAME}")
  elseif(DEFINED CACHE{ZIG_SYSTEM_NAME})
    set(ZIG_SYSTEM_NAME "$CACHE{ZIG_SYSTEM_NAME}")
  endif()
endif()

if(NOT DEFINED ZIG_TARGET OR ZIG_TARGET STREQUAL "")
  if(DEFINED ENV{ZIG_TARGET})
    set(ZIG_TARGET "$ENV{ZIG_TARGET}")
  elseif(DEFINED CACHE{ZIG_TARGET})
    set(ZIG_TARGET "$CACHE{ZIG_TARGET}")
  endif()
endif()

if(NOT DEFINED ZIG_SYSTEM_NAME OR ZIG_SYSTEM_NAME STREQUAL "")
  message(FATAL_ERROR "ZIG_SYSTEM_NAME is required (example: Linux, Darwin, Windows)")
endif()

if(NOT DEFINED ZIG_TARGET OR ZIG_TARGET STREQUAL "")
  message(FATAL_ERROR "ZIG_TARGET is required (example: x86_64-linux-gnu, aarch64-macos, x86_64-windows-gnu)")
endif()

set(CMAKE_SYSTEM_NAME "${ZIG_SYSTEM_NAME}")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZIG_TARGET ZIG_SYSTEM_NAME)

find_program(ZIG_EXECUTABLE zig REQUIRED)

set(CMAKE_C_COMPILER "${ZIG_EXECUTABLE}")
set(CMAKE_C_COMPILER_ARG1 cc)
set(CMAKE_CXX_COMPILER "${ZIG_EXECUTABLE}")
set(CMAKE_CXX_COMPILER_ARG1 c++)

# Ensure static libraries are archived/indexed with Zig's LLVM tools.
# Host ranlib (e.g. macOS ranlib) cannot index Windows COFF archives correctly.
set(_ZIG_WRAPPER_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_AR "${_ZIG_WRAPPER_DIR}/zig-ar")
set(CMAKE_RANLIB "${_ZIG_WRAPPER_DIR}/zig-ranlib")

set(CMAKE_C_FLAGS_INIT "-target ${ZIG_TARGET}")
set(CMAKE_CXX_FLAGS_INIT "-target ${ZIG_TARGET}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-target ${ZIG_TARGET}")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_ZIG_MAC_SDK "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
  set(_ZIG_DARWIN_FLAGS
      "-isysroot ${_ZIG_MAC_SDK} -F ${_ZIG_MAC_SDK}/System/Library/Frameworks -isystem ${_ZIG_MAC_SDK}/usr/include")
  set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${_ZIG_DARWIN_FLAGS}")
  set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${_ZIG_DARWIN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} -isysroot ${_ZIG_MAC_SDK} -F ${_ZIG_MAC_SDK}/System/Library/Frameworks")
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

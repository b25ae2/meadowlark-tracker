# Meadowlark desktop build: core/ compiled for the host as Tracker Mini without the
# proprietary synth, plus the desktop platform layer. The firmware Makefiles are separate.
#
#   cmake -S platforms/desktop -B build/desktop -G Ninja
#   cmake --build build/desktop
#
# Options: -DMEADOWLARK_SDL=OFF builds the headless runner only.
#          -DMEADOWLARK_SANITIZE=address,undefined adds sanitizers.

cmake_minimum_required(VERSION 3.25)
project(meadowlark LANGUAGES C CXX)

set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
cmake_path(NORMAL_PATH ROOT_DIR)
set(CORE_DIR ${ROOT_DIR}/core)

option(MEADOWLARK_SDL "Build the SDL3 front end (window, input)" ON)
set(MEADOWLARK_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list, empty for none")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Version: the Mini numbers from version.mk, so there is one place to bump them ---------
file(STRINGS ${ROOT_DIR}/version.mk VERSION_LINES REGEX "^(EDITION_NAME|MINI_VER_[A-Z]+)[ \t]*:=")
foreach(line IN LISTS VERSION_LINES)
  if(line MATCHES "^([A-Z_]+)[ \t]*:=[ \t]*([^ \t#]+)")
    set(VER_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
  endif()
endforeach()
foreach(var EDITION_NAME MINI_VER_MAJOR MINI_VER_MINOR MINI_VER_PATCH MINI_VER_BETA)
  if(NOT DEFINED VER_${var})
    message(FATAL_ERROR "version.mk: ${var} not found")
  endif()
endforeach()

# --- Compile settings shared by core and the desktop layer ---------------------------------
# Mini feature set, no FEATURE_SYNTH, no Teensy/Kinetis defines (those enable ARM asm).
set(MEADOWLARK_DEFINES
  PLATFORM_IMX TRACKER_MINI PLATFORM_DESKTOP
  AUDIO_PERFORMANCE_INFO_ENABLED MEMORY_USAGE_INFO_ENABLED USB_MIDI LAYOUT_US_ENGLISH
  FW_EDITION_NAME="${VER_EDITION_NAME}"
  FW_VER_MAJOR=${VER_MINI_VER_MAJOR} FW_VER_MINOR=${VER_MINI_VER_MINOR}
  FW_VER_PATCH=${VER_MINI_VER_PATCH} FW_VER_BETA=${VER_MINI_VER_BETA})

set(CORE_INCLUDE_DIRS
  ${CORE_DIR}/include ${CORE_DIR}/config ${CORE_DIR}/source ${CORE_DIR}/synth
  ${CORE_DIR}/libs ${CORE_DIR}/libs/Audio ${CORE_DIR}/libs/ElapsedMillis
  ${CORE_DIR}/libs/Audio/utility ${CORE_DIR}/libs/FT812 ${CORE_DIR}/libs/fonts
  ${CORE_DIR}/libs/NewDisplay ${CORE_DIR}/interface ${CORE_DIR}/effector
  ${CORE_DIR}/FileManager ${CORE_DIR}/Exporter ${CORE_DIR}/AudioEngine
  ${CORE_DIR}/libs/Audio/Polyverb ${CORE_DIR}/AudioEngine/ConstantValues
  ${CORE_DIR}/AudioEngine/MasterEnhancer
  ${ROOT_DIR}/util/include ${ROOT_DIR}/util/3rdparty ${ROOT_DIR}/synth/include)

# -funsigned-char: the firmware is built for ARM, where plain char is unsigned.
set(COMMON_FLAGS -funsigned-char -fno-common)
set(CXX_ONLY_FLAGS -fno-rtti -fno-exceptions -include cstdint)

# Vendored and legacy code is noisy; keep only the warnings that flag portability bugs.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CORE_WARN_FLAGS -Wno-everything -Wreturn-type -Wint-to-pointer-cast -Wformat
      $<$<COMPILE_LANGUAGE:C>:-Wpointer-to-int-cast>)
else()
  set(CORE_WARN_FLAGS -w)
endif()

if(MEADOWLARK_SANITIZE)
  list(APPEND COMMON_FLAGS -fsanitize=${MEADOWLARK_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${MEADOWLARK_SANITIZE})
endif()

# --- core/ as a static library -------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
  ${CORE_DIR}/source/*.cpp ${CORE_DIR}/libs/*.cpp ${CORE_DIR}/libs/*.c
  ${CORE_DIR}/interface/*.cpp ${CORE_DIR}/effector/*.cpp ${CORE_DIR}/FileManager/*.cpp
  ${CORE_DIR}/Exporter/*.cpp ${CORE_DIR}/AudioEngine/*.cpp ${CORE_DIR}/synth/*.cpp)

add_library(meadowlark_core STATIC ${CORE_SOURCES})
target_compile_definitions(meadowlark_core PUBLIC ${MEADOWLARK_DEFINES})
target_include_directories(meadowlark_core PUBLIC ${CORE_INCLUDE_DIRS})
target_compile_options(meadowlark_core
  PUBLIC ${COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:${CXX_ONLY_FLAGS}>
  PRIVATE ${CORE_WARN_FLAGS})

# --- desktop platform layer ----------------------------------------------------------------
# OBJECT library: core/ calls back into these (interrupt lock, sin_q15, mute), so their
# objects must always be linked, whatever the linker's archive ordering rules are.
add_library(meadowlark_platform OBJECT
  source/AudioLock.cpp
  source/hardware.cpp
  source/platform_math.cpp
  source/PlatformDesktop.cpp)
target_include_directories(meadowlark_platform PUBLIC source)
target_link_libraries(meadowlark_platform PUBLIC meadowlark_core)
target_compile_options(meadowlark_platform PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(meadowlark_platform PUBLIC Threads::Threads)

# --- executables ---------------------------------------------------------------------------
# Headless runner: renders audio blocks and display lists without a window or sound device.
add_executable(meadowlark-headless source/main.cpp)
target_link_libraries(meadowlark-headless PRIVATE meadowlark_platform)
target_compile_options(meadowlark-headless PRIVATE -Wall -Wextra)

if(MEADOWLARK_SDL)
  find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3)
  add_executable(sdl_input_probe ${ROOT_DIR}/tools/sdl_input_probe.c)
  target_link_libraries(sdl_input_probe PRIVATE SDL3::SDL3)
endif()

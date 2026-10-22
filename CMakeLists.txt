cmake_minimum_required(VERSION 3.15)
project(zcdi VERSION 0.1.0 LANGUAGES C CXX)

option(ZCDI_ENABLE_LIBRETRO "Build as a libretro core" ON)
option(ZCDI_BUILD_TESTS "Build unit tests" OFF)

# Set by the libretro CI templates (ci-templates on git.libretro.com):
#   LIBRETRO_STATIC  build the core as a static archive (web build)
#   LIBRETRO_SUFFIX  platform suffix of the core's file name, e.g. _emscripten

# Emscripten has no shared libraries.
if(ZCDI_ENABLE_LIBRETRO AND NOT LIBRETRO_STATIC AND NOT EMSCRIPTEN)
    set(ZCDI_LIBRARY_TYPE SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
    set(ZCDI_LIBRARY_TYPE STATIC)
endif()

add_subdirectory(third_party/musashi)
# The SCC68070 model needs address errors and interrupt acknowledge.
target_compile_definitions(musashi PRIVATE
    M68K_EMULATE_ADDRESS_ERROR=1
    M68K_EMULATE_INT_ACK=1
)

set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libchdr)

add_subdirectory(third_party/libmpeg2)
target_sources(mpeg2 PRIVATE src/dvc/mpeg2_glue.c)
target_include_directories(mpeg2 PRIVATE src)

set(ZCDI_SOURCES
    src/core/bus.cpp
    src/core/machine.cpp
    src/core/machine_inspector.cpp
    src/core/pointing_device.cpp
    src/core/cpu/musashi_core.cpp
    src/core/cpu/musashi_state.c
    src/disc/disc.cpp
    src/disc/file_io.cpp
    src/disc/cue_parser.cpp
    src/disc/m3u_parser.cpp
    src/disc/raw2352_disc.cpp
    src/disc/chd_disc.cpp
    mamecdi/mcd212/mcd212.cpp
    mamecdi/scc68070/scc68070.cpp
    mamecdi/slave/slave.cpp
    mamecdi/cdic/cdic.cpp
    mamecdi/cdic/cdic_adpcm.cpp
    mamecdi/mk48t08/mk48t08.cpp
    mamecdi/vmpeg/vmpeg.cpp
    src/dvc/dvc_video.cpp
    src/dvc/dvc_audio.cpp
    src/dvc/mpeg2_state.cpp
    src/audio/mixer.cpp
)
set(ZCDI_LIBRETRO_SOURCES
    src/libretro/libretro_core.cpp
    src/libretro/core_options.cpp
    src/libretro/disk_control.cpp
    src/libretro/helpers.cpp
    src/libretro/input_mapping.cpp
    src/libretro/savestate.cpp
)

add_library(zcdi ${ZCDI_LIBRARY_TYPE} ${ZCDI_SOURCES} "$<$<BOOL:${ZCDI_ENABLE_LIBRETRO}>:${ZCDI_LIBRETRO_SOURCES}>")
add_library(zcdi::zcdi ALIAS zcdi)

target_compile_definitions(zcdi PRIVATE
    ZCDI_VERSION_MAJOR=${zcdi_VERSION_MAJOR}
    ZCDI_VERSION_MINOR=${zcdi_VERSION_MINOR}
    ZCDI_VERSION_PATCH=${zcdi_VERSION_PATCH}
    ZCDI_VERSION_PRERELEASE="${VERSION_PRERELEASE}"
    ZCDI_VERSION_METADATA="${VERSION_METADATA}"
)
target_compile_features(zcdi PUBLIC cxx_std_20 PRIVATE c_std_11)
target_include_directories(zcdi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/mamecdi
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(zcdi PRIVATE musashi mpeg2 chdr-static)

target_compile_options(zcdi PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra>)
target_compile_options(zcdi PRIVATE $<$<BOOL:${EMSCRIPTEN}>:-fno-exceptions>) # https://emscripten.org/docs/porting/exceptions.html

# Link MinGW's C/C++ runtime statically, so the DLL doesn't need
# libgcc/libstdc++ DLLs installed beside the frontend.
target_link_options(zcdi PRIVATE "$<$<BOOL:${MINGW}>:-static-libgcc;-static-libstdc++>")

foreach(_lib musashi mpeg2 chdr-static chdr-lzma miniz zstd)
    if(TARGET ${_lib})
        target_sources(zcdi PRIVATE $<$<AND:$<BOOL:${ZCDI_ENABLE_LIBRETRO}>,$<STREQUAL:${ZCDI_LIBRARY_TYPE},STATIC>>:$<TARGET_OBJECTS:${_lib}>>)
    endif()
endforeach()

if(ZCDI_ENABLE_LIBRETRO)
    # Frontends expect "zcdi_libretro<suffix>.<ext>", with no "lib" prefix.
    set_target_properties(zcdi PROPERTIES PREFIX "" OUTPUT_NAME "zcdi_libretro${LIBRETRO_SUFFIX}")
    # The CI templates build --target <corename>_libretro.
    add_custom_target(zcdi_libretro)
    add_dependencies(zcdi_libretro zcdi)
    # libretro's web build takes the core archive as .bc.
    if(EMSCRIPTEN)
        add_custom_command(TARGET zcdi POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:zcdi> $<TARGET_FILE_DIR:zcdi>/zcdi_libretro${LIBRETRO_SUFFIX}.bc)
    endif()
endif()

if(ZCDI_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()

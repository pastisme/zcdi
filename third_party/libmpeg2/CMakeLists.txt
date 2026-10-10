# Vendored libmpeg2 0.5.1 (GPL-2.0-or-later): the portable C decoder only,
# with our own config.h (no CPU acceleration, no convert/ or libvo/).
# decode.c and header.c are compiled by zcdi's src/dvc/mpeg2_glue.c.
add_library(mpeg2 STATIC
    libmpeg2/alloc.c
    libmpeg2/cpu_accel.c
    libmpeg2/cpu_state.c
    libmpeg2/idct.c
    libmpeg2/motion_comp.c
    libmpeg2/slice.c
)
target_include_directories(mpeg2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/libmpeg2)
target_include_directories(mpeg2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Vendored C code: silence its warnings, they aren't ours to fix.
if(NOT MSVC)
    target_compile_options(mpeg2 PRIVATE -w)
endif()

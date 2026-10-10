# Vendored Musashi (MIT) — Karl Stenerud. See readme.txt.
#
# m68kops.c / m68kops.h are COMMITTED generated files (see REGENERATE.md); they
# compile like any other source. m68kmake.c (generator) and m68k_in.c (template)
# are intentionally NOT listed here, nor m68kfpu.c (m68kcpu.c #includes it).
add_library(musashi STATIC
    m68kcpu.c
    m68kops.c
    m68kdasm.c
    softfloat/softfloat.c
)
target_include_directories(musashi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Vendored C89/C9x code: silence its warnings, they aren't ours to fix.
if(NOT MSVC)
    target_compile_options(musashi PRIVATE -w)
endif()

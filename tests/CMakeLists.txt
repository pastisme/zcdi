list(APPEND CMAKE_MODULE_PATH ${zcdi_SOURCE_DIR}/third_party/doctest/scripts/cmake)
include(doctest)

add_library(zcdi_doctest INTERFACE)
target_include_directories(zcdi_doctest SYSTEM INTERFACE ${zcdi_SOURCE_DIR}/third_party/doctest)
target_include_directories(zcdi_doctest INTERFACE
    ${zcdi_SOURCE_DIR}/src
    ${zcdi_SOURCE_DIR}/mamecdi
    ${zcdi_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(zcdi_doctest INTERFACE cxx_std_20)

file(GLOB_RECURSE ZCDI_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
file(GLOB_RECURSE ZCDI_LIBRETRO_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/libretro/*.cpp)
list(REMOVE_ITEM ZCDI_UNIT_TEST_SOURCES ${ZCDI_LIBRETRO_TEST_SOURCES})
add_executable(zcdi_unit_tests support/doctest_main.cpp ${ZCDI_UNIT_TEST_SOURCES}
    "$<$<BOOL:${ZCDI_ENABLE_LIBRETRO}>:${ZCDI_LIBRETRO_TEST_SOURCES}>")
target_link_libraries(zcdi_unit_tests PRIVATE zcdi_doctest zcdi::zcdi)
target_compile_options(zcdi_unit_tests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra>)
target_compile_definitions(zcdi_unit_tests PRIVATE
    $<TARGET_PROPERTY:zcdi,COMPILE_DEFINITIONS>
    ZCDI_LIBRETRO_INFO_PATH="${zcdi_SOURCE_DIR}/src/libretro/zcdi_libretro.info")

doctest_discover_tests(zcdi_unit_tests ADD_LABELS 1)

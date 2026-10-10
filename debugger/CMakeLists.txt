cmake_minimum_required(VERSION 3.15)
project(zcdi_dbg C CXX)

set(ZCDI_ENABLE_LIBRETRO OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. zcdi)

add_executable(zcdi_dbg
	${CMAKE_CURRENT_LIST_DIR}/src/sokol_impl.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/main.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/app.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/emu_runner.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/names.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/symbols.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/breakpoints_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/devices_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/mcd212_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/trace_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/cpu_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/disassembly_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/stack_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/memory_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/widgets/rendering_widget.cpp
	${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/imgui.cpp
	${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/imgui_draw.cpp
	${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/imgui_tables.cpp
	${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/imgui_widgets.cpp
)

target_compile_features(zcdi_dbg PRIVATE c_std_11 cxx_std_20)
target_compile_options(zcdi_dbg PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra>)

target_link_libraries(zcdi_dbg PRIVATE zcdi::zcdi musashi)

target_compile_definitions(zcdi_dbg PRIVATE $<TARGET_PROPERTY:zcdi,COMPILE_DEFINITIONS>)

target_compile_definitions(musashi PRIVATE M68K_INSTRUCTION_HOOK=1)

target_include_directories(zcdi_dbg PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/third_party
	${CMAKE_CURRENT_LIST_DIR}/src
)

if(UNIX AND NOT APPLE AND NOT ANDROID)
	find_package(Threads REQUIRED)
	find_package(OpenGL REQUIRED)
	find_package(X11 COMPONENTS Xi Xcursor REQUIRED)
	find_package(ALSA)
	target_link_libraries(zcdi_dbg PRIVATE
		OpenGL::GL
		X11::X11 X11::Xi X11::Xcursor
		Threads::Threads
		ALSA::ALSA
		m dl
	)
elseif(APPLE)
	set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/sokol_impl.cpp
		PROPERTIES COMPILE_OPTIONS "-x;objective-c++")
	target_link_libraries(zcdi_dbg PRIVATE
		"-framework AppKit" "-framework QuartzCore"
		"-framework Metal" "-framework AudioToolbox"
	)
endif()

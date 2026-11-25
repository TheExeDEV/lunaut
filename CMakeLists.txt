cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/luau.version" LUNAUT_LUAU_VERSION)
string(STRIP "${LUNAUT_LUAU_VERSION}" LUNAUT_LUAU_VERSION)

project(lunaut VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LUNAUT_DEBUG "Enable debug diagnostics" OFF)
option(LUNAUT_STATIC_RUNTIME "Link C++ runtime statically" ON)
option(LUNAUT_COVERAGE "Build lunaut with gcov coverage counters" OFF)

if(MSVC AND LUNAUT_STATIC_RUNTIME)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)
endif()

include(FetchContent)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/luau/CMakeLists.txt")
	message(FATAL_ERROR "extern/luau is empty. Run: git submodule update --init")
endif()

add_subdirectory(extern/luau EXCLUDE_FROM_ALL)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
	GIT_REPOSITORY https://github.com/nlohmann/json.git
	GIT_TAG v3.11.3
	GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(json)

configure_file(src/Lunaut/build.hpp.in "${CMAKE_BINARY_DIR}/generated/build.hpp" @ONLY)

file(GLOB_RECURSE LUNAUT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Lunaut/*.cpp")

add_executable(lunaut ${LUNAUT_SOURCES})
target_include_directories(lunaut PRIVATE src/Lunaut "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(lunaut PRIVATE Luau.Analysis Luau.Ast Luau.Config Luau.Compiler Luau.VM nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_definitions(lunaut PRIVATE LUNAUT_DEBUG=$<BOOL:${LUNAUT_DEBUG}>)

if(MSVC)
	target_compile_options(lunaut PRIVATE /W3 /bigobj /utf-8 /permissive-)
	target_compile_definitions(lunaut PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
	target_compile_options(lunaut PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
	if(MINGW)
		target_compile_options(lunaut PRIVATE -Wa,-mbig-obj)
		if(LUNAUT_STATIC_RUNTIME)
			target_link_options(lunaut PRIVATE -static -static-libgcc -static-libstdc++)
		endif()
	endif()
endif()

if(LUNAUT_COVERAGE AND NOT MSVC)
	target_compile_options(lunaut PRIVATE --coverage -O0 -fprofile-update=atomic)
	target_link_options(lunaut PRIVATE --coverage)
endif()

install(TARGETS lunaut RUNTIME DESTINATION bin)

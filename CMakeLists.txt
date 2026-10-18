cmake_minimum_required(VERSION 3.16)
project(canopy CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

set(SYSROOT ${CMAKE_SOURCE_DIR}/third_party/sysroot)
set(SYSLIB /usr/lib/x86_64-linux-gnu)
# Runtime libraries are the system ones; dev headers come from the extracted .debs.
set(SDL2_LIB ${SYSLIB}/libSDL2-2.0.so.0)
set(FREETYPE_LIB ${SYSLIB}/libfreetype.so.6)
set(HARFBUZZ_LIB ${SYSLIB}/libharfbuzz.so.0)
set(FONTCONFIG_LIB ${SYSLIB}/libfontconfig.so.1)

find_package(Threads REQUIRED)

# ---- core: VT emulation (no SDL/GL) ----
set(VT_SRC src/core/cell.cpp src/core/pty.cpp src/core/terminal.cpp src/core/vt_parser.cpp src/core/keys.cpp
           src/core/blocks.cpp src/core/session.cpp)
# ---- core: model (json, config, activity tree, structured output) ----
set(MODEL_SRC src/core/json.cpp src/core/config.cpp src/core/activity.cpp src/core/structure.cpp src/core/summarizer.cpp)

foreach(f ${VT_SRC} ${MODEL_SRC})
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/${f})
    file(WRITE ${CMAKE_SOURCE_DIR}/${f} "// placeholder: not yet implemented\n")
  endif()
endforeach()

add_library(canopy_vt STATIC ${VT_SRC})
target_include_directories(canopy_vt PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(canopy_vt PUBLIC Threads::Threads util)

add_library(canopy_model STATIC ${MODEL_SRC})
target_include_directories(canopy_model PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(canopy_core INTERFACE)
target_link_libraries(canopy_core INTERFACE canopy_vt canopy_model)

# ---- gfx (SDL2 + GL + FreeType); header-only dependency on core ----
file(GLOB GFX_SRC ${CMAKE_SOURCE_DIR}/src/gfx/*.cpp)
add_library(canopy_gfx STATIC ${GFX_SRC})
target_include_directories(canopy_gfx PUBLIC ${CMAKE_SOURCE_DIR}/src
  SYSTEM PUBLIC ${SYSROOT}/usr/include ${SYSROOT}/usr/include/x86_64-linux-gnu ${SYSROOT}/usr/include/SDL2 ${SYSROOT}/usr/include/freetype2 ${SYSROOT}/usr/include/harfbuzz)
target_compile_definitions(canopy_gfx PUBLIC _REENTRANT)
target_link_libraries(canopy_gfx PUBLIC ${SDL2_LIB} ${FREETYPE_LIB} ${HARFBUZZ_LIB} ${FONTCONFIG_LIB} dl)

# ---- app ----
file(GLOB UI_SRC ${CMAKE_SOURCE_DIR}/src/ui/*.cpp ${CMAKE_SOURCE_DIR}/src/*.cpp)
if(UI_SRC)
  add_executable(canopy ${UI_SRC})
  target_link_libraries(canopy PRIVATE canopy_gfx canopy_core)
endif()

# ---- tools ----
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/vtdump.cpp)
  add_executable(vtdump tools/vtdump.cpp)
  target_link_libraries(vtdump PRIVATE canopy_vt)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/gfxdemo.cpp)
  add_executable(gfxdemo tools/gfxdemo.cpp)
  target_link_libraries(gfxdemo PRIVATE canopy_gfx)
endif()

# ---- tests ----
enable_testing()
file(GLOB TEST_SRC ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(t ${TEST_SRC})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  if(${name} MATCHES "test_(terminal|vt|keys|pty|session|blocks)")
    target_link_libraries(${name} PRIVATE canopy_vt)
  else()
    target_link_libraries(${name} PRIVATE canopy_model canopy_vt)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endforeach()

if(TARGET canopy)
  install(TARGETS canopy RUNTIME DESTINATION bin)
endif()
install(DIRECTORY shell/ DESTINATION share/canopy/shell)

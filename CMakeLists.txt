cmake_minimum_required(VERSION 3.26)

project(lua_python_bridge VERSION 0.1.0 LANGUAGES C)

# scikit-build-core builds the Python extension; a plain CMake / luarocks build makes the Lua module.
if(DEFINED SKBUILD)
  set(_lpb_default_python ON)
  set(_lpb_default_lua OFF)
else()
  set(_lpb_default_python OFF)
  set(_lpb_default_lua ON)
endif()

option(LPB_BUILD_PYTHON_MODULE "Build the Python extension module 'lua' (Lua inside Python)" ${_lpb_default_python})
option(LPB_BUILD_LUA_MODULE "Build the Lua module 'python' (Python inside Lua)" ${_lpb_default_lua})
option(LPB_BUILD_TESTS "Build the C embedding test and register the test suites with CTest" OFF)
option(LPB_LUA_APICHECK "Build the bundled Lua with LUA_USE_APICHECK (for tests)" OFF)
option(LPB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(LPB_LUA_TARGET "" CACHE STRING "Existing CMake target providing Lua for the Lua module (embedding)")
set(LUA_INCLUDE_DIR "" CACHE PATH "Lua headers for the Lua module (when LPB_LUA_TARGET is not set)")
set(LUA_LIBRARIES "" CACHE STRING "Lua import library for the Lua module (Windows only)")
set(LPB_PYTHON_HEADERS "vendored" CACHE STRING "Python headers for the Lua module: vendored or system")
set_property(CACHE LPB_PYTHON_HEADERS PROPERTY STRINGS vendored system)
set(LPB_LUA_MODULE_INSTALL_DIR "lib/lua" CACHE PATH "Install directory of the Lua module")

find_package(Threads REQUIRED)

set(LPB_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LPB_CORE_SOURCES
  ${LPB_DIR}/src/runtime.c
  ${LPB_DIR}/src/convert.c
  ${LPB_DIR}/src/pythoninlua.c
  ${LPB_DIR}/src/luainpython.c)

function(lpb_setup_target tgt)
  set_target_properties(${tgt} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden PREFIX "")
  target_include_directories(${tgt} PRIVATE ${LPB_DIR}/src)
  target_compile_definitions(${tgt} PRIVATE LPB_VERSION="${PROJECT_VERSION}")
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W3 $<$<BOOL:${LPB_WARNINGS_AS_ERRORS}>:/WX>)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter
      $<$<BOOL:${LPB_WARNINGS_AS_ERRORS}>:-Werror>)
  endif()
  target_link_libraries(${tgt} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endfunction()

# Restrict the dynamic symbol table to `exports` (list of patterns).
function(lpb_exports tgt name)
  set(exports ${ARGN})
  if(APPLE)
    list(TRANSFORM exports PREPEND "_")
    list(JOIN exports "\n" text)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.exp "${text}\n")
    target_link_options(${tgt} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/${name}.exp")
  elseif(UNIX)
    list(JOIN exports "; " text)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.map "{ global: ${text}; local: *; };\n")
    target_link_options(${tgt} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/${name}.map")
  endif()
endfunction()

# ------------------------------------------------------------------------------------------------
# Bundled Lua 5.5 (Python extension, embedding test)
# ------------------------------------------------------------------------------------------------
if(LPB_BUILD_PYTHON_MODULE OR LPB_BUILD_TESTS)
  file(GLOB LPB_LUA_SOURCES ${LPB_DIR}/third_party/lua-5.5.1/src/*.c)
  if(WIN32)
    # Binary Lua modules link against the Lua DLL by name (PLAN.md 5.9).
    add_library(lpb_lua SHARED ${LPB_LUA_SOURCES})
    set_target_properties(lpb_lua PROPERTIES OUTPUT_NAME lua55)
    target_compile_definitions(lpb_lua PUBLIC LUA_BUILD_AS_DLL)
  else()
    add_library(lpb_lua STATIC ${LPB_LUA_SOURCES})
    set_target_properties(lpb_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(APPLE)
      target_compile_definitions(lpb_lua PRIVATE LUA_USE_MACOSX)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_compile_definitions(lpb_lua PRIVATE LUA_USE_LINUX)
    else()
      target_compile_definitions(lpb_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
    endif()
    target_link_libraries(lpb_lua PUBLIC m ${CMAKE_DL_LIBS})
  endif()
  set_target_properties(lpb_lua PROPERTIES C_STANDARD 99)
  target_include_directories(lpb_lua PUBLIC ${LPB_DIR}/third_party/lua-5.5.1/src)
  if(LPB_LUA_APICHECK)
    target_compile_definitions(lpb_lua PUBLIC LUA_USE_APICHECK)
  endif()
endif()

# ------------------------------------------------------------------------------------------------
# Python extension module `lua` (wheel): links Python normally, bundles Lua
# ------------------------------------------------------------------------------------------------
if(LPB_BUILD_PYTHON_MODULE)
  if(DEFINED SKBUILD_SABI_COMPONENT AND SKBUILD_SABI_COMPONENT)
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter ${SKBUILD_SABI_COMPONENT})
  else()
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.SABIModule)
  endif()
  Python_add_library(lpb_python MODULE USE_SABI 3.10 WITH_SOABI ${LPB_CORE_SOURCES})
  lpb_setup_target(lpb_python)
  set_target_properties(lpb_python PROPERTIES OUTPUT_NAME lua)
  if(WIN32)
    target_link_libraries(lpb_python PRIVATE lpb_lua)
  else()
    # The whole Lua API is exported for binary Lua modules (PLAN.md 5.9).
    target_link_libraries(lpb_python PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,lpb_lua>")
    lpb_exports(lpb_python lua_exports PyInit_lua "lua_*" "luaL_*" "luaopen_*")
  endif()
  install(TARGETS lpb_python LIBRARY DESTINATION . RUNTIME DESTINATION .)
  if(WIN32)
    install(TARGETS lpb_lua RUNTIME DESTINATION .)
  endif()
  install(FILES ${LPB_DIR}/python/lua.pyi DESTINATION .)
endif()

# ------------------------------------------------------------------------------------------------
# Lua module `python` (rock): Lua comes from the host, libpython is loaded at runtime
# ------------------------------------------------------------------------------------------------
if(LPB_BUILD_LUA_MODULE)
  add_library(lpb_lua_module MODULE ${LPB_CORE_SOURCES} ${LPB_DIR}/src/pyloader.c)
  lpb_setup_target(lpb_lua_module)
  set_target_properties(lpb_lua_module PROPERTIES OUTPUT_NAME python)
  if(NOT WIN32)
    set_target_properties(lpb_lua_module PROPERTIES SUFFIX ".so")
  endif()
  target_compile_definitions(lpb_lua_module PRIVATE
    Py_LIMITED_API=0x030A0000 LPB_DYNAMIC_PYTHON LPB_LUA_MODULE)

  if(LPB_PYTHON_HEADERS STREQUAL "system")
    find_package(Python 3.10 REQUIRED COMPONENTS Development.Module)
    target_include_directories(lpb_lua_module PRIVATE ${Python_INCLUDE_DIRS})
  else()
    if(APPLE)
      set(_lpb_pyconfig macos)
    elseif(WIN32)
      set(_lpb_pyconfig windows)
    else()
      set(_lpb_pyconfig linux)
    endif()
    target_include_directories(lpb_lua_module PRIVATE
      ${LPB_DIR}/third_party/python3.10-headers/pyconfig/${_lpb_pyconfig}
      ${LPB_DIR}/third_party/python3.10-headers/include)
  endif()
  if(WIN32)
    # No dllimport declarations and no automatic link to pythonXY.lib.
    target_compile_definitions(lpb_lua_module PRIVATE Py_NO_ENABLE_SHARED)
  endif()

  if(LPB_LUA_TARGET)
    target_link_libraries(lpb_lua_module PRIVATE ${LPB_LUA_TARGET})
  else()
    if(NOT LUA_INCLUDE_DIR)
      message(FATAL_ERROR "Set LUA_INCLUDE_DIR (or LPB_LUA_TARGET) to build the Lua module")
    endif()
    target_include_directories(lpb_lua_module PRIVATE ${LUA_INCLUDE_DIR})
    if(WIN32)
      if(NOT LUA_LIBRARIES)
        message(FATAL_ERROR "Set LUA_LIBRARIES to the Lua import library to build the Lua module")
      endif()
      target_compile_definitions(lpb_lua_module PRIVATE LUA_BUILD_AS_DLL)
      target_link_libraries(lpb_lua_module PRIVATE ${LUA_LIBRARIES})
    elseif(APPLE)
      # The Lua API comes from the host process.
      target_link_options(lpb_lua_module PRIVATE "LINKER:-undefined,dynamic_lookup")
    endif()
  endif()
  lpb_exports(lpb_lua_module python_exports luaopen_python)
  install(TARGETS lpb_lua_module LIBRARY DESTINATION ${LPB_LUA_MODULE_INSTALL_DIR}
                                 RUNTIME DESTINATION ${LPB_LUA_MODULE_INSTALL_DIR})
endif()

# ------------------------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------------------------
if(LPB_BUILD_TESTS)
  # A C host with two Lua states and several threads, loading the Lua module through require.
  add_executable(lpb_embed_test ${LPB_DIR}/tests/embed/embed.c)
  target_link_libraries(lpb_embed_test PRIVATE lpb_lua Threads::Threads)
  set_target_properties(lpb_embed_test PROPERTIES ENABLE_EXPORTS ON C_STANDARD 11)
endif()

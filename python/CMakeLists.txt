cmake_minimum_required(VERSION 3.10)
project(python)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_SKIP_BUILD_RPATH FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
    set(CMAKE_MACOSX_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
elseif(UNIX)
    set(CMAKE_SKIP_BUILD_RPATH FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/.")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

set(SOURCES 
    ${PROJECT_SOURCE_DIR}/lunatic-python/src/pythoninlua.c
    ${PROJECT_SOURCE_DIR}/lunatic-python/src/luainpython.c
    ${PROJECT_SOURCE_DIR}/dynamic_python_interface.cpp
)

if (NOT TARGET lua)
    SET(BUILD_DESKTOP_LUA TRUE)
    add_subdirectory(${PROJECT_SOURCE_DIR}/../../../3party/lua_wrapper ${CMAKE_CURRENT_BINARY_DIR}/lua_bld)
endif()



add_library(${PROJECT_NAME} SHARED ${SOURCES} )
set(ADDITIONAL_LIBS "")

if(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PYTHON_LIBRT)
elseif(WIN32)
    target_compile_definitions(${PROJECT_NAME} PUBLIC Py_BUILD_CORE)
    target_compile_definitions(${PROJECT_NAME} PUBLIC LUA_BUILD_AS_DLL LUA_LIB)
    list(APPEND ADDITIONAL_LIBS "psapi")

endif()

if(UNIX)
    if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
        set(PYTHON_INCLUDES ${PROJECT_SOURCE_DIR}/../../../3party/python-headers/python3.10-unix-32bits)
    else()
        set(PYTHON_INCLUDES ${PROJECT_SOURCE_DIR}/../../../3party/python-headers/python3.10-unix)
    endif()
else(WIN32)
    set(PYTHON_INCLUDES ${PROJECT_SOURCE_DIR}/../../../3party/python-headers/python3.10-windows)
endif()



target_include_directories(${PROJECT_NAME} PUBLIC
    ${LUA_INCLUDE}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/../../../common
    ${PROJECT_SOURCE_DIR}/../../../3party
    ${PROJECT_SOURCE_DIR}/lunatic-python/src/
    ${PROJECT_SOURCE_DIR}/../../../3party/dylib/include
    ${PYTHON_INCLUDES}
)


target_link_libraries(${PROJECT_NAME} 
    lua 
    ${ADDITIONAL_LIBS}
)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "python")
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
install(TARGETS ${PROJECT_NAME} DESTINATION ./)

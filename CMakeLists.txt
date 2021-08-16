cmake_minimum_required (VERSION 2.8)
project (iflytekrecog)

# Set source files
set (IFLYTEK_RECOG_SOURCES
	src/iflytek_recog_engine.c
)
source_group ("src" FILES ${IFLYTEK_RECOG_SOURCES})

#SET(CMAKE_BUILD_TYPE "Release")
SET(CMAKE_BUILD_TYPE "Debug")

include(FindPkgConfig)
if($ENV{PKG_CONFIG_PATH})
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/unimrcp/lib/pkgconfig")
else()
  set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:/usr/local/unimrcp/lib/pkgconfig")
endif()

# Looking for unimrcpplugin.pc
pkg_check_modules (UNIMRCPPLUGIN REQUIRED unimrcpplugin>=1.4.0)
message(STATUS "unimrcpplugin ${PKG_CONFIG_FOUND} ${UNIMRCPPLUGIN_VERSION}, 
    ${UNIMRCPPLUGIN_INCLUDEDIR}
    ${UNIMRCPPLUGIN_INCLUDE_DIRS}
    ${UNIMRCPPLUGIN_LIBDIR}
    ${UNIMRCPPLUGIN_LIBRARY_DIRS}
    ${UNIMRCPPLUGIN_CFLAGS}
    ${UNIMRCPPLUGIN_LDFLAGS}
    ${UNIMRCPPLUGIN_LIBRARIES}
")
#link_directories(${UNIMRCPPLUGIN_LIBRARY_DIRS})

# Plug-in declaration
add_library (${PROJECT_NAME} MODULE ${IFLYTEK_RECOG_SOURCES})

#set(CPACK_PACKAGE_VERSION_MAJOR "1")
#set(CPACK_PACKAGE_VERSION_MINOR "0")
#set(CPACK_PACKAGE_VERSION_PATCH "0")
#set(LIB_VERSION_STRING ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH})
set (LIB_VERSION_STRING ${UNIMRCPPLUGIN_VERSION})
set_target_properties (${PROJECT_NAME} PROPERTIES
                    FOLDER "plugins"
                    VERSION ${LIB_VERSION_STRING})

# Input unimrcpplugin libraries
add_compile_options(${UNIMRCPPLUGIN_CFLAGS})
target_link_libraries(${PROJECT_NAME}
	${UNIMRCPPLUGIN_LDFLAGS}
)

# Input system libraries
if (WIN32)
	target_link_libraries(${PROJECT_NAME} ws2_32 winmm)
elseif (UNIX)
	target_link_libraries(${PROJECT_NAME} m)
endif ()

# Include directories
include_directories (
	${PROJECT_SOURCE_DIR}/include
	${UNIMRCPPLUGIN_INCLUDE_DIRS}
)

# Installation directives
install (TARGETS ${PROJECT_NAME} LIBRARY DESTINATION plugin)
if (MSVC)
	install (FILES ${PROJECT_BINARY_DIR}/Debug/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS Debug)
	install (FILES ${PROJECT_BINARY_DIR}/RelWithDebInfo/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS RelWithDebInfo)
endif (MSVC)

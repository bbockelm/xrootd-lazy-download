
cmake_minimum_required( VERSION 2.6 )
project( xrootd-lazy-download )

set( CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake )

find_package( Xrootd REQUIRED )

if( CMAKE_COMPILER_IS_GNUCXX )
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror" )
endif()

if( CMAKE_COMPILER_IS_GNUCC )
  set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror" )
endif()

SET( CMAKE_SHARED_LINKER_FLAGS "--no-undefined")
SET( CMAKE_MODULE_LINKER_FLAGS "--no-undefined")
SET( CMAKE_EXE_LINKER_FLAGS "--no-undefined")

include_directories( "${PROJECT_SOURCE_DIR}" "${XROOTD_INCLUDES}" )

add_library(XrdClLazyDownload MODULE
  src/XrdClLazyDownload.cc
  src/LocalFileSystem.cc
  )
target_link_libraries(XrdClLazyDownload ${XROOTD_CL})

if (NOT DEFINED CMAKE_INSTALL_LIBDIR)
  SET(CMAKE_INSTALL_LIBDIR "${CMAKE_INSTALL_PREFIX}/lib")
endif()

if (NOT DEFINED SYSCONF_INSTALL_DIR)
  SET(SYSCONF_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/etc")
endif()

configure_file("${PROJECT_SOURCE_DIR}/config/lazy-download.conf.in"
  "${PROJECT_BINARY_DIR}/config/lazy-download.conf")

install(
  TARGETS XrdClLazyDownload
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} )

install(
  FILES "${PROJECT_BINARY_DIR}/config/lazy-download.conf"
  DESTINATION "${SYSCONF_INSTALL_DIR}/xrootd/client.plugins.d" )


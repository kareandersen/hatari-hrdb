# Locate the libvncserver library
#
# This module defines:
#  VncServer_FOUND - system has libvncserver
#  VNCSERVER_INCLUDE_DIR - the libvncserver include directory
#  VNCSERVER_LIBRARY - link these to use libvncserver

find_package(PkgConfig QUIET)
pkg_check_modules(PC_VNCSERVER libvncserver)

find_path(VNCSERVER_INCLUDE_DIR
	rfb/rfb.h
	HINTS ${PC_VNCSERVER_INCLUDEDIR} ${PC_VNCSERVER_INCLUDE_DIRS}
)

find_library(VNCSERVER_LIBRARY
	NAMES vncserver
	HINTS ${PC_VNCSERVER_LIBDIR} ${PC_VNCSERVER_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(VncServer DEFAULT_MSG
                                  VNCSERVER_LIBRARY VNCSERVER_INCLUDE_DIR)

mark_as_advanced(VNCSERVER_INCLUDE_DIR VNCSERVER_LIBRARY)

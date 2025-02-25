#.rst:
# FindFstrcmp
# --------
# Finds the fstrcmp library
#
# This will define the following variables::
#
# FSTRCMP_FOUND - system has libfstrcmp
# FSTRCMP_INCLUDE_DIRS - the libfstrcmp include directory
# FSTRCMP_LIBRARIES - the libfstrcmp libraries
#

if(ENABLE_INTERNAL_FSTRCMP)
  find_program(LIBTOOL libtool REQUIRED)
  include(ExternalProject)
  include(cmake/scripts/common/ModuleHelpers.cmake)

  get_archive_name(libfstrcmp)

  # allow user to override the download URL with a local tarball
  # needed for offline build envs
  if(FSTRCMP_URL)
    get_filename_component(FSTRCMP_URL "${FSTRCMP_URL}" ABSOLUTE)
  else()
    set(FSTRCMP_URL http://mirrors.kodi.tv/build-deps/sources/${LIBFSTRCMP_ARCHIVE})
  endif()
  if(VERBOSE)
    message(STATUS "FSTRCMPURL: ${FSTRCMP_URL}")
  endif()

  set(FSTRCMP_LIBRARY ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/lib/libfstrcmp.a)
  set(FSTRCMP_INCLUDE_DIR ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/include)
  externalproject_add(fstrcmp
                      URL ${FSTRCMP_URL}
                      URL_HASH ${FSTRCMP_HASH}
                      DOWNLOAD_DIR ${TARBALL_DIR}
                      PREFIX ${CORE_BUILD_DIR}/fstrcmp
                      PATCH_COMMAND autoreconf -vif
                      CONFIGURE_COMMAND ./configure --prefix ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}
                      BUILD_BYPRODUCTS ${FSTRCMP_LIBRARY}
                      BUILD_COMMAND make lib/libfstrcmp.la
                      BUILD_IN_SOURCE 1
                      INSTALL_COMMAND make install-libdir install-include)

  set_target_properties(fstrcmp PROPERTIES FOLDER "External Projects")
else()
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_FSTRCMP fstrcmp QUIET)
  endif()

  find_path(FSTRCMP_INCLUDE_DIR NAMES fstrcmp.h
                                 PATHS ${PC_FSTRCMP_INCLUDEDIR})

  find_library(FSTRCMP_LIBRARY NAMES fstrcmp
                                PATHS ${PC_FSTRCMP_LIBDIR})

  set(FSTRCMP_VER ${PC_FSTRCMP_VERSION})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Fstrcmp
                                  REQUIRED_VARS FSTRCMP_LIBRARY FSTRCMP_INCLUDE_DIR
                                  VERSION_VAR FSTRCMP_VER)

if(FSTRCMP_FOUND)
  set(FSTRCMP_INCLUDE_DIRS ${FSTRCMP_INCLUDE_DIR})
  set(FSTRCMP_LIBRARIES ${FSTRCMP_LIBRARY})
endif()

if(NOT TARGET fstrcmp)
  add_library(fstrcmp UNKNOWN IMPORTED)
  set_target_properties(fstrcmp PROPERTIES
                                IMPORTED_LOCATION "${FSTRCMP_LIBRARY}"
                                INTERFACE_INCLUDE_DIRECTORIES "${FSTRCMP_INCLUDE_DIR}")
endif()
mark_as_advanced(FSTRCMP_INCLUDE_DIR FSTRCMP_LIBRARY)

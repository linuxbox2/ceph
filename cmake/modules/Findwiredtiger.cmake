# CMake module to search for wiredtiger
#
# If it's found it sets WIREDTIGER_FOUND to TRUE
# and following variables are set:
# WIREDTIGER_INCLUDE_DIRS
# WIREDTIGER_LIBRARIES

if(WIREDTIGER_PREFIX)
  set(WTI ${WIREDTIGER_PREFIX}/include)
  set(WTL ${WIREDTIGER_PREFIX}/lib)
endif()

find_path(WIREDTIGER_INCLUDE_DIR
  wiredtiger.h
  PATHS
  /usr/include
  /usr/local/include
  ${WTI})

find_library(WIREDTIGER_LIBRARY NAMES wiredtiger
  PATHS
  /usr/local/lib
  /usr/lib
  ${WTL})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WIREDTIGER DEFAULT_MSG WIREDTIGER_LIBRARY WIREDTIGER_INCLUDE_DIR)

mark_as_advanced(WIREDTIGER_LIBRARY WIREDTIGER_INCLUDE_DIR)

if(WIREDTIGER_FOUND)
  message(STATUS "wt include: ${WIREDTIGER_INCLUDE_DIR}")
  message(STATUS "wt lib: ${WIREDTIGER_LIBRARY}")

  set(WIREDTIGER_INCLUDE_DIRS "${WIREDTIGER_INCLUDE_DIR}")
  set(WIREDTIGER_LIBRARIES "${WIREDTIGER_LIBRARY}")

endif()

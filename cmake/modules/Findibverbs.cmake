# Copyright (C) 2007-2012 Hypertable, Inc.
#
# This file is part of Hypertable.
#
# Hypertable is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3
# of the License, or any later version.
#
# Hypertable is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Hypertable. If not, see <http://www.gnu.org/licenses/>
#

# - Find libibverbs
# Find the ibverbs library and includes
#
# IBVERBS_INCLUDE_DIR - where to find ibverbs.h, etc.
# IBVERBS_LIBRARIES - List of libraries when using ibverbs.
# IBVERBS_FOUND - True if ibverbs found.

find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)

set(IBVERBS_NAMES ${IBVERBS_NAMES} ibverbs)
find_library(IBVERBS_LIBRARY NAMES ${IBVERBS_NAMES})

if (IBVERBS_INCLUDE_DIR AND IBVERBS_LIBRARY)
  set(IBVERBS_FOUND TRUE)
  set( IBVERBS_LIBRARIES ${IBVERBS_LIBRARY} )
else ()
  set(IBVERBS_FOUND FALSE)
  set( IBVERBS_LIBRARIES )
endif ()

if (IBVERBS_FOUND)
  message(STATUS "Found libibverbs: ${IBVERBS_LIBRARY}")
else ()
  message(STATUS "Not Found libibverbs: ${IBVERBS_LIBRARY}")
  if (IBVERBS_FIND_REQUIRED)
    message(STATUS "Looked for libibverbs named ${IBVERBS_NAMES}.")
    message(FATAL_ERROR "Could NOT find libibverbs")
  endif ()
endif ()

# handle the QUIETLY and REQUIRED arguments and set UUID_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ibverbs DEFAULT_MSG IBVERBS_LIBRARIES IBVERBS_INCLUDE_DIR)

mark_as_advanced(
  IBVERBS_LIBRARY
  IBVERBS_I
)

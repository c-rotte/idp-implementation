#  Copyright (c) 2018, Facebook, Inc.
#  All rights reserved.
#
#  This source code is licensed under the BSD-style license found in the
#  LICENSE file in the root directory of this source tree.

# This module sets the following variables:
#   proxygen_FOUND
#   proxygen_INCLUDE_DIRS
#
# This module exports the following target:
#    proxygen::proxygen
#
# which can be used with target_link_libraries() to pull in the proxygen
# library.


####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was proxygen-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)
include_directories(proxygen/_build/deps/include)
link_directories(proxygen/_build/deps/lib)
find_dependency(fmt)
find_dependency(folly)
find_dependency(wangle)
if ("On")
  find_dependency(mvfst)
endif()
find_dependency(Fizz)
# For now, anything that depends on Proxygen has to copy its FindZstd.cmake
# and issue a `find_package(Zstd)`.  Uncommenting this won't work because
# this Zstd module exposes a library called `zstd`.  The right fix is
# discussed on D24686032.
#
# find_dependency(Zstd)
find_dependency(ZLIB)
find_dependency(OpenSSL)
find_dependency(Threads)
find_dependency(Boost 1.58 REQUIRED
  COMPONENTS
    iostreams
    context
    filesystem
    program_options
    regex
    system
    thread
)

if(NOT TARGET proxygen::proxygen)
    include("${CMAKE_CURRENT_LIST_DIR}/proxygen-targets.cmake")
    get_target_property(proxygen_INCLUDE_DIRS proxygen::proxygen INTERFACE_INCLUDE_DIRECTORIES)
endif()

if(NOT proxygen_FIND_QUIETLY)
    message(STATUS "Found proxygen: ${PACKAGE_PREFIX_DIR}")
endif()

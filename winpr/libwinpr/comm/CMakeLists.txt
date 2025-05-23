# WinPR: Windows Portable Runtime
# libwinpr-comm cmake build script
#
# Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(MODULE_NAME "winpr-comm")
set(MODULE_PREFIX "WINPR_COMM")

if(NOT WIN32)
  set(${MODULE_PREFIX}_SRCS comm.c comm.h)
  if(NOT EMSCRIPTEN AND NOT APPLE)
    winpr_definition_add(WINPR_HAVE_SERIAL_SUPPORT)
    list(
      APPEND
      ${MODULE_PREFIX}_SRCS
      comm_io.c
      comm_ioctl.c
      comm_ioctl.h
      comm_serial_sys.c
      comm_serial_sys.h
      comm_sercx_sys.c
      comm_sercx_sys.h
      comm_sercx2_sys.c
      comm_sercx2_sys.h
    )
  else()
    list(APPEND ${MODULE_PREFIX}_SRCS comm_ioctl_dummy.c comm_ioctl.h)
  endif()

  winpr_module_add(${${MODULE_PREFIX}_SRCS})

  if(NOT EMSCRIPTEN)
    if(BUILD_TESTING_INTERNAL AND BUILD_COMM_TESTS)
      add_subdirectory(test)
    endif()
  endif()
endif()

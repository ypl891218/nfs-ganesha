# SPDX-License-Identifier: LGPL-3.0-or-later
#-------------------------------------------------------------------------------
#
# Copyright Panasas, 2012
# Contributor: Jim Lieb <jlieb@panasas.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
#-------------------------------------------------------------------------------
include_directories(
  ${LTTNG_INCLUDE_DIR}
)

set(ganesha_trace_LIB_SRCS
  lttng_probes.c
)

add_library(ganesha_trace MODULE ${ganesha_trace_LIB_SRCS})
add_sanitizers(ganesha_trace)

target_link_libraries(ganesha_trace
  ${LTTNG_LIBRARIES}
)

install(TARGETS ganesha_trace COMPONENT tracing DESTINATION ${LIB_INSTALL_DIR} )

add_library(ganesha_trace_symbols INTERFACE)
target_sources(ganesha_trace_symbols INTERFACE ${CMAKE_CURRENT_LIST_DIR}/lttng_defines.c)
target_link_libraries(ganesha_trace_symbols INTERFACE ${LTTNG_LIBRARIES})

add_custom_target(gsh_trace_header_generate
  DEPENDS ntirpc_generate_lttng_trace_headers gsh_generate_lttng_trace_headers
)

add_dependencies(ganesha_trace_symbols gsh_trace_header_generate)
add_dependencies(ganesha_trace gsh_trace_header_generate)
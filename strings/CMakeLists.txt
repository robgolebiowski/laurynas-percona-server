# Copyright (c) 2006, 2016, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include)
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/strings)

SET(STRINGS_SOURCES
  ctype-big5.cc
  ctype-bin.cc
  ctype.cc
  ctype-cp932.cc
  ctype-czech.cc
  ctype-eucjpms.cc
  ctype-euc_kr.cc
  ctype-extra.cc
  ctype-gb18030.cc
  ctype-gb2312.cc
  ctype-gbk.cc
  ctype-latin1.cc
  ctype-mb.cc
  ctype-simple.cc
  ctype-sjis.cc
  ctype-tis620.cc
  ctype-uca.cc
  ctype-ucs2.cc
  ctype-ujis.cc
  ctype-utf8.cc
  ctype-win1250ch.cc
  decimal.cc
  dtoa.cc
  int2str.cc
  longlong2str.cc
  my_strchr.cc
  my_strtoll10.cc
  my_vsnprintf.cc
  str2int.cc
  str_alloc.cc
  strcont.cc
  strmake.cc
  strxmov.cc
  strxnmov.cc
  xml.cc
)

# Avoid dependencies on perschema data defined in mysys
ADD_DEFINITIONS(-DDISABLE_MYSQL_THREAD_H)
ADD_CONVENIENCE_LIBRARY(strings ${STRINGS_SOURCES})

ADD_EXECUTABLE(conf_to_src conf_to_src.cc)
TARGET_LINK_LIBRARIES(conf_to_src strings)

IF(MSVC)
   INSTALL_DEBUG_TARGET(strings DESTINATION ${INSTALL_LIBDIR}/debug)
ENDIF()

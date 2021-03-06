# Copyright (c) 2009, 2016, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

# Charsets and collations
IF(NOT DEFAULT_CHARSET)
  SET(DEFAULT_CHARSET "latin1")
ENDIF()

IF(NOT DEFAULT_COLLATION)
  SET(DEFAULT_COLLATION "latin1_swedish_ci")
ENDIF()

IF(WITH_EXTRA_CHARSETS AND NOT WITH_EXTRA_CHARSETS STREQUAL "all")
  MESSAGE(WARNING "Option WITH_EXTRA_CHARSETS is no longer supported")
ENDIF()

SET(MYSQL_DEFAULT_CHARSET_NAME "${DEFAULT_CHARSET}") 
SET(MYSQL_DEFAULT_COLLATION_NAME "${DEFAULT_COLLATION}")

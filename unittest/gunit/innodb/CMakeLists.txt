# Copyright (c) 2013, 2016, Oracle and/or its affiliates. All rights reserved.
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

INCLUDE_DIRECTORIES(
  ${GTEST_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/sql/auth
  ${CMAKE_SOURCE_DIR}/storage/innobase/include
  ${CMAKE_SOURCE_DIR}/unittest/gunit
)

INCLUDE(${CMAKE_SOURCE_DIR}/storage/innobase/innodb.cmake)

SET(TESTS
  #example
  ha_innodb
  mem0mem
  ut0crc32
  ut0lock_free_hash
  ut0mem
  ut0new
)

IF (MERGE_UNITTESTS)
  SET(MERGE_INNODB_TESTS ${CMAKE_CURRENT_BINARY_DIR}/merge_innodb_tests-t.cc)
  SET_SOURCE_FILES_PROPERTIES(MERGE_INNODB_TESTS PROPERTIES GENERATED 1)

  ADD_COMPILE_FLAGS(
    ${MERGE_INNODB_TESTS}
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

  FILE(WRITE ${MERGE_INNODB_TESTS} "// Merging InnoDB unit tests\n")
  FILE(APPEND ${MERGE_INNODB_TESTS} "#include \"my_config.h\"\n")
  FILE(APPEND ${MERGE_INNODB_TESTS} "#include \"univ.i\"\n")

  FOREACH(test ${TESTS})
    FILE(APPEND ${MERGE_INNODB_TESTS} "#include \"${test}-t.cc\"\n")
  ENDFOREACH()

  SET(SRC_FILES ${MERGE_INNODB_TESTS})
  IF(WIN32)
    LIST(APPEND SRC_FILES ../../../sql/nt_servc.cc)
  ENDIF()

  ADD_EXECUTABLE(merge_innodb_tests-t ${SRC_FILES})

  TARGET_LINK_LIBRARIES(merge_innodb_tests-t
    sql binlog rpl master slave sql)
  TARGET_LINK_LIBRARIES(merge_innodb_tests-t
    gunit_large strings dbug regex mysys)
  TARGET_LINK_LIBRARIES(merge_innodb_tests-t
    sql binlog rpl master slave sql)

  # Download TBB from https://www.threadingbuildingblocks.org
  #SET(TBB_INCDIR /path/to/tbb43_20150611oss/include)
  #SET(TBB_LIBDIR /path/to/tbb43_20150611oss/build/linux_intel64_gcc_cc4.9.1_libc2.12_kernel2.6.39_release)
  #TARGET_LINK_LIBRARIES(merge_innodb_tests-t tbb -L${TBB_LIBDIR} -Wl,-rpath=${TBB_LIBDIR})
  #INCLUDE_DIRECTORIES(${TBB_INCDIR})

  IF(WITH_PERFSCHEMA_STORAGE_ENGINE)
    TARGET_LINK_LIBRARIES(merge_innodb_tests-t perfschema)
  ENDIF()

  ADD_DEPENDENCIES(merge_innodb_tests-t GenError)

  ADD_TEST(merge_innodb_tests merge_innodb_tests-t)

ELSE()

  ADD_COMPILE_FLAGS(
    ut0lock_free_hash-t.cc
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )
  FOREACH(test ${TESTS})
    SET(SRC_FILES ${test}-t.cc)
    IF(WIN32)
      LIST(APPEND SRC_FILES ../../../sql/nt_servc.cc)
    ENDIF()
    ADD_EXECUTABLE(${test}-t ${SRC_FILES})

    TARGET_LINK_LIBRARIES(${test}-t sql binlog rpl master slave sql)
    TARGET_LINK_LIBRARIES(${test}-t gunit_large strings dbug regex mysys)
    TARGET_LINK_LIBRARIES(${test}-t sql binlog rpl master slave sql)

    IF(WITH_PERFSCHEMA_STORAGE_ENGINE)
      TARGET_LINK_LIBRARIES(${test}-t perfschema)
    ENDIF()

    ADD_DEPENDENCIES(${test}-t GenError)

    ADD_TEST(${test} ${test}-t)
  ENDFOREACH()

ENDIF()

# Copyright (c) 2010, 2016, Oracle and/or its affiliates. All rights reserved.
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

# We want gmock-1.7.0.zip in order to build these unit tests.
# If you have already downloaded it,
# invoke cmake with -DWITH_GMOCK=/path/to/gmock-1.7.0.zip
#                or -DWITH_GMOCK=/path/to
#
# Alternatively, set an environment variable
# export WITH_GMOCK=/path/to/gmock-1.7.0.zip
#
# You can also do cmake -DENABLE_DOWNLOADS=1 
# and we will download it from http://googlemock.googlecode.com
#
# Either way: we will unpack the zip, compile gmock-all.cc and gtest-all.cc
# and link them into the executables.

# Default location for where to download and build gmock/gtest.
IF(NOT DOWNLOAD_ROOT)
  SET(DOWNLOAD_ROOT ${CMAKE_SOURCE_DIR}/source_downloads)
ENDIF()

# We want googlemock version 1.7, which also contains googletest.
SET(GMOCK_PACKAGE_NAME "gmock-1.7.0")

IF (DEFINED ENV{WITH_GMOCK} AND NOT DEFINED WITH_GMOCK)
  FILE(TO_CMAKE_PATH "$ENV{WITH_GMOCK}" WITH_GMOCK)
ENDIF()

IF(LOCAL_GMOCK_ZIP
   AND NOT ${LOCAL_GMOCK_ZIP} MATCHES ".*${GMOCK_PACKAGE_NAME}\\.zip")
 SET(LOCAL_GMOCK_ZIP 0)
ENDIF()

IF (WITH_GMOCK)
  ## Did we get a full path name, including file name?
  IF (${WITH_GMOCK} MATCHES ".*\\.zip")
    GET_FILENAME_COMPONENT(GMOCK_DIR ${WITH_GMOCK} PATH)
    GET_FILENAME_COMPONENT(GMOCK_ZIP ${WITH_GMOCK} NAME)
    FIND_FILE(LOCAL_GMOCK_ZIP
              NAMES ${GMOCK_ZIP}
              PATHS ${GMOCK_DIR}
              NO_DEFAULT_PATH
             )
  ELSE()
    ## Did we get a path name to the directory of the .zip file?
    FIND_FILE(LOCAL_GMOCK_ZIP
              NAMES "${GMOCK_PACKAGE_NAME}.zip"
              PATHS ${WITH_GMOCK}
              NO_DEFAULT_PATH
              )
    ## If WITH_GMOCK is a directory, use it for download.
    SET(DOWNLOAD_ROOT ${WITH_GMOCK})
  ENDIF()
  MESSAGE(STATUS "Local gmock zip ${LOCAL_GMOCK_ZIP}")
ENDIF()

IF(NOT EXISTS DOWNLOAD_ROOT)
  MAKE_DIRECTORY(${DOWNLOAD_ROOT})
ENDIF()
SET(GMOCK_SOURCE_DIR ${DOWNLOAD_ROOT}/${GMOCK_PACKAGE_NAME})
SET(GTEST_SOURCE_DIR ${DOWNLOAD_ROOT}/${GMOCK_PACKAGE_NAME}/gtest)

# We may have downloaded gmock/gtest already, building in a different directory.
IF(EXISTS ${GMOCK_SOURCE_DIR} OR EXISTS ${LOCAL_GMOCK_ZIP})
  MESSAGE(STATUS "GMOCK_SOURCE_DIR:${GMOCK_SOURCE_DIR}")
  SET(GMOCK_DOWNLOADED 1 CACHE INTERNAL "")
  SET(GMOCK_FOUND 1 CACHE INTERNAL "")
# If source dir does not exist, reset dependent variables (might be set from before).
ELSE()
  SET(LOCAL_GMOCK_ZIP 0 CACHE INTERNAL "")
  SET(GMOCK_DOWNLOADED 0 CACHE INTERNAL "")
  SET(GMOCK_FOUND 0 CACHE INTERNAL "")
  SET(GMOCK_INCLUDE_DIRS 0 CACHE INTERNAL "")
ENDIF()


IF(LOCAL_GMOCK_ZIP AND NOT EXISTS ${GMOCK_SOURCE_DIR})
  # Unpack tarball
  EXECUTE_PROCESS(
    COMMAND ${CMAKE_COMMAND} -E tar xfz  "${LOCAL_GMOCK_ZIP}"
    WORKING_DIRECTORY "${DOWNLOAD_ROOT}"
    RESULT_VARIABLE tar_result
  )
  IF (tar_result MATCHES 0)
    SET(GMOCK_FOUND 1 CACHE INTERNAL "")
  ENDIF()
ENDIF()


OPTION(ENABLE_DOWNLOADS
  "Download and build 3rd party source code components, e.g. google mock"
  OFF)

# While experimenting, use local URL rather than google.
SET(GMOCK_TARBALL "${GMOCK_PACKAGE_NAME}.zip")
SET(GMOCK_DOWNLOAD_URL 
  "http://googlemock.googlecode.com/files/${GMOCK_TARBALL}"
  )
  
MACRO(HTTP_PROXY_HINT)
  MESSAGE(STATUS
    "If you are inside a firewall, you may need to use an http proxy: "
    "export http_proxy=http://example.com:80")
ENDMACRO()


IF(NOT GMOCK_FOUND)
  IF(NOT ENABLE_DOWNLOADS)
    # Give one-time warning
    IF(NOT ONETIME_GTEST_WARNING)
      MESSAGE(STATUS 
        "Googlemock was not found. gtest-based unit tests will be disabled. "
        "You can run cmake . -DENABLE_DOWNLOADS=1 to automatically download "
        "and build required components from source.")
      HTTP_PROXY_HINT()
      SET(ONETIME_GTEST_WARNING 1 CACHE INTERNAL "")
    ENDIF()
    RETURN()
  ENDIF()
  
  # Download gmock source
  IF(NOT EXISTS ${GMOCK_SOURCE_DIR})
    IF(NOT EXISTS ${DOWNLOAD_ROOT}/${GMOCK_TARBALL})
      # Download the tarball
      # Use CMake builtin download capabilities
      FILE(DOWNLOAD ${GMOCK_DOWNLOAD_URL}
        ${DOWNLOAD_ROOT}/${GMOCK_TARBALL} TIMEOUT 30 STATUS ERR)
      IF(ERR EQUAL 0)
        SET(DOWNLOAD_SUCCEEDED 1)
      ELSE()
        MESSAGE(STATUS "Download failed, error: ${ERR}")
        # A failed DOWNLOAD leaves an empty file, remove it
        FILE(REMOVE ${DOWNLOAD_ROOT}/${GMOCK_TARBALL})
      ENDIF()

      IF (DOWNLOAD_SUCCEEDED)
        MESSAGE(STATUS 
          "Successfully downloaded ${GMOCK_DOWNLOAD_URL} to ${DOWNLOAD_ROOT}")
      ELSE()
        MESSAGE(STATUS 
          "To enable google test, please download ${GMOCK_DOWNLOAD_URL} "
          "to the directory ${DOWNLOAD_ROOT}")
        HTTP_PROXY_HINT()
        RETURN()
      ENDIF()
    ENDIF()
    
    # Unpack tarball
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} -E tar xfz  "${DOWNLOAD_ROOT}/${GMOCK_TARBALL}"
        WORKING_DIRECTORY "${DOWNLOAD_ROOT}"
      )
  ENDIF()
  IF(EXISTS ${GMOCK_SOURCE_DIR})
    SET(GMOCK_DOWNLOADED 1 CACHE INTERNAL "")
    SET(GMOCK_FOUND 1 CACHE INTERNAL "")
  ENDIF()
ENDIF()


IF(NOT GMOCK_FOUND)
  RETURN()
ENDIF()

SET(GMOCK_INCLUDE_DIRS
  ${GMOCK_SOURCE_DIR}
  ${GMOCK_SOURCE_DIR}/include
  ${GTEST_SOURCE_DIR}
  ${GTEST_SOURCE_DIR}/include
  CACHE INTERNAL "")

# Build gmock/gtest libraries.
INCLUDE_DIRECTORIES(
  ${GMOCK_SOURCE_DIR}
  ${GMOCK_SOURCE_DIR}/include
  ${GTEST_SOURCE_DIR}
  ${GTEST_SOURCE_DIR}/include
)

# Turn off some warning flags when compiling GUnit
IF(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-undef")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-field-initializers")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-format-attribute")

  MY_CHECK_CXX_COMPILER_FLAG("-Wunused-local-typedefs" HAVE_UNUSED_TYPEDEFS)
  IF(HAVE_UNUSED_TYPEDEFS)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs")
  ENDIF()
ENDIF()

MY_CHECK_CXX_COMPILER_FLAG("-fno-builtin-memcmp" HAVE_NO_BUILTIN_MEMCMP)

ADD_LIBRARY(gmock STATIC ${GMOCK_SOURCE_DIR}/src/gmock-all.cc)
ADD_LIBRARY(gtest STATIC ${GTEST_SOURCE_DIR}/src/gtest-all.cc)
SET(GTEST_LIBRARIES gmock gtest)

IF (WITH_SSL STREQUAL "bundled")
  ADD_SUBDIRECTORY(yassl)
ENDIF()

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/libbinlogevents/include
  ${ZLIB_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/regex
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/sql/auth
  ${CMAKE_SOURCE_DIR}/storage/example
)

# main-wrapper libraries (with tap-compatible option).
ADD_LIBRARY(gunit_small STATIC
  fake_costmodel.cc
  gunit_test_main.cc
  skip_trailing.cc
  strnxfrm.cc
  tap_event_listener.cc
  thread_utils.cc
  fake_table.cc
)
ADD_LIBRARY(gunit_large STATIC
  gunit_test_main_server.cc
  tap_event_listener.cc
  test_utils.cc
  thread_utils.cc
)
ADD_DEPENDENCIES(gunit_small GenError)
ADD_DEPENDENCIES(gunit_large GenError)
TARGET_LINK_LIBRARIES(gunit_small
  mysys mysys_ssl mytap dbug strings ${GTEST_LIBRARIES})
TARGET_LINK_LIBRARIES(gunit_large
  mysys mysys_ssl mytap dbug strings ${GTEST_LIBRARIES})
MESSAGE(STATUS "GTEST_LIBRARIES:${GTEST_LIBRARIES}")

IF(WIN32)
  ## Create an object library of nt_servc.cc, so we don't have to
  ## compile it again for each target
  ADD_LIBRARY(my_nt_servc OBJECT ../../sql/nt_servc.cc)
ENDIF()

# Add some defines.
ADD_DEFINITIONS(-DMYSQL_SERVER -DHAVE_REPLICATION -DEXTRA_CODE_FOR_UNIT_TESTING)
ADD_DEFINITIONS(-DERRMSG_DIR="${PROJECT_BINARY_DIR}/sql/share")
ADD_DEFINITIONS(-DDATA_DIR="${CMAKE_CURRENT_BINARY_DIR}")

INCLUDE(${MYSQL_CMAKE_SCRIPT_DIR}/compile_flags.cmake)

# Add tests (link them with gunit/gmock libraries) 
SET(TESTS
  alignment
  bounded_queue
#  bounds_checked_array
  bitmap
  byteorder
  calloc
  cost_estimate
  dbug
  decimal
  dynarray
  filesort_buffer
  filesort_compare
  float_compare
  inplace_vector
  key
  like_range
  mdl
  my_bitmap
  my_error
  my_fileutils
  my_murmur3
  my_qsort_vs_stdsort
  my_regex
  my_thread
  mysys_base64
  mysys_lf
  mysys_my_atomic
  mysys_my_loadpath
  mysys_my_malloc
  mysys_my_pwrite
  mysys_my_rdtsc
  mysys_my_symlink
  mysys_my_vsnprintf
  mysys_my_write
  nullable
  opt_recperkey
  partitioned_rwlock
  prealloced_array
  priority_queue
  record_buffer
  sql_list
  sql_plist
  sql_string
  stdcxx
  stl_alloc
  strings_skip_trailing
  strings_strnxfrm
  strtoll
  thread_utils
  my_timer
  timespec
  my_alloc
  pump_object_filter
  val_int_compare
  )
 
IF(WIN32)
  LIST(APPEND TESTS win_tests)
ENDIF()

# Add tests (link them with gunit/gmock libraries and the server libraries) 
SET(SERVER_TESTS
  copy_info
  create_field
  dd_properties
  dd_sdi
  dd_schema
  dd_table
  dd_cache
  debug_sync
  explain_filename
  field
  get_diagnostics
  gis_algos
  handler
  histograms
  initialize_password
  insert_delayed
  into_syntax
  item
  item_filter
  item_func_case
  item_func_now_local
  item_like
  item_timefunc
  join_syntax
  join_tab_sort
  json_binary
  json_dom
  json_path
  locking_service
  log_throttle
  make_sortkey
  mdl_sync
  my_decimal
  opt_costconstants
  opt_costmodel
  opt_guessrecperkey
  opt_range
  opt_ref
  opt_trace
  security_context
  segfault
  select_lex_visitor
  sql_table
  strings_utf8
  subquery_syntax
  table_cache
  table_factor_syntax
  tc_log_mmap
  thd_manager
  union_syntax
  unique
  wild_case_compare
)

SET(SQL_GUNIT_LIB_SOURCE 
  ${CMAKE_SOURCE_DIR}/sql/filesort_utils.cc 
  ${CMAKE_SOURCE_DIR}/sql/mdl.cc 
  ${CMAKE_SOURCE_DIR}/sql/sql_list.cc 
  ${CMAKE_SOURCE_DIR}/sql-common/sql_string.cc
  ${CMAKE_SOURCE_DIR}/sql/thr_malloc.cc
  )

IF(WIN32)
  LIST(APPEND SQL_GUNIT_LIB_SOURCE ${CMAKE_SOURCE_DIR}/sql/named_pipe.cc)
ENDIF()
ADD_CONVENIENCE_LIBRARY(sqlgunitlib ${SQL_GUNIT_LIB_SOURCE})
ADD_DEPENDENCIES(sqlgunitlib GenError)

## Merging tests into fewer executables saves *a lot* of
## link time and disk space ...
OPTION(MERGE_UNITTESTS "Merge tests into one executable" ON)
IF (MERGE_UNITTESTS)
  SET(MERGE_LARGE_TESTS ${CMAKE_CURRENT_BINARY_DIR}/merge_large_tests.cc)
  SET(MERGE_SMALL_TESTS ${CMAKE_CURRENT_BINARY_DIR}/merge_small_tests.cc)
  SET_SOURCE_FILES_PROPERTIES(MERGE_SMALL_TESTS MERGE_LARGE_TESTS
    PROPERTIES GENERATED 1)

  ## BOOST_HEAP_ASSERT generates if () ; else ;
  IF(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    ADD_COMPILE_FLAGS(
      ${MERGE_SMALL_TESTS}
      COMPILE_FLAGS "-Wno-empty-body"
    )
  ENDIF()
  ADD_COMPILE_FLAGS(
    ${MERGE_SMALL_TESTS}
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )
  ADD_COMPILE_FLAGS(
    ${MERGE_LARGE_TESTS}
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )
  ## Merging unit tests on Windows gives so many sections in the object file
  ## that /bigobj is required
  IF(WIN32)
    ADD_COMPILE_FLAGS(${MERGE_SMALL_TESTS} COMPILE_FLAGS "/bigobj")
    ADD_COMPILE_FLAGS(${MERGE_LARGE_TESTS} COMPILE_FLAGS "/bigobj")
  ENDIF()
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

  FILE(WRITE ${MERGE_LARGE_TESTS} "// Merging large unit tests\n")
  FILE(WRITE ${MERGE_SMALL_TESTS} "// Merging small unit tests\n")
  FOREACH(test ${TESTS})
    FILE(APPEND ${MERGE_SMALL_TESTS} "#include \"${test}-t.cc\"\n")
  ENDFOREACH()
  FOREACH(test ${SERVER_TESTS})
    FILE(APPEND ${MERGE_LARGE_TESTS} "#include \"${test}-t.cc\"\n")
  ENDFOREACH()

  ADD_EXECUTABLE(merge_small_tests-t ${MERGE_SMALL_TESTS})
  SET(SRC_FILES ${MERGE_LARGE_TESTS})
  IF(WIN32)
    LIST(APPEND SRC_FILES $<TARGET_OBJECTS:my_nt_servc>)
  ENDIF()
  LIST(APPEND SRC_FILES ../../storage/example/ha_example.cc)
  ADD_COMPILE_FLAGS(
    ../../storage/example/ha_example.cc
    COMPILE_FLAGS "-DDISABLE_DTRACE"
  )
  ADD_EXECUTABLE(merge_large_tests-t ${SRC_FILES})

  TARGET_LINK_LIBRARIES(merge_small_tests-t
    gunit_small sqlgunitlib strings dbug regex)
  TARGET_LINK_LIBRARIES(merge_large_tests-t sql binlog rpl master slave sql)
  TARGET_LINK_LIBRARIES(merge_large_tests-t
    gunit_large strings dbug regex mysys)
  TARGET_LINK_LIBRARIES(merge_large_tests-t sql binlog rpl master slave sql)

  IF(WITH_PERFSCHEMA_STORAGE_ENGINE)
    TARGET_LINK_LIBRARIES(merge_large_tests-t perfschema)
    TARGET_LINK_LIBRARIES(merge_small_tests-t perfschema pfs_server_stubs)
  ENDIF()

  ADD_TEST(merge_large_tests merge_large_tests-t)
  ADD_TEST(merge_small_tests merge_small_tests-t)

ENDIF(MERGE_UNITTESTS)

  FOREACH(test ${TESTS})
    ADD_EXECUTABLE(${test}-t ${test}-t.cc)
    TARGET_LINK_LIBRARIES(${test}-t gunit_small sqlgunitlib strings dbug regex)
    IF(WITH_PERFSCHEMA_STORAGE_ENGINE)
      TARGET_LINK_LIBRARIES(${test}-t perfschema pfs_server_stubs)
    ENDIF()
    IF(MERGE_UNITTESTS)
      SET_PROPERTY(TARGET ${test}-t PROPERTY EXCLUDE_FROM_ALL TRUE)
      IF(WIN32)
        SET_PROPERTY(TARGET ${test}-t PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
      ENDIF()
    ELSE()
      ADD_TEST(${test} ${test}-t)
    ENDIF()
  ENDFOREACH()
  
  # See comments about __builtin_memcmp in the source file.
  IF(HAVE_NO_BUILTIN_MEMCMP)
    ADD_COMPILE_FLAGS(
      filesort_compare-t.cc
      COMPILE_FLAGS "-fno-builtin-memcmp"
  )
  ENDIF()

  ## BOOST_HEAP_ASSERT generates if () ; else ;
  IF(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    ADD_COMPILE_FLAGS(
      bounded_queue-t.cc
      COMPILE_FLAGS "-Wno-empty-body"
  )
  ENDIF()
  ADD_COMPILE_FLAGS(
    bounded_queue-t.cc
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )
  ADD_COMPILE_FLAGS(
    pump_object_filter-t.cc
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )

  ADD_COMPILE_FLAGS(
    dd_sdi-t.cc
    COMPILE_FLAGS -I${BOOST_PATCHES_DIR} -I${BOOST_INCLUDE_DIR}
  )

  FOREACH(test ${SERVER_TESTS})
    SET(SRC_FILES ${test}-t.cc)
    IF(WIN32)
      LIST(APPEND SRC_FILES $<TARGET_OBJECTS:my_nt_servc>)
    ENDIF()
    IF(test MATCHES "table_cache")
      LIST(APPEND SRC_FILES ../../storage/example/ha_example.cc)
      ADD_COMPILE_FLAGS(
        ../../storage/example/ha_example.cc
        COMPILE_FLAGS "-DDISABLE_DTRACE"
      )
    ENDIF()
    ADD_EXECUTABLE(${test}-t ${SRC_FILES})
    TARGET_LINK_LIBRARIES(${test}-t sql binlog rpl master slave sql)
    TARGET_LINK_LIBRARIES(${test}-t gunit_large strings dbug regex mysys)
    TARGET_LINK_LIBRARIES(${test}-t sql binlog rpl master slave sql)

    IF(WITH_PERFSCHEMA_STORAGE_ENGINE)
      TARGET_LINK_LIBRARIES(${test}-t perfschema)
    ENDIF()

    IF(MERGE_UNITTESTS)
      SET_PROPERTY(TARGET ${test}-t PROPERTY EXCLUDE_FROM_ALL TRUE)
      IF(WIN32)
        SET_PROPERTY(TARGET ${test}-t PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
      ENDIF()
    ELSE()
      ADD_TEST(${test} ${test}-t)
    ENDIF()
  ENDFOREACH()

## Most executables depend on libeay32.dll (through mysys_ssl).
COPY_OPENSSL_DLLS(copy_openssl_gunit)

ADD_SUBDIRECTORY(innodb)
ADD_SUBDIRECTORY(keyring)

## Add tests for all components.
FILE(GLOB dirs_components ${CMAKE_CURRENT_SOURCE_DIR}/components/*)
FOREACH(dir ${dirs_components})
  IF (EXISTS ${dir}/CMakeLists.txt)
    ADD_SUBDIRECTORY(${dir})
  ENDIF()
ENDFOREACH()

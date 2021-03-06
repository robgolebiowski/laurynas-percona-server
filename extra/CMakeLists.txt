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

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include 
${ZLIB_INCLUDE_DIR}
${LZ4_INCLUDE_DIR}
# Following is for perror, in case NDB is compiled in.
${CMAKE_SOURCE_DIR}/storage/ndb/include
${CMAKE_BINARY_DIR}/storage/ndb/include
${CMAKE_SOURCE_DIR}/storage/ndb/include/ndbapi
${CMAKE_SOURCE_DIR}/storage/ndb/include/mgmapi)

INCLUDE(${MYSQL_CMAKE_SCRIPT_DIR}/compile_flags.cmake)

# Default install component for the files is Server here
SET(MYSQL_INSTALL_COMPONENT Server)

IF(NOT CMAKE_CROSSCOMPILING)
 ADD_EXECUTABLE(comp_err comp_err.cc)
 TARGET_LINK_LIBRARIES(comp_err mysys mysys_ssl)
ENDIF()

ADD_EXECUTABLE(comp_client_err
  comp_client_err.cc ${CMAKE_SOURCE_DIR}/libmysql/errmsg.c)
TARGET_LINK_LIBRARIES(comp_client_err mysys mysys_ssl)

ADD_CUSTOM_COMMAND(OUTPUT ${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h
  COMMAND comp_client_err
  --in_file=${PROJECT_SOURCE_DIR}/include/errmsg.h
  --out_file=${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/comp_client_err.cc)

ADD_CUSTOM_TARGET(GenClientError
                  ALL
                  DEPENDS ${PROJECT_BINARY_DIR}/include/mysqlclient_ername.h)


IF (BUILD_BUNDLED_LZ4)
  ADD_CONVENIENCE_LIBRARY(lz4_lib
    lz4/lz4.c
    lz4/lz4frame.c
    lz4/lz4hc.c
    lz4/xxhash.c
  )
ENDIF()

ADD_CUSTOM_COMMAND(OUTPUT ${PROJECT_BINARY_DIR}/include/mysqld_error.h 
                   ${PROJECT_BINARY_DIR}/sql/share/english/errmsg.sys
                   COMMAND comp_err
                    --charset=${PROJECT_SOURCE_DIR}/sql/share/charsets 
                    --out-dir=${PROJECT_BINARY_DIR}/sql/share/
                    --header_file=${PROJECT_BINARY_DIR}/include/mysqld_error.h 
                    --name_file=${PROJECT_BINARY_DIR}/include/mysqld_ername.h
                    --in_file=${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt
                   DEPENDS ${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt
                   ${CMAKE_CURRENT_SOURCE_DIR}/comp_err.cc)

ADD_CUSTOM_TARGET(GenError
                  ALL
                  DEPENDS 
                  ${PROJECT_BINARY_DIR}/include/mysqld_error.h
                  ${PROJECT_BINARY_DIR}/sql/share/english/errmsg.sys 
                  ${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt)

# Set InnoDB mutex type
ADD_DEFINITIONS(-DMUTEX_EVENT)

## Most executables depend on libeay32.dll (through mysys_ssl).
## For 'comp_err' we need it to be available during build.
COPY_OPENSSL_DLLS(copy_openssl_extra)
IF (WIN32 AND WITH_SSL_PATH AND HAVE_CRYPTO_DLL)
  ADD_DEPENDENCIES(GenError copy_openssl_extra)
ENDIF()


MYSQL_ADD_EXECUTABLE(my_print_defaults my_print_defaults.c)
TARGET_LINK_LIBRARIES(my_print_defaults  mysys mysys_ssl)
SET_TARGET_PROPERTIES(my_print_defaults PROPERTIES LINKER_LANGUAGE CXX)

MYSQL_ADD_EXECUTABLE(perror perror.c)
ADD_DEPENDENCIES(perror GenError)
TARGET_LINK_LIBRARIES(perror mysys mysys_ssl)
SET_TARGET_PROPERTIES(perror PROPERTIES LINKER_LANGUAGE CXX)

MYSQL_ADD_EXECUTABLE(resolveip resolveip.c)
TARGET_LINK_LIBRARIES(resolveip mysys mysys_ssl)
SET_TARGET_PROPERTIES(resolveip PROPERTIES LINKER_LANGUAGE CXX)
IF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  INCLUDE(CheckFunctionExists)
  INCLUDE(CheckLibraryExists)
  MY_SEARCH_LIBS(inet_aton  "nsl;socket;resolv" SOLARIS_NSL)
  TARGET_LINK_LIBRARIES(resolveip ${SOLARIS_NSL})
ENDIF()


MYSQL_ADD_EXECUTABLE(replace replace.c)
TARGET_LINK_LIBRARIES(replace mysys)

IF (BUILD_BUNDLED_LZ4)
  MYSQL_ADD_EXECUTABLE(lz4_decompress lz4_decompress.cc)
  TARGET_LINK_LIBRARIES(lz4_decompress ${LZ4_LIBRARY})
ENDIF()

IF (BUILD_BUNDLED_ZLIB)
  MYSQL_ADD_EXECUTABLE(zlib_decompress zlib_decompress.cc)
  TARGET_LINK_LIBRARIES(zlib_decompress ${ZLIB_LIBRARY})
ENDIF()

IF(WITH_INNOBASE_STORAGE_ENGINE)

  IF(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
    ADD_DEFINITIONS(-DHAVE_LZ4=1)
    INCLUDE_DIRECTORIES(${LZ4_INCLUDE_DIR})
  ENDIF()

  IF(NOT MSVC)
    IF(NOT CMAKE_CROSSCOMPILING)
    CHECK_C_SOURCE_RUNS(
    "#include<stdint.h>
    int main()
    {
      unsigned char       a = 0;
      unsigned char       b = 0;
      unsigned char       c = 1;

      __atomic_exchange(&a, &b,  &c, __ATOMIC_RELEASE);
      __atomic_compare_exchange(&a, &b, &c, 0,
                                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
      return(0);
    }"
    HAVE_IB_GCC_ATOMIC_COMPARE_EXCHANGE
    )
    ENDIF()

    IF(HAVE_IB_GCC_ATOMIC_COMPARE_EXCHANGE)
      ADD_DEFINITIONS(-DHAVE_IB_GCC_ATOMIC_COMPARE_EXCHANGE=1)
    ENDIF()
  ENDIF(NOT MSVC)

  # Add path to the InnoDB headers
  INCLUDE_DIRECTORIES(
    ${CMAKE_SOURCE_DIR}/storage/innobase/
    ${CMAKE_SOURCE_DIR}/storage/innobase/include
    ${CMAKE_SOURCE_DIR}/sql
  )

  # We use the InnoDB code directly in case the code changes.
  MYSQL_ADD_EXECUTABLE(innochecksum innochecksum.cc)
  SET_TARGET_PROPERTIES(innochecksum
    PROPERTIES COMPILE_DEFINITIONS "UNIV_LIBRARY")
  TARGET_LINK_LIBRARIES(innochecksum
    mysys_ssl innodb_zipdecompress ${LZ4_LIBRARY})
  ADD_DEPENDENCIES(innochecksum GenError)

 MYSQL_ADD_EXECUTABLE(ibd2sdi ibd2sdi.cc)
  SET_TARGET_PROPERTIES(ibd2sdi
    PROPERTIES COMPILE_DEFINITIONS "UNIV_LIBRARY")
  TARGET_LINK_LIBRARIES(ibd2sdi
    mysys_ssl innodb_zipdecompress ${LZ4_LIBRARY})
  ADD_DEPENDENCIES(ibd2sdi GenError)

ENDIF()

IF(UNIX)
  MYSQL_ADD_EXECUTABLE(resolve_stack_dump resolve_stack_dump.cc)
  TARGET_LINK_LIBRARIES(resolve_stack_dump mysys mysys_ssl)
ENDIF()

# In published release builds on Solaris, we need to bundle gcc source.
# PB2 will take care of putting it in extra/ when needed
IF(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gcc-4.8.1.tar.bz2)
  INSTALL(FILES gcc-4.8.1.tar.bz2 
          DESTINATION ${INSTALL_MYSQLSHAREDIR} COMPONENT Development)
ENDIF()

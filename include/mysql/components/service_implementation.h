/* Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1307  USA */

#ifndef SERVICE_IMPLEMENTATION_H
#define SERVICE_IMPLEMENTATION_H

#include "service.h"

/**
  @file include/mysql/components/service_implementation.h
  Specifies macros to define Service Implementations.
*/

/**
  Declares a Service Implementation. It builds standard implementation
  info structure. Only a series of pointers to methods the Service
  Implementation implements as respective Service methods are expected to be
  used after this macro and before the END_SERVICE_IMPLEMENTATION counterpart.

  @param component Name of the Component to create implementation in.
  @param service Name of the Service to create implementation for.
*/
#define BEGIN_SERVICE_IMPLEMENTATION(component, service) \
  SERVICE_TYPE(service) imp_ ## component ## _ ## service = {

/**
  A macro to end the last declaration of a Service Implementation.
*/
#define END_SERVICE_IMPLEMENTATION() \
  } ;

/**
  A macro to ensure method implementation has required properties, that is it
  does not throw exceptions and is static. This macro should be used with
  exactly the same arguments as DECLARE_METHOD.

  @param retval Type of return value. It cannot contain comma characters, but
    as only simple structures are possible, this shouldn't be a problem.
  @param name Method name.
  @param args a list of arguments in parenthesis.
*/
#define DEFINE_METHOD(retval, name, args) \
  retval name args noexcept

/**
  A short macro to define method that returns bool, which is the most common
  case.

  @param name Method name.
  @param args a list of arguments in parenthesis.
*/
#define DEFINE_BOOL_METHOD(name, args) \
  DEFINE_METHOD(mysql_service_status_t, name, args)

#endif /* SERVICE_IMPLEMENTATION_H */

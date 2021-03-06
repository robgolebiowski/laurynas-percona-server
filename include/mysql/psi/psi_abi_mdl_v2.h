/* Copyright (c) 2008, 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/**
  @file include/mysql/psi/psi_abi_mdl_v2.h
  ABI check for mysql/psi/psi_mdl.h, when using PSI_MDL_VERSION_2.
  This file is only used to automate detection of changes between versions.
  Do not include this file, include mysql/psi/psi_mdl.h instead.
*/
#define USE_PSI_MDL_2
#define HAVE_PSI_INTERFACE
#define MY_GLOBAL_INCLUDED
#include "mysql/psi/psi_mdl.h"


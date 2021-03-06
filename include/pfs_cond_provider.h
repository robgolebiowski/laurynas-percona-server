/* Copyright (c) 2012, 2016, Oracle and/or its affiliates. All rights reserved.

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

#ifndef PFS_COND_PROVIDER_H
#define PFS_COND_PROVIDER_H

/**
  @file include/pfs_cond_provider.h
  Performance schema instrumentation (declarations).
*/

#ifdef HAVE_PSI_COND_INTERFACE
#ifdef MYSQL_SERVER
#ifndef EMBEDDED_LIBRARY
#ifndef MYSQL_DYNAMIC_PLUGIN

#include "mysql/psi/psi_cond.h"

#define PSI_COND_CALL(M) pfs_ ## M ## _v1

C_MODE_START

void pfs_register_cond_v1(const char *category,
                          PSI_cond_info_v1 *info,
                          int count);

PSI_cond*
pfs_init_cond_v1(PSI_cond_key key, const void *identity);

void pfs_destroy_cond_v1(PSI_cond* cond);

PSI_cond_locker*
pfs_start_cond_wait_v1(PSI_cond_locker_state *state,
                       PSI_cond *cond, PSI_mutex *mutex,
                       PSI_cond_operation op,
                       const char *src_file, uint src_line);

void pfs_signal_cond_v1(PSI_cond* cond);

void pfs_broadcast_cond_v1(PSI_cond* cond);

void pfs_end_cond_wait_v1(PSI_cond_locker* locker, int rc);

C_MODE_END

#endif /* EMBEDDED_LIBRARY */
#endif /* MYSQL_DYNAMIC_PLUGIN */
#endif /* MYSQL_SERVER */
#endif /* HAVE_PSI_THREAD_INTERFACE */

#endif


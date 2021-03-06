/* Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql_thd_internal_api.h"

#include "binlog.h"               // mysql_bin_log
#include "current_thd.h"          // current_thd
#include "mysqld_thd_manager.h"   // Global_THD_manager
#include "rpl_filter.h"           // binlog_filter
#include "sql_class.h"            // THD
#include "sql_parse.h"            // sqlcom_can_generate_row_events
#include "mysqld.h"

int thd_init(THD *thd, char *stack_start, bool bound, PSI_thread_key psi_key)
{
  DBUG_ENTER("thd_init");
  // TODO: Purge threads currently terminate too late for them to be added.
  // Note that P_S interprets all threads with thread_id != 0 as
  // foreground threads. And THDs need thread_id != 0 to be added
  // to the global THD list.
  if (thd->system_thread != SYSTEM_THREAD_BACKGROUND)
  {
    thd->set_new_thread_id();
    Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
    thd_manager->add_thd(thd);
  }
#ifdef HAVE_PSI_INTERFACE
  PSI_thread *psi;
  psi= PSI_THREAD_CALL(new_thread)(psi_key, thd, thd->thread_id());
  if (bound)
  {
    PSI_THREAD_CALL(set_thread_os_id)(psi);
  }
  thd->set_psi(psi);
#endif

  if (!thd->system_thread)
  {
    DBUG_PRINT("info", ("init new connection. thd: %p fd: %d",
                        thd, mysql_socket_getfd(
            thd->get_protocol_classic()->get_vio()->mysql_socket)));
  }
  thd_set_thread_stack(thd, stack_start);

  int retval= thd->store_globals();
  DBUG_RETURN(retval);
}


THD *create_thd(bool enable_plugins, bool background_thread, bool bound, PSI_thread_key psi_key)
{
  THD *thd= new THD(enable_plugins);
  if (background_thread)
    thd->system_thread= SYSTEM_THREAD_BACKGROUND;
  (void)thd_init(thd, reinterpret_cast<char*>(&thd), bound, psi_key);
  return thd;
}


void destroy_thd(THD *thd)
{
  thd->release_resources();
  // TODO: Purge threads currently terminate too late for them to be added.
  if (thd->system_thread != SYSTEM_THREAD_BACKGROUND)
  {
    Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
    thd_manager->remove_thd(thd);
  }
  delete thd;
}


void thd_set_thread_stack(THD *thd, const char *stack_start)
{
  thd->thread_stack= stack_start;
}


extern "C"
void thd_enter_cond(void *opaque_thd, mysql_cond_t *cond, mysql_mutex_t *mutex,
                    const PSI_stage_info *stage, PSI_stage_info *old_stage,
                    const char *src_function, const char *src_file,
                    int src_line)
{
  THD *thd= static_cast<THD*>(opaque_thd);
  if (!thd)
    thd= current_thd;

  return thd->enter_cond(cond, mutex, stage, old_stage,
                         src_function, src_file, src_line);
}

extern "C"
void thd_exit_cond(void *opaque_thd, const PSI_stage_info *stage,
                   const char *src_function, const char *src_file,
                   int src_line)
{
  THD *thd= static_cast<THD*>(opaque_thd);
  if (!thd)
    thd= current_thd;

  thd->exit_cond(stage, src_function, src_file, src_line);
}


void thd_increment_bytes_sent(size_t length)
{
  THD *thd= current_thd;
  if (likely(thd != NULL))
  { /* current_thd==NULL when close_connection() calls net_send_error() */
    thd->status_var.bytes_sent+= length;
  }
}


void thd_increment_bytes_received(size_t length)
{
  THD *thd= current_thd;
  if (likely(thd != NULL))
    thd->status_var.bytes_received+= length;
}


partition_info *thd_get_work_part_info(THD *thd)
{
  return thd->work_part_info;
}


enum_tx_isolation thd_get_trx_isolation(const THD *thd)
{
  return thd->tx_isolation;
}


const struct charset_info_st *thd_charset(THD *thd)
{
  return(thd->charset());
}


LEX_CSTRING thd_query_unsafe(THD *thd)
{
  DBUG_ASSERT(current_thd == thd);
  return thd->query();
}


size_t thd_query_safe(THD *thd, char *buf, size_t buflen)
{
  mysql_mutex_lock(&thd->LOCK_thd_query);
  LEX_CSTRING query_string= thd->query();
  size_t len= MY_MIN(buflen - 1, query_string.length);
  if (len > 0)
    strncpy(buf, query_string.str, len);
  buf[len]= '\0';
  mysql_mutex_unlock(&thd->LOCK_thd_query);
  return len;
}


int thd_slave_thread(const THD *thd)
{
  return(thd->slave_thread);
}


int thd_non_transactional_update(const THD *thd)
{
  return thd->get_transaction()->has_modified_non_trans_table(
    Transaction_ctx::SESSION);
}


int thd_binlog_format(const THD *thd)
{
  if (mysql_bin_log.is_open() && (thd->variables.option_bits & OPTION_BIN_LOG))
    return (int) thd->variables.binlog_format;
  else
    return BINLOG_FORMAT_UNSPEC;
}


bool thd_binlog_filter_ok(const THD *thd)
{
  return binlog_filter->db_ok(thd->db().str);
}


bool thd_sqlcom_can_generate_row_events(const THD *thd)
{
  return sqlcom_can_generate_row_events(thd->lex->sql_command);
}


enum durability_properties thd_get_durability_property(const THD *thd)
{
  enum durability_properties ret= HA_REGULAR_DURABILITY;

  if (thd != NULL)
    ret= thd->durability_property;

  return ret;
}


void thd_get_autoinc(const THD *thd, ulong* off, ulong* inc)
{
  *off = thd->variables.auto_increment_offset;
  *inc = thd->variables.auto_increment_increment;
}


bool thd_is_strict_mode(const THD *thd)
{
  return thd->is_strict_mode();
}

bool is_mysql_datadir_path(const char *path)
{
  if (path == NULL || strlen(path) >= FN_REFLEN)
    return false;

  char mysql_data_dir[FN_REFLEN], path_dir[FN_REFLEN];
  convert_dirname(path_dir, path, NullS);
  convert_dirname(mysql_data_dir, mysql_unpacked_real_data_home, NullS);
  size_t mysql_data_home_len= dirname_length(mysql_data_dir);
  size_t path_len= dirname_length(path_dir);

  if (path_len < mysql_data_home_len)
    return true;

  if (!lower_case_file_system)
    return memcmp(mysql_data_dir, path_dir, mysql_data_home_len);

  return files_charset_info->coll->strnncoll(files_charset_info,
                                             reinterpret_cast<uchar*>(path_dir),
                                             path_len,
                                             reinterpret_cast<uchar*>(mysql_data_dir),
                                             mysql_data_home_len,
                                             TRUE);
}


int mysql_tmpfile_path(const char *path, const char *prefix)
{
  DBUG_ASSERT(path != NULL);
  DBUG_ASSERT((strlen(path) + strlen(prefix)) <= FN_REFLEN);

  char filename[FN_REFLEN];
  File fd = create_temp_file(filename, path, prefix,
#ifdef _WIN32
                             O_TRUNC | O_SEQUENTIAL |
#endif /* _WIN32 */
                             O_CREAT | O_EXCL | O_RDWR,
                             MYF(MY_WME));
  if (fd >= 0)
    unlink(filename);

  return fd;
}

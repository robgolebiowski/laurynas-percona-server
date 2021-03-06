/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

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

#include "sql_tablespace.h"

#include "derror.h"                             // ER_THD
#include "lock.h"                               // lock_tablespace_name
#include "sql_class.h"                          // THD
#include "sql_table.h"                          // write_bin_log
#include "table.h"                              // ident_name_check

#include "dd/dd_tablespace.h"                   // dd::create_tablespace


static bool update_tablespace_dictionary(
              THD *thd, st_alter_tablespace *ts_info, handlerton *hton);


Ident_name_check check_tablespace_name(const char *tablespace_name)
{
  size_t name_length= 0;                       ///< Length as number of bytes
  size_t name_length_symbols= 0;               ///< Length as number of symbols

  // Name must be != NULL and length must be > 0
  if (!tablespace_name || (name_length= strlen(tablespace_name)) == 0)
  {
    my_error(ER_WRONG_TABLESPACE_NAME, MYF(0), tablespace_name);
    return Ident_name_check::WRONG;
  }

  // If we do not have too many bytes, we must check the number of symbols,
  // provided the system character set may use more than one byte per symbol.
  if (name_length <= NAME_LEN && use_mb(system_charset_info))
  {
    const char *name= tablespace_name;   ///< The actual tablespace name
    const char *end= name + name_length; ///< Pointer to first byte after name

    // Loop over all symbols as long as we don't have too many already
    while (name != end && name_length_symbols <= NAME_CHAR_LEN)
    {
      int len= my_ismbchar(system_charset_info, name, end);
      if (len)
        name += len;
      else
        name++;

      name_length_symbols++;
    }
  }

  if (name_length_symbols > NAME_CHAR_LEN || name_length > NAME_LEN)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), tablespace_name);
    return Ident_name_check::TOO_LONG;
  }

  return Ident_name_check::OK;
}


/**
  Check whether this is a tablespace related command.

  The structure st_alter_tablespace is used for various purposes, not
  only tablespace related commands (e.g., it is used for CREATE LOGFILE).
  We need to find the purpose of the command in order to know whether we,
  e.g., should validate the tablespace name (which is NULL in the event
  of a CREATE LOGFILE command).

  @param ts_cmd_type  The type of command

  @retval true        This is a tablespace related command
  @retval false       This is a not a tablespace related command
 */

static bool is_tablespace_command(ts_command_type ts_cmd_type)
{
  switch (ts_cmd_type)
  {
  case CREATE_TABLESPACE:
  case ALTER_TABLESPACE:
  case DROP_TABLESPACE:
  case CHANGE_FILE_TABLESPACE:
  case ALTER_ACCESS_MODE_TABLESPACE:
    return true;

  default:
    return false;
  }
}


bool mysql_alter_tablespace(THD *thd, st_alter_tablespace *ts_info)
{
  int error= HA_ADMIN_NOT_IMPLEMENTED;

  DBUG_ENTER("mysql_alter_tablespace");

  DBUG_ASSERT(ts_info);
  handlerton *hton= ts_info->storage_engine;

  /*
    If the user hasn't defined an engine, this will fallback to using the
    default storage engine.
  */
  if (hton == NULL || hton->state != SHOW_OPTION_YES)
  {
    hton= ha_default_handlerton(thd);
    if (ts_info->storage_engine != 0)
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_WARN_USING_OTHER_HANDLER,
                          ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                          ha_resolve_storage_engine_name(hton),
                          ts_info->tablespace_name ? ts_info->tablespace_name
                                                : ts_info->logfile_group_name);
  }

  // Check if tablespace create or alter is disallowed by the stroage engine.
  if ((ts_info->ts_cmd_type == CREATE_TABLESPACE ||
       ts_info->ts_cmd_type == ALTER_TABLESPACE) &&
      ha_is_storage_engine_disabled(hton))
  {
    my_error(ER_DISABLED_STORAGE_ENGINE, MYF(0),
              ha_resolve_storage_engine_name(hton));
    DBUG_RETURN(true);
  }

  // If this is a tablespace related command, check the tablespace name
  // and acquire and MDL X lock on it. Also validate the data file path.
  if (is_tablespace_command(ts_info->ts_cmd_type) &&
      (check_tablespace_name(ts_info->tablespace_name) !=
       Ident_name_check::OK ||
       lock_tablespace_name(thd, ts_info->tablespace_name)))
    DBUG_RETURN(true);


  if (hton->alter_tablespace)
  {
    if ((error= hton->alter_tablespace(hton, thd, ts_info)))
    {
      const char* sql_syntax[] =
      {
        "this functionallity",
        "CREATE TABLESPACE",
        "ALTER TABLESPACE",
        "CREATE LOGFILE GROUP",
        "ALTER LOGFILE GROUP",
        "DROP TABLESPACE",
        "DROP LOGFILE GROUP",
        "CHANGE FILE TABLESPACE",
        "ALTER TABLESPACE ACCESS MODE"
      };
      int cmd_type;

      switch (error)
      {
      case 1:
        DBUG_RETURN(true);
      case HA_ADMIN_NOT_IMPLEMENTED:
        cmd_type = 1 + static_cast<uint>(ts_info->ts_cmd_type);
        my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), sql_syntax[cmd_type]);
        break;
      case HA_ERR_TABLESPACE_MISSING:
        my_error(ER_TABLESPACE_MISSING, MYF(0), ts_info->tablespace_name);
        break;
      case HA_ERR_TABLESPACE_IS_NOT_EMPTY:
        my_error(ER_TABLESPACE_IS_NOT_EMPTY, MYF(0), ts_info->tablespace_name);
        break;
      case HA_ERR_WRONG_FILE_NAME:
        my_error(ER_WRONG_FILE_NAME, MYF(0), ts_info->data_file_name);
        break;
      case HA_ADMIN_FAILED:
        my_error(ER_CANT_CREATE_FILE, MYF(0), ts_info->data_file_name);
        break;
      case HA_ERR_INNODB_READ_ONLY:
        my_error(ER_INNODB_READ_ONLY, MYF(0));
        break;
      case HA_ERR_RECORD_FILE_FULL:
        my_error(ER_RECORD_FILE_FULL, MYF(0), ts_info->tablespace_name);
        break;
      case HA_WRONG_CREATE_OPTION:
        my_error(ER_ILLEGAL_HA, MYF(0), ts_info->tablespace_name);
        break;
      case HA_ERR_TABLESPACE_EXISTS:
        my_error(ER_TABLESPACE_EXISTS, MYF(0), ts_info->tablespace_name);
        break;
      default:
        char errbuf[MYSQL_ERRMSG_SIZE];
        my_error(ER_GET_ERRNO, MYF(0), error,
                 my_strerror(errbuf, MYSQL_ERRMSG_SIZE, error));
      }
      DBUG_RETURN((error != 0));
    }

    // Update data dictionary tables
    if (update_tablespace_dictionary(thd, ts_info, hton))
    {
      /*
        In practice DD operations should not fail as respective
        SE tablespace operations are already successfull at this point.
      */
      DBUG_RETURN(true);
    }

  }
  else
  {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
             ha_resolve_storage_engine_name(hton),
             "TABLESPACE or LOGFILE GROUP");
    DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
  }
  error= write_bin_log(thd, false, thd->query().str, thd->query().length);
  DBUG_RETURN((error != 0));
}


/*
  Handle following operations,
    - Create tablespace.
    - Drop tablespace.
    - Add/Drop datafile.

  @param thd     - Thread executing the operation.
  @param ts_info - Tablespace metadata from the DDL.
  @param hton    - Handlerton in which tablespace reside.

  @return false - On success.
  @return true - On failure.
*/
static bool update_tablespace_dictionary(
              THD *thd, st_alter_tablespace *ts_info, handlerton *hton)
{
  DBUG_ENTER("dd_alter_tablespace");

  switch (ts_info->ts_cmd_type)
  {
  case CREATE_TABLESPACE:
    DBUG_RETURN(dd::create_tablespace(thd, ts_info, hton));

  case DROP_TABLESPACE:
    DBUG_RETURN(dd::drop_tablespace(thd, ts_info, hton));

  case ALTER_TABLESPACE:
    DBUG_RETURN(dd::alter_tablespace(thd, ts_info, hton));


  /*
    Below tablespace alter operations are handled by SE
    and data dictionary does not capture metadata related
    to these operations. OR the operation is not implemented
    by the SE. The server just returns success in these cases
    letting the SE to take actions.
  */

  /*
    Metadata related to LOGFILE GROUP are not stored in
    dictionary as of now.
  */
  case CREATE_LOGFILE_GROUP:
  case ALTER_LOGFILE_GROUP:
  case DROP_LOGFILE_GROUP:

  /*
    Change file operation is not implemented by any storage
    engine.
  */
  case CHANGE_FILE_TABLESPACE:

  /*
    Access mode of tablespace is set by SE operation and MySQL
    ignores it.
  */
  case ALTER_ACCESS_MODE_TABLESPACE:
  default:
    ;
  }

  DBUG_RETURN(false);
}


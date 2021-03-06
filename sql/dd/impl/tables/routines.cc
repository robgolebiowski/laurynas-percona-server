/* Copyright (c) 2016 Oracle and/or its affiliates. All rights reserved.

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

#include "dd/impl/tables/routines.h"

#include "dd/dd.h"                       // dd::create_object
#include "dd/impl/raw/object_keys.h"     // dd::Routine_name_key
#include "dd/impl/raw/raw_record.h"      // dd::Raw_record
#include "dd/impl/types/routine_impl.h"  // dd::Routine_impl
#include "dd/types/function.h"           // dd::Function
#include "dd/types/procedure.h"          // dd::Procedure


namespace dd {
namespace tables {

const Routines &Routines::instance()
{
  static Routines *s_instance= new Routines();
  return *s_instance;
}

Routines::Routines()
{
  m_target_def.table_name(table_name());
  m_target_def.dd_version(1);

  m_target_def.add_field(FIELD_ID,
                         "FIELD_ID",
                         "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT");
  m_target_def.add_field(FIELD_SCHEMA_ID,
                         "FIELD_SCHEMA_ID",
                         "schema_id BIGINT UNSIGNED NOT NULL");
  m_target_def.add_field(FIELD_NAME,
                         "FIELD_NAME",
                         "name VARCHAR(64) NOT NULL COLLATE utf8_general_ci");
  m_target_def.add_field(FIELD_TYPE,
                         "FIELD_TYPE",
                         "type ENUM('FUNCTION', 'PROCEDURE') NOT NULL");
  m_target_def.add_field(FIELD_RESULT_DATA_TYPE,
                         "FIELD_RESULT_DATA_TYPE",
                         "result_data_type ENUM(\n"
                         "    'MYSQL_TYPE_DECIMAL', 'MYSQL_TYPE_TINY',\n"
                         "    'MYSQL_TYPE_SHORT',  'MYSQL_TYPE_LONG',\n"
                         "    'MYSQL_TYPE_FLOAT',  'MYSQL_TYPE_DOUBLE',\n"
                         "    'MYSQL_TYPE_NULL', 'MYSQL_TYPE_TIMESTAMP',\n"
                         "    'MYSQL_TYPE_LONGLONG','MYSQL_TYPE_INT24',\n"
                         "    'MYSQL_TYPE_DATE',   'MYSQL_TYPE_TIME',\n"
                         "    'MYSQL_TYPE_DATETIME', 'MYSQL_TYPE_YEAR',\n"
                         "    'MYSQL_TYPE_NEWDATE', 'MYSQL_TYPE_VARCHAR',\n"
                         "    'MYSQL_TYPE_BIT', 'MYSQL_TYPE_TIMESTAMP2',\n"
                         "    'MYSQL_TYPE_DATETIME2', 'MYSQL_TYPE_TIME2',\n"
                         "    'MYSQL_TYPE_NEWDECIMAL', 'MYSQL_TYPE_ENUM',\n"
                         "    'MYSQL_TYPE_SET', 'MYSQL_TYPE_TINY_BLOB',\n"
                         "    'MYSQL_TYPE_MEDIUM_BLOB', "
                         "    'MYSQL_TYPE_LONG_BLOB', 'MYSQL_TYPE_BLOB',\n"
                         "    'MYSQL_TYPE_VAR_STRING',\n"
                         "    'MYSQL_TYPE_STRING', 'MYSQL_TYPE_GEOMETRY',\n"
                         "    'MYSQL_TYPE_JSON'\n"
                         "  ) DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_IS_ZEROFILL,
                         "FIELD_RESULT_IS_ZEROFILL",
                         "result_is_zerofill BOOL DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_IS_UNSIGNED,
                         "FIELD_RESULT_IS_UNSIGNED",
                         "result_is_unsigned BOOL DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_CHAR_LENGTH,
                         "FIELD_RESULT_CHAR_LENGTH",
                         "result_char_length INT UNSIGNED DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_NUMERIC_PRECISION,
                         "FIELD_RESULT_NUMERIC_PRECISION",
                         "result_numeric_precision INT UNSIGNED DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_NUMERIC_SCALE,
                         "FIELD_RESULT_NUMERIC_SCALE",
                         "result_numeric_scale INT UNSIGNED DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_DATETIME_PRECISION,
                         "FIELD_RESULT_DATETIME_PRECISION",
                         "result_datetime_precision INT UNSIGNED DEFAULT NULL");
  m_target_def.add_field(FIELD_RESULT_COLLATION_ID,
                         "FIELD_RESULT_COLLATION_ID",
                         "result_collation_id BIGINT UNSIGNED DEFAULT NULL");
  m_target_def.add_field(FIELD_DEFINITION,
                         "FIELD_DEFINITION",
                         "definition LONGBLOB");
  m_target_def.add_field(FIELD_DEFINITION_UTF8,
                         "FIELD_DEFINITION_UTF8",
                         "definition_utf8 LONGTEXT");
  m_target_def.add_field(FIELD_PARAMETER_STR,
                         "FIELD_PARAMETER_STR",
                         "parameter_str BLOB");
  m_target_def.add_field(FIELD_IS_DETERMINISTIC,
                         "FIELD_IS_DETERMINISTIC",
                         "is_deterministic BOOL NOT NULL");
  m_target_def.add_field(FIELD_SQL_DATA_ACCESS,
                         "FIELD_SQL_DATA_ACCESS",
                         "sql_data_access ENUM('CONTAINS_SQL', 'NO_SQL',\n"
                         "     'READS_SQL_DATA',\n"
                         "     'MODIFIES_SQL_DATA') NOT NULL");
  m_target_def.add_field(FIELD_SECURITY_TYPE,
                         "FIELD_SECURITY_TYPE",
                         "security_type ENUM('DEFAULT', 'INVOKER', 'DEFINER') NOT NULL");
  m_target_def.add_field(FIELD_DEFINER,
                         "FIELD_DEFINER",
                         "definer VARCHAR(93) NOT NULL");
  m_target_def.add_field(FIELD_SQL_MODE,
                         "FIELD_SQL_MODE",
                         "sql_mode SET( \n"
                         "'REAL_AS_FLOAT',\n"
                         "'PIPES_AS_CONCAT',\n"
                         "'ANSI_QUOTES',\n"
                         "'IGNORE_SPACE',\n"
                         "'NOT_USED',\n"
                         "'ONLY_FULL_GROUP_BY',\n"
                         "'NO_UNSIGNED_SUBTRACTION',\n"
                         "'NO_DIR_IN_CREATE',\n"
                         "'POSTGRESQL',\n"
                         "'ORACLE',\n"
                         "'MSSQL',\n"
                         "'DB2',\n"
                         "'MAXDB',\n"
                         "'NO_KEY_OPTIONS',\n"
                         "'NO_TABLE_OPTIONS',\n"
                         "'NO_FIELD_OPTIONS',\n"
                         "'MYSQL323',\n"
                         "'MYSQL40',\n"
                         "'ANSI',\n"
                         "'NO_AUTO_VALUE_ON_ZERO',\n"
                         "'NO_BACKSLASH_ESCAPES',\n"
                         "'STRICT_TRANS_TABLES',\n"
                         "'STRICT_ALL_TABLES',\n"
                         "'NO_ZERO_IN_DATE',\n"
                         "'NO_ZERO_DATE',\n"
                         "'INVALID_DATES',\n"
                         "'ERROR_FOR_DIVISION_BY_ZERO',\n"
                         "'TRADITIONAL',\n"
                         "'NO_AUTO_CREATE_USER',\n"
                         "'HIGH_NOT_PRECEDENCE',\n"
                         "'NO_ENGINE_SUBSTITUTION',\n"
                         "'PAD_CHAR_TO_FULL_LENGTH') NOT NULL");
  m_target_def.add_field(FIELD_CLIENT_COLLATION_ID,
                         "FIELD_CLIENT_COLLATION_ID",
                         "client_collation_id BIGINT UNSIGNED NOT NULL");
  m_target_def.add_field(FIELD_CONNECTION_COLLATION_ID,
                         "FIELD_CONNECTION_COLLATION_ID",
                         "connection_collation_id BIGINT UNSIGNED NOT NULL");
  m_target_def.add_field(FIELD_SCHEMA_COLLATION_ID,
                         "FIELD_SCHEMA_COLLATION_ID",
                         "schema_collation_id BIGINT UNSIGNED NOT NULL");
  m_target_def.add_field(FIELD_CREATED,
                         "FIELD_CREATED",
                         "created TIMESTAMP NOT NULL DEFAULT "
                         "CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP");
  m_target_def.add_field(FIELD_LAST_ALTERED,
                         "FIELD_LAST_ALTERED",
                         "last_altered TIMESTAMP NOT NULL DEFAULT NOW()");
  m_target_def.add_field(FIELD_COMMENT,
                         "FIELD_COMMENT",
                         "comment TEXT NOT NULL");

  m_target_def.add_index("PRIMARY KEY(id)");
  m_target_def.add_index("UNIQUE KEY(schema_id, type, name)");

  m_target_def.add_foreign_key("FOREIGN KEY (schema_id) "
                               "REFERENCES schemata(id)");
  m_target_def.add_foreign_key("FOREIGN KEY (result_collation_id) "
                               "REFERENCES collations(id)");
  m_target_def.add_foreign_key("FOREIGN KEY (client_collation_id) "
                               "REFERENCES collations(id)");
  m_target_def.add_foreign_key("FOREIGN KEY (connection_collation_id) "
                               "REFERENCES collations(id)");
  m_target_def.add_foreign_key("FOREIGN KEY (schema_collation_id) "
                               "REFERENCES collations(id)");
}

///////////////////////////////////////////////////////////////////////////

Dictionary_object *Routines::create_dictionary_object(
  const Raw_record &r) const
{
  Routine::enum_routine_type routine_type=
    (Routine::enum_routine_type) r.read_int(FIELD_TYPE);

  if (routine_type == Routine::RT_FUNCTION)
    return dd::create_object<Function>();
  else
    return dd::create_object<Procedure>();
}

///////////////////////////////////////////////////////////////////////////

bool Routines::update_object_key(Routine_name_key *key,
                                 Object_id schema_id,
                                 Routine::enum_routine_type type,
                                 const std::string &routine_name)
{
  key->update(FIELD_SCHEMA_ID, schema_id,
              FIELD_TYPE, type,
              FIELD_NAME, routine_name.c_str());
  return false;
}

///////////////////////////////////////////////////////////////////////////

Object_key *Routines::create_key_by_schema_id(Object_id schema_id)
{
  return new (std::nothrow) Parent_id_range_key(1, FIELD_SCHEMA_ID, schema_id);
}

///////////////////////////////////////////////////////////////////////////

} // namespace tables
} // namespace dd

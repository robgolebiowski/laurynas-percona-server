/* Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.

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

#ifndef DD_TABLES__COLUMNS_INCLUDED
#define DD_TABLES__COLUMNS_INCLUDED

#include "my_global.h"

#include "dd/object_id.h"                    // dd::Object_id
#include "dd/impl/types/object_table_impl.h" // dd::Object_table_impl

namespace dd {
  class Object_key;
namespace tables {

///////////////////////////////////////////////////////////////////////////

class Columns : public Object_table_impl
{
public:
  static const Columns &instance();

  static const std::string &table_name()
  {
    static std::string s_table_name("columns");
    return s_table_name;
  }

public:
  enum enum_fields
  {
    FIELD_ID,
    FIELD_TABLE_ID,
    FIELD_NAME,
    FIELD_ORDINAL_POSITION,
    FIELD_TYPE,
    FIELD_IS_NULLABLE,
    FIELD_IS_ZEROFILL,
    FIELD_IS_UNSIGNED,
    FIELD_CHAR_LENGTH,
    FIELD_NUMERIC_PRECISION,
    FIELD_NUMERIC_SCALE,
    FIELD_DATETIME_PRECISION,
    FIELD_COLLATION_ID,
    FIELD_HAS_NO_DEFAULT,
    FIELD_DEFAULT_VALUE,
    FIELD_DEFAULT_VALUE_UTF8,
    FIELD_DEFAULT_OPTION,
    FIELD_UPDATE_OPTION,
    FIELD_IS_AUTO_INCREMENT,
    FIELD_IS_VIRTUAL,
    FIELD_GENERATION_EXPRESSION,
    FIELD_GENERATION_EXPRESSION_UTF8,
    FIELD_COMMENT,
    FIELD_HIDDEN,
    FIELD_OPTIONS,
    FIELD_SE_PRIVATE_DATA,
    FIELD_COLUMN_KEY,
    FIELD_COLUMN_TYPE_UTF8
  };

public:
  Columns();

  virtual const std::string &name() const
  { return Columns::table_name(); }

public:
  static Object_key *create_key_by_table_id(Object_id table_id);
};

///////////////////////////////////////////////////////////////////////////

}
}

#endif // DD_TABLES__COLUMNS_INCLUDED

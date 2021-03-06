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

#include "dd/impl/system_registry.h"

#include "table.h"                                   // MYSQL_SYSTEM_SCHEMA

#include "dd/properties.h"                           // Needed for destructor
#include "dd/impl/tables/catalogs.h"                 // Catalog
#include "dd/impl/tables/character_sets.h"           // Character_sets
#include "dd/impl/tables/collations.h"               // Collations
#include "dd/impl/tables/column_type_elements.h"     // Column_type_elements
#include "dd/impl/tables/columns.h"                  // Columns
#include "dd/impl/tables/events.h"                   // Events
#include "dd/impl/tables/foreign_key_column_usage.h" // Foreign_key_column_usage
#include "dd/impl/tables/foreign_keys.h"             // Foreign_keys
#include "dd/impl/tables/index_column_usage.h"       // Index_column_usage
#include "dd/impl/tables/index_partitions.h"         // Index_partitions
#include "dd/impl/tables/index_stats.h"              // Index_stats
#include "dd/impl/tables/indexes.h"                  // Indexes
#include "dd/impl/tables/parameters.h"               // Parameters
#include "dd/impl/tables/parameter_type_elements.h"  // Parameter_type_elements
#include "dd/impl/tables/routines.h"                 // Routines
#include "dd/impl/tables/schemata.h"                 // Schemata
#include "dd/impl/tables/spatial_reference_systems.h"// Spatial_reference_systems
#include "dd/impl/tables/table_partition_values.h"   // Table_partition_values
#include "dd/impl/tables/table_partitions.h"         // Table_partitions
#include "dd/impl/tables/table_stats.h"              // Table_stats
#include "dd/impl/tables/tables.h"                   // Tables
#include "dd/impl/tables/tablespace_files.h"         // Tablespace_files
#include "dd/impl/tables/tablespaces.h"              // Tablespaces
#include "dd/impl/tables/triggers.h"                 // Triggers
#include "dd/impl/tables/version.h"                  // Version
#include "dd/impl/tables/view_routine_usage.h"       // View_routine_usage
#include "dd/impl/tables/view_table_usage.h"         // View_table_usage

using namespace dd::tables;

///////////////////////////////////////////////////////////////////////////

namespace {
template <typename X>
void register_system_table()
{
  dd::System_tables::instance()->add(MYSQL_SCHEMA_NAME.str,
                                     X::instance().table_name(),
                                     dd::System_tables::Types::CORE,
                                     &X::instance());
}
}

namespace dd {

System_tables * System_tables::instance()
{
  static System_tables s_instance;
  return &s_instance;
}

System_views * System_views::instance()
{
  static System_views s_instance;
  return &s_instance;
}

System_tablespaces * System_tablespaces::instance()
{
  static System_tablespaces s_instance;
  return &s_instance;
}



void System_tables::init()
{
  // Order is dictated by the foreign key constraints
  register_system_table<Version>();
  register_system_table<Character_sets>();
  register_system_table<Collations>();
  register_system_table<Tablespaces>();
  register_system_table<Tablespace_files>();
  register_system_table<Catalogs>();
  register_system_table<Schemata>();
  register_system_table<Spatial_reference_systems>();
  register_system_table<Tables>();
  register_system_table<View_table_usage>();
  register_system_table<View_routine_usage>();
  register_system_table<Columns>();
  register_system_table<Indexes>();
  register_system_table<Index_column_usage>();
  register_system_table<Column_type_elements>();
  register_system_table<Foreign_keys>();
  register_system_table<Foreign_key_column_usage>();
  register_system_table<Table_partitions>();
  register_system_table<Table_partition_values>();
  register_system_table<Index_partitions>();
  register_system_table<Table_stats>();
  register_system_table<Index_stats>();
  register_system_table<Events>();
  register_system_table<Routines>();
  register_system_table<Parameters>();
  register_system_table<Parameter_type_elements>();
  register_system_table<Triggers>();
}

void System_views::init()
{
  static const char *system_view_names[]=
  {
    "CHARACTER_SETS",
    "COLLATIONS",
    "COLLATION_CHARACTER_SET_APPLICABILITY",
    "COLUMNS",
    "KEY_COLUMN_USAGE",
    "SCHEMATA",
    "SHOW_STATISTICS",
    "SHOW_STATISTICS_DYNAMIC",
    "STATISTICS_BASE",
    "STATISTICS_DYNAMIC",
    "STATISTICS",
    "TABLE_CONSTRAINTS",
    "TABLES",
    "TABLES_DYNAMIC",
    "VIEWS",
    nullptr
  };
  for (int i= 0; system_view_names[i] != NULL; ++i)
    System_views::instance()->add(INFORMATION_SCHEMA_NAME.str,
                                  system_view_names[i],
                                  System_views::Types::INFORMATION_SCHEMA);
}
}

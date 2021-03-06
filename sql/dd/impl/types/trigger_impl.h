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

#ifndef DD__TRIGGER_IMPL_INCLUDED
#define DD__TRIGGER_IMPL_INCLUDED

#include "my_global.h"

#include "dd/impl/types/entity_object_impl.h"  // dd::Entity_object_impl
#include "dd/types/object_type.h"              // dd::Object_type
#include "dd/types/trigger.h"                  // dd::Trigger
#include "dd/impl/types/table_impl.h"          // dd::Table_impl

namespace dd {

///////////////////////////////////////////////////////////////////////////

class Trigger_impl : virtual public Entity_object_impl,
                     virtual public Trigger
{
public:
  Trigger_impl();

  Trigger_impl(Table_impl *table);

  Trigger_impl(const Trigger_impl &src, Table_impl *parent);

  virtual ~Trigger_impl()
  { }

public:
  virtual const Object_table &object_table() const override
  { return Trigger::OBJECT_TABLE(); }

  virtual bool validate() const override;

  virtual bool restore_attributes(const Raw_record &r) override;

  virtual bool store_attributes(Raw_record *r) override;

  virtual void debug_print(std::string &outb) const override;

  void set_ordinal_position(uint ordinal_position)
  {
    m_ordinal_position= ordinal_position;
  }

  uint ordinal_position() const
  { return m_ordinal_position; }

public:
  /////////////////////////////////////////////////////////////////////////
  // Table.
  /////////////////////////////////////////////////////////////////////////

  virtual const Table &table() const;

  virtual Table &table();

  /* non-virtual */ const Table_impl &table_impl() const
  { return *m_table; }

  /* non-virtual */ Table_impl &table_impl()
  { return *m_table; }

  /////////////////////////////////////////////////////////////////////////
  // schema.
  /////////////////////////////////////////////////////////////////////////

  virtual Object_id schema_id() const override
  {
    return (m_table != nullptr ? m_table->schema_id() : INVALID_OBJECT_ID);
  }

  /////////////////////////////////////////////////////////////////////////
  // event type
  /////////////////////////////////////////////////////////////////////////

  virtual enum_event_type event_type() const override
  { return m_event_type; }

  virtual void set_event_type(enum_event_type event_type) override
  { m_event_type= event_type; }

  /////////////////////////////////////////////////////////////////////////
  // table.
  /////////////////////////////////////////////////////////////////////////

  virtual Object_id table_id() const override
  { return m_table->id(); }

  /////////////////////////////////////////////////////////////////////////
  // action timing
  /////////////////////////////////////////////////////////////////////////

  virtual enum_action_timing action_timing() const override
  { return m_action_timing; }

  virtual void set_action_timing(enum_action_timing
                                 action_timing) override
  { m_action_timing= action_timing; }

  /////////////////////////////////////////////////////////////////////////
  // action_order.
  /////////////////////////////////////////////////////////////////////////

  virtual uint action_order() const override
  { return m_action_order; }

  virtual void set_action_order(uint action_order) override
  { m_action_order= action_order; }

  /////////////////////////////////////////////////////////////////////////
  // action_statement/utf8.
  /////////////////////////////////////////////////////////////////////////

  virtual const std::string &action_statement() const override
  { return m_action_statement; }

  virtual void set_action_statement(const std::string
                                    &action_statement) override
  { m_action_statement= action_statement; }

  virtual const std::string &action_statement_utf8() const override
  { return m_action_statement_utf8; }

  virtual void set_action_statement_utf8(const std::string
                                         &action_statement_utf8) override
  { m_action_statement_utf8= action_statement_utf8; }

  /////////////////////////////////////////////////////////////////////////
  // created.
  /////////////////////////////////////////////////////////////////////////

  virtual timeval created() const override
  { return m_created; }

  virtual void set_created(timeval created) override
  { m_created= created; }

  /////////////////////////////////////////////////////////////////////////
  // last altered.
  /////////////////////////////////////////////////////////////////////////

  virtual timeval last_altered() const override
  { return m_last_altered; }

  virtual void set_last_altered(timeval last_altered) override
  { m_last_altered= last_altered; }

  /////////////////////////////////////////////////////////////////////////
  // sql_mode
  /////////////////////////////////////////////////////////////////////////

  virtual ulonglong sql_mode() const override
  { return m_sql_mode; }

  virtual void set_sql_mode(ulonglong sql_mode) override
  { m_sql_mode= sql_mode; }

  /////////////////////////////////////////////////////////////////////////
  // definer.
  /////////////////////////////////////////////////////////////////////////

  virtual const std::string &definer_user() const override
  { return m_definer_user; }

  virtual const std::string &definer_host() const override
  { return m_definer_host; }

  virtual void set_definer(const std::string &username,
                           const std::string &hostname) override
  {
    m_definer_user= username;
    m_definer_host= hostname;
  }

  /////////////////////////////////////////////////////////////////////////
  // collation.
  /////////////////////////////////////////////////////////////////////////

  virtual Object_id client_collation_id() const override
  { return m_client_collation_id; }

  virtual void set_client_collation_id(Object_id
                                       client_collation_id) override
  { m_client_collation_id= client_collation_id; }

  virtual Object_id connection_collation_id() const override
  { return m_connection_collation_id; }

  virtual void set_connection_collation_id(Object_id
                                           connection_collation_id) override
  { m_connection_collation_id= connection_collation_id; }

  virtual Object_id schema_collation_id() const override
  { return m_schema_collation_id; }

  virtual void set_schema_collation_id(Object_id
                                       schema_collation_id) override
  { m_schema_collation_id= schema_collation_id; }

  // Fix "inherits ... via dominance" warnings
  virtual Weak_object_impl *impl() override
  { return Weak_object_impl::impl(); }

  virtual const Weak_object_impl *impl() const override
  { return Weak_object_impl::impl(); }

  virtual Object_id id() const override
  { return Entity_object_impl::id(); }

  virtual bool is_persistent() const override
  { return Entity_object_impl::is_persistent(); }

  virtual const std::string &name() const override
  { return Entity_object_impl::name(); }

  virtual void set_name(const std::string &name) override
  { Entity_object_impl::set_name(name); }

public:
  static Trigger_impl *restore_item(Table_impl *table)
  {
    return new (std::nothrow) Trigger_impl(table);
  }

  static Trigger_impl *clone(const Trigger_impl &other,
                             Table_impl *table)
  {
    return new (std::nothrow) Trigger_impl(other, table);
  }

private:
  enum_event_type     m_event_type;
  enum_action_timing  m_action_timing;

  /*
    We use m_ordinal_position to help implement
    add_trigger_following and add_trigger_preceding.
    This is required mainly because we maintain a single
    collection to maintain all triggers.
  */
  uint        m_ordinal_position;
  uint        m_action_order;

  ulonglong m_sql_mode;
  timeval m_created;
  timeval m_last_altered;

  std::string m_action_statement_utf8;
  std::string m_action_statement;
  std::string m_definer_user;
  std::string m_definer_host;

  // References to tightly-coupled objects.

  Table_impl *m_table;

  // References to loosely-coupled objects.

  Object_id m_client_collation_id;
  Object_id m_connection_collation_id;
  Object_id m_schema_collation_id;
};

///////////////////////////////////////////////////////////////////////////

class Trigger_type : public Object_type
{
public:
  virtual void register_tables(Open_dictionary_tables_ctx *otx) const;

  virtual Weak_object *create_object() const
  { return new (std::nothrow) Trigger_impl(); }
};

///////////////////////////////////////////////////////////////////////////

}

#endif // DD__TRIGGER_IMPL_INCLUDED

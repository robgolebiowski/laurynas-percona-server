/*
      Copyright (c) 2013, 2016, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file storage/perfschema/table_replication_applier_status_by_worker.cc
  Table replication_applier_status_by_worker (implementation).
*/

#include "my_global.h"

#ifndef EMBEDDED_LIBRARY
#define HAVE_REPLICATION
#endif /* EMBEDDED_LIBRARY */

#include "table_replication_applier_status_by_worker.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "rpl_slave.h"
#include "rpl_info.h"
#include  "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
#include "rpl_rli_pdb.h"
#include "rpl_msr.h"    /*Multi source replication */

THR_LOCK table_replication_applier_status_by_worker::m_table_lock;

/* numbers in varchar count utf8 characters. */
static const TABLE_FIELD_TYPE field_types[]=
{

  {
    {C_STRING_WITH_LEN("CHANNEL_NAME")},
    {C_STRING_WITH_LEN("char(64)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("WORKER_ID")},
    {C_STRING_WITH_LEN("bigint")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("THREAD_ID")},
    {C_STRING_WITH_LEN("bigint")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("SERVICE_STATE")},
    {C_STRING_WITH_LEN("enum('ON','OFF')")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_SEEN_TRANSACTION")},
    {C_STRING_WITH_LEN("char(57)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_NUMBER")},
    {C_STRING_WITH_LEN("int(11)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_MESSAGE")},
    {C_STRING_WITH_LEN("varchar(1024)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_TIMESTAMP")},
    {C_STRING_WITH_LEN("timestamp")},
    {NULL, 0}
  },
};

TABLE_FIELD_DEF
table_replication_applier_status_by_worker::m_field_def=
{ 8, field_types };

PFS_engine_table_share
table_replication_applier_status_by_worker::m_share=
{
  { C_STRING_WITH_LEN("replication_applier_status_by_worker") },
  &pfs_readonly_acl,
  table_replication_applier_status_by_worker::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_applier_status_by_worker::get_row_count, /*records*/
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  &m_field_def,
  false, /* checked */
  false  /* perpetual */
};

#ifdef HAVE_REPLICATION
bool PFS_index_rpl_applier_status_by_worker_by_channel::match(Master_info *mi)
{
  if (m_fields >= 1)
  {
    st_row_worker row;

    /* Mutex locks not necessary for channel name. */
    row.channel_name_length= mi->get_channel() ? (uint)strlen(mi->get_channel()) : 0;
    memcpy(row.channel_name, mi->get_channel(), row.channel_name_length);

    if (!m_key.match(row.channel_name, row.channel_name_length))
      return false;
  }

  return true;
}

bool PFS_index_rpl_applier_status_by_worker_by_thread::match(Master_info *mi)
{
  if (m_fields >= 1)
  {
    st_row_worker row;
    row.thread_id_is_null= true;

    mysql_mutex_lock(&mi->rli->data_lock);

    if (mi->rli->slave_running)
    {
      PSI_thread *psi= thd_get_psi(mi->rli->info_thd);
      PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
      if(pfs)
      {
        row.thread_id= pfs->m_thread_internal_id;
        row.thread_id_is_null= false;
      }
    }

    mysql_mutex_unlock(&mi->rli->data_lock);

    if (row.thread_id_is_null)
      return false;

    if (!m_key.match(row.thread_id))
      return false;
  }

  return true;
}
#endif

PFS_engine_table* table_replication_applier_status_by_worker::create(void)
{
  return new table_replication_applier_status_by_worker();
}

table_replication_applier_status_by_worker
  ::table_replication_applier_status_by_worker()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(), m_next_pos(),
    m_applier_pos(0), m_applier_next_pos(0)
{}

table_replication_applier_status_by_worker
  ::~table_replication_applier_status_by_worker()
{}

void table_replication_applier_status_by_worker::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
  m_applier_pos.m_index=0;
  m_applier_next_pos.m_index=0;
}

ha_rows table_replication_applier_status_by_worker::get_row_count()
{
#ifdef HAVE_REPLICATION
  /*
    Return an estimate, number of master info's multipled by worker threads
  */
  return channel_map.get_max_channels()*32;
#else
  return 0;
#endif /* HAVE_REPLICATION */
}


int table_replication_applier_status_by_worker::rnd_next(void)
{
  int res= HA_ERR_END_OF_FILE;

#ifdef HAVE_REPLICATION
  Slave_worker *worker;
  Master_info *mi;

  channel_map.rdlock();

  /*
    For each SQL Thread in all channels get the respective Master_info and
    construct a row to display its status in
    'replication_applier_status_by_worker' table in case of single threaded
    slave mode.
  */
  for(m_applier_pos.set_at(&m_applier_next_pos);
      m_applier_pos.m_index < channel_map.get_max_channels();
      m_applier_pos.next())
  {
    mi= channel_map.get_mi_at_pos(m_applier_pos.m_index);

    if (mi && mi->host[0] && mi->rli && mi->rli->get_worker_count()==0)
    {
      make_row(mi);
      m_applier_next_pos.set_after(&m_applier_pos);

      channel_map.unlock();
      return 0;
    }
  }

  for (m_pos.set_at(&m_next_pos);
       m_pos.has_more_channels(channel_map.get_max_channels()) && res != 0;
       m_pos.next_channel())
  {
    mi= channel_map.get_mi_at_pos(m_pos.m_index_1);

    if (mi && mi->host[0])
    {
      worker= mi->rli->get_worker(m_pos.m_index_2);
      if (worker)
      {
        make_row(worker);
        m_next_pos.set_after(&m_pos);
        res= 0;
      }
    }
  }

  channel_map.unlock();
#endif /* HAVE_REPLICATION */

  return res;
}

int table_replication_applier_status_by_worker::rnd_pos(const void *pos)
{
  int res= HA_ERR_RECORD_DELETED;

#ifdef HAVE_REPLICATION
  Slave_worker *worker;
  Master_info *mi;

  set_position(pos);

  channel_map.rdlock();

  mi= channel_map.get_mi_at_pos(m_pos.m_index_1);

  if (!mi || !mi->rli || !mi->host[0])
    goto end;

  DBUG_ASSERT(m_pos.m_index_1 < mi->rli->get_worker_count());
  /*
    Display SQL Thread's status only in the case of single threaded mode.
  */
  if (mi->rli->get_worker_count() == 0)
  {
    make_row(mi);
    res= 0;
    goto end;
  }
  worker= mi->rli->get_worker(m_pos.m_index_2);

  if (worker != NULL)
  {
    make_row(worker);
    res= 0;
  }

end:
  channel_map.unlock();
#endif /* HAVE_REPLICATION */

  return res;
}

int table_replication_applier_status_by_worker::index_init(uint idx, bool sorted)
{
#ifdef HAVE_REPLICATION
  PFS_index_rpl_applier_status_by_worker *result= NULL;

  switch(idx)
  {
  case 0:
    result= PFS_NEW(PFS_index_rpl_applier_status_by_worker_by_channel);
    break;
  case 1:
    result= PFS_NEW(PFS_index_rpl_applier_status_by_worker_by_thread);
    break;
  default:
    DBUG_ASSERT(false);
    break;
  }
  m_opened_index= result;
  m_index= result;
#endif
  return 0;
}

int table_replication_applier_status_by_worker::index_next(void)
{
  int res= HA_ERR_END_OF_FILE;

#ifdef HAVE_REPLICATION
  Slave_worker *worker;
  Master_info *mi;

  channel_map.rdlock();

  /*
    For each SQL Thread in all channels get the respective Master_info and
    construct a row to display its status in
    'replication_applier_status_by_worker' table in case of single threaded
    slave mode.
  */
  for(m_applier_pos.set_at(&m_applier_next_pos);
      m_applier_pos.m_index < channel_map.get_max_channels();
      m_applier_pos.next())
  {
    mi= channel_map.get_mi_at_pos(m_applier_pos.m_index);

    if (mi && mi->host[0] && mi->rli && mi->rli->get_worker_count()==0)
    {
      if (m_opened_index->match(mi))
      {
        make_row(mi);
        m_applier_next_pos.set_after(&m_applier_pos);

        channel_map.unlock();
        return 0;
      }
    }
  }

  for (m_pos.set_at(&m_next_pos);
       m_pos.has_more_channels(channel_map.get_max_channels()) && res != 0;
       m_pos.next_channel())
  {
    mi= channel_map.get_mi_at_pos(m_pos.m_index_1);

    if (mi && mi->host[0])
    {
      if (m_opened_index->match(mi))
      {
        worker= mi->rli->get_worker(m_pos.m_index_2);
        if (worker)
        {
          make_row(worker);
          m_next_pos.set_after(&m_pos);
          res= 0;
        }
      }
    }
  }

  channel_map.unlock();
#endif /* HAVE_REPLICATION */

  return res;
}


#ifdef HAVE_REPLICATION
/**
  Function to display SQL Thread's status as part of
  'replication_applier_status_by_worker' in single threaded slave mode.

   @param[in] mi Master_info
*/
void table_replication_applier_status_by_worker::make_row(Master_info *mi)
{
  m_row_exists= false;

  m_row.worker_id= 0;

  m_row.thread_id= 0;

  DBUG_ASSERT(mi != NULL);
  DBUG_ASSERT(mi->rli != NULL);

  mysql_mutex_lock(&mi->rli->data_lock);

  m_row.channel_name_length= strlen(mi->get_channel());
  memcpy(m_row.channel_name, (char*)mi->get_channel(), m_row.channel_name_length);

  if (mi->rli->slave_running)
  {
    PSI_thread *psi= thd_get_psi(mi->rli->info_thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
    else
      m_row.thread_id_is_null= true;
  }
  else
    m_row.thread_id_is_null= true;

  if (mi->rli->slave_running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  if (mi->rli->currently_executing_gtid.type == GTID_GROUP)
  {
    global_sid_lock->rdlock();
    m_row.last_seen_transaction_length=
      mi->rli->currently_executing_gtid.to_string(global_sid_map,
                                            m_row.last_seen_transaction);
    global_sid_lock->unlock();
  }
  else if (mi->rli->currently_executing_gtid.type == ANONYMOUS_GROUP)
  {
    m_row.last_seen_transaction_length=
      mi->rli->currently_executing_gtid.to_string((rpl_sid *)NULL,
                                            m_row.last_seen_transaction);
  }
  else
  {
    /*
      For SQL thread currently_executing_gtid, type is set to
      AUTOMATIC_GROUP when the SQL thread is not executing any
      transaction.  For this case, the field should be empty.
    */
    DBUG_ASSERT(mi->rli->currently_executing_gtid.type == AUTOMATIC_GROUP);
    m_row.last_seen_transaction_length= 0;
    memcpy(m_row.last_seen_transaction, "", 1);
  }

  mysql_mutex_lock(&mi->rli->err_lock);

  m_row.last_error_number= (long int) mi->rli->last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char *temp_store= (char*) mi->rli->last_error().message;
    m_row.last_error_message_length= strlen(temp_store);
    memcpy(m_row.last_error_message, temp_store,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= (ulonglong)mi->rli->last_error().skr*1000000;
  }

  mysql_mutex_unlock(&mi->rli->err_lock);
  mysql_mutex_unlock(&mi->rli->data_lock);
  m_row_exists= true;
}
#endif /* HAVE_REPLICATION */

#ifdef HAVE_REPLICATION
void table_replication_applier_status_by_worker::make_row(Slave_worker *w)
{
  m_row_exists= false;

  m_row.worker_id= w->get_internal_id();

  m_row.thread_id= 0;

  m_row.channel_name_length= strlen(w->get_channel());
  memcpy(m_row.channel_name, (char*)w->get_channel(), m_row.channel_name_length);

  mysql_mutex_lock(&w->jobs_lock);
  if (w->running_status == Slave_worker::RUNNING)
  {
    PSI_thread *psi= thd_get_psi(w->info_thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
    else /* no instrumentation found */
      m_row.thread_id_is_null= true;
  }
  else
    m_row.thread_id_is_null= true;

  if (w->running_status == Slave_worker::RUNNING)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  m_row.last_error_number= (unsigned int) w->last_error().number;

  if (w->currently_executing_gtid.type == GTID_GROUP)
  {
    global_sid_lock->rdlock();
    m_row.last_seen_transaction_length=
      w->currently_executing_gtid.to_string(global_sid_map,
                                            m_row.last_seen_transaction);
    global_sid_lock->unlock();
  }
  else if (w->currently_executing_gtid.type == ANONYMOUS_GROUP)
  {
    m_row.last_seen_transaction_length=
      w->currently_executing_gtid.to_string((rpl_sid *)NULL,
                                            m_row.last_seen_transaction);
  }
  else
  {
    /*
      For worker->currently_executing_gtid, type is set to
      AUTOMATIC_GROUP when the worker is not executing any
      transaction.  For this case, the field should be empty.
    */
    DBUG_ASSERT(w->currently_executing_gtid.type == AUTOMATIC_GROUP);
    m_row.last_seen_transaction_length= 0;
    memcpy(m_row.last_seen_transaction, "", 1);
  }

  m_row.last_error_number= (unsigned int) w->last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char * temp_store= (char*)w->last_error().message;
    m_row.last_error_message_length= strlen(temp_store);
    memcpy(m_row.last_error_message, w->last_error().message,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= (ulonglong)w->last_error().skr*1000000;
  }
  mysql_mutex_unlock(&w->jobs_lock);

  m_row_exists= true;
}
#endif /* HAVE_REPLICATION */

int table_replication_applier_status_by_worker
  ::read_row_values(TABLE *table, unsigned char *buf,  Field **fields,
                    bool read_all)
{
#ifdef HAVE_REPLICATION
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /** channel_name */
        set_field_char_utf8(f, m_row.channel_name, m_row.channel_name_length);
        break;
      case 1: /*worker_id*/
        set_field_ulonglong(f, m_row.worker_id);
        break;
      case 2: /*thread_id*/
        if(m_row.thread_id_is_null)
          f->set_null();
        else
          set_field_ulonglong(f, m_row.thread_id);
        break;
      case 3: /*service_state*/
        set_field_enum(f, m_row.service_state);
        break;
      case 4: /*last_seen_transaction*/
        set_field_char_utf8(f, m_row.last_seen_transaction, m_row.last_seen_transaction_length);
        break;
      case 5: /*last_error_number*/
        set_field_ulong(f, m_row.last_error_number);
        break;
      case 6: /*last_error_message*/
        set_field_varchar_utf8(f, m_row.last_error_message, m_row.last_error_message_length);
        break;
      case 7: /*last_error_timestamp*/
        set_field_timestamp(f, m_row.last_error_timestamp);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
#else
  return HA_ERR_RECORD_DELETED;
#endif /* HAVE_REPLICATION */
}

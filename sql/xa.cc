/* Copyright (c) 2013, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/xa.h"

#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <functional>

#include "m_ctype.h"
#include "m_string.h"
#include "map_helpers.h"
#include "my_dbug.h"
#include "my_loglevel.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/mysql_mutex_bits.h"
#include "mysql/components/services/psi_mutex_bits.h"
#include "mysql/plugin.h"  // MYSQL_XIDDATASIZE
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_transaction.h"
#include "mysql/psi/psi_base.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/binlog.h"  // is_transaction_empty
#include "sql/clone_handler.h"
#include "sql/debug_sync.h"  // DEBUG_SYNC
#include "sql/handler.h"     // handlerton
#include "sql/item.h"
#include "sql/log.h"
#include "sql/mdl.h"
#include "sql/mdl_context_backup.h"  // MDL_context_backup_manager
#include "sql/mysqld.h"              // server_id
#include "sql/protocol.h"
#include "sql/psi_memory_key.h"  // key_memory_XID
#include "sql/query_options.h"
#include "sql/rpl_context.h"
#include "sql/rpl_gtid.h"
#include "sql/sql_class.h"  // THD
#include "sql/sql_const.h"
#include "sql/sql_error.h"
#include "sql/sql_list.h"
#include "sql/sql_plugin.h"  // plugin_foreach
#include "sql/sql_table.h"   // filename_to_tablename
#include "sql/system_variables.h"
#include "sql/tc_log.h"       // tc_log
#include "sql/transaction.h"  // trans_begin, trans_rollback
#include "sql/transaction_info.h"
#include "sql_string.h"
#include "template_utils.h"
#include "thr_mutex.h"

const char *XID_STATE::xa_state_names[] = {"NON-EXISTING", "ACTIVE", "IDLE",
                                           "PREPARED", "ROLLBACK ONLY"};

const char *strnchr(const char *str, char c, size_t n);
Prepared_xa_txnids prepared_xa_txnids;
extern int g_did_binlog_recovery;
extern int print_extra_info;
extern int ddc_mode;

struct transaction_free_hash {
  void operator()(Transaction_ctx *) const;
};

static bool inited = false;
static mysql_mutex_t LOCK_transaction_cache;
static malloc_unordered_map<std::string, std::shared_ptr<Transaction_ctx>>
    transaction_cache{key_memory_XID};

static const uint MYSQL_XID_PREFIX_LEN = 8;  // must be a multiple of 8
static const uint MYSQL_XID_OFFSET = MYSQL_XID_PREFIX_LEN + sizeof(server_id);
static const uint MYSQL_XID_GTRID_LEN = MYSQL_XID_OFFSET + sizeof(my_xid);

static void attach_native_trx(THD *thd);
static std::shared_ptr<Transaction_ctx> transaction_cache_search(XID *xid);
static bool transaction_cache_insert(XID *xid, Transaction_ctx *transaction);
static bool transaction_cache_insert_recovery(XID *xid);

static int INTERNAL_MYSQL_SYSTEM_XID = -1;

my_xid xid_t::get_my_xid() const {
  static_assert(XIDDATASIZE == MYSQL_XIDDATASIZE,
                "Our #define needs to match the one in plugin.h.");

  if (gtrid_length == static_cast<long>(MYSQL_XID_GTRID_LEN) &&
      bqual_length == 0 &&
      !memcmp(data, MYSQL_XID_PREFIX, MYSQL_XID_PREFIX_LEN)) {
    my_xid tmp;
    memcpy(&tmp, data + MYSQL_XID_OFFSET, sizeof(tmp));
    return tmp;
  }
  return 0;
}

void xid_t::set(my_xid xid) {
  formatID = 1;
  /*
    trx_is_mysql_xa() assumes xid is 0 if it's external XA txn and non-zero
	if internal. And queries executed internally in mysql at start of mysqld
	before any query is executed, thd->query_id is 0, so assign
	INTERNAL_MYSQL_SYSTEM_XID in this case.
  */
  if (xid == 0) xid = INTERNAL_MYSQL_SYSTEM_XID;

  memcpy(data, MYSQL_XID_PREFIX, MYSQL_XID_PREFIX_LEN);
  memcpy(data + MYSQL_XID_PREFIX_LEN, &server_id, sizeof(server_id));
  memcpy(data + MYSQL_XID_OFFSET, &xid, sizeof(xid));
  gtrid_length = MYSQL_XID_GTRID_LEN;
  bqual_length = 0;
}

const char *XID_STATE ::get_xa_xid() const
{
  if (m_xid_str[0] == '\0')
  {
    my_xid xid = m_xid.get_my_xid();
    if (xid == 0)
      m_xid.serialize(m_xid_str);
    else
      snprintf(m_xid_str, sizeof(m_xid_str), "%llu", xid);
  }
  return m_xid_str;
}


static bool xacommit_handlerton(THD *, plugin_ref plugin, void *arg) {
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover) {
    xa_status_code ret = hton->commit_by_xid(hton, (XID *)arg);

    /*
      Consider XAER_NOTA as success since not every storage should be
      involved into XA transaction, therefore absence of transaction
      specified by xid in storage engine doesn't mean that a real error
      happened. To illustrate it, lets consider the corner case
      when no one storage engine is involved into XA transaction:
      XA START 'xid1';
      XA END 'xid1';
      XA PREPARE 'xid1';
      XA COMMIT 'xid1';
      For this use case, handing of the statement XA COMMIT leads to
      returning XAER_NOTA by ha_innodb::commit_by_xid because there isn't
      a real transaction managed by innodb. So, there is no XA transaction
      with specified xid in resource manager represented by InnoDB storage
      engine although such transaction exists in transaction manager
      represented by mysql server runtime.
    */
    if (ret != XA_OK && ret != XAER_NOTA) {
      my_error(ER_XAER_RMERR, MYF(0));
      return true;
    }
    return false;
  }

  return false;
}

static bool xarollback_handlerton(THD *, plugin_ref plugin, void *arg) {
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover) {
    xa_status_code ret = hton->rollback_by_xid(hton, (XID *)arg);

    /*
      Consider XAER_NOTA as success since not every storage should be
      involved into XA transaction, therefore absence of transaction
      specified by xid in storage engine doesn't mean that a real error
      happened. To illustrate it, lets consider the corner case
      when no one storage engine is involved into XA transaction:
      XA START 'xid1';
      XA END 'xid1';
      XA PREPARE 'xid1';
      XA COMMIT 'xid1';
      For this use case, handing of the statement XA COMMIT leads to
      returning XAER_NOTA by ha_innodb::commit_by_xid because there isn't
      a real transaction managed by innodb. So, there is no XA transaction
      with specified xid in resource manager represented by InnoDB storage
      engine although such transaction exists in transaction manager
      represented by mysql server runtime.
    */
    if (ret != XA_OK && ret != XAER_NOTA) {
      my_error(ER_XAER_RMERR, MYF(0));
      return true;
    }
    return false;
  }
  return false;
}

static bool ha_commit_or_rollback_by_xid(THD *, XID *xid, bool commit) {
  return plugin_foreach(nullptr,
                        commit ? xacommit_handlerton : xarollback_handlerton,
                        MYSQL_STORAGE_ENGINE_PLUGIN, xid);
}

Recovered_xa_transactions *Recovered_xa_transactions::m_instance = nullptr;

Recovered_xa_transactions::Recovered_xa_transactions()
    : m_prepared_xa_trans(Malloc_allocator<XA_recover_txn *>(
          key_memory_Recovered_xa_transactions)),
      m_mem_root_inited(false) {}

Recovered_xa_transactions &Recovered_xa_transactions::instance() {
  return *m_instance;
}

bool Recovered_xa_transactions::init() {
  m_instance = new (std::nothrow) Recovered_xa_transactions();
  return m_instance == nullptr;
}

void Recovered_xa_transactions::destroy() {
  delete m_instance;
  m_instance = nullptr;
}

bool Recovered_xa_transactions::add_prepared_xa_transaction(
    XA_recover_txn *prepared_xa_trn_arg) {
  XA_recover_txn *prepared_xa_trn = new (&m_mem_root) XA_recover_txn();

  if (prepared_xa_trn == nullptr) {
    LogErr(ERROR_LEVEL, ER_SERVER_OUTOFMEMORY,
           static_cast<int>(sizeof(XA_recover_txn)));
    return true;
  }

  prepared_xa_trn->id = prepared_xa_trn_arg->id;
  prepared_xa_trn->mod_tables = prepared_xa_trn_arg->mod_tables;

  m_prepared_xa_trans.push_back(prepared_xa_trn);

  return false;
}

MEM_ROOT *Recovered_xa_transactions::get_allocated_memroot() {
  if (!m_mem_root_inited) {
    init_sql_alloc(key_memory_XID, &m_mem_root, TABLE_ALLOC_BLOCK_SIZE, 0);
    m_mem_root_inited = true;
  }
  return &m_mem_root;
}

void Recovered_xa_transactions::clear()
{
  m_prepared_xa_trans.clear();
}

struct xarecover_st {
  typedef std::set<std::string> xa_prepared_set_t;
  xarecover_st(XA_recover_txn_list *txnlist,
      const memroot_unordered_set<my_xid> *clist,
      xa_prepared_set_t *xap_engine,
      const xa_prepared_set_t *xap, const xa_prepared_set_t *cop,
      const xa_prepared_set_t *committed, const xa_prepared_set_t *aborted) :
    txn_list(txnlist), commit_list(clist),
    binlog_xa_prepared_engine(xap_engine), binlog_xa_prepared(xap),
    binlog_xa_cop(cop), binlog_xa_committed(committed),
    binlog_xa_aborted(aborted)
  {
    DBUG_ASSERT((commit_list && binlog_xa_prepared_engine &&
                 binlog_xa_prepared && binlog_xa_cop && binlog_xa_committed &&
                 binlog_xa_aborted) ||
                (!commit_list && !binlog_xa_prepared_engine &&
                 !binlog_xa_prepared && !binlog_xa_cop &&
                 !binlog_xa_committed && !binlog_xa_aborted));

    found_foreign_xids = 0;
    found_my_xids = 0;
    dry_run = false;
    m_do_binlog_recovery = (commit_list != NULL);
  }

  bool do_binlog_recovery() const { return m_do_binlog_recovery; }
  int found_foreign_xids, found_my_xids;
  XA_recover_txn_list *txn_list; // prepared txn list returned by storage engine.
  const memroot_unordered_set<my_xid> *commit_list;
  // prepared xa txn branches that are found from storage engines(e.g. innodb, etc)
  // during recovery.
  xa_prepared_set_t *binlog_xa_prepared_engine;
  // prepared xa txn branches that are found from XA_PREPARED_LIST of
  // PREV_GTIDS_LIST event of last binlog file, as well as last binlog file.
  const xa_prepared_set_t *binlog_xa_prepared;
  // stores XA txn ids from binlog which were commtted by XA COMMIT ... ONE PHASE
  const xa_prepared_set_t *binlog_xa_cop;

  // stores XA txn ids from binlog which were commtted by XA COMMIT
  const xa_prepared_set_t *binlog_xa_committed;
  // stores XA txn ids from binlog which were aborted by XA rollback.
  const xa_prepared_set_t *binlog_xa_aborted;
  bool dry_run;
  bool m_do_binlog_recovery;
};


static bool
fetch_xa_prepared_handlerton(THD * /*unused*/, plugin_ref plugin, void *arg)
{
  handlerton *hton= plugin_data<handlerton*>(plugin);
  XA_recover_txn_list *txn_list = (XA_recover_txn_list *)arg;
  int got;

  if (hton->state == SHOW_OPTION_YES && hton->recover &&
      ((got= hton->recover(hton, txn_list,
        Recovered_xa_transactions::instance().get_allocated_memroot())) > 0))
  {
    sql_print_information("Found %d prepared transaction(s) in %s",
                          got, ha_resolve_storage_engine_name(hton));
    XA_recover_txn_list::iterator itr = txn_list->begin();
    for (int i= 0; i < got; i++, ++itr)
    {
      XA_recover_txn&target_xrt = *itr;
      XID &target_xid = target_xrt.id;
      my_xid x= target_xid.get_my_xid();

      if (!x) // not "mine" - that is generated by external TM
      {
        std::string xid_data(target_xid.get_data(), target_xid.get_gtrid_length());
        prepared_xa_txnids.add_id(xid_data);
        if (Recovered_xa_transactions::instance().add_prepared_xa_transaction(&target_xrt))
          return true;
      }
      else
      {
        sql_print_error("Error: found internal XA XID %llu. fetch_xa_prepared()"
                        " should not be called if recovery should be done.", x);
        return true;
      }
    }
  }
  return false;

}


int fetch_xa_prepared()
{
  XA_recover_txn_list txn_list;
  DBUG_ENTER("fetch_xa_prepared");

  if (total_ha_2pc <= (ulong)opt_bin_log)
    DBUG_RETURN(0);

  plugin_foreach(NULL, fetch_xa_prepared_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &txn_list);
  
  DBUG_RETURN(0);
}
static bool xarecover_create_mdl_backup(XA_recover_txn &txn,
                                        MEM_ROOT *mem_root) {
  MDL_request_list mdl_requests;
  List_iterator<st_handler_tablename> table_list_it(*txn.mod_tables);
  st_handler_tablename *tbl_name;

  while ((tbl_name = table_list_it++)) {
    MDL_request *table_mdl_request = new (mem_root) MDL_request;
    if (table_mdl_request == nullptr) {
      /* Out of memory: Abort() */
      return true;
    }

    char db_buff[NAME_CHAR_LEN * FILENAME_CHARSET_MBMAXLEN + 1];
    int len = filename_to_tablename(tbl_name->db, db_buff, sizeof(db_buff));
    db_buff[len] = '\0';

    char name_buff[NAME_CHAR_LEN * FILENAME_CHARSET_MBMAXLEN + 1];
    len = filename_to_tablename(tbl_name->tablename, name_buff,
                                sizeof(name_buff));
    name_buff[len] = '\0';

    /*
      We do not have information about the actual lock taken
      during the transaction. Hence we are going with a strong
      lock to be safe.
    */
    MDL_REQUEST_INIT(table_mdl_request, MDL_key::TABLE, db_buff, name_buff,
                     MDL_SHARED_WRITE, MDL_TRANSACTION);
    mdl_requests.push_front(table_mdl_request);
  }

  return MDL_context_backup_manager::instance().create_backup(
      &mdl_requests, txn.id.key(), txn.id.key_length());
}

bool Recovered_xa_transactions::recover_prepared_xa_transactions() {
  bool ret = false;

  if (m_mem_root_inited) {
    while (!m_prepared_xa_trans.empty()) {
      auto prepared_xa_trn = m_prepared_xa_trans.front();
      transaction_cache_insert_recovery(&prepared_xa_trn->id);

      if (xarecover_create_mdl_backup(*prepared_xa_trn, &m_mem_root)) {
        ret = true;
        break;
      }

      m_prepared_xa_trans.pop_front();
    }
    free_root(&m_mem_root, MYF(0));
    m_mem_root_inited = false;
  }

  return ret;
}

static bool xarecover_handlerton(THD *, plugin_ref plugin, void *arg) {
  handlerton *hton = plugin_data<handlerton *>(plugin);
  xarecover_st *info = (struct xarecover_st *)arg;
  int got;

  if (hton->state == SHOW_OPTION_YES && hton->recover &&
      (got = hton->recover(hton, info->txn_list,
       Recovered_xa_transactions::instance().get_allocated_memroot())) > 0) {
    LogErr(INFORMATION_LEVEL, ER_XA_RECOVER_FOUND_TRX_IN_SE, got,
           ha_resolve_storage_engine_name(hton));

    XA_recover_txn_list::iterator itr = info->txn_list->begin();
    for (int i = 0; i < got; i++, ++itr) {

      XA_recover_txn&target_xrt = *itr;
      XID &target_xid = target_xrt.id;
      my_xid x = target_xid.get_my_xid();
      bool commit_i= false;

      if (!x)  // not "mine" - that is generated by external TM
      {
        if (info->dry_run)
        {
          info->found_foreign_xids++;
          continue;
        }

        std::string xid_data(target_xid.get_data(), target_xid.get_gtrid_length());

        if (info->binlog_xa_cop &&
            info->binlog_xa_cop->find(xid_data) != info->binlog_xa_cop->end())
        {
          /*
            External XA txn branch which was committed by XA COMMIT ... ONE PHASE.
            It's already in binlog so we will commit it in storage engine down below.
          */
          commit_i= true;
        }
        else if (info->binlog_xa_prepared &&
                 info->binlog_xa_prepared->find(xid_data) == info->binlog_xa_prepared->end())
        {
          /*
            External XA txn which was prepared in engine but doesn't exist
            in binlog, have to rollback it. It can be formed by XA PREPARE
            or XA COMMIT ... ONE PHASE.
          */
          goto abort_i;
        }
        else
        {
          if (info->binlog_xa_committed &&
              info->binlog_xa_committed->find(xid_data) != info->binlog_xa_committed->end())
          {
            commit_i= true;
          }
          else if (info->binlog_xa_aborted &&
                   info->binlog_xa_aborted->find(xid_data) != info->binlog_xa_aborted->end())
          {
            goto abort_i;
          }
          else
          {
            if (target_xrt.one_phase_prepared) {
              // this is only possible for 1st startup of cloned instance.
              goto abort_i;
            }

            if (Recovered_xa_transactions::instance().add_prepared_xa_transaction(&target_xrt))
              return true;

            info->found_foreign_xids++;
            if (info->binlog_xa_prepared_engine)
              info->binlog_xa_prepared_engine->insert(xid_data);
            continue;
          }
        }
      }

      if (x && info->dry_run) {
        info->found_my_xids++;
        continue;
      }
      // recovery mode
      if (commit_i ||
          (info->do_binlog_recovery() ?
           info->commit_list->find(x) != info->commit_list->end() :
           tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)) {
#ifndef DBUG_OFF
        char buf[XIDDATASIZE * 4 + 6];  // see xid_to_str
        LogErr(INFORMATION_LEVEL, ER_XA_COMMITTING_XID, target_xid.xid_to_str(buf));
#endif
        hton->commit_by_xid(hton, &target_xid);
      } else {
abort_i:
#ifndef DBUG_OFF
        char buf[XIDDATASIZE * 4 + 6];  // see xid_to_str
        LogErr(INFORMATION_LEVEL, ER_XA_ROLLING_BACK_XID,
               target_xid.xid_to_str(buf));
#endif
        hton->rollback_by_xid(hton, &target_xid);
        if (print_extra_info && info->do_binlog_recovery() == false)
        {
          char buf2[XID::ser_buf_size];
          sql_print_warning("Aborting engine prepared transaction %s in normal recovery(not binlog recovery), which is only expected at 1st startup of a cloned instance.",
              target_xid.serialize(buf2));
        }
      }
    }
  }
  return false;
}

int ha_recover(const memroot_unordered_set<my_xid> *commit_list,
               const xarecover_st::xa_prepared_set_t *xa_prepared,
               const xarecover_st::xa_prepared_set_t* xa_cop,
               const std::set<std::string> *xa_committed,
               const std::set<std::string> *xa_aborted,
               xarecover_st::xa_prepared_set_t *engine_prepared) {

  XA_recover_txn_list txn_list;
  xarecover_st info(&txn_list, commit_list, engine_prepared, xa_prepared,
      xa_cop, xa_committed, xa_aborted);

  DBUG_TRACE;
  info.dry_run =
      (info.do_binlog_recovery() == false && tc_heuristic_recover == TC_HEURISTIC_NOT_USED);

  /* commit_list and tc_heuristic_recover cannot be set both */
  DBUG_ASSERT(info.do_binlog_recovery() == false ||
              tc_heuristic_recover == TC_HEURISTIC_NOT_USED);
  /* if either is set, total_ha_2pc must be set too */
  DBUG_ASSERT(info.dry_run || total_ha_2pc > (ulong)opt_bin_log);

  if (total_ha_2pc <= (ulong)opt_bin_log) return 0;

  if (info.do_binlog_recovery()) LogErr(SYSTEM_LEVEL, ER_XA_STARTING_RECOVERY);

  if (total_ha_2pc > (ulong)opt_bin_log + 1) {
    if (tc_heuristic_recover == TC_HEURISTIC_RECOVER_ROLLBACK) {
      LogErr(ERROR_LEVEL, ER_XA_NO_MULTI_2PC_HEURISTIC_RECOVER);
      return 1;
    }
  } else {
    /*
      If there is only one 2pc capable storage engine it is always safe
      to rollback. This setting will be ignored if we are in automatic
      recovery mode.
    */
    tc_heuristic_recover = TC_HEURISTIC_RECOVER_ROLLBACK;  // forcing ROLLBACK
    info.dry_run = false;
  }


  if (plugin_foreach(nullptr, xarecover_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN,
                     &info)) {
    return 1;
  }

  if (info.do_binlog_recovery())
    g_did_binlog_recovery = 1;

  if (info.found_foreign_xids)
    LogErr(INFORMATION_LEVEL, ER_XA_RECOVER_FOUND_XA_TRX, info.found_foreign_xids);
  if (info.dry_run && info.found_my_xids) {
    LogErr(ERROR_LEVEL, ER_XA_RECOVER_EXPLANATION, info.found_my_xids,
           opt_tc_log_file);
    return 1;
  }
  if (info.do_binlog_recovery()) LogErr(SYSTEM_LEVEL, ER_XA_RECOVERY_DONE);
  return 0;
}

bool xa_trans_force_rollback(THD *thd) {
  /*
    We must reset rm_error before calling ha_rollback(),
    so thd->transaction.xid structure gets reset
    by ha_rollback()/THD::transaction::cleanup().
  */
  thd->get_transaction()->xid_state()->reset_error();
  if (ha_rollback_trans(thd, true)) {
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }
  return false;
}

void cleanup_trans_state(THD *thd) {
  thd->variables.option_bits &= ~OPTION_BEGIN;
  thd->server_status &=
      ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  thd->get_transaction()->reset_unsafe_rollback_flags(Transaction_ctx::SESSION);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  transaction_cache_delete(thd->get_transaction());
}

/**
  Find XA transaction in cache by its xid value.

  @param thd                     Thread context
  @param xid_for_trn_in_recover  xid value to look for in transaction cache
  @param xid_state               State of XA transaction in current session

  @return Pointer to an instance of Transaction_ctx corresponding to a
          xid in argument. If XA transaction not found returns nullptr and
          sets an error in DA to specify a reason of search failure.
*/

static std::shared_ptr<Transaction_ctx>
find_trn_for_recover_and_check_its_state(THD *thd,
                                         xid_t *xid_for_trn_in_recover,
                                         XID_STATE *xid_state) {
  if (!xid_state->has_state(XID_STATE::XA_NOTR)) {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    return nullptr;
  }

  /*
    Note, that there is no race condition here between
    transaction_cache_search and transaction_cache_delete,
    since we always delete our own XID
    (m_xid == thd->transaction().xid_state().m_xid).
    The only case when m_xid != thd->transaction.xid_state.m_xid
    and xid_state->in_thd == 0 is in the function
    transaction_cache_insert_recovery(XID), which is called before starting
    client connections, and thus is always single-threaded.
  */
  std::shared_ptr<Transaction_ctx> transaction =
      transaction_cache_search(xid_for_trn_in_recover);

  XID_STATE *xs = (transaction ? transaction->xid_state() : nullptr);
  if (!xs || !xs->is_in_recovery()) {
    my_error(ER_XAER_NOTA, MYF(0));
    return nullptr;
  } else if (thd->in_active_multi_stmt_transaction()) {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    return nullptr;
  }

  DBUG_ASSERT(xs->is_in_recovery());

  return transaction;
}

/**
  Commit and terminate a XA transaction.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_commit::trans_xa_commit(THD *thd) {
  XID_STATE *xid_state = thd->get_transaction()->xid_state();
  bool res = true;

  DBUG_ASSERT(!thd->slave_thread || xid_state->get_xid()->is_null() ||
              m_xa_opt == XA_ONE_PHASE);

  /* Inform clone handler of XA operation. */
  Clone_handler::XA_Operation xa_guard(thd);
  if (!xid_state->has_same_xid(m_xid)) {
    res = process_external_xa_commit(thd, m_xid, xid_state);
  } else {
    res = process_internal_xa_commit(thd, xid_state);
  }
  return (res);
}

/**
  Acquire Commit metadata lock and all locks acquired by a prepared XA
  transaction before server was shutdown or terminated.

  @param thd           Thread context
  @param external_xid  XID value specified by XA COMMIT or XA ROLLBACK that
                       corresponds to a XA transaction generated outside
                       current session context.

  @retval false        Success
  @retval true         Failure
*/
static bool acquire_mandatory_metadata_locks(THD *thd, xid_t *external_xid) {
  /*
    Acquire metadata lock which will ensure that XA ROLLBACK is blocked
    by active FLUSH TABLES WITH READ LOCK (and vice versa ROLLBACK in
    progress blocks FTWRL). This is to avoid binlog and redo entries
    while a backup is in progress.
  */
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::COMMIT, "", "",
                   MDL_INTENTION_EXCLUSIVE, MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout)) {
    return true;
  }

  /*
    Like in the commit case a failure to store gtid is regarded
    as the resource manager issue.
  */

  if (MDL_context_backup_manager::instance().restore_backup(
          &thd->mdl_context, external_xid->key(), external_xid->key_length())) {
    return true;
  }

  return false;
}

/**
  Handle the statement XA COMMIT for the case when xid corresponds to
  an external XA transaction, that it a transaction generated outside
  current session context.

  @param thd           Thread context
  @param external_xid  XID value specified by XA COMMIT that corresponds to
                       a XA transaction generated outside current session
                       context. In fact, it means that XA COMMIT is run
                       against a XA transaction recovered after server restart.
  @param xid_state     State of XA transaction corresponding to the current
                       session that expected to have the value
                       XID_STATE::XA_NOTR

  @return  operation result
    @retval false  Success
    @retval true   Failure
*/
bool Sql_cmd_xa_commit::process_external_xa_commit(THD *thd,
                                                   xid_t *external_xid,
                                                   XID_STATE *xid_state) {
  std::shared_ptr<Transaction_ctx> transaction =
      find_trn_for_recover_and_check_its_state(thd, external_xid, xid_state);

  if (!transaction) return true;

  XID_STATE *xs = transaction->xid_state();

  DBUG_ASSERT(xs->get_xid()->eq(external_xid));

  /*
    Resumed transaction XA-commit.
    The case deals with the "external" XA-commit by either a slave applier
    or a different than XA-prepared transaction session.
  */
  bool res = xs->xa_trans_rolled_back();

  DEBUG_SYNC(thd, "external_xa_commit_before_acquire_xa_lock");
  /*
    Acquire XID_STATE::m_xa_lock to prevent concurrent running of two
    XA COMMIT/XA ROLLBACK statements. Without acquiring this lock an attempt
    to run two XA COMMIT/XA ROLLBACK statement for the same xid value may lead
    to writing two events for the same xid into the binlog (e.g. twice
    XA COMMIT event, that is an event for XA COMMIT some_xid_value
    followed by an another event XA COMMIT with the same xid value).
    As a consequences, presence of two XA COMMIT/XA ROLLACK statements for
    the same xid value in binlog would break replication.
  */
  std::lock_guard<std::mutex> lk(xs->get_xa_lock());
  /*
    Double check that the XA transaction still does exist since the transaction
    could be removed from the cache by another XA COMMIT/XA ROLLBACK statement
    being executed concurrently from parallel session with the same xid value.
  */
  if (!find_trn_for_recover_and_check_its_state(thd, external_xid, xid_state))
    return true;

  if (acquire_mandatory_metadata_locks(thd, external_xid)) {
    /*
      We can't rollback an XA transaction on lock failure due to
      Innodb redo log and bin log update is involved in rollback.
      Return error to user for a retry.
    */
    my_error(ER_XA_RETRY, MYF(0));
    return true;
  }

  DEBUG_SYNC(thd, "external_xa_commit_after_acquire_commit_lock");

  /* Do not execute gtid wrapper whenever 'res' is true (rm error) */
  bool need_clear_owned_gtid = false;
  bool gtid_error = commit_owned_gtids(thd, true, &need_clear_owned_gtid);
  if (gtid_error) my_error(ER_XA_RBROLLBACK, MYF(0));
  res = res || gtid_error;

  /*
    xs' is_binlogged() is passed through xid_state's member to low-level
    logging routines for deciding how to log.  The same applies to
    Rollback case.
  */
  if (xs->is_binlogged())
    xid_state->set_binlogged();
  else
    xid_state->unset_binlogged();

  res = ha_commit_or_rollback_by_xid(thd, external_xid, !res) || res;

  xid_state->unset_binlogged();

  MDL_context_backup_manager::instance().delete_backup(
      external_xid->key(), external_xid->key_length());

  transaction_cache_delete(transaction.get());
  gtid_state_commit_or_rollback(thd, need_clear_owned_gtid, !gtid_error);

  return res;
}

/**
  Handle the statement XA COMMIT for the case when xid corresponds to
  an internal XA transaction, that is a transaction generated by
  current session context.

  @param thd           Thread context
  @param xid_state     State of XA transaction corresponding to the current
                       session.

  @return  operation result
    @retval false  Success
    @retval true   Failure
*/
bool Sql_cmd_xa_commit::process_internal_xa_commit(THD *thd,
                                                   XID_STATE *xid_state) {
  DBUG_TRACE;
  bool res = false;
  bool gtid_error = false, need_clear_owned_gtid = false;

  if (xid_state->xa_trans_rolled_back()) {
    xa_trans_force_rollback(thd);
    res = thd->is_error();
  } else if (xid_state->has_state(XID_STATE::XA_IDLE) &&
             m_xa_opt == XA_ONE_PHASE) {
    int r = ha_commit_trans(thd, true);
    if ((res = r)) my_error(r == 1 ? ER_XA_RBROLLBACK : ER_XAER_RMERR, MYF(0));
  } else if (xid_state->has_state(XID_STATE::XA_PREPARED) &&
             m_xa_opt == XA_NONE) {
    MDL_request mdl_request;

    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_REQUEST_INIT(&mdl_request, MDL_key::COMMIT, "", "",
                     MDL_INTENTION_EXCLUSIVE, MDL_STATEMENT);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout)) {
      /*
        We can't rollback an XA transaction on lock failure due to
        Innodb redo log and bin log update are involved in rollback.
        Return error to user for a retry.
      */
      my_error(ER_XA_RETRY, MYF(0));
      return true;
    }

    gtid_error = commit_owned_gtids(thd, true, &need_clear_owned_gtid);
    if (gtid_error) {
      res = true;
      /*
        Failure to store gtid is regarded as a unilateral one of the
        resource manager therefore the transaction is to be rolled back.
        The specified error is the same as @c xa_trans_force_rollback.
        The prepared XA will be rolled back along and so will do Gtid state,
        see ha_rollback_trans().

        Todo/fixme: fix binlogging, "XA rollback" event could be missed out.
        Todo/fixme: as to XAER_RMERR, should not it be XA_RBROLLBACK?
                    Rationale: there's no consistency concern after rollback,
                    unlike what XAER_RMERR suggests.
      */
      ha_rollback_trans(thd, true);
      my_error(ER_XAER_RMERR, MYF(0));
    } else {
      DBUG_EXECUTE_IF("simulate_crash_on_commit_xa_trx", DBUG_SUICIDE(););
      DEBUG_SYNC(thd, "trans_xa_commit_after_acquire_commit_lock");

      if (tc_log)
        res = tc_log->commit(thd, /* all */ true);
      else
        res = ha_commit_low(thd, /* all */ true);

      DBUG_EXECUTE_IF("simulate_xa_commit_log_failure", { res = true; });

      if (res)
        my_error(ER_XAER_RMERR, MYF(0));  // todo/fixme: consider to rollback it
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
      else {
        /*
          Since we don't call ha_commit_trans() for prepared transactions,
          we need to explicitly mark the transaction as committed.
        */
        MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
      }

      thd->m_transaction_psi = nullptr;
#endif
    }
  } else {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    return true;
  }

  gtid_state_commit_or_rollback(thd, need_clear_owned_gtid, !gtid_error);
  cleanup_trans_state(thd);

  xid_state->set_state(XID_STATE::XA_NOTR);
  xid_state->unset_binlogged();
  trans_track_end_trx(thd);
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == nullptr || res);
  return res;
}

inline static std::string get_xa_txnid(const xid_t *xid)
{
  // Note that the XID::data string doens't end with '\0'.
  std::string id(xid->get_data(), xid->get_gtrid_length());
  
  return id;
}

bool Sql_cmd_xa_commit::execute(THD *thd) {
  std::string xa_txnid(get_xa_txnid(m_xid));
  bool st = trans_xa_commit(thd);

  if (!st) {
    thd->mdl_context.release_transactional_locks();
    /*
        We've just done a commit, reset transaction
        isolation level and access mode to the session default.
    */
    trans_reset_one_shot_chistics(thd);
    prepared_xa_txnids.del_id(xa_txnid);
    my_ok(thd);
  }
  return st;
}

/**
  Roll back and terminate a XA transaction.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_rollback::trans_xa_rollback(THD *thd) {
  XID_STATE *xid_state = thd->get_transaction()->xid_state();
  bool res = true;

  /* Inform clone handler of XA operation. */
  Clone_handler::XA_Operation xa_guard(thd);
  if (!xid_state->has_same_xid(m_xid)) {
    res = process_external_xa_rollback(thd, m_xid, xid_state);
  } else {
    res = process_internal_xa_rollback(thd, xid_state);
  }
  return (res);
}

/**
  Handle the statement XA ROLLBACK for the case when xid corresponds to
  an external XA transaction, that it a transaction generated outside
  current session context.

  @param thd           Thread context
  @param external_xid  XID value specified by XA ROLLBACK that corresponds to
                       a XA transaction generated outside current session
                       context. In fact, it means that XA ROLLBACK is run
                       against a XA transaction recovered after server restart.
  @param xid_state     State of XA transaction corresponding to the current
                       session that expected to have the value
                       XID_STATE::XA_NOTR

  @return  operation result
    @retval false  Success
    @retval true   Failure
*/
bool Sql_cmd_xa_rollback::process_external_xa_rollback(THD *thd,
                                                       xid_t *external_xid,
                                                       XID_STATE *xid_state) {
  DBUG_TRACE;

  std::shared_ptr<Transaction_ctx> transaction =
      find_trn_for_recover_and_check_its_state(thd, external_xid, xid_state);

  if (!transaction) return true;

  XID_STATE *xs = transaction->xid_state();

  DBUG_ASSERT(xs->get_xid()->eq(external_xid));

  /*
    Acquire XID_STATE::m_xa_lock to prevent concurrent running of two
    XA COMMIT/XA ROLLBACK statements. Without acquiring this lock an attempt
    to run two XA COMMIT/XA ROLLBACK statement for the same xid value may lead
    to writing two events for the same xid into the binlog (e.g. twice
    XA ROLLBACK event, that is an event for XA ROLLBACK some_xid_value
    followed by an another event XA ROLLBACK with the same xid value).
    As a consequences, presence of two XA COMMIT/XA ROLLACK statements for
    the same xid value in binlog would break replication.
  */
  std::lock_guard<std::mutex> lk(xs->get_xa_lock());
  /*
    Double check that the XA transaction still does exist since the transaction
    could be removed from the cache by another XA COMMIT/XA ROLLBACK statement
    being executed concurrently from parallel session with the same xid value.
  */
  if (!find_trn_for_recover_and_check_its_state(thd, external_xid, xid_state))
    return true;

  if (acquire_mandatory_metadata_locks(thd, external_xid)) {
    /*
      We can't rollback an XA transaction on lock failure due to
      Innodb redo log and bin log update is involved in rollback.
      Return error to user for a retry.
    */
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }

  bool need_clear_owned_gtid = false;
  bool gtid_error = commit_owned_gtids(thd, true, &need_clear_owned_gtid);
  if (gtid_error) my_error(ER_XA_RBROLLBACK, MYF(0));
  bool res = xs->xa_trans_rolled_back();

  if (xs->is_binlogged())
    xid_state->set_binlogged();
  else
    xid_state->unset_binlogged();

  res = ha_commit_or_rollback_by_xid(thd, external_xid, false) || res;

  xid_state->unset_binlogged();

  MDL_context_backup_manager::instance().delete_backup(
      external_xid->key(), external_xid->key_length());
  transaction_cache_delete(transaction.get());
  gtid_state_commit_or_rollback(thd, need_clear_owned_gtid, !gtid_error);
  return res || gtid_error;
}

/**
  Handle the statement XA ROLLBACK for the case when xid corresponds to
  an internal XA transaction, that is a transaction generated by
  current session context.

  @param thd           Thread context
  @param xid_state     State of XA transaction corresponding to the current
                       session.

  @return  operation result
    @retval false  Success
    @retval true   Failure
*/
bool Sql_cmd_xa_rollback::process_internal_xa_rollback(THD *thd,
                                                       XID_STATE *xid_state) {
  DBUG_TRACE;

  if (xid_state->has_state(XID_STATE::XA_NOTR) ||
      xid_state->has_state(XID_STATE::XA_ACTIVE)) {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    return true;
  }

  /*
    Acquire metadata lock which will ensure that XA ROLLBACK is blocked
    by active FLUSH TABLES WITH READ LOCK (and vice versa ROLLBACK in
    progress blocks FTWRL). This is to avoid binlog and redo entries
    while a backup is in progress.
  */
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::COMMIT, "", "",
                   MDL_INTENTION_EXCLUSIVE, MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout)) {
    /*
      We can't rollback an XA transaction on lock failure due to
      Innodb redo log and bin log update is involved in rollback.
      Return error to user for a retry.
    */
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }

  bool need_clear_owned_gtid = false;
  bool gtid_error = commit_owned_gtids(thd, true, &need_clear_owned_gtid);
  bool res = xa_trans_force_rollback(thd) || gtid_error;
  gtid_state_commit_or_rollback(thd, need_clear_owned_gtid, !gtid_error);
  // todo: report a bug in that the raised rm_error in this branch
  //       is masked unlike the "external" rollback branch above.
  DBUG_EXECUTE_IF("simulate_xa_rm_error", {
    my_error(ER_XA_RBROLLBACK, MYF(0));
    res = true;
  });

  cleanup_trans_state(thd);

  xid_state->set_state(XID_STATE::XA_NOTR);
  xid_state->unset_binlogged();
  trans_track_end_trx(thd);
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == nullptr);
  return res;
}

bool Sql_cmd_xa_rollback::execute(THD *thd) {
  std::string xa_txnid(get_xa_txnid(m_xid));
  bool st = trans_xa_rollback(thd);

  if (!st) {
    thd->mdl_context.release_transactional_locks();
    /*
      We've just done a rollback, reset transaction
      isolation level and access mode to the session default.
    */
    trans_reset_one_shot_chistics(thd);
    prepared_xa_txnids.del_id(xa_txnid);
    my_ok(thd);
  }

  DBUG_EXECUTE_IF("crash_after_xa_rollback", DBUG_SUICIDE(););

  return st;
}

/**
  Start a XA transaction with the given xid value.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_start::trans_xa_start(THD *thd) {
  XID_STATE *xid_state = thd->get_transaction()->xid_state();
  DBUG_TRACE;

  if (xid_state->has_state(XID_STATE::XA_IDLE) && m_xa_opt == XA_RESUME) {
    bool not_equal = !xid_state->has_same_xid(m_xid);
    if (not_equal)
      my_error(ER_XAER_NOTA, MYF(0));
    else {
      xid_state->set_state(XID_STATE::XA_ACTIVE);
      MYSQL_SET_TRANSACTION_XA_STATE(
          thd->m_transaction_psi,
          (int)thd->get_transaction()->xid_state()->get_state());
    }
    return not_equal;
  }

  bool is_valid_xid = true;

  /* TODO: JOIN is not supported yet. */
  if (m_xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_NOTR))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
    my_error(ER_XAER_OUTSIDE, MYF(0));
  else if (!(is_valid_xid = (strnchr(m_xid->get_data(), '|',
            m_xid->get_bqual_length() + m_xid->get_gtrid_length()) == NULL)))
    // forbid XA txn id containing | because it's used in xa-prepared-ids of Prev_gtid_list to seperate txn ids.
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!trans_begin(thd)) {
    xid_state->start_normal_xa(m_xid);
    MYSQL_SET_TRANSACTION_XID(thd->m_transaction_psi,
                              (const void *)xid_state->get_xid(),
                              (int)xid_state->get_state());
    xid_state->set_xa_type(XID_STATE::XA_EXTERNAL); // started by 'xa start'.
    if (transaction_cache_insert(m_xid, thd->get_transaction())) {
      xid_state->reset();
      trans_rollback(thd);
    }
  }

  return !is_valid_xid || thd->is_error() || !xid_state->has_state(XID_STATE::XA_ACTIVE);
}

bool Sql_cmd_xa_start::execute(THD *thd) {
  bool st = trans_xa_start(thd);

  if (!st) {
    thd->rpl_detach_engine_ha_data();
    my_ok(thd);
  }

  return st;
}

/**
  Put a XA transaction in the IDLE state.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_end::trans_xa_end(THD *thd) {
  XID_STATE *xid_state = thd->get_transaction()->xid_state();
  DBUG_TRACE;

  /* TODO: SUSPEND and FOR MIGRATE are not supported yet. */
  if (m_xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_ACTIVE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(m_xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (!xid_state->xa_trans_rolled_back()) {
    xid_state->set_state(XID_STATE::XA_IDLE);
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
  } else {
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
  }

  return thd->is_error() || !xid_state->has_state(XID_STATE::XA_IDLE);
}

bool Sql_cmd_xa_end::execute(THD *thd) {
  bool st = trans_xa_end(thd);

  if (!st) my_ok(thd);

  return st;
}

/**
  Put a XA transaction in the PREPARED state.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_prepare::trans_xa_prepare(THD *thd) {
  XID_STATE *xid_state = thd->get_transaction()->xid_state();
  DBUG_TRACE;

  if (!xid_state->has_state(XID_STATE::XA_IDLE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(m_xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (thd->slave_thread &&
           is_transaction_empty(
               thd))  // No changes in none of the storage engine
                      // means, filtered statements in the slave
    my_error(ER_XA_REPLICATION_FILTERS,
             MYF(0));  // Empty XA transactions not allowed
  else {
    if ((!thd->slave_thread || opt_log_slave_updates) && opt_bin_log &&
        thd->variables.sql_log_bin)
      thd->durability_property = HA_IGNORE_DURABILITY;

    /*
      Acquire metadata lock which will ensure that XA PREPARE is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa PREPARE in
      progress blocks FTWRL). This is to avoid binlog and redo entries
      while a backup is in progress.
    */
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::COMMIT, "", "",
                     MDL_INTENTION_EXCLUSIVE, MDL_STATEMENT);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout) ||
        ha_prepare(thd)) {
      /*
        Rollback the transaction if lock failed. For ha_prepare() failure
        scenarios, transaction is already rolled back by ha_prepare().
      */
      if (!mdl_request.ticket) ha_rollback_trans(thd, true);

#ifdef HAVE_PSI_TRANSACTION_INTERFACE
      DBUG_ASSERT(thd->m_transaction_psi == NULL);
#endif

      /*
        Reset rm_error in case ha_prepare() returned error,
        so thd->transaction.xid structure gets reset
        by THD::transaction::cleanup().
      */
      thd->get_transaction()->xid_state()->reset_error();
      cleanup_trans_state(thd);
      xid_state->set_state(XID_STATE::XA_NOTR);
      thd->get_transaction()->cleanup();
      my_error(ER_XA_RBROLLBACK, MYF(0));
    } else {
      xid_state->set_state(XID_STATE::XA_PREPARED);
      MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                     (int)xid_state->get_state());
      if (thd->rpl_thd_ctx.session_gtids_ctx().notify_after_xa_prepare(thd))
        LogErr(WARNING_LEVEL, ER_TRX_GTID_COLLECT_REJECT);
    }
  }

  return thd->is_error() || !xid_state->has_state(XID_STATE::XA_PREPARED);
}

bool Sql_cmd_xa_prepare::execute(THD *thd) {
  std::string xa_txnid(get_xa_txnid(m_xid));
  prepared_xa_txnids.add_id(xa_txnid);

  bool st = trans_xa_prepare(thd);

  if (!st) {
    if (!thd->rpl_unflag_detached_engine_ha_data() ||
        !(st = applier_reset_xa_trans(thd)))
      my_ok(thd);
  }

  return st;
}

/**
  Return the list of XID's to a client, the same way SHOW commands do.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure

  @note
    I didn't find in XA specs that an RM cannot return the same XID twice,
    so trans_xa_recover does not filter XID's to ensure uniqueness.
    It can be easily fixed later, if necessary.
*/

bool Sql_cmd_xa_recover::trans_xa_recover(THD *thd) {
  List<Item> field_list;
  Protocol *protocol = thd->get_protocol();

  DBUG_TRACE;

  field_list.push_back(
      new Item_int(NAME_STRING("formatID"), 0, MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("gtrid_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("bqual_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_empty_string("data", XIDDATASIZE * 2 + 2));

  if (thd->send_result_metadata(&field_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    return true;

  mysql_mutex_lock(&LOCK_transaction_cache);

  for (const auto &key_and_value : transaction_cache) {
    Transaction_ctx *transaction = key_and_value.second.get();
    XID_STATE *xs = transaction->xid_state();
    if (xs->has_state(XID_STATE::XA_PREPARED) &&
        (m_xid == NULL || xs->get_xid()->eq(m_xid))) {      
      protocol->start_row();
      xs->store_xid_info(protocol, m_print_xid_as_hex);

      if (protocol->end_row()) {
        mysql_mutex_unlock(&LOCK_transaction_cache);
        return true;
      }
    }
  }

  mysql_mutex_unlock(&LOCK_transaction_cache);
  my_eof(thd);
  return false;
}

/**
  Check if the current user has a privilege to perform XA RECOVER.

  @param thd    Current thread

  @retval false  A user has a privilege to perform XA RECOVER
  @retval true   A user doesn't have a privilege to perform XA RECOVER
*/

bool Sql_cmd_xa_recover::check_xa_recover_privilege(THD *thd) const {
  Security_context *sctx = thd->security_context();

  if (!sctx->has_global_grant(STRING_WITH_LEN("XA_RECOVER_ADMIN")).first) {
    /*
      Report an error ER_XAER_RMERR. A supplementary error
      ER_SPECIFIC_ACCESS_DENIED_ERROR is also reported when
      SHOW WARNINGS is issued. This provides more information
      about the reason for failure.
    */
    my_error(ER_XAER_RMERR, MYF(0));
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "XA_RECOVER_ADMIN");
    return true;
  }

  return false;
}

bool Sql_cmd_xa_recover::execute(THD *thd) {
  bool st = check_xa_recover_privilege(thd) || trans_xa_recover(thd);

  DBUG_EXECUTE_IF("crash_after_xa_recover", { DBUG_SUICIDE(); });

  return st;
}

bool XID_STATE::xa_trans_rolled_back() {
  DBUG_EXECUTE_IF("simulate_xa_rm_error", rm_error = true;);
  if (rm_error) {
    switch (rm_error) {
      case ER_LOCK_WAIT_TIMEOUT:
        my_error(ER_XA_RBTIMEOUT, MYF(0));
        break;
      case ER_LOCK_DEADLOCK:
        my_error(ER_XA_RBDEADLOCK, MYF(0));
        break;
      default:
        my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    xa_state = XID_STATE::XA_ROLLBACK_ONLY;
  }

  return (xa_state == XID_STATE::XA_ROLLBACK_ONLY);
}

bool XID_STATE::check_xa_idle_or_prepared(bool report_error) const {
  if (xa_state == XA_IDLE || xa_state == XA_PREPARED) {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);

    return true;
  }

  return false;
}

bool XID_STATE::check_has_uncommitted_xa() const {
  if (xa_state == XA_IDLE || xa_state == XA_PREPARED ||
      xa_state == XA_ROLLBACK_ONLY) {
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}

bool XID_STATE::check_in_xa(bool report_error) const {
  if (xa_state != XA_NOTR) {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}

void XID_STATE::set_error(THD *thd) {
  if (xa_state != XA_NOTR) rm_error = thd->get_stmt_da()->mysql_errno();
}

void XID_STATE::store_xid_info(Protocol *protocol,
                               bool print_xid_as_hex) const {
  protocol->store_longlong(static_cast<longlong>(m_xid.formatID), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.gtrid_length), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.bqual_length), false);

  if (print_xid_as_hex) {
    /*
      xid_buf contains enough space for 0x followed by HEX representation
      of the binary XID data and one null termination character.
    */
    char xid_buf[XIDDATASIZE * 2 + 2 + 1];

    xid_buf[0] = '0';
    xid_buf[1] = 'x';

    size_t xid_str_len =
        bin_to_hex_str(xid_buf + 2, sizeof(xid_buf) - 2, m_xid.data,
                       m_xid.gtrid_length + m_xid.bqual_length) +
        2;
    protocol->store_string(xid_buf, xid_str_len, &my_charset_bin);
  } else {
    protocol->store_string(m_xid.data, m_xid.gtrid_length + m_xid.bqual_length,
                           &my_charset_bin);
  }
}

#ifndef DBUG_OFF
char *XID::xid_to_str(char *buf) const {
  char *s = buf;
  *s++ = '\'';

  for (int i = 0; i < gtrid_length + bqual_length; i++) {
    /* is_next_dig is set if next character is a number */
    bool is_next_dig = false;
    if (i < XIDDATASIZE) {
      char ch = data[i + 1];
      is_next_dig = (ch >= '0' && ch <= '9');
    }
    if (i == gtrid_length) {
      *s++ = '\'';
      if (bqual_length) {
        *s++ = '.';
        *s++ = '\'';
      }
    }
    uchar c = static_cast<uchar>(data[i]);
    if (c < 32 || c > 126) {
      *s++ = '\\';
      /*
        If next character is a number, write current character with
        3 octal numbers to ensure that the next number is not seen
        as part of the octal number
      */
      if (c > 077 || is_next_dig) *s++ = _dig_vec_lower[c >> 6];
      if (c > 007 || is_next_dig) *s++ = _dig_vec_lower[(c >> 3) & 7];
      *s++ = _dig_vec_lower[c & 7];
    } else {
      if (c == '\'' || c == '\\') *s++ = '\\';
      *s++ = c;
    }
  }
  *s++ = '\'';
  *s = 0;
  return buf;
}
#endif

static inline std::string to_string(const XID &xid) {
  return std::string(pointer_cast<const char *>(xid.key()), xid.key_length());
}

/**
  Callback that is called to do cleanup.

  @param transaction  pointer to free
*/

void transaction_free_hash::operator()(Transaction_ctx *transaction) const {
  // Only time it's allocated is during recovery process.
  if (transaction->xid_state()->is_in_recovery()) delete transaction;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_transaction_cache;

static PSI_mutex_info transaction_cache_mutexes[] = {
    {&key_LOCK_transaction_cache, "LOCK_transaction_cache", PSI_FLAG_SINGLETON,
     0, PSI_DOCUMENT_ME}};

static void init_transaction_cache_psi_keys(void) {
  const char *category = "sql";
  int count;

  count = static_cast<int>(array_elements(transaction_cache_mutexes));
  mysql_mutex_register(category, transaction_cache_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */

bool transaction_cache_init() {
#ifdef HAVE_PSI_INTERFACE
  init_transaction_cache_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_transaction_cache, &LOCK_transaction_cache,
                   MY_MUTEX_INIT_FAST);
  inited = true;
  return false;
}

void transaction_cache_free() {
  if (inited) {
    transaction_cache.clear();
    mysql_mutex_destroy(&LOCK_transaction_cache);
  }
}

/**
  Search information about XA transaction by a XID value.

  @param xid    Pointer to a XID structure that identifies a XA transaction.

  @return  pointer to a Transaction_ctx that describes the whole transaction
           including XA-specific information (XID_STATE).
    @retval  NULL     failure
    @retval  != NULL  success
*/

static std::shared_ptr<Transaction_ctx> transaction_cache_search(XID *xid) {
  std::shared_ptr<Transaction_ctx> res{nullptr};
  mysql_mutex_lock(&LOCK_transaction_cache);

  const auto it = transaction_cache.find(to_string(*xid));
  if (it != transaction_cache.end()) res = it->second;

  mysql_mutex_unlock(&LOCK_transaction_cache);
  return res;
}

/**
  Insert information about XA transaction into a cache indexed by XID.

  @param xid     Pointer to a XID structure that identifies a XA transaction.
  @param transaction
                 Pointer to Transaction object that is inserted.

  @return  operation result
    @retval  false   success or a cache already contains XID_STATE
                     for this XID value
    @retval  true    failure
*/

bool transaction_cache_insert(XID *xid, Transaction_ctx *transaction) {
  mysql_mutex_lock(&LOCK_transaction_cache);
  std::shared_ptr<Transaction_ctx> ptr(transaction, transaction_free_hash());
  bool res = !transaction_cache.emplace(to_string(*xid), std::move(ptr)).second;
  mysql_mutex_unlock(&LOCK_transaction_cache);
  if (res) {
    my_error(ER_XAER_DUPID, MYF(0));
  }
  return res;
}

static bool create_and_insert_new_transaction(XID *xid, bool is_binlogged_arg) {
  Transaction_ctx *transaction = new (std::nothrow) Transaction_ctx();
  XID_STATE *xs;

  if (!transaction) {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), sizeof(Transaction_ctx));
    return true;
  }
  xs = transaction->xid_state();
  xs->start_recovery_xa(xid, is_binlogged_arg);

  return !transaction_cache
              .emplace(to_string(*xs->get_xid()),
                       std::shared_ptr<Transaction_ctx>(
                           transaction, transaction_free_hash()))
              .second;
}

bool transaction_cache_detach(Transaction_ctx *transaction) {
  bool res = false;
  XID_STATE *xs = transaction->xid_state();
  XID xid = *(xs->get_xid());
  bool was_logged = xs->is_binlogged();

  DBUG_ASSERT(xs->has_state(XID_STATE::XA_PREPARED));

  mysql_mutex_lock(&LOCK_transaction_cache);

  DBUG_ASSERT(transaction_cache.count(to_string(xid)) != 0);
  transaction_cache.erase(to_string(xid));
  res = create_and_insert_new_transaction(&xid, was_logged);

  mysql_mutex_unlock(&LOCK_transaction_cache);

  return res;
}

/**
  Insert information about XA transaction being recovered into a cache
  indexed by XID.

  @param xid     Pointer to a XID structure that identifies a XA transaction.

  @return  operation result
    @retval  false   success or a cache already contains Transaction_ctx
                     for this XID value
    @retval  true    failure
*/

bool transaction_cache_insert_recovery(XID *xid) {
  mysql_mutex_lock(&LOCK_transaction_cache);

  if (transaction_cache.count(to_string(*xid))) {
    mysql_mutex_unlock(&LOCK_transaction_cache);
    return false;
  }

  /*
    It's assumed that XA transaction was binlogged before the server
    shutdown. If --log-bin has changed since that from OFF to ON, XA
    COMMIT or XA ROLLBACK of this transaction may be logged alone into
    the binary log.
  */
  bool res = create_and_insert_new_transaction(xid, true);

  mysql_mutex_unlock(&LOCK_transaction_cache);

  return res;
}

void transaction_cache_delete(Transaction_ctx *transaction) {
  mysql_mutex_lock(&LOCK_transaction_cache);
  const auto it =
      transaction_cache.find(to_string(*transaction->xid_state()->get_xid()));
  if (it != transaction_cache.end() && it->second.get() == transaction)
    transaction_cache.erase(it);
  mysql_mutex_unlock(&LOCK_transaction_cache);
}

/**
  The function restores previously saved storage engine transaction context.

  @param     thd     Thread context
*/
static void attach_native_trx(THD *thd) {
  Ha_trx_info *ha_info =
      thd->get_transaction()->ha_trx_info(Transaction_ctx::SESSION);
  Ha_trx_info *ha_info_next;

  if (ha_info) {
    for (; ha_info; ha_info = ha_info_next) {
      handlerton *hton = ha_info->ht();
      reattach_engine_ha_data_to_thd(thd, hton);
      ha_info_next = ha_info->next();
      ha_info->reset();
    }
  } else {
    /*
      Although the current `Ha_trx_info` object is null, we need to make sure
      that the data engine plugins have the oportunity to attach their internal
      transactions and clean up the session.
     */
    thd->rpl_reattach_engine_ha_data();
  }
}

std::string &XID_STATE::to_string(std::string &str) const
{
  char buf[384];
  int retlen = snprintf(buf, sizeof(buf),
      "xid: %s, state: %s, type: %s, %s, %s, %u",get_xa_xid(),
           state_name(), get_xa_type_str(), in_recovery ? "in_recovery" : "",
           m_is_binlogged ? "binlogged" : "", rm_error);
  DBUG_ASSERT(retlen < (int)sizeof(buf));
  str= buf;
  return str;
}


/*
  @param [in] buf serialized xid string
  @retval true on format error; false on success.
*/
bool deserialize_xid(const char *buf, long &fmt, long &gln, long &bln,
                     char *dat)
{
  if (ddc_mode) {
    size_t bufl = strlen(buf);
    if (bufl < 3 || buf[0] != '\'' || buf[bufl - 1] != '\'')
      return true;
    memcpy(dat, buf + 1, bufl - 2);
    gln = bufl - 2;
    bln = 0;
    return false;
  }

  if (!(buf[0] == 'X' && buf[1] == '\''))
    return true;

  int i= 2, start, j= 0;

  for (start= i; buf[i] && buf[i] != '\''; i+= 2, j++)
  {
    dat[j]= buf[i] - (isdigit(buf[i]) ? '0' : 'a' - 10);
    dat[j] <<= 4;
    dat[j] |= (buf[i + 1] - (isdigit(buf[i + 1]) ? '0' : 'a' - 10));
  }

  gln= (i - start) / 2;

  if (!buf[i])
    return true;
  i++;
  if (!(buf[i] == ',' && buf[i + 1] == 'X' && buf[i + 2]== '\''))
    return true;
  i+=3;

  for (start= i; buf[i] && buf[i] != '\''; i+= 2, j++)
  {
    dat[j]= buf[i] - (isdigit(buf[i]) ? '0' : 'a' - 10);
    dat[j] <<= 4;
    dat[j] |= (buf[i + 1] - (isdigit(buf[i + 1]) ? '0' : 'a' - 10));
  }

  bln= i - 4 - gln;

  if (buf[i + 1] != ',' || !buf[i + 2])
    return true;
  i+= 2;
  sscanf(buf + i, "%lu", &fmt);
  return false;
}

/**
  This is a specific to "slave" applier collection of standard cleanup
  actions to reset XA transaction states at the end of XA prepare rather than
  to do it at the transaction commit, see @c ha_commit_one_phase.
  THD of the slave applier is dissociated from a transaction object in engine
  that continues to exist there.

  @param  thd current thread
  @return the value of is_error()
*/

bool applier_reset_xa_trans(THD *thd) {
  DBUG_TRACE;
  Transaction_ctx *trn_ctx = thd->get_transaction();
  XID_STATE *xid_state = trn_ctx->xid_state();

  /*
    Return error is not an option as XA is in prepared state and
    connection is gone. Log the error and continue.
  */
  if (MDL_context_backup_manager::instance().create_backup(
          &thd->mdl_context, xid_state->get_xid()->key(),
          xid_state->get_xid()->key_length())) {
    LogErr(ERROR_LEVEL, ER_XA_CANT_CREATE_MDL_BACKUP);
  }
  /*
    In the following the server transaction state gets reset for
    a slave applier thread similarly to xa_commit logics
    except commit does not run.
  */
  thd->variables.option_bits &= ~OPTION_BEGIN;
  trn_ctx->reset_unsafe_rollback_flags(Transaction_ctx::STMT);
  thd->server_status &= ~SERVER_STATUS_IN_TRANS;
  /* Server transaction ctx is detached from THD */
  transaction_cache_detach(trn_ctx);
  xid_state->reset();
  /*
     The current engine transactions is detached from THD, and
     previously saved is restored.
  */
  attach_native_trx(thd);
  trn_ctx->set_ha_trx_info(Transaction_ctx::SESSION, NULL);
  trn_ctx->set_no_2pc(Transaction_ctx::SESSION, false);
  trn_ctx->cleanup();
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
  thd->m_transaction_psi = NULL;
#endif
  thd->mdl_context.release_transactional_locks();
  /*
    On client sessions a XA PREPARE will always be followed by a XA COMMIT
    or a XA ROLLBACK, and both statements will reset the tx isolation level
    and access mode when the statement is finishing a transaction.

    For replicated workload it is possible to have other transactions between
    the XA PREPARE and the XA [COMMIT|ROLLBACK].

    So, if the slave applier changed the current transaction isolation level,
    it needs to be restored to the session default value after having the
    XA transaction prepared.
  */
  trans_reset_one_shot_chistics(thd);

  return thd->is_error();
}

/**
  The function detaches existing storage engines transaction
  context from thd. Backup area to save it is provided to low level
  storage engine function.

  is invoked by plugin_foreach() after
  trans_xa_start() for each storage engine.

  @param[in,out]     thd     Thread context
  @param             plugin  Reference to handlerton

  @return    false   on success, true otherwise.
*/

bool detach_native_trx(THD *thd, plugin_ref plugin, void *) {
  DBUG_TRACE;
  handlerton *hton = plugin_data<handlerton *>(plugin);

  if (hton->replace_native_transaction_in_thd) {
    /* Ensure any active backup engine ha_data won't be overwritten */
    DBUG_ASSERT(!thd->get_ha_data(hton->slot)->ha_ptr_backup);

    hton->replace_native_transaction_in_thd(
        thd, NULL, &thd->get_ha_data(hton->slot)->ha_ptr_backup);
  }

  return false;
}

bool reattach_native_trx(THD *thd, plugin_ref plugin, void *) {
  DBUG_TRACE;
  handlerton *hton = plugin_data<handlerton *>(plugin);

  if (hton->replace_native_transaction_in_thd) {
    /* restore the saved original engine transaction's link with thd */
    void **trx_backup = &thd->get_ha_data(hton->slot)->ha_ptr_backup;

    hton->replace_native_transaction_in_thd(thd, *trx_backup, NULL);
    *trx_backup = NULL;
  }
  return false;
}

int Prepared_xa_txnids::parse(const char *str, Txnids_t &ids)
{
  char *p= const_cast<char *>(str), *q= 0;

  while ((q= strchr(p, '|')))
  {
    *q= '\0';
    std::string s(p);
    ids.insert(s);
    *q='|';
    p= q+1;
  }

  if (p != q && p && *p)
  {
    std::string s(p);
    ids.insert(s);
  }

  // An empty list is totally OK and possible.
  return 0;
}

void Prepared_xa_txnids::from_recovery(Txnids_t &prepared)
{
  for (Txnids_t::iterator j= prepared.begin(); j != prepared.end(); ++j)
  {
    add_id(*j);
  }
}

void Prepared_xa_txnids::
from_recovery(Txnids_t &prepared, const Txnids_t &committed,
              const Txnids_t &aborted)
{
  Txnids_t::iterator j;

  for (Txnids_t::iterator i= committed.begin(); i != committed.end(); ++i)
    if ((j= prepared.find(*i)) != prepared.end())
      prepared.erase(j);

  for (Txnids_t::iterator i= aborted.begin(); i != aborted.end(); ++i)
    if ((j= prepared.find(*i)) != prepared.end())
      prepared.erase(j);
  for (j= prepared.begin(); j != prepared.end(); ++j)
  {
    add_id(*j);
  }
}

/*
  this function is only called when rotating a log and LOCK_LOG is held by the
  same thread, thus there can't be concurrent add/del to/from this object, we
  are always dumping a consistent and complete bunch of IDs.
*/
void Prepared_xa_txnids::serialize(std::string &id)
{
  id.reserve(1024*32);

  for (size_t i = 0; i < NSLOTS; i++)
  {
    m_slots[i].serialize(id);
  }
}

void Prepared_xa_txnids::Slot::serialize(std::string &id)
{
  pthread_mutex_lock(&mutex);
  int cnt = 0;
  for (Txnids_t::iterator i= txnids.begin(); i != txnids.end(); ++i, cnt++)
  {
    if (cnt > 0 || id.length() > 0)
      id+= "|";
    id+= *i;
  }
  pthread_mutex_unlock(&mutex);
}
const char *strnchr(const char *str, char c, size_t n)
{
  for (size_t i = 0; i < n && str[i] != '\0'; i++)
    if (str[i] == c)
      return str + i;
  return NULL;
}

#include "sql/xa_utils.cc"
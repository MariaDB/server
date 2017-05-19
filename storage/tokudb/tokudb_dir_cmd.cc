/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "hatoku_hton.h"
#include "tokudb_dir_cmd.h"
#include "my_dbug.h"
#include "sql_base.h"

#include <vector>
#include <string>

namespace tokudb {

const char tokens_delimiter = ' ';
const char tokens_escape_delimiter_char = '\\';

static int MDL_and_TDC(THD *thd,
                       const char *db,
                       const char *table,
                       const dir_cmd_callbacks &cb) {
    int error;
    LEX_STRING db_arg;
    LEX_STRING table_arg;

    db_arg.str = const_cast<char *>(db);
    db_arg.length = strlen(db);;
    table_arg.str = const_cast<char *>(table);
    table_arg.length = strlen(table);
    Table_ident table_ident(thd, db_arg, table_arg, true);;
    thd->lex->select_lex.add_table_to_list(
        thd, &table_ident, NULL, 1, TL_UNLOCK, MDL_EXCLUSIVE, 0, 0, 0);
    /* The lock will be released at the end of mysq_execute_command() */
    error = lock_table_names(thd,
                             thd->lex->select_lex.table_list.first,
                             NULL,
                             thd->variables.lock_wait_timeout,
                             0);
    if (error) {
        if (cb.set_error)
            cb.set_error(thd,
                         error,
                         "Can't lock table '%s.%s'",
                         db,
                         table);
        return error;
    }
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, db, table, false);
    return error;
}

static bool parse_db_and_table(const char *dname,
                              std::string /*out*/ &db_name,
                              std::string /*out*/ &table_name) {
    const char *begin;
    const char *end;
    const char *db_name_begin;
    const char *db_name_end;

    begin = strchr(dname, '/');
    if (!begin)
        return false;
    ++begin;
    end = strchr(begin, '/');
    if (!end)
        return false;

    db_name_begin = begin;
    db_name_end = end;

    begin = end + 1;

    end = strchr(begin, '-');
    if (!end)
        return false;

    if (strncmp(end, "-main", strlen("-main")) &&
        strncmp(end, "-status", strlen("-status")) &&
        strncmp(end, "-key", strlen("-key")))
        return false;

    db_name.assign(db_name_begin, db_name_end);
    table_name.assign(begin, end);

    return true;
}

static int attach(THD *thd,
                   const std::string &dname,
                   const std::string &iname,
                   const dir_cmd_callbacks &cb) {
    int error;
    DB_TXN* txn = NULL;
    DB_TXN *parent_txn = NULL;
    tokudb_trx_data *trx = NULL;

    std::string db_name;
    std::string table_name;

    if (parse_db_and_table(dname.c_str(), db_name, table_name)) {
        error = MDL_and_TDC(thd, db_name.c_str(), table_name.c_str(), cb);
        if (error)
            goto cleanup;
    }

    trx = (tokudb_trx_data *) thd_get_ha_data(thd, tokudb_hton);
    if (trx && trx->sub_sp_level)
        parent_txn = trx->sub_sp_level;
    error = txn_begin(db_env, parent_txn, &txn, 0, thd);
    if (error)
        goto cleanup;

    error = db_env->dirtool_attach(db_env,
                                   txn,
                                   dname.c_str(),
                                   iname.c_str());
cleanup:
    if (txn) {
        if (error) {
            abort_txn(txn);
        }
        else {
            commit_txn(txn, 0);
        }
    }
    return error;
}

static int detach(THD *thd,
                  const std::string &dname,
                  const dir_cmd_callbacks &cb) {
    int error;
    DB_TXN* txn = NULL;
    DB_TXN *parent_txn = NULL;
    tokudb_trx_data *trx = NULL;

    std::string db_name;
    std::string table_name;

    if (parse_db_and_table(dname.c_str(), db_name, table_name)) {
        error = MDL_and_TDC(thd, db_name.c_str(), table_name.c_str(), cb);
        if (error)
            goto cleanup;
    }

    trx = (tokudb_trx_data *) thd_get_ha_data(thd, tokudb_hton);
    if (trx && trx->sub_sp_level)
        parent_txn = trx->sub_sp_level;
    error = txn_begin(db_env, parent_txn, &txn, 0, thd);
    if (error)
        goto cleanup;

    error = db_env->dirtool_detach(db_env,
                                   txn,
                                   dname.c_str());
cleanup:
    if (txn) {
        if (error) {
            abort_txn(txn);
        }
        else {
            commit_txn(txn, 0);
        }
    }
    return error;
}

static int move(THD *thd,
                const std::string &old_dname,
                const std::string &new_dname,
                const dir_cmd_callbacks &cb) {
    int error;
    DB_TXN* txn = NULL;
    DB_TXN *parent_txn = NULL;
    tokudb_trx_data *trx = NULL;

    std::string db_name;
    std::string table_name;

    if (parse_db_and_table(old_dname.c_str(), db_name, table_name)) {
        error = MDL_and_TDC(thd, db_name.c_str(), table_name.c_str(), cb);
        if (error)
            goto cleanup;
    }

    trx = (tokudb_trx_data *) thd_get_ha_data(thd, tokudb_hton);
    if (trx && trx->sub_sp_level)
        parent_txn = trx->sub_sp_level;
    error = txn_begin(db_env, parent_txn, &txn, 0, thd);
    if (error)
        goto cleanup;

    error = db_env->dirtool_move(db_env,
                                 txn,
                                 old_dname.c_str(),
                                 new_dname.c_str());
cleanup:
    if (txn) {
        if (error) {
            abort_txn(txn);
        }
        else {
            commit_txn(txn, 0);
        }
    }
    return error;
}

static void tokenize(const char *cmd_str,
                     std::vector<std::string> /*out*/ &tokens) {
    DBUG_ASSERT(cmd_str);

    bool was_escape = false;
    const char *token_begin = cmd_str;
    const char *token_end = token_begin;

    while (*token_end) {
      if (*token_end == tokens_escape_delimiter_char) {
        was_escape = true;
      }
      else if (*token_end == tokens_delimiter) {
        if (was_escape)
          was_escape = false;
        else {
          if (token_begin == token_end)
            ++token_begin;
          else {
            tokens.push_back(std::string(token_begin, token_end));
            token_begin = token_end + 1;
          }
        }
      }
      else {
        was_escape = false;
      }
      ++token_end;
    }

    if (token_begin != token_end)
      tokens.push_back(std::string(token_begin, token_end));
}

void process_dir_cmd(THD *thd,
                     const char *cmd_str,
                     const dir_cmd_callbacks &cb) {

    DBUG_ASSERT(thd);
    DBUG_ASSERT(cmd_str);

    std::vector<std::string> tokens;
    tokenize(cmd_str, tokens);

    if (tokens.empty())
        return;

    const std::string &cmd = tokens[0];

    if (!cmd.compare("attach")) {
        if (tokens.size() != 3) {
            if (cb.set_error)
                cb.set_error(thd,
                             EINVAL,
                             "attach command requires two arguments");
        }
        else {
            int r = attach(thd, tokens[1], tokens[2], cb);
            if (r && cb.set_error)
                cb.set_error(thd, r, "Attach command error");
        }
    }
    else if (!cmd.compare("detach")) {
        if (tokens.size() != 2) {
            if (cb.set_error)
                cb.set_error(thd,
                             EINVAL,
                             "detach command requires one argument");
        }
        else {
            int r = detach(thd, tokens[1], cb);
            if (r && cb.set_error)
                cb.set_error(thd, r, "detach command error");
        }
    }
    else if (!cmd.compare("move")) {
        if (tokens.size() != 3) {
            if (cb.set_error)
                cb.set_error(thd,
                             EINVAL,
                             "move command requires two arguments");
        }
        else {
            int r = move(thd, tokens[1], tokens[2], cb);
            if (r && cb.set_error)
                cb.set_error(thd, r, "move command error");
        }
    }
    else {
        if (cb.set_error)
            cb.set_error(thd,
                         ENOENT,
                         "Unknown command '%s'",
                         cmd.c_str());
    }

    return;
};


} // namespace tokudb

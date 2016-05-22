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

// Modify inames in the tokudb.directory
//
// Requirements:
// The directory containing the tokudb environment is passed as a parameter.
// Needs the log*.tokulog* crash recovery log files.
// Needs a clean shutdown in the recovery log.
// Needs the tokudb.* metadata files.
//
// Effects:
// Modifies the inames in tokudb.directory.
// Creates a new crash recovery log.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <db.h>

static int fixup_directory(DB_ENV *env, DB_TXN *txn, DB *db) {
    db = db;
    int r;
    DBC *c = NULL;
    r = env->get_cursor_for_directory(env, txn, &c);
    assert(r == 0);

    DBT key = { }; key.flags = DB_DBT_REALLOC;
    DBT val = { }; val.flags = DB_DBT_REALLOC;
    while (1) {
        r = c->c_get(c, &key, &val, DB_NEXT);
        if (r == DB_NOTFOUND)
            break;
        printf("dname=%s oldiname=%s ", (char *) key.data, (char *) val.data);
        assert(r == 0);

        // TODO insert iname match and replace here
        char newiname[strlen((char *) val.data) + 32];
        sprintf(newiname, "%s", (char *) val.data);
        printf("newiname=%s\n", newiname);
        // TODO end patch
        
        // this modifies the iname in the row
        DBT newval = {}; newval.data = newiname; newval.size = strlen(newiname)+1;
        r = db->put(db, txn, &key, &newval, 0);
        assert(r == 0);
    }
    free(key.data);
    free(val.data);

    r = c->c_close(c);
    assert(r == 0);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "datadir name missing\n");
        return 1;
    }
    char *datadir = argv[1];

    // open the env
    int r;
    DB_ENV *env = NULL;
    r = db_env_create(&env, 0);
    assert(r == 0);

    env->set_errfile(env, stderr);
    r = env->open(env, datadir, DB_INIT_LOCK+DB_INIT_MPOOL+DB_INIT_TXN+DB_INIT_LOG + DB_PRIVATE+DB_CREATE, 
                  S_IRWXU+S_IRWXG+S_IRWXO);
    // open will fail if the recovery log was not cleanly shutdown
    assert(r == 0);

    // use a single txn to cover all of the status file changes
    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0);
    assert(r == 0);

    DB *db = env->get_db_for_directory(env);
    assert(db != NULL);

    r = fixup_directory(env, txn, db);
    assert(r == 0);

    r = txn->commit(txn, 0);
    assert(r == 0);

    // close the env
    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}

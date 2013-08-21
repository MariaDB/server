/* Copyright 2011 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

//#include <mysqld.h>
#include <sql_class.h>
//#include <sql_plugin.h>
//#include <set_var.h>

#include "wsrep_mysqld.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* This file is about checking for correctness of mysql configuration options */

struct opt
{
    const char* const name;
    const char*       value;
};

/* A list of options to check.
 * At first we assume default values and then see if they are changed on CLI or
 * in my.cnf */
static struct opt opts[] =
{
    { "wsrep_slave_threads",     "1" }, // mysqld.cc
    { "bind_address",      "0.0.0.0" }, // mysqld.cc
    { "wsrep_sst_method",    "rsync" }, // mysqld.cc
    { "wsrep_sst_receive_address","AUTO"}, // mysqld.cc
    { "binlog_format",         "ROW" }, // mysqld.cc
    { "wsrep_provider",       "none" }, // mysqld.cc
    { "query_cache_type",        "0" }, // mysqld.cc
    { "query_cache_size",        "0" }, // mysqld.cc
    { "locked_in_memory",        "0" }, // mysqld.cc
    { "wsrep_cluster_address",   "0" }, // mysqld.cc
    { "locks_unsafe_for_binlog", "0" }, // ha_innodb.cc
    { "autoinc_lock_mode",       "1" }, // ha_innodb.cc
    { 0, 0 }
};

enum
{
    WSREP_SLAVE_THREADS,
    BIND_ADDRESS,
    WSREP_SST_METHOD,
    WSREP_SST_RECEIVE_ADDRESS,
    BINLOG_FORMAT,
    WSREP_PROVIDER,
    QUERY_CACHE_TYPE,
    QUERY_CACHE_SIZE,
    LOCKED_IN_MEMORY,
    WSREP_CLUSTER_ADDRESS,
    LOCKS_UNSAFE_FOR_BINLOG,
    AUTOINC_LOCK_MODE
};


/* A class to make a copy of argv[] vector */
struct argv_copy
{
    int    const argc_;
    char**       argv_;

    argv_copy (int const argc, const char* const argv[]) :
        argc_ (argc),
        argv_ (reinterpret_cast<char**>(calloc(argc_, sizeof(char*))))
    {
        if (argv_)
        {
            for (int i = 0; i < argc_; ++i)
            {
                argv_[i] = strdup(argv[i]);

                if (!argv_[i])
                {
                    argv_free (); // free whatever bee allocated
                    return;
                }
            }
        }
    }

    ~argv_copy () { argv_free (); }

private:
    argv_copy (const argv_copy&);
    argv_copy& operator= (const argv_copy&);

    void argv_free()
    {
        if (argv_)
        {
            for (int i = 0; (i < argc_) && argv_[i] ; ++i) free (argv_[i]);
            free (argv_);
            argv_ = 0;
        }
    }
};

/* a short corresponding to '--' byte sequence */
static short const long_opt_prefix ('-' + ('-' << 8));

/* Normalizes long options to have '_' instead of '-' */
static int
normalize_opts (argv_copy& a)
{
    if (a.argv_)
    {
        for (int i = 0; i < a.argc_; ++i)
        {
            char* ptr = a.argv_[i];
            if (long_opt_prefix == *(short*)ptr) // long option
            {
                ptr += 2;
                const char* end = strchr(ptr, '=');

                if (!end) end = ptr + strlen(ptr);

                for (; ptr != end; ++ptr) if ('-' == *ptr) *ptr = '_';
            }
        }

        return 0;
    }

    return EINVAL;
}

/* Find required options in the argument list and change their values */
static int
find_opts (argv_copy& a, struct opt* const opts)
{
    for (int i = 0; i < a.argc_; ++i)
    {
        char* ptr = a.argv_[i] + 2; // we're interested only in long options

        struct opt* opt = opts;
        for (; 0 != opt->name; ++opt)
        {
            if (!strstr(ptr, opt->name)) continue; // try next option

            /* 1. try to find value after the '=' */
            opt->value = strchr(ptr, '=') + 1;

            /* 2. if no '=', try next element in the argument vector */
            if (reinterpret_cast<void*>(1) == opt->value)
            {
                /* also check that the next element is not an option itself */
                if (i + 1 < a.argc_ && *(a.argv_[i + 1]) != '-')
                {
                    ++i;
                    opt->value = a.argv_[i];
                }
                else opt->value = ""; // no value supplied (like boolean opt)
            }

            break; // option found, break inner loop
        }
    }

    return 0;
}

/* Parses string for an integer. Returns 0 on success. */
int get_long_long (const struct opt& opt, long long* const val, int const base)
{
    const char* const str = opt.value;

    if ('\0' != *str)
    {
        char* endptr;

        *val = strtoll (str, &endptr, base);

        if ('k' == *endptr || 'K' == *endptr) 
        { 
            *val *= 1024L;
            endptr++;
        } 
        else if ('m' == *endptr || 'M' == *endptr) 
        {
            *val *= 1024L * 1024L;
            endptr++;
        }
        else if ('g' == *endptr || 'G' == *endptr) 
        {
            *val *= 1024L * 1024L * 1024L;
            endptr++;
        }

        if ('\0' == *endptr) return 0; // the whole string was a valid integer
    }

    WSREP_ERROR ("Bad value for *%s: '%s'. Should be integer.",
                 opt.name, opt.value);

    return EINVAL;
}

/* This is flimzy coz hell knows how mysql interprets boolean strings...
 * and, no, I'm not going to become versed in how mysql handles options -
 * I'd rather sing.

 Aha, http://dev.mysql.com/doc/refman/5.1/en/dynamic-system-variables.html:
 Variables that have a type of “boolean” can be set to 0, 1, ON or OFF. (If you
 set them on the command line or in an option file, use the numeric values.)

 So it is '0' for FALSE, '1' or empty string for TRUE

 */
int get_bool (const struct opt& opt, bool* const val)
{
    const char* str = opt.value;

    while (isspace(*str)) ++str; // skip initial whitespaces

    ssize_t str_len = strlen(str);
    switch (str_len)
    {
    case 0:
        *val = true;
        return 0;
    case 1:
        if ('0' == *str || '1' == *str)
        {
            *val = ('1' == *str);
            return 0;
        }
    }

    WSREP_ERROR ("Bad value for *%s: '%s'. Should be '0', '1' or empty string.",
                 opt.name, opt.value);

    return EINVAL;
}

static int
check_opts (int const argc, const char* const argv[], struct opt opts[])
{
    /* First, make a copy of argv to be able to manipulate it */
    argv_copy a(argc, argv);

    if (!a.argv_)
    {
        WSREP_ERROR ("Could not copy argv vector: not enough memory.");
        return ENOMEM;
    }

    int err = normalize_opts (a);
    if (err)
    {
        WSREP_ERROR ("Failed to normalize options.");
        return err;
    }

    err = find_opts (a, opts);
    if (err)
    {
        WSREP_ERROR ("Failed to parse options.");
        return err;
    }

    /* At this point we have updated default values in our option list to
       what has been specified on the command line / my.cnf */

    long long slave_threads;
    err = get_long_long (opts[WSREP_SLAVE_THREADS], &slave_threads, 10);
    if (err) return err;

    int rcode = 0;

    if (slave_threads > 1)
        /* Need to check AUTOINC_LOCK_MODE and LOCKS_UNSAFE_FOR_BINLOG */
    {
        long long autoinc_lock_mode;
        err = get_long_long (opts[AUTOINC_LOCK_MODE], &autoinc_lock_mode, 10);
        if (err) return err;

        bool locks_unsafe_for_binlog;
        err = get_bool (opts[LOCKS_UNSAFE_FOR_BINLOG],&locks_unsafe_for_binlog);
        if (err) return err;

        if (autoinc_lock_mode != 2)
        {
            WSREP_ERROR ("Parallel applying (wsrep_slave_threads > 1) requires"
                         " innodb_autoinc_lock_mode = 2.");
            rcode = EINVAL;
        }
    }

    long long query_cache_size, query_cache_type;
    if ((err = get_long_long (opts[QUERY_CACHE_SIZE], &query_cache_size, 10)))
        return err;
    if ((err = get_long_long (opts[QUERY_CACHE_TYPE], &query_cache_type, 10)))
        return err;

    if (0 != query_cache_size && 0 != query_cache_type)
    {
        WSREP_ERROR ("Query cache is not supported (size=%lld type=%lld)",
                     query_cache_size, query_cache_type);
        rcode = EINVAL;
    }

    bool locked_in_memory;
    err = get_bool (opts[LOCKED_IN_MEMORY], &locked_in_memory);
    if (err) { WSREP_ERROR("get_bool error: %s", strerror(err)); return err; }
    if (locked_in_memory)
    {
        WSREP_ERROR ("Memory locking is not supported (locked_in_memory=%s)",
                     locked_in_memory ? "ON" : "OFF");
        rcode = EINVAL;
    }

    if (!strcasecmp(opts[WSREP_SST_METHOD].value,"mysqldump"))
    {
        if (!strcasecmp(opts[BIND_ADDRESS].value, "127.0.0.1") ||
            !strcasecmp(opts[BIND_ADDRESS].value, "localhost"))
        {
            WSREP_ERROR ("wsrep_sst_method is set to 'mysqldump' yet "
                         "mysqld bind_address is set to '%s', which makes it "
                         "impossible to receive state transfer from another "
                         "node, since mysqld won't accept such connections. "
                         "If you wish to use mysqldump state transfer method, "
                         "set bind_address to allow mysql client connections "
                         "from other cluster members (e.g. 0.0.0.0).",
                         opts[BIND_ADDRESS].value);
            rcode = EINVAL;
        }
    }
    else
    {
        // non-mysqldump SST requires wsrep_cluster_address on startup
        if (strlen(opts[WSREP_CLUSTER_ADDRESS].value) == 0)
        {
            WSREP_ERROR ("%s SST method requires wsrep_cluster_address to be "
                         "configured on startup.",opts[WSREP_SST_METHOD].value);
            rcode = EINVAL;
        }
    }

    if (strcasecmp(opts[WSREP_SST_RECEIVE_ADDRESS].value, "AUTO"))
    {
        if (!strncasecmp(opts[WSREP_SST_RECEIVE_ADDRESS].value,
                         "127.0.0.1", strlen("127.0.0.1"))       ||
            !strncasecmp(opts[WSREP_SST_RECEIVE_ADDRESS].value,
                         "localhost", strlen("localhost")))
        {
            WSREP_WARN  ("wsrep_sst_receive_address is set to '%s' which "
                         "makes it impossible for another host to reach this "
                         "one. Please set it to the address which this node "
                         "can be connected at by other cluster members.",
                         opts[WSREP_SST_RECEIVE_ADDRESS].value);
//            rcode = EINVAL;
        }
    }

    if (strcasecmp(opts[WSREP_PROVIDER].value, "none"))
    {
        if (strcasecmp(opts[BINLOG_FORMAT].value, "ROW"))
        {
            WSREP_ERROR ("Only binlog_format = 'ROW' is currently supported. "
                         "Configured value: '%s'. Please adjust your "
                         "configuration.", opts[BINLOG_FORMAT].value);

            rcode = EINVAL;
        }
    }

    return rcode;
}

int
wsrep_check_opts (int const argc, char* const* const argv)
{
    return check_opts (argc, argv, opts);
}


/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

#include <exception>
#include <string.h>

#include <db.h>

namespace ftcxx {

    class ft_exception : public std::exception {
        int _code;

        static const char *ft_strerror(int code) {
            switch (code) {
            case DB_RUNRECOVERY:
                return "DB_RUNRECOVERY";
            case DB_KEYEXIST:
                return "DB_KEYEXIST";
            case DB_LOCK_DEADLOCK:
                return "DB_LOCK_DEADLOCK";
            case DB_LOCK_NOTGRANTED:
                return "DB_LOCK_NOTGRANTED";
            case DB_NOTFOUND:
                return "DB_NOTFOUND";
            case DB_SECONDARY_BAD:
                return "DB_SECONDARY_BAD";
            case DB_DONOTINDEX:
                return "DB_DONOTINDEX";
            case DB_BUFFER_SMALL:
                return "DB_BUFFER_SMALL";
            case DB_BADFORMAT:
                return "DB_BADFORMAT";
            case TOKUDB_OUT_OF_LOCKS:
                return "TOKUDB_OUT_OF_LOCKS";
            case TOKUDB_SUCCEEDED_EARLY:
                return "TOKUDB_SUCCEEDED_EARLY";
            case TOKUDB_FOUND_BUT_REJECTED:
                return "TOKUDB_FOUND_BUT_REJECTED";
            case TOKUDB_USER_CALLBACK_ERROR:
                return "TOKUDB_USER_CALLBACK_ERROR";
            case TOKUDB_DICTIONARY_TOO_OLD:
                return "TOKUDB_DICTIONARY_TOO_OLD";
            case TOKUDB_DICTIONARY_TOO_NEW:
                return "TOKUDB_DICTIONARY_TOO_NEW";
            case TOKUDB_DICTIONARY_NO_HEADER:
                return "TOKUDB_DICTIONARY_NO_HEADER";
            case TOKUDB_CANCELED:
                return "TOKUDB_CANCELED";
            case TOKUDB_NO_DATA:
                return "TOKUDB_NO_DATA";
            case TOKUDB_ACCEPT:
                return "TOKUDB_ACCEPT";
            case TOKUDB_MVCC_DICTIONARY_TOO_NEW:
                return "TOKUDB_MVCC_DICTIONARY_TOO_NEW";
            case TOKUDB_UPGRADE_FAILURE:
                return "TOKUDB_UPGRADE_FAILURE";
            case TOKUDB_TRY_AGAIN:
                return "TOKUDB_TRY_AGAIN";
            case TOKUDB_NEEDS_REPAIR:
                return "TOKUDB_NEEDS_REPAIR";
            case TOKUDB_CURSOR_CONTINUE:
                return "TOKUDB_CURSOR_CONTINUE";
            case TOKUDB_BAD_CHECKSUM:
                return "TOKUDB_BAD_CHECKSUM";
            case TOKUDB_HUGE_PAGES_ENABLED:
                return "TOKUDB_HUGE_PAGES_ENABLED";
            case TOKUDB_OUT_OF_RANGE:
                return "TOKUDB_OUT_OF_RANGE";
            case TOKUDB_INTERRUPTED:
                return "TOKUDB_INTERRUPTED";
            default:
                return "unknown ft error";
            }
        }

    public:
        ft_exception(int c) : _code(c) {}

        int code() const noexcept {
            return _code;
        }

        virtual const char *what() const noexcept {
            return ft_strerror(_code);
        }
    };

    class system_exception : public std::exception {
        int _code;

    public:
        system_exception(int c) : _code(c) {}

        int code() const noexcept {
            return _code;
        }

        virtual const char *what() const noexcept {
            return strerror(_code);
        }
    };

    inline void handle_ft_retval(int r) {
        if (r == 0) {
            return;
        }
        if (r < 0) {
            throw ft_exception(r);
        }
        if (r > 0) {
            throw system_exception(r);
        }
    }

} // namespace ftcxx

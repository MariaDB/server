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

#include <map>
#include <memory>
#include <string>

#include <db.h>

#include "db_env.hpp"

namespace ftcxx {

    void DBEnv::get_status(DBEnv::Status &status, fs_redzone_state &redzone_state, uint64_t &env_panic, std::string &panic_string) const{
        uint64_t num_rows;
        int r = _env->get_engine_status_num_rows(_env, &num_rows);
        handle_ft_retval(r);

        std::unique_ptr<TOKU_ENGINE_STATUS_ROW_S[]> buf(new TOKU_ENGINE_STATUS_ROW_S[num_rows]);
        char panic_string_buf[1<<12];
        panic_string_buf[0] = '\0';

        r = _env->get_engine_status(_env, buf.get(), num_rows, &num_rows,
                                    &redzone_state,
                                    &env_panic, panic_string_buf, sizeof panic_string_buf,
                                    toku_engine_status_include_type(TOKU_ENGINE_STATUS | TOKU_GLOBAL_STATUS));
        handle_ft_retval(r);

        panic_string = std::string(panic_string_buf);

        for (uint64_t i = 0; i < num_rows; ++i) {
            status[buf[i].keyname] = buf[i];
        }
    }

} // namespace ftcxx

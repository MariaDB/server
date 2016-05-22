/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
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

#include "portability/toku_portability.h"

#include "ft/msg.h"
#include "ft/txn/xids.h"
#include "util/dbt.h"

ft_msg::ft_msg(const DBT *key, const DBT *val, enum ft_msg_type t, MSN m, XIDS x) :
    _key(key ? *key : toku_empty_dbt()),
    _val(val ? *val : toku_empty_dbt()),
    _type(t), _msn(m), _xids(x) {
}

ft_msg ft_msg::deserialize_from_rbuf(struct rbuf *rb, XIDS *x, bool *is_fresh) {
    const void *keyp, *valp;
    uint32_t keylen, vallen;
    enum ft_msg_type t = (enum ft_msg_type) rbuf_char(rb);
    *is_fresh = rbuf_char(rb);
    MSN m = rbuf_MSN(rb);
    toku_xids_create_from_buffer(rb, x);
    rbuf_bytes(rb, &keyp, &keylen);
    rbuf_bytes(rb, &valp, &vallen);

    DBT k, v;
    return ft_msg(toku_fill_dbt(&k, keyp, keylen), toku_fill_dbt(&v, valp, vallen), t, m, *x);
}

ft_msg ft_msg::deserialize_from_rbuf_v13(struct rbuf *rb, MSN m, XIDS *x) {
    const void *keyp, *valp;
    uint32_t keylen, vallen;
    enum ft_msg_type t = (enum ft_msg_type) rbuf_char(rb);
    toku_xids_create_from_buffer(rb, x);
    rbuf_bytes(rb, &keyp, &keylen);
    rbuf_bytes(rb, &valp, &vallen);

    DBT k, v;
    return ft_msg(toku_fill_dbt(&k, keyp, keylen), toku_fill_dbt(&v, valp, vallen), t, m, *x);
}

const DBT *ft_msg::kdbt() const {
    return &_key;
}

const DBT *ft_msg::vdbt() const {
    return &_val;
}

enum ft_msg_type ft_msg::type() const {
    return _type;
}

MSN ft_msg::msn() const {
    return _msn;
}

XIDS ft_msg::xids() const {
    return _xids;
}

size_t ft_msg::total_size() const {
    // Must store two 4-byte lengths
    static const size_t key_val_overhead = 8;

    // 1 byte type, 1 byte freshness, then 8 byte MSN
    static const size_t msg_overhead = 2 + sizeof(MSN);

    static const size_t total_overhead = key_val_overhead + msg_overhead;

    const size_t keyval_size = _key.size + _val.size;
    const size_t xids_size = toku_xids_get_serialize_size(xids());
    return total_overhead + keyval_size + xids_size;
}

void ft_msg::serialize_to_wbuf(struct wbuf *wb, bool is_fresh) const {
    wbuf_nocrc_char(wb, (unsigned char) _type);
    wbuf_nocrc_char(wb, (unsigned char) is_fresh);
    wbuf_MSN(wb, _msn);
    wbuf_nocrc_xids(wb, _xids);
    wbuf_nocrc_bytes(wb, _key.data, _key.size);
    wbuf_nocrc_bytes(wb, _val.data, _val.size);
}


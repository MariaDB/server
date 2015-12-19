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

#pragma once

#include "ft/ft.h"
#include "ft/serialize/block_table.h"

size_t toku_serialize_ft_size(struct ft_header *h);
void toku_serialize_ft_to(int fd, struct ft_header *h, block_table *bt, CACHEFILE cf);
void toku_serialize_ft_to_wbuf(struct wbuf *wbuf, struct ft_header *h, DISKOFF translation_location_on_disk, DISKOFF translation_size_on_disk);
void toku_serialize_descriptor_contents_to_fd(int fd, DESCRIPTOR desc, DISKOFF offset);
void toku_serialize_descriptor_contents_to_wbuf(struct wbuf *wb, DESCRIPTOR desc);

int toku_deserialize_ft_from(int fd, LSN max_acceptable_lsn, FT *ft);

// TODO rename
int deserialize_ft_from_fd_into_rbuf(int fd,
                                     toku_off_t offset_of_header,
                                     struct rbuf *rb,
                                     uint64_t *checkpoint_count,
                                     LSN *checkpoint_lsn,
                                     uint32_t *version_p);

// used by verify
// TODO rename
int deserialize_ft_versioned(int fd, struct rbuf *rb, FT *ft, uint32_t version);

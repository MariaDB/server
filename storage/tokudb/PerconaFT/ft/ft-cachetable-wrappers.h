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

#include "ft/cachetable/cachetable.h"
#include "ft/ft-internal.h"
#include "ft/node.h"

/**
 * Put an empty node (that is, no fields filled) into the cachetable. 
 * In the process, write dependent nodes out for checkpoint if 
 * necessary.
 */
void
cachetable_put_empty_node_with_dep_nodes(
    FT ft,
    uint32_t num_dependent_nodes,
    FTNODE* dependent_nodes,
    BLOCKNUM* name, //output
    uint32_t* fullhash, //output
    FTNODE* result
    );

/**
 * Create a new ftnode with specified height and number of children.
 * In the process, write dependent nodes out for checkpoint if 
 * necessary.
 */
void
create_new_ftnode_with_dep_nodes(
    FT ft,
    FTNODE *result,
    int height,
    int n_children,
    uint32_t num_dependent_nodes,
    FTNODE* dependent_nodes
    );

/**
 * Create a new ftnode with specified height
 * and children. 
 * Used for test functions only.
 */
void
toku_create_new_ftnode (
    FT_HANDLE t,
    FTNODE *result,
    int height,
    int n_children
    );

// This function returns a pinned ftnode to the caller.
int
toku_pin_ftnode_for_query(
    FT_HANDLE ft_h,
    BLOCKNUM blocknum,
    uint32_t fullhash,
    UNLOCKERS unlockers,
    ANCESTORS ancestors,
    const pivot_bounds &bounds,
    ftnode_fetch_extra *bfe,
    bool apply_ancestor_messages, // this bool is probably temporary, for #3972, once we know how range query estimates work, will revisit this
    FTNODE *node_p,
    bool* msgs_applied
    );

// Pins an ftnode without dependent pairs
void toku_pin_ftnode(
    FT ft,
    BLOCKNUM blocknum,
    uint32_t fullhash,
    ftnode_fetch_extra *bfe,
    pair_lock_type lock_type,
    FTNODE *node_p,
    bool move_messages
    );

// Pins an ftnode with dependent pairs
// Unlike toku_pin_ftnode_for_query, this function blocks until the node is pinned.
void toku_pin_ftnode_with_dep_nodes(
    FT ft,
    BLOCKNUM blocknum,
    uint32_t fullhash,
    ftnode_fetch_extra *bfe,
    pair_lock_type lock_type,
    uint32_t num_dependent_nodes,
    FTNODE *dependent_nodes,
    FTNODE *node_p,
    bool move_messages
    );

/**
 * This function may return a pinned ftnode to the caller, if pinning is cheap.
 * If the node is already locked, or is pending a checkpoint, the node is not pinned and -1 is returned.
 */
int toku_maybe_pin_ftnode_clean(FT ft, BLOCKNUM blocknum, uint32_t fullhash, pair_lock_type lock_type, FTNODE *nodep);

/**
 * Effect: Unpin an ftnode.
 */
void toku_unpin_ftnode(FT ft, FTNODE node);
void toku_unpin_ftnode_read_only(FT ft, FTNODE node);

// Effect: Swaps pair values of two pinned nodes
void toku_ftnode_swap_pair_values(FTNODE nodea, FTNODE nodeb);

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

#include <db.h>

#include "portability/toku_pthread.h"
#include "portability/toku_stdint.h"
#include "portability/toku_stdlib.h"

// RBTree(Red-black tree) with max hole sizes for subtrees.

// This is a tentative data struct to improve the block allocation time
// complexity from the linear time to the log time. Please be noted this DS only
// supports first-fit for now. It is actually easier to do it with
// best-fit.(just
// sort by size).

// RBTree is a classic data struct with O(log(n)) for insertion, deletion and
// search. Many years have seen its efficiency.

// a *hole* is the representation of an available BlockPair for allocation.
// defined as (start_address,size) or (offset, size) interchangably.

// each node has a *label* to indicate a pair of the max hole sizes for its
// subtree.

// We are implementing a RBTree with max hole sizes for subtree. It is a red
// black tree that is sorted by the start_address but also labeld with the max
// hole sizes of the subtrees.

//        [(6,3)]  -> [(offset, size)], the hole
//        [{2,5}]  -> [{mhs_of_left, mhs_of_right}], the label
/*        /     \           */
// [(0, 1)]    [(10,  5)]
// [{0, 2}]    [{0,   0}]
/*        \                 */
//       [(3,  2)]
//       [{0,  0}]
// request of allocation size=2 goes from root to [(3,2)].

// above example shows a simplified RBTree_max_holes.
// it is easier to tell the search time is O(log(n)) as we can make a decision
// on each descent until we get to the target.

// the only question is if we can keep the maintenance cost low -- and i think
// it is not a problem becoz an insertion/deletion is only going to update the
// max_hole_sizes of the nodes along the path from the root to the node to be
// deleted/inserted. The path can be cached and search is anyway O(log(n)).

// unlike the typical rbtree, Tree has to handle the inserts and deletes
// with more care: an allocation that triggers the delete might leave some
// unused space which we can simply update the start_addr and size without
// worrying overlapping. An free might not only mean the insertion but also
// *merging* with the adjacent holes.

namespace MhsRbTree {

#define offset_t uint64_t
    enum class EColor { RED, BLACK };
    enum class EDirection { NONE = 0, LEFT, RIGHT };

    // I am a bit tired of fixing overflow/underflow, just quickly craft some
    // int
    // class that has an infinity-like max value and prevents overflow and
    // underflow. If you got a file offset larger than MHS_MAX_VAL, it is not
    // a problem here. :-/  - JYM
    class OUUInt64 {
       public:
        static const uint64_t MHS_MAX_VAL = 0xffffffffffffffff;
        OUUInt64() : _value(0) {}
        OUUInt64(uint64_t s) : _value(s) {}
        OUUInt64(const OUUInt64& o) : _value(o._value) {}
        bool operator<(const OUUInt64 &r) const {
            invariant(!(_value == MHS_MAX_VAL && r.ToInt() == MHS_MAX_VAL));
            return _value < r.ToInt();
        }
        bool operator>(const OUUInt64 &r) const {
            invariant(!(_value == MHS_MAX_VAL && r.ToInt() == MHS_MAX_VAL));
            return _value > r.ToInt();
        }
        bool operator<=(const OUUInt64 &r) const {
            invariant(!(_value == MHS_MAX_VAL && r.ToInt() == MHS_MAX_VAL));
            return _value <= r.ToInt();
        }
        bool operator>=(const OUUInt64 &r) const {
            invariant(!(_value == MHS_MAX_VAL && r.ToInt() == MHS_MAX_VAL));
            return _value >= r.ToInt();
        }
        OUUInt64 operator+(const OUUInt64 &r) const {
            if (_value == MHS_MAX_VAL || r.ToInt() == MHS_MAX_VAL) {
                OUUInt64 tmp(MHS_MAX_VAL);
                return tmp;
            } else {
                // detecting overflow
                invariant((MHS_MAX_VAL - _value) >= r.ToInt());
                uint64_t plus = _value + r.ToInt();
                OUUInt64 tmp(plus);
                return tmp;
            }
        }
        OUUInt64 operator-(const OUUInt64 &r) const {
            invariant(r.ToInt() != MHS_MAX_VAL);
            if (_value == MHS_MAX_VAL) {
                return *this;
            } else {
                invariant(_value >= r.ToInt());
                uint64_t minus = _value - r.ToInt();
                OUUInt64 tmp(minus);
                return tmp;
            }
        }
        OUUInt64 operator-=(const OUUInt64 &r) {
            if (_value != MHS_MAX_VAL) {
                invariant(r.ToInt() != MHS_MAX_VAL);
                invariant(_value >= r.ToInt());
                _value -= r.ToInt();
            }
            return *this;
        }
        OUUInt64 operator+=(const OUUInt64 &r) {
            if (_value != MHS_MAX_VAL) {
                if (r.ToInt() == MHS_MAX_VAL) {
                    _value = MHS_MAX_VAL;
                } else {
                    invariant((MHS_MAX_VAL - _value) >= r.ToInt());
                    this->_value += r.ToInt();
                }
            }
            return *this;
        }
        bool operator==(const OUUInt64 &r) const {
            return _value == r.ToInt();
        }
        bool operator!=(const OUUInt64 &r) const {
            return _value != r.ToInt();
        }
        OUUInt64 operator=(const OUUInt64 &r) {
            _value = r.ToInt();
            return *this;
        }
        uint64_t ToInt() const { return _value; }

       private:
        uint64_t _value;
    };

    class Node {
       public:
        class BlockPair {
           public:
            OUUInt64 _offset;
            OUUInt64 _size;

            BlockPair() : _offset(0), _size(0) {}
            BlockPair(uint64_t o, uint64_t s) : _offset(o), _size(s) {}
            BlockPair(OUUInt64 o, OUUInt64 s) : _offset(o), _size(s) {}
            BlockPair(const BlockPair &o)
                : _offset(o._offset), _size(o._size) {}
            BlockPair& operator=(const BlockPair&) = default;

            int operator<(const BlockPair &rhs) const {
                return _offset < rhs._offset;
            }
            int operator<(const uint64_t &o) const { return _offset < o; }
        };

        struct Pair {
            uint64_t _left;
            uint64_t _right;
            Pair(uint64_t l, uint64_t r) : _left(l), _right(r) {}
        };

        EColor _color;
        BlockPair _hole;
        Pair _label;
        Node *_left;
        Node *_right;
        Node *_parent;

        Node(EColor c,
             Node::BlockPair h,
             Pair lb,
             Node *l,
             Node *r,
             Node *p)
            : _color(c),
              _hole(h),
              _label(lb),
              _left(l),
              _right(r),
              _parent(p) {}
    };

    class Tree {
       private:
        Node *_root;
        uint64_t _align;

       public:
        Tree();
        Tree(uint64_t);
        ~Tree();

        void PreOrder();
        void InOrder();
        void PostOrder();
        // immutable operations
        Node *SearchByOffset(uint64_t addr);
        Node *SearchFirstFitBySize(uint64_t size);

        Node *MinNode();
        Node *MaxNode();

        Node *Successor(Node *);
        Node *Predecessor(Node *);

        // mapped from tree_allocator::free_block
        int Insert(Node::BlockPair pair);
        // mapped from tree_allocator::alloc_block
        uint64_t Remove(size_t size);
        // mapped from tree_allocator::alloc_block_after

        void RawRemove(uint64_t offset);
        void Destroy();
        // print the tree
        void Dump();
        // validation
        // balance
        void ValidateBalance();
        void ValidateInOrder(Node::BlockPair *);
        void InOrderVisitor(void (*f)(void *, Node *, uint64_t), void *);
        void ValidateMhs();

       private:
        void PreOrder(Node *node) const;
        void InOrder(Node *node) const;
        void PostOrder(Node *node) const;
        Node *SearchByOffset(Node *node, offset_t addr) const;
        Node *SearchFirstFitBySize(Node *node, size_t size) const;

        Node *MinNode(Node *node);
        Node *MaxNode(Node *node);

        // rotations to fix up. we will have to update the labels too.
        void LeftRotate(Node *&root, Node *x);
        void RightRotate(Node *&root, Node *y);

        int Insert(Node *&root, Node::BlockPair pair);
        int InsertFixup(Node *&root, Node *node);

        void RawRemove(Node *&root, Node *node);
        uint64_t Remove(Node *&root, Node *node, size_t size);
        void RawRemoveFixup(Node *&root, Node *node, Node *parent);

        void Destroy(Node *&tree);
        void Dump(Node *tree, Node::BlockPair pair, EDirection dir);
        void RecalculateMhs(Node *node);
        void IsNewNodeMergable(Node *, Node *, Node::BlockPair, bool *, bool *);
        void AbsorbNewNode(Node *, Node *, Node::BlockPair, bool, bool, bool);
        Node *SearchFirstFitBySizeHelper(Node *x, uint64_t size);

        Node *SuccessorHelper(Node *y, Node *x);

        Node *PredecessorHelper(Node *y, Node *x);

        void InOrderVisitor(Node *,
                            void (*f)(void *, Node *, uint64_t),
                            void *,
                            uint64_t);
        uint64_t ValidateMhs(Node *);

        uint64_t EffectiveSize(Node *);
// mixed with some macros.....
#define rbn_parent(r) ((r)->_parent)
#define rbn_color(r) ((r)->_color)
#define rbn_is_red(r) ((r)->_color == EColor::RED)
#define rbn_is_black(r) ((r)->_color == EColor::BLACK)
#define rbn_set_black(r)     \
    do {                     \
        (r)->_color = EColor::BLACK; \
    } while (0)
#define rbn_set_red(r)     \
    do {                   \
        (r)->_color = EColor::RED; \
    } while (0)
#define rbn_set_parent(r, p) \
    do {                     \
        (r)->_parent = (p);  \
    } while (0)
#define rbn_set_color(r, c) \
    do {                    \
        (r)->_color = (c);  \
    } while (0)
#define rbn_set_offset(r)         \
    do {                          \
        (r)->_hole._offset = (c); \
    } while (0)
#define rbn_set_size(r, c)      \
    do {                        \
        (r)->_hole._size = (c); \
    } while (0)
#define rbn_set_left_mhs(r, c)   \
    do {                         \
        (r)->_label._left = (c); \
    } while (0)
#define rbn_set_right_mhs(r, c)   \
    do {                          \
        (r)->_label._right = (c); \
    } while (0)
#define rbn_size(r) ((r)->_hole._size)
#define rbn_offset(r) ((r)->_hole._offset)
#define rbn_key(r) ((r)->_hole._offset)
#define rbn_left_mhs(r) ((r)->_label._left)
#define rbn_right_mhs(r) ((r)->_label._right)
#define mhs_of_subtree(y) \
    (std::max(std::max(rbn_left_mhs(y), rbn_right_mhs(y)), EffectiveSize(y)))
    };

}  // namespace MhsRbTree

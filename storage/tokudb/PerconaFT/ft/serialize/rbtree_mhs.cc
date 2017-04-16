/*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
    MERCHANTABILIT or FITNESS FOR A PARTICULAR PURPOSE.  See the
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

#include "ft/serialize/rbtree_mhs.h"
#include "portability/toku_assert.h"
#include "portability/toku_portability.h"
#include <algorithm>

namespace MhsRbTree {

    Tree::Tree() : _root(NULL), _align(1) {}

    Tree::Tree(uint64_t align) : _root(NULL), _align(align) {}

    Tree::~Tree() { Destroy(); }

    void Tree::PreOrder(Node *tree) const {
        if (tree != NULL) {
            fprintf(stderr, "%" PRIu64 " ", rbn_offset(tree).ToInt());
            PreOrder(tree->_left);
            PreOrder(tree->_right);
        }
    }

    void Tree::PreOrder() { PreOrder(_root); }

    void Tree::InOrder(Node *tree) const {
        if (tree != NULL) {
            InOrder(tree->_left);
            fprintf(stderr, "%" PRIu64 " ", rbn_offset(tree).ToInt());
            InOrder(tree->_right);
        }
    }

    // yeah, i only care about in order visitor. -Jun
    void Tree::InOrderVisitor(Node *tree,
                              void (*f)(void *, Node *, uint64_t),
                              void *extra,
                              uint64_t depth) {
        if (tree != NULL) {
            InOrderVisitor(tree->_left, f, extra, depth + 1);
            f(extra, tree, depth);
            InOrderVisitor(tree->_right, f, extra, depth + 1);
        }
    }

    void Tree::InOrderVisitor(void (*f)(void *, Node *, uint64_t),
                              void *extra) {
        InOrderVisitor(_root, f, extra, 0);
    }

    void Tree::InOrder() { InOrder(_root); }

    void Tree::PostOrder(Node *tree) const {
        if (tree != NULL) {
            PostOrder(tree->_left);
            PostOrder(tree->_right);
            fprintf(stderr, "%" PRIu64 " ", rbn_offset(tree).ToInt());
        }
    }

    void Tree::PostOrder() { PostOrder(_root); }

    Node *Tree::SearchByOffset(uint64_t offset) {
        Node *x = _root;
        while ((x != NULL) && (rbn_offset(x).ToInt() != offset)) {
            if (offset < rbn_offset(x).ToInt())
                x = x->_left;
            else
                x = x->_right;
        }

        return x;
    }

    // mostly for testing
    Node *Tree::SearchFirstFitBySize(uint64_t size) {
        if (EffectiveSize(_root) < size && rbn_left_mhs(_root) < size &&
            rbn_right_mhs(_root) < size) {
            return nullptr;
        } else {
            return SearchFirstFitBySizeHelper(_root, size);
        }
    }

    Node *Tree::SearchFirstFitBySizeHelper(Node *x, uint64_t size) {
        if (EffectiveSize(x) >= size) {
            // only possible to go left
            if (rbn_left_mhs(x) >= size)
                return SearchFirstFitBySizeHelper(x->_left, size);
            else
                return x;
        }
        if (rbn_left_mhs(x) >= size)
            return SearchFirstFitBySizeHelper(x->_left, size);

        if (rbn_right_mhs(x) >= size)
            return SearchFirstFitBySizeHelper(x->_right, size);

        // this is an invalid state
        Dump();
        ValidateBalance();
        ValidateMhs();
        invariant(0);
        return NULL;
    }

    Node *Tree::MinNode(Node *tree) {
        if (tree == NULL)
            return NULL;

        while (tree->_left != NULL)
            tree = tree->_left;
        return tree;
    }

    Node *Tree::MinNode() { return MinNode(_root); }

    Node *Tree::MaxNode(Node *tree) {
        if (tree == NULL)
            return NULL;

        while (tree->_right != NULL)
            tree = tree->_right;
        return tree;
    }

    Node *Tree::MaxNode() { return MaxNode(_root); }

    Node *Tree::SuccessorHelper(Node *y, Node *x) {
        while ((y != NULL) && (x == y->_right)) {
            x = y;
            y = y->_parent;
        }
        return y;
    }
    Node *Tree::Successor(Node *x) {
        if (x->_right != NULL)
            return MinNode(x->_right);

        Node *y = x->_parent;
        return SuccessorHelper(y, x);
    }

    Node *Tree::PredecessorHelper(Node *y, Node *x) {
        while ((y != NULL) && (x == y->_left)) {
            x = y;
            y = y->_parent;
        }

        return y;
    }
    Node *Tree::Predecessor(Node *x) {
        if (x->_left != NULL)
            return MaxNode(x->_left);

        Node *y = x->_parent;
        return SuccessorHelper(y, x);
    }

    /*
    *      px                              px
    *     /                               /
    *    x                               y
    *   /  \      --(left rotation)-->  / \               #
    *  lx   y                          x  ry
    *     /   \                       /  \
    *    ly   ry                      lx  ly
    *  max_hole_size updates are pretty local
    */

    void Tree::LeftRotate(Node *&root, Node *x) {
        Node *y = x->_right;

        x->_right = y->_left;
        rbn_right_mhs(x) = rbn_left_mhs(y);

        if (y->_left != NULL)
            y->_left->_parent = x;

        y->_parent = x->_parent;

        if (x->_parent == NULL) {
            root = y;
        } else {
            if (x->_parent->_left == x) {
                x->_parent->_left = y;
            } else {
                x->_parent->_right = y;
            }
        }
        y->_left = x;
        rbn_left_mhs(y) = mhs_of_subtree(x);

        x->_parent = y;
    }

    /*            py                               py
     *           /                                /
     *          y                                x
     *         /  \      --(right rotate)-->    /  \                     #
     *        x   ry                           lx   y
     *       / \                                   / \                   #
     *      lx  rx                                rx  ry
     *
     */

    void Tree::RightRotate(Node *&root, Node *y) {
        Node *x = y->_left;

        y->_left = x->_right;
        rbn_left_mhs(y) = rbn_right_mhs(x);

        if (x->_right != NULL)
            x->_right->_parent = y;

        x->_parent = y->_parent;

        if (y->_parent == NULL) {
            root = x;
        } else {
            if (y == y->_parent->_right)
                y->_parent->_right = x;
            else
                y->_parent->_left = x;
        }

        x->_right = y;
        rbn_right_mhs(x) = mhs_of_subtree(y);
        y->_parent = x;
    }

    // walking from this node up to update the mhs info
    // whenver there is change on left/right mhs or size we should recalculate.
    // prerequisit: the children of the node are mhs up-to-date.
    void Tree::RecalculateMhs(Node *node) {
        uint64_t *p_node_mhs = 0;
        Node *parent = node->_parent;

        if (!parent)
            return;

        uint64_t max_mhs = mhs_of_subtree(node);
        if (node == parent->_left) {
            p_node_mhs = &rbn_left_mhs(parent);
        } else if (node == parent->_right) {
            p_node_mhs = &rbn_right_mhs(parent);
        } else {
            return;
        }
        if (*p_node_mhs != max_mhs) {
            *p_node_mhs = max_mhs;
            RecalculateMhs(parent);
        }
    }

    void Tree::IsNewNodeMergable(Node *pred,
                                 Node *succ,
                                 Node::BlockPair pair,
                                 bool *left_merge,
                                 bool *right_merge) {
        if (pred) {
            OUUInt64 end_of_pred = rbn_size(pred) + rbn_offset(pred);
            if (end_of_pred < pair._offset)
                *left_merge = false;
            else {
                invariant(end_of_pred == pair._offset);
                *left_merge = true;
            }
        }
        if (succ) {
            OUUInt64 begin_of_succ = rbn_offset(succ);
            OUUInt64 end_of_node = pair._offset + pair._size;
            if (end_of_node < begin_of_succ) {
                *right_merge = false;
            } else {
                invariant(end_of_node == begin_of_succ);
                *right_merge = true;
            }
        }
    }

    void Tree::AbsorbNewNode(Node *pred,
                             Node *succ,
                             Node::BlockPair pair,
                             bool left_merge,
                             bool right_merge,
                             bool is_right_child) {
        invariant(left_merge || right_merge);
        if (left_merge && right_merge) {
            // merge to the succ
            if (!is_right_child) {
                rbn_size(succ) += pair._size;
                rbn_offset(succ) = pair._offset;
                // merge to the pred
                rbn_size(pred) += rbn_size(succ);
                // to keep the invariant of the tree -no overlapping holes
                rbn_offset(succ) += rbn_size(succ);
                rbn_size(succ) = 0;
                RecalculateMhs(succ);
                RecalculateMhs(pred);
                // pred dominates succ. this is going to
                // update the pred labels separately.
                // remove succ
                RawRemove(_root, succ);
            } else {
                rbn_size(pred) += pair._size;
                rbn_offset(succ) = rbn_offset(pred);
                rbn_size(succ) += rbn_size(pred);
                rbn_offset(pred) += rbn_size(pred);
                rbn_size(pred) = 0;
                RecalculateMhs(pred);
                RecalculateMhs(succ);
                // now remove pred
                RawRemove(_root, pred);
            }
        } else if (left_merge) {
            rbn_size(pred) += pair._size;
            RecalculateMhs(pred);
        } else if (right_merge) {
            rbn_offset(succ) -= pair._size;
            rbn_size(succ) += pair._size;
            RecalculateMhs(succ);
        }
    }
    // this is the most tedious part, but not complicated:
    // 1.find where to insert the pair
    // 2.if the pred and succ can merge with the pair. merge with them. either
    // pred
    // or succ can be removed.
    // 3. if only left-mergable or right-mergeable, just merge
    // 4. non-mergable case. insert the node and run the fixup.

    int Tree::Insert(Node *&root, Node::BlockPair pair) {
        Node *x = _root;
        Node *y = NULL;
        bool left_merge = false;
        bool right_merge = false;
        Node *node = NULL;

        while (x != NULL) {
            y = x;
            if (pair._offset < rbn_key(x))
                x = x->_left;
            else
                x = x->_right;
        }

        // we found where to insert, lets find out the pred and succ for
        // possible
        // merges.
        //  node->parent = y;
        Node *pred, *succ;
        if (y != NULL) {
            if (pair._offset < rbn_key(y)) {
                // as the left child
                pred = PredecessorHelper(y->_parent, y);
                succ = y;
                IsNewNodeMergable(pred, succ, pair, &left_merge, &right_merge);
                if (left_merge || right_merge) {
                    AbsorbNewNode(
                        pred, succ, pair, left_merge, right_merge, false);
                } else {
                    // construct the node
                    Node::Pair mhsp {0, 0};
                    node =
                        new Node(EColor::BLACK, pair, mhsp, nullptr, nullptr, nullptr);
                    if (!node)
                        return -1;
                    y->_left = node;
                    node->_parent = y;
                    RecalculateMhs(node);
                }

            } else {
                // as the right child
                pred = y;
                succ = SuccessorHelper(y->_parent, y);
                IsNewNodeMergable(pred, succ, pair, &left_merge, &right_merge);
                if (left_merge || right_merge) {
                    AbsorbNewNode(
                        pred, succ, pair, left_merge, right_merge, true);
                } else {
                    // construct the node
                    Node::Pair mhsp {0, 0};
                    node =
                        new Node(EColor::BLACK, pair, mhsp, nullptr, nullptr, nullptr);
                    if (!node)
                        return -1;
                    y->_right = node;
                    node->_parent = y;
                    RecalculateMhs(node);
                }
            }
        } else {
            Node::Pair mhsp {0, 0};
            node = new Node(EColor::BLACK, pair, mhsp, nullptr, nullptr, nullptr);
            if (!node)
                return -1;
            root = node;
        }
        if (!left_merge && !right_merge) {
            invariant_notnull(node);
            node->_color = EColor::RED;
            return InsertFixup(root, node);
        }
        return 0;
    }

    int Tree::InsertFixup(Node *&root, Node *node) {
        Node *parent, *gparent;
        while ((parent = rbn_parent(node)) && rbn_is_red(parent)) {
            gparent = rbn_parent(parent);
            if (parent == gparent->_left) {
                {
                    Node *uncle = gparent->_right;
                    if (uncle && rbn_is_red(uncle)) {
                        rbn_set_black(uncle);
                        rbn_set_black(parent);
                        rbn_set_red(gparent);
                        node = gparent;
                        continue;
                    }
                }

                if (parent->_right == node) {
                    Node *tmp;
                    LeftRotate(root, parent);
                    tmp = parent;
                    parent = node;
                    node = tmp;
                }

                rbn_set_black(parent);
                rbn_set_red(gparent);
                RightRotate(root, gparent);
            } else {
                {
                    Node *uncle = gparent->_left;
                    if (uncle && rbn_is_red(uncle)) {
                        rbn_set_black(uncle);
                        rbn_set_black(parent);
                        rbn_set_red(gparent);
                        node = gparent;
                        continue;
                    }
                }

                if (parent->_left == node) {
                    Node *tmp;
                    RightRotate(root, parent);
                    tmp = parent;
                    parent = node;
                    node = tmp;
                }
                rbn_set_black(parent);
                rbn_set_red(gparent);
                LeftRotate(root, gparent);
            }
        }
        rbn_set_black(root);
        return 0;
    }

    int Tree::Insert(Node::BlockPair pair) { return Insert(_root, pair); }

    uint64_t Tree::Remove(size_t size) {
        Node *node = SearchFirstFitBySize(size);
        return Remove(_root, node, size);
    }

    void Tree::RawRemove(Node *&root, Node *node) {
        Node *child, *parent;
        EColor color;

        if ((node->_left != NULL) && (node->_right != NULL)) {
            Node *replace = node;
            replace = replace->_right;
            while (replace->_left != NULL)
                replace = replace->_left;

            if (rbn_parent(node)) {
                if (rbn_parent(node)->_left == node)
                    rbn_parent(node)->_left = replace;
                else
                    rbn_parent(node)->_right = replace;
            } else {
                root = replace;
            }
            child = replace->_right;
            parent = rbn_parent(replace);
            color = rbn_color(replace);

            if (parent == node) {
                parent = replace;
            } else {
                if (child)
                    rbn_parent(child) = parent;

                parent->_left = child;
                rbn_left_mhs(parent) = rbn_right_mhs(replace);
                RecalculateMhs(parent);
                replace->_right = node->_right;
                rbn_set_parent(node->_right, replace);
                rbn_right_mhs(replace) = rbn_right_mhs(node);
            }

            replace->_parent = node->_parent;
            replace->_color = node->_color;
            replace->_left = node->_left;
            rbn_left_mhs(replace) = rbn_left_mhs(node);
            node->_left->_parent = replace;
            RecalculateMhs(replace);
            if (color == EColor::BLACK)
                RawRemoveFixup(root, child, parent);
            delete node;
            return;
        }

        if (node->_left != NULL)
            child = node->_left;
        else
            child = node->_right;

        parent = node->_parent;
        color = node->_color;

        if (child)
            child->_parent = parent;

        if (parent) {
            if (parent->_left == node) {
                parent->_left = child;
                rbn_left_mhs(parent) = child ? mhs_of_subtree(child) : 0;
            } else {
                parent->_right = child;
                rbn_right_mhs(parent) = child ? mhs_of_subtree(child) : 0;
            }
            RecalculateMhs(parent);
        } else
            root = child;
        if (color == EColor::BLACK)
            RawRemoveFixup(root, child, parent);
        delete node;
    }

    void Tree::RawRemove(uint64_t offset) {
        Node *node = SearchByOffset(offset);
        RawRemove(_root, node);
    }
    static inline uint64_t align(uint64_t value, uint64_t ba_alignment) {
        return ((value + ba_alignment - 1) / ba_alignment) * ba_alignment;
    }
    uint64_t Tree::Remove(Node *&root, Node *node, size_t size) {
        OUUInt64 n_offset = rbn_offset(node);
        OUUInt64 n_size = rbn_size(node);
        OUUInt64 answer_offset(align(rbn_offset(node).ToInt(), _align));

        invariant((answer_offset + size) <= (n_offset + n_size));
        if (answer_offset == n_offset) {
            rbn_offset(node) += size;
            rbn_size(node) -= size;
            RecalculateMhs(node);
            if (rbn_size(node) == 0) {
                RawRemove(root, node);
            }

        } else {
            if (answer_offset + size == n_offset + n_size) {
                rbn_size(node) -= size;
                RecalculateMhs(node);
            } else {
                // well, cut in the middle...
                rbn_size(node) = answer_offset - n_offset;
                RecalculateMhs(node);
                Insert(_root,
                       {(answer_offset + size),
                        (n_offset + n_size) - (answer_offset + size)});
            }
        }
        return answer_offset.ToInt();
    }

    void Tree::RawRemoveFixup(Node *&root, Node *node, Node *parent) {
        Node *other;
        while ((!node || rbn_is_black(node)) && node != root) {
            if (parent->_left == node) {
                other = parent->_right;
                if (rbn_is_red(other)) {
                    // Case 1: the brother of X, w, is read
                    rbn_set_black(other);
                    rbn_set_red(parent);
                    LeftRotate(root, parent);
                    other = parent->_right;
                }
                if ((!other->_left || rbn_is_black(other->_left)) &&
                    (!other->_right || rbn_is_black(other->_right))) {
                    // Case 2: w is black and both of w's children are black
                    rbn_set_red(other);
                    node = parent;
                    parent = rbn_parent(node);
                } else {
                    if (!other->_right || rbn_is_black(other->_right)) {
                        // Case 3: w is black and left child of w is red but
                        // right
                        // child is black
                        rbn_set_black(other->_left);
                        rbn_set_red(other);
                        RightRotate(root, other);
                        other = parent->_right;
                    }
                    // Case 4: w is black and right child of w is red,
                    // regardless of
                    // left child's color
                    rbn_set_color(other, rbn_color(parent));
                    rbn_set_black(parent);
                    rbn_set_black(other->_right);
                    LeftRotate(root, parent);
                    node = root;
                    break;
                }
            } else {
                other = parent->_left;
                if (rbn_is_red(other)) {
                    // Case 1: w is red
                    rbn_set_black(other);
                    rbn_set_red(parent);
                    RightRotate(root, parent);
                    other = parent->_left;
                }
                if ((!other->_left || rbn_is_black(other->_left)) &&
                    (!other->_right || rbn_is_black(other->_right))) {
                    // Case 2: w is black and both children are black
                    rbn_set_red(other);
                    node = parent;
                    parent = rbn_parent(node);
                } else {
                    if (!other->_left || rbn_is_black(other->_left)) {
                        // Case 3: w is black and left child of w is red whereas
                        // right child is black
                        rbn_set_black(other->_right);
                        rbn_set_red(other);
                        LeftRotate(root, other);
                        other = parent->_left;
                    }
                    // Case 4:w is black and right child of w is red, regardless
                    // of
                    // the left child's color
                    rbn_set_color(other, rbn_color(parent));
                    rbn_set_black(parent);
                    rbn_set_black(other->_left);
                    RightRotate(root, parent);
                    node = root;
                    break;
                }
            }
        }
        if (node)
            rbn_set_black(node);
    }

    void Tree::Destroy(Node *&tree) {
        if (tree == NULL)
            return;

        if (tree->_left != NULL)
            Destroy(tree->_left);
        if (tree->_right != NULL)
            Destroy(tree->_right);

        delete tree;
        tree = NULL;
    }

    void Tree::Destroy() { Destroy(_root); }

    void Tree::Dump(Node *tree, Node::BlockPair pair, EDirection dir) {
        if (tree != NULL) {
            if (dir == EDirection::NONE)
                fprintf(stderr,
                        "(%" PRIu64 ",%" PRIu64 ", mhs:(%" PRIu64 ",%" PRIu64
                        "))(B) is root\n",
                        rbn_offset(tree).ToInt(),
                        rbn_size(tree).ToInt(),
                        rbn_left_mhs(tree),
                        rbn_right_mhs(tree));
            else
                fprintf(stderr,
                        "(%" PRIu64 ",%" PRIu64 ",mhs:(%" PRIu64 ",%" PRIu64
                        "))(%c) is %" PRIu64 "'s %s\n",
                        rbn_offset(tree).ToInt(),
                        rbn_size(tree).ToInt(),
                        rbn_left_mhs(tree),
                        rbn_right_mhs(tree),
                        rbn_is_red(tree) ? 'R' : 'B',
                        pair._offset.ToInt(),
                        dir == EDirection::RIGHT ? "right child" : "left child");

            Dump(tree->_left, tree->_hole, EDirection::LEFT);
            Dump(tree->_right, tree->_hole, EDirection::RIGHT);
        }
    }

    uint64_t Tree::EffectiveSize(Node *node) {
        OUUInt64 offset = rbn_offset(node);
        OUUInt64 size = rbn_size(node);
        OUUInt64 end = offset + size;
        OUUInt64 aligned_offset(align(offset.ToInt(), _align));
        if (aligned_offset > end) {
            return 0;
        }
        return (end - aligned_offset).ToInt();
    }

    void Tree::Dump() {
        if (_root != NULL)
            Dump(_root, _root->_hole, (EDirection)0);
    }

    static void vis_bal_f(void *extra, Node *node, uint64_t depth) {
        uint64_t **p = (uint64_t **)extra;
        uint64_t min = *p[0];
        uint64_t max = *p[1];
        if (node->_left) {
            Node *left = node->_left;
            invariant(node == left->_parent);
        }

        if (node->_right) {
            Node *right = node->_right;
            invariant(node == right->_parent);
        }

        if (!node->_left || !node->_right) {
            if (min > depth) {
                *p[0] = depth;
            } else if (max < depth) {
                *p[1] = depth;
            }
        }
    }

    void Tree::ValidateBalance() {
        uint64_t min_depth = 0xffffffffffffffff;
        uint64_t max_depth = 0;
        if (!_root) {
            return;
        }
        uint64_t *p[2] = {&min_depth, &max_depth};
        InOrderVisitor(vis_bal_f, (void *)p);
        invariant((min_depth + 1) * 2 >= max_depth + 1);
    }

    static void vis_cmp_f(void *extra, Node *node, uint64_t UU(depth)) {
        Node::BlockPair **p = (Node::BlockPair **)extra;

        invariant_notnull(*p);
        invariant((*p)->_offset == node->_hole._offset);

        *p = *p + 1;
    }

    // validate the input pairs matches with sorted pairs
    void Tree::ValidateInOrder(Node::BlockPair *pairs) {
        InOrderVisitor(vis_cmp_f, &pairs);
    }

    uint64_t Tree::ValidateMhs(Node *node) {
        if (!node)
            return 0;
        else {
            uint64_t mhs_left = ValidateMhs(node->_left);
            uint64_t mhs_right = ValidateMhs(node->_right);
            if (mhs_left != rbn_left_mhs(node)) {
                printf("assert failure: mhs_left = %" PRIu64 "\n", mhs_left);
                Dump(node, node->_hole, (EDirection)0);
            }
            invariant(mhs_left == rbn_left_mhs(node));

            if (mhs_right != rbn_right_mhs(node)) {
                printf("assert failure: mhs_right = %" PRIu64 "\n", mhs_right);
                Dump(node, node->_hole, (EDirection)0);
            }
            invariant(mhs_right == rbn_right_mhs(node));
            return std::max(EffectiveSize(node), std::max(mhs_left, mhs_right));
        }
    }

    void Tree::ValidateMhs() {
        if (!_root)
            return;
        uint64_t mhs_left = ValidateMhs(_root->_left);
        uint64_t mhs_right = ValidateMhs(_root->_right);
        invariant(mhs_left == rbn_left_mhs(_root));
        invariant(mhs_right == rbn_right_mhs(_root));
    }

}  // namespace MhsRbTree

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

//******************************************************************************
//
// Overview: A doubly linked list with elements of type T.
//   Each element that wants to be put into the list provides a
//   LinkedListElement<T> as well as a pointer to the the object of type T.
//   Typically, the user embeds the linked list element into the object itself,
//   for example as
//     struct foo {
//       toku::LinkedListElement<struct foo *> linked_list_elt;
//       ... other elements of foo
//     };
//   then when inserting foo into a list defined as 
//      toku::DoublyLinkedList<struct foo *> list_of_foos;
//   you write
//      struct foo f;
//      list_of_foos->insert(&f->linked_list_elt, &f);
//
// Operations:  Constructor and deconstructors are provided (they don't 
//   need to anything but fill in a field) for the DoublyLinkedList.
//   Operations to insert an element and remove it, as well as to pop
//   an element out of the list.
//   Also a LinkedListElement class is provided with a method to get a
//   pointer to the object of type T.
//******************************************************************************

#include <stdbool.h>
#include <portability/toku_assert.h>

namespace toku {

template<typename T> class DoublyLinkedList;

template<typename T> class LinkedListElement {
    friend class DoublyLinkedList<T>;
 private:
    T container;
    LinkedListElement<T> *prev, *next;
 public:
    T get_container(void) {
	return container;
    }
};

template<typename T> class DoublyLinkedList {
 public:
    void init (void);
    // Effect: Initialize a doubly linked list (to be empty).

    void insert(LinkedListElement<T> *ll_elt, T container); 
    // Effect: Add an item to a linked list.
    // Implementation note: Push the item to the head of the list.

    void remove(LinkedListElement<T> *ll_elt);
    // Effect: Remove an item from a linked list.
    // Requires: The item is in the list identified by head.

    bool pop(LinkedListElement<T> **ll_eltp);
    // Effect: if the list is empty, return false.
    //   Otherwise return true and set *ll_eltp to the first item, and remove that item from the list.

    template<typename extra_t> int iterate(int (*fun)(T container, extra_t extra), extra_t extra);
    // Effect: Call fun(e, extra) on every element of the linked list.  If ever fun returns nonzero, then quit early and return that value.
    //  If fun always return zero, then this function returns zero.

 private:
    LinkedListElement<T> *m_first;
};

//******************************************************************************
// DoublyLinkedList implementation starts here.
//******************************************************************************

#include <stddef.h>



template<typename T> void DoublyLinkedList<T>::init(void) {
    m_first     = NULL;
}

template<typename T> void DoublyLinkedList<T>::insert(LinkedListElement<T> *ll_elt, T container) {
    LinkedListElement<T> *old_first = m_first;
    ll_elt->container = container;
    ll_elt->next      = old_first;
    ll_elt->prev      = NULL;
    if (old_first!=NULL) {
	old_first->prev = ll_elt;
    }
    m_first = ll_elt;
}

template<typename T> void DoublyLinkedList<T>::remove(LinkedListElement<T> *ll_elt) {
    LinkedListElement<T> *old_prev = ll_elt->prev;
    LinkedListElement<T> *old_next = ll_elt->next;

    if (old_prev==NULL) {
	m_first = old_next;
    } else {
	old_prev->next = old_next;
    }
    if (old_next==NULL) {
	/* nothing */
    } else {
	old_next->prev = old_prev;
    }
}

template<typename T> bool DoublyLinkedList<T>::pop(LinkedListElement<T> **ll_eltp) {
    LinkedListElement<T> *first = m_first;
    if (first) {
	invariant(first->prev==NULL);
	m_first = first->next;
	if (first->next) {
	    first->next->prev = NULL;
	}
	first->next=NULL;
	*ll_eltp = first;
	return true;
    } else {
	return false;
    }
}

template<typename T>
template<typename extra_t>
int DoublyLinkedList<T>::iterate(int (*fun)(T container, extra_t extra), extra_t extra) {
    for (LinkedListElement<T> *le = m_first; le; le=le->next) {
	int r = fun(le->container, extra);
	if (r!=0) return r;
    }
    return 0;
}

}

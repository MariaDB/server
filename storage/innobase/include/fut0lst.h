/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/fut0lst.h
File-based list utilities

Created 11/28/1995 Heikki Tuuri
***********************************************************************/

#ifndef fut0lst_h
#define fut0lst_h

#ifdef UNIV_INNOCHECKSUM
# include "fil0fil.h"
#else
#include "fut0fut.h"
#include "mtr0log.h"

/* The C 'types' of base node and list node: these should be used to
write self-documenting code. Of course, the sizeof macro cannot be
applied to these types! */

typedef	byte	flst_base_node_t;
typedef	byte	flst_node_t;

#endif /* !UNIV_INNOCHECKSUM */

/* The physical size of a list base node in bytes */
#define	FLST_BASE_NODE_SIZE	(4 + 2 * FIL_ADDR_SIZE)
/* The physical size of a list node in bytes */
#define	FLST_NODE_SIZE		(2 * FIL_ADDR_SIZE)

#ifndef UNIV_INNOCHECKSUM
/* We define the field offsets of a node for the list */
#define FLST_PREV	0	/* 6-byte address of the previous list element;
				the page part of address is FIL_NULL, if no
				previous element */
#define FLST_NEXT	FIL_ADDR_SIZE	/* 6-byte address of the next
				list element; the page part of address
				is FIL_NULL, if no next element */

/* We define the field offsets of a base node for the list */
#define FLST_LEN	0	/* 32-bit list length field */
#define	FLST_FIRST	4	/* 6-byte address of the first element
				of the list; undefined if empty list */
#define	FLST_LAST	(4 + FIL_ADDR_SIZE) /* 6-byte address of the
				last element of the list; undefined
				if empty list */

/** Initialize a zero-initialized list base node.
@param[in,out]	block	file page
@param[in]	ofs	byte offset of the list base node
@param[in,out]	mtr	mini-transaction */
inline void flst_init(buf_block_t* block, uint16_t ofs, mtr_t* mtr)
{
	ut_ad(0 == mach_read_from_2(FLST_LEN + ofs + block->frame));
	ut_ad(0 == mach_read_from_2(FLST_FIRST + FIL_ADDR_BYTE + ofs
				    + block->frame));
	ut_ad(0 == mach_read_from_2(FLST_LAST + FIL_ADDR_BYTE + ofs
				    + block->frame));
	compile_time_assert(FIL_NULL == 0xffU * 0x1010101U);
	mlog_memset(block, FLST_FIRST + FIL_ADDR_PAGE + ofs, 4, 0xff, mtr);
	mlog_memset(block, FLST_LAST + FIL_ADDR_PAGE + ofs, 4, 0xff, mtr);
}

/** Write a null file address.
@param[in,out]	faddr	file address to be zeroed otu
@param[in,out]	mtr	mini-transaction */
inline void flst_zero_addr(fil_faddr_t* faddr, mtr_t* mtr)
{
	if (mach_read_from_4(faddr + FIL_ADDR_PAGE) != FIL_NULL) {
		mlog_memset(faddr + FIL_ADDR_PAGE, 4, 0xff, mtr);
	}
	if (mach_read_from_2(faddr + FIL_ADDR_BYTE)) {
		mlog_write_ulint(faddr + FIL_ADDR_BYTE, 0, MLOG_2BYTES, mtr);
	}
}

/********************************************************************//**
Initializes a list base node. */
UNIV_INLINE
void
flst_init(
/*======*/
	flst_base_node_t*	base,	/*!< in: pointer to base node */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Adds a node as the last node in a list. */
void
flst_add_last(
/*==========*/
	flst_base_node_t*	base,	/*!< in: pointer to base node of list */
	flst_node_t*		node,	/*!< in: node to add */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Adds a node as the first node in a list. */
void
flst_add_first(
/*===========*/
	flst_base_node_t*	base,	/*!< in: pointer to base node of list */
	flst_node_t*		node,	/*!< in: node to add */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Removes a node. */
void
flst_remove(
/*========*/
	flst_base_node_t*	base,	/*!< in: pointer to base node of list */
	flst_node_t*		node2,	/*!< in: node to remove */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/** Get the length of a list.
@param[in]	base	base node
@return length */
UNIV_INLINE
uint32_t
flst_get_len(
	const flst_base_node_t*	base);
/********************************************************************//**
Gets list first node address.
@return file address */
UNIV_INLINE
fil_addr_t
flst_get_first(
/*===========*/
	const flst_base_node_t*	base,	/*!< in: pointer to base node */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Gets list last node address.
@return file address */
UNIV_INLINE
fil_addr_t
flst_get_last(
/*==========*/
	const flst_base_node_t*	base,	/*!< in: pointer to base node */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Gets list next node address.
@return file address */
UNIV_INLINE
fil_addr_t
flst_get_next_addr(
/*===============*/
	const flst_node_t*	node,	/*!< in: pointer to node */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Gets list prev node address.
@return file address */
UNIV_INLINE
fil_addr_t
flst_get_prev_addr(
/*===============*/
	const flst_node_t*	node,	/*!< in: pointer to node */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Writes a file address. */
UNIV_INLINE
void
flst_write_addr(
/*============*/
	fil_faddr_t*	faddr,	/*!< in: pointer to file faddress */
	fil_addr_t	addr,	/*!< in: file address */
	mtr_t*		mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Reads a file address.
@return file address */
UNIV_INLINE
fil_addr_t
flst_read_addr(
/*===========*/
	const fil_faddr_t*	faddr,	/*!< in: pointer to file faddress */
	mtr_t*			mtr);	/*!< in: mini-transaction handle */
/********************************************************************//**
Validates a file-based list.
@return TRUE if ok */
ibool
flst_validate(
/*==========*/
	const flst_base_node_t*	base,	/*!< in: pointer to base node of list */
	mtr_t*			mtr1);	/*!< in: mtr */

#include "fut0lst.inl"

#endif /* !UNIV_INNOCHECKSUM */

#endif

/*****************************************************************************

Copyright (c) 1996, 2009, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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

/**************************************************//**
@file include/que0types.h
Query graph global types

Created 5/27/1996 Heikki Tuuri
*******************************************************/

#ifndef que0types_h
#define que0types_h

#include "data0data.h"

/* Pseudotype for all graph nodes */
typedef void	que_node_t;

/* Query graph root is a fork node */
typedef	struct que_fork_t	que_t;

struct row_prebuilt_t;
struct que_thr_t;

/* Query graph node types */
#define	QUE_NODE_LOCK		1
#define	QUE_NODE_INSERT		2
#define QUE_NODE_UPDATE		4
#define	QUE_NODE_CURSOR		5
#define	QUE_NODE_SELECT		6
#define	QUE_NODE_AGGREGATE	7
#define QUE_NODE_FORK		8
#define QUE_NODE_THR		9
#define QUE_NODE_UNDO		10
#define QUE_NODE_COMMIT		11
#define QUE_NODE_ROLLBACK	12
#define QUE_NODE_PURGE		13
#define QUE_NODE_CREATE_TABLE	14
#define QUE_NODE_CREATE_INDEX	15
#define QUE_NODE_SYMBOL		16
#define QUE_NODE_RES_WORD	17
#define QUE_NODE_FUNC		18
#define QUE_NODE_ORDER		19
#define QUE_NODE_PROC		(20 + QUE_NODE_CONTROL_STAT)
#define QUE_NODE_IF		(21 + QUE_NODE_CONTROL_STAT)
#define QUE_NODE_WHILE		(22 + QUE_NODE_CONTROL_STAT)
#define QUE_NODE_ASSIGNMENT	23
#define QUE_NODE_FETCH		24
#define QUE_NODE_OPEN		25
#define QUE_NODE_COL_ASSIGNMENT	26
#define QUE_NODE_FOR		(27 + QUE_NODE_CONTROL_STAT)
#define QUE_NODE_RETURN		28
#define QUE_NODE_ROW_PRINTF	29
#define QUE_NODE_ELSIF		30
#define QUE_NODE_CALL		31
#define QUE_NODE_EXIT		32

/* Common struct at the beginning of each query graph node; the name of this
substruct must be 'common' */

struct que_common_t{
	ulint		type;	/*!< query node type */
	que_node_t*	parent;	/*!< back pointer to parent node, or NULL */
	que_node_t*	brother;/* pointer to a possible brother node */
	dfield_t	val;	/*!< evaluated value for an expression */
	ulint		val_buf_size;
				/* buffer size for the evaluated value data,
				if the buffer has been allocated dynamically:
				if this field is != 0, and the node is a
				symbol node or a function node, then we
				have to free the data field in val
				explicitly */

	/** Constructor */
	que_common_t(ulint type, que_node_t* parent) :
		type(type), parent(parent), brother(NULL),
		val(), val_buf_size(0)
	{}
};

#endif

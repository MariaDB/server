#*****************************************************************************
# Copyright (c) 2019, 2020, MariaDB Corporation.
#****************************************************************************/

SET(XPAND_SOURCES ha_xpand.cc xpand_connection.cc ha_xpand_pushdown.cc)
MYSQL_ADD_PLUGIN(xpand ${XPAND_SOURCES} STORAGE_ENGINE COMPONENT xpand-engine)

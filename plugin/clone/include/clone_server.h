/* Copyright (c) 2017, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
@file clone/include/clone_server.h
Clone Plugin: Server interface

*/

#ifndef CLONE_SERVER_H
#define CLONE_SERVER_H

#include "clone.h"
#include "clone_hton.h"
#include "clone_os.h"

/* Namespace for all clone data types */
namespace myclone {
/** For Remote Clone, "Clone Server" is created at donor. It retrieves data
from Storage Engines and transfers over network to remote "Clone Client". */
class Server {
 public:
  /** Construct clone server. Initialize storage and external handle
  @param[in,out]	thd	server thread handle */
  Server(THD *thd);

  /** Destructor: Free the transfer buffer, if created. */
  ~Server();

  /** Get storage handle vector for data transfer.
  @return storage handle vector */
  Storage_Vector &get_storage_vector() { return (m_storage_vec); }

  /** Get clone locator for a storage engine at specified index.
  @param[in]   index   locator index
  @param[out]  loc_len locator length in bytes
  @return storage locator */
  const uchar *get_locator(uint index, uint &loc_len) const
  {
    assert(index < m_storage_vec.size());
    loc_len = m_storage_vec[index].m_loc_len;
    return (m_storage_vec[index].m_loc);
  }

  /** Get server thread handle
  @return server thread */
  THD *get_thd() { return (m_server_thd); }

  /** Get clone execution stage and acquire appropriate lock if requested.
  @param[in] sub_cmd sub command for execution
  @param[out] stage execution stage
  @param[in] lock true if needs to lock. Only master task should ask for lock.
  @return error code */
  int get_stage_and_lock(Sub_Command sub_cmd, Ha_clone_stage &stage,
                         bool lock);
 private:
  /** Server thread object */
  THD *m_server_thd;

  /** If this is the master task */
  bool m_is_master;

  /** Clone storage handle */
  Storage_Vector m_storage_vec;

  /** If backup lock is acquired */
  bool m_acquired_backup_lock;

  /** Negotiated protocol version */
  uint32_t m_protocol_version;
};
}  // namespace myclone

#endif /* CLONE_SERVER_H */

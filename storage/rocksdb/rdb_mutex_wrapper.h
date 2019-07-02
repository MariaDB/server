/*
   Copyright (c) 2015, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#pragma once

/* C++ standard header file */
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <unordered_map>

/* MySQL header files */
#include "./my_sys.h"
#include "mysql/plugin.h"

/* RocksDB header files */
#include "rocksdb/utilities/transaction_db_mutex.h"
#include "rdb_mariadb_port.h"

namespace myrocks {

class Rdb_mutex : public rocksdb::TransactionDBMutex {
  Rdb_mutex(const Rdb_mutex &p) = delete;
  Rdb_mutex &operator=(const Rdb_mutex &p) = delete;

 public:
  Rdb_mutex();
  virtual ~Rdb_mutex() override;

  /*
    Override parent class's virtual methods of interrest.
  */

  // Attempt to acquire lock.  Return OK on success, or other Status on failure.
  // If returned status is OK, TransactionDB will eventually call UnLock().
  virtual rocksdb::Status Lock() override;

  // Attempt to acquire lock.  If timeout is non-negative, operation should be
  // failed after this many microseconds.
  // Returns OK on success,
  //         TimedOut if timed out,
  //         or other Status on failure.
  // If returned status is OK, TransactionDB will eventually call UnLock().
  virtual rocksdb::Status TryLockFor(
      int64_t timeout_time MY_ATTRIBUTE((__unused__))) override;

  // Unlock Mutex that was successfully locked by Lock() or TryLockUntil()
  virtual void UnLock() override;

 private:
  mysql_mutex_t m_mutex;
  friend class Rdb_cond_var;

#ifndef STANDALONE_UNITTEST
  void set_unlock_action(const PSI_stage_info *const old_stage_arg);
  std::unordered_map<THD *, std::shared_ptr<PSI_stage_info>> m_old_stage_info;
#endif
};

class Rdb_cond_var : public rocksdb::TransactionDBCondVar {
  Rdb_cond_var(const Rdb_cond_var &) = delete;
  Rdb_cond_var &operator=(const Rdb_cond_var &) = delete;

 public:
  Rdb_cond_var();
  virtual ~Rdb_cond_var() override;

  /*
    Override parent class's virtual methods of interrest.
  */

  // Block current thread until condition variable is notified by a call to
  // Notify() or NotifyAll().  Wait() will be called with mutex locked.
  // Returns OK if notified.
  // Returns non-OK if TransactionDB should stop waiting and fail the operation.
  // May return OK spuriously even if not notified.
  virtual rocksdb::Status Wait(
      const std::shared_ptr<rocksdb::TransactionDBMutex> mutex) override;

  // Block current thread until condition variable is notifiesd by a call to
  // Notify() or NotifyAll(), or if the timeout is reached.
  // If timeout is non-negative, operation should be failed after this many
  // microseconds.
  // If implementing a custom version of this class, the implementation may
  // choose to ignore the timeout.
  //
  // Returns OK if notified.
  // Returns TimedOut if timeout is reached.
  // Returns other status if TransactionDB should otherwis stop waiting and
  //  fail the operation.
  // May return OK spuriously even if not notified.
  virtual rocksdb::Status WaitFor(
      const std::shared_ptr<rocksdb::TransactionDBMutex> mutex,
      int64_t timeout_time) override;

  // If any threads are waiting on *this, unblock at least one of the
  // waiting threads.
  virtual void Notify() override;

  // Unblocks all threads waiting on *this.
  virtual void NotifyAll() override;

 private:
  mysql_cond_t m_cond;
};

class Rdb_mutex_factory : public rocksdb::TransactionDBMutexFactory {
 public:
  Rdb_mutex_factory(const Rdb_mutex_factory &) = delete;
  Rdb_mutex_factory &operator=(const Rdb_mutex_factory &) = delete;
  Rdb_mutex_factory() {}
  /*
    Override parent class's virtual methods of interrest.
  */

  virtual std::shared_ptr<rocksdb::TransactionDBMutex> AllocateMutex()
      override {
    return std::make_shared<Rdb_mutex>();
  }

  virtual std::shared_ptr<rocksdb::TransactionDBCondVar> AllocateCondVar()
      override {
    return std::make_shared<Rdb_cond_var>();
  }

  virtual ~Rdb_mutex_factory() override {}
};

}  // namespace myrocks

/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "wsrep_priv.h"
#include "wsrep_api.h"
#include "wsrep_sr_file.h"
#include "wsrep_applier.h" // wsrep_apply_cb()
#include "wsrep_thd.h"
#include "wsrep_utils.h"
#include <map>
#include <string>
#include <ostream>
#include <iostream>
#include <fstream>
#include <list>
#include <dirent.h>
#include <sstream>
#include <iomanip>

/* to prepare wsrep_uuid_t usable in std::map */
inline bool operator==(const node_trx_t& lhs, const node_trx_t& rhs) {
  return(memcmp(lhs.data, rhs.data, 24) == 0);
}
inline bool operator!=(const node_trx_t& lhs, const node_trx_t& rhs) {
  return !operator==(lhs,rhs);
}
inline bool operator< (const node_trx_t& lhs, const node_trx_t& rhs) {
  return(memcmp(lhs.data, rhs.data, 24) < 0);
}
inline bool operator> (const node_trx_t& lhs, const node_trx_t& rhs) {
  return  operator< (rhs,lhs);
}
inline bool operator<=(const node_trx_t& lhs, const node_trx_t& rhs) {
  return !operator> (lhs,rhs);
}
inline bool operator>=(const node_trx_t& lhs, const node_trx_t& rhs) {
  return !operator< (lhs,rhs);
}

typedef std::map<uint64_t, std::vector<unsigned char> > frag_list_t;
typedef std::map<node_trx_t, frag_list_t>  db_t;

class SR_file {
  std::string   const name_;
  std::ofstream outfile_;

  size_t    size_;
  long      frags_;
  int const order_;

  typedef std::map<node_trx_t, bool> trxs_t;
  trxs_t trxs_;

  /* will go in file header */
  signed long long min_seqno_;
  signed long long max_seqno_;

public:
  SR_file(std::string name, int order) : 
    name_(name),
    order_(order)
  {
    outfile_.open(name.c_str(), std::ios::out | std::ios::app | std::ios::binary );
    size_      = 0;
    min_seqno_ = 0;
    max_seqno_ = 0;
  }
  SR_file() : order_(0)
  {
  }
  int get_order() { return order_; }
  std::string get_name() { return name_; }

  void write_file_header();
  void read_file_header();
  void write_frag_header(
			 wsrep_uuid_t *node_uuid,
			 wsrep_trx_id_t trx, 
			 wsrep_seqno_t seqno,
			 uint32_t flags)
  {
    char uuid_str[37] = {'\0',};
    wsrep_uuid_print(node_uuid, uuid_str, 37);

    outfile_ << uuid_str;
    outfile_ << ' ' << trx << ' ' << seqno;

    if (flags & WSREP_FLAG_TRX_START) {
      outfile_ << 'B';
    } else {
      outfile_ << ' ';
    }
    if (flags & WSREP_FLAG_TRX_END) {
      outfile_ << 'C';
    } else {
      outfile_ << ' ';
    }
    if (flags & WSREP_FLAG_ROLLBACK) {
      outfile_ << 'R';
    } else {
      outfile_ << ' ';
    }
  }
  void append(wsrep_uuid_t *node_uuid,
	      wsrep_trx_id_t trx,
	      wsrep_seqno_t seqno,
	      uint32_t flags,
	      const uchar *buf,
	      size_t buf_len)
  {
    if (seqno < min_seqno_ || min_seqno_ == 0) min_seqno_ = seqno;
    if (seqno > max_seqno_ || max_seqno_ == 0) max_seqno_ = seqno;
    
    write_frag_header(node_uuid, trx, seqno, flags);
    outfile_ << buf_len << '#';
    outfile_.write((char*)(buf), buf_len);
    size_ += buf_len;
    size_ += 35; /* for header */

    node_trx_t trxid = { {'\0',} };
    memcpy(trxid.data, node_uuid->data, 16);
    sprintf((char*)&(trxid.data[16]), "%ld", trx);
    trxs_[trxid] = true;
  };

  bool remove(const wsrep_uuid_t *node_uuid,
	      const wsrep_seqno_t seqno)
  {
    node_trx_t trxid = { {'\0',} };
    memcpy(trxid.data, node_uuid->data, 16);
    sprintf((char*)&(trxid.data[16]), "%ld", seqno);
    
    //std::string trxid = node_uuid + std::to_string(seqno);
    trxs_[trxid] = false;
    
    /* check if all transactions are over in this file */
    bool all_gone(true);
    
    trxs_t::iterator iterator;
    for(iterator = trxs_.begin(); iterator != trxs_.end(); iterator++) {
      if (iterator->second == true)
	{
	  all_gone = false;
	  continue;
	}
    }
    
    return all_gone;
  }
  
  size_t size()
  {
    return size_;
  }
  
  class File_hdr {
  };
  
  class Frag_hdr {
  };
  
  void close()
  {
    outfile_.close();
  }
};

int SR_storage_file::max_file_order()
{
  int max = 0;
  std::list<SR_file*>::iterator iterator;
  for (iterator = files_.begin(); iterator != files_.end(); ++iterator) {
    if ((*iterator)->get_order() > max) max = (*iterator)->get_order();
  }
  return max;
}

std::string *SR_storage_file::new_name(int order)
{
  std::string *name = new std::string(dir_); 
  *name += "/wsrep_SR_";
  *name += order;
  *name += ".dat";
  return name;
}

SR_file *SR_storage_file::append_file() {

  int order = max_file_order() + 1;
  std::stringstream ss;
  ss << dir_ <<  "/wsrep_SR_store." << order;
  SR_file *file = new SR_file(ss.str(), order);
  files_.push_back(file);
  return file;
}

void SR_storage_file::remove_file(SR_file *file) {
  remove(file->get_name().c_str());
  delete file;
}

SR_storage_file::SR_storage_file(
			   const char *dir, 
			   size_t limit, 
			   const char * cluster_uuid_str) 
{
  dir_ = std::string(dir);
  size_limit_ = limit;
  wsrep_uuid_scan(cluster_uuid_str, 37, &cluster_uuid_);
  restored_ = false;

  curr_file_ = NULL;

  WSREP_DEBUG("SR pool initialized, group: %s", cluster_uuid_str);
}

int SR_storage_file::init(const char* cluster_uuid_str, Wsrep_schema* unused) 
{
  int rcode = 0;

  WSREP_DEBUG("SR pool initialized,");
  WSREP_DEBUG("cluster_uuid:str: %s", cluster_uuid_str);
  wsrep_uuid_scan(cluster_uuid_str, 37, &cluster_uuid_);

  return rcode;
}

THD* SR_storage_file::append_frag(THD*         thd,
				  uint32_t     flags,
				  const uchar* buf,
				  size_t       buf_len)
{
  wsrep_uuid_t *node_uuid = &(thd->wsrep_trx_meta.stid.node);
  wsrep_trx_id_t trx      = thd->wsrep_trx_meta.stid.trx;
  wsrep_seqno_t seqno     = thd->wsrep_trx_meta.gtid.seqno;

  if (!restored_) return NULL;

  wsp::auto_lock lock(&LOCK_wsrep_SR_store);

  if (!curr_file_) curr_file_ = append_file();
  
  assert(curr_file_);
    
  curr_file_->append(node_uuid, trx, seqno, flags, buf, buf_len);

  if (curr_file_->size() > size_limit_) 
  {
    curr_file_->close();
    curr_file_ = NULL;
  }

  return NULL;
}

void SR_storage_file::remove_trx( THD *thd )
{
  assert (thd);
  const wsrep_uuid_t *node_uuid = &(thd->wsrep_trx_meta.stid.node);
  const wsrep_trx_id_t trxid    = thd->wsrep_trx_meta.stid.trx;

  /* remove from all SR files */
  wsp::auto_lock lock(&LOCK_wsrep_SR_store);

  std::list<SR_file*>::iterator iterator;
  for (iterator = files_.begin(); iterator != files_.end();) {
    if ((*iterator)->remove(node_uuid, trxid))
    {
      if (curr_file_ == *iterator) curr_file_ = NULL;

      remove_file(*iterator);

      std::list<SR_file*>::iterator prev = iterator++;
      files_.erase(prev);
    }
    else
    {
      ++iterator;
    }
  }
}

void SR_storage_file::rollback_trx( THD* thd )

{
  WSREP_DEBUG("SR_storage_file::commit_trx");
  remove_trx(thd);
}

int SR_storage_file::replay_trx(THD* thd, const wsrep_trx_meta_t& meta)
{
  WSREP_ERROR("SR_storage_file::replay_trx not implemented");
  return 1;
}


void SR_storage_file::read_trxs_from_file(
        std::string file, trxs_t *trxs, THD *thd, enum read_mode mode)
{
  std::ifstream infile;

  WSREP_DEBUG("read_trxs_from_file");

  if (!strcmp(file.c_str(), "---"))
  {
    WSREP_DEBUG("SR file comment line skipped");
    return;
  }
  infile.open(file.c_str(), std::ios::in | std::ios::binary);
  infile.exceptions ( 
    std::ifstream::failbit | std::ifstream::badbit | std::ifstream::eofbit);

  try {

    while (infile.good())
    {
      char uuid[37] = {'\0',};
      wsrep_uuid_t node_uuid;
      wsrep_trx_id_t trxid;
      wsrep_seqno_t seqno;
      char begin    = '#';
      char commit   = '#';
      char rollback = '#';
      int  len;
      char *buf;

     /* 1 source node UUID */
      if (infile.get(uuid, 37))
      {
        wsrep_uuid_scan(uuid, 37, &node_uuid);
      }
      else
        break;

      /* 2 source node trx ID */
      infile >> trxid;

      /* 3 trx seqno */
      infile >> seqno;

      /* 4 flags: Begin-Commit-Rollback */
      begin    = (char)infile.get();
      commit   = (char)infile.get();
      rollback = (char)infile.get();

      /* 5 rbr buffer length */
      infile >> len;

      /* # after header part */
      if ( (char)infile.get() != '#')
      {
        WSREP_WARN("SR frament file bad line: %s %ld %ld %c %c %c",
                   uuid, trxid, seqno, begin, commit, rollback);
        return;
      }

      /* 6 the buffer */
      buf = new char [len];
      infile.read(buf, len);

      /* */
      node_trx_t nodetrx;
      memcpy(nodetrx.data, node_uuid.data, 16);
      sprintf((char*)&(nodetrx.data[16]), "%ld", trxid);

      int flags = 0;
      wsrep_trx_meta_t meta;
      meta.gtid.uuid  = cluster_uuid_;
      meta.gtid.seqno = seqno;
      meta.stid.node  = node_uuid;
      meta.stid.trx   = trxid;

      if (begin == 'B')
      {
        if (mode == FILTER)
        {
          WSREP_DEBUG("new trx in SR file: trx %ld seqno %ld", trxid, seqno);
          (*trxs)[nodetrx] = true;
        }
        else
        {
          flags |= WSREP_FLAG_TRX_START;
        }
      }
      else if (!(*trxs)[nodetrx])
      {
        WSREP_WARN("unfinished trx in SR file: trx %ld seqno %ld", 
                   trxid, seqno);
      }

      if (mode == FILTER && (commit == 'C' || rollback == 'R'))
      {
        WSREP_DEBUG("trx commit in SR file: trx %ld seqno %ld", trxid, seqno);
        (*trxs)[nodetrx] = false;
      }

      if (mode == POPULATE && (*trxs)[nodetrx])
      {
        /* pending transaction to launch in sr_pool */
        WSREP_DEBUG("launching SR trx: %ld", trxid);

        wsrep_buf_t const ws= { buf, size_t(len) };
        void*  err_buf;
        size_t err_len;
        if (wsrep_apply_cb(thd, flags, &ws, &meta, &err_buf, &err_len) !=
            WSREP_CB_SUCCESS)
        {
          WSREP_WARN("Streaming Replication fragment restore failed: %s",
                     err_buf ? (char*)err_buf : "(null)");
          free(err_buf);
          return;
        }
        DBUG_ASSERT(NULL == err_buf);
        free(err_buf);
      }
      else if (mode == POPULATE)
        WSREP_DEBUG("not populating trx %ld seqno %ld", trxid, seqno);

      delete[] buf;
    }
  }
  catch(std::exception e)
    {
      if(infile.eof())
      {
        WSREP_DEBUG("infile EOF");
      }
      else
        WSREP_DEBUG("infile exception");
    }
}

int SR_storage_file::restore( THD *thd )
{
  std::ifstream infile;
  std::string file(dir_ + '/' + "wsrep_SR_info");
  THD * SRthd = NULL;

  char cluster_uuid_str[37] = {'\0',};

  wsp::auto_lock lock(&LOCK_wsrep_SR_store);

  if (restored_)
  {
    return 0;
  }
  wsrep_uuid_print(&cluster_uuid_, cluster_uuid_str, 37);

  WSREP_DEBUG("SR pool restore, group %s", cluster_uuid_str);

  /* read SR store info */
  infile.open(file.c_str(), std::ios::in);
  if (infile.is_open())
  {
    std::string line;
    getline (infile, line);
      
    /* this should be cluster uuid */
    if (line.length() != 36) 
    {
        WSREP_WARN("Streaming Replication info file is corrupted");
        restored_ = true;
        return -1;
    }

    if (strncmp(cluster_uuid_str, line.c_str(), 36)) {
      WSREP_WARN("Streaming Replication cluster uuid has changed, \n"
                 "cluster in SR file: %s\n"
                 "current cluster:    %s",
                 line.c_str(), cluster_uuid_str);
      restored_ = true;
      return -2;
    }

    trxs_t trxs;

    bool thd_started = false;
    if (!thd)
    {
      SRthd = wsrep_start_SR_THD((char*)&infile);
      SRthd->wsrep_SR_thd = false;
      thd_started = true;
      SRthd->store_globals();
    }
    else
    {
      SRthd = thd;
    }

    /* read all fragment files, and filter out committed trxs */
    while ( getline (infile, line) && strcmp(line.c_str(), "---"))
    {
      WSREP_DEBUG("SR file filtering line: %s", line.c_str());
      read_trxs_from_file(line, &trxs, SRthd, FILTER);
    }
    /* read again, and populate pending trxs */
    infile.seekg(infile.beg);
    getline (infile, line);

    while ( getline (infile, line) )
    {
      WSREP_DEBUG("SR file populating line: %s", line.c_str());
      read_trxs_from_file(line, &trxs, SRthd, POPULATE);
    }
    infile.close();
    if (thd_started)
    {
      wsrep_end_SR_THD(SRthd);
    }
    else
    {
      thd->store_globals();
    }
  }
  
  restored_ = true;
  remove(file.c_str());
  return 0;
}

void SR_storage_file::close() 
{
  WSREP_DEBUG("SR_storage_file::close()");
  wsp::auto_lock lock(&LOCK_wsrep_SR_store);

  /* write store info */
  std::string file(dir_ + '/' + "wsrep_SR_info");
  std::ofstream srinfo;
  char cluster_uuid_str[37] = {'\0',};

  wsrep_uuid_print(&cluster_uuid_, cluster_uuid_str, 37);

  srinfo.open(file.c_str());
  srinfo << cluster_uuid_str << '\n';

  /* close transactions */
  std::list<SR_file*>::iterator iterator;
  for (iterator = files_.begin(); iterator != files_.end();)
  {
    std::list<SR_file*>::iterator prev = iterator++;
    WSREP_DEBUG("Closing streaming replication file: %s", 
		(*prev)->get_name().c_str());
    
    srinfo << (*prev)->get_name() << '\n';
    (*prev)->close();
  }
  srinfo << "---" << '\n';

  srinfo.close();

  restored_ = false;
}

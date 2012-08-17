
// Cassandra includes:
#include <inttypes.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>

#include "Thrift.h"
#include "transport/TSocket.h"
#include "transport/TTransport.h"
#include "transport/TBufferTransports.h"
#include "protocol/TProtocol.h"
#include "protocol/TBinaryProtocol.h"
#include "gen-cpp/Cassandra.h"
// cassandra includes end

#include "cassandra_se.h"

using namespace std;
using namespace apache::thrift;
using namespace apache::thrift::transport;
using namespace apache::thrift::protocol;
using namespace org::apache::cassandra;

void Cassandra_se_interface::print_error(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  // it's not a problem if output was truncated
  vsnprintf(err_buffer, sizeof(err_buffer), format, ap);
  va_end(ap);
}

/*
  Implementation of connection to one Cassandra column family (ie., table)
*/
class Cassandra_se_impl: public Cassandra_se_interface
{
  CassandraClient *cass; /* Connection to cassandra */
  ConsistencyLevel::type cur_consistency_level;

  std::string column_family;
  std::string keyspace;
  
  /* DDL checks */
  KsDef ks_def; /* KeySpace we're using (TODO: put this in table->share) */
  CfDef cf_def; /* Column family we're using (TODO: put in table->share)*/
  std::vector<ColumnDef>::iterator column_ddl_it;

  /* The list that was returned by the last key lookup */
  std::vector<ColumnOrSuperColumn> col_supercol_vec;

public:
  
  Cassandra_se_impl() : cass(NULL) {}
  virtual ~Cassandra_se_impl(){ delete cass; }
  
  bool connect(const char *host, const char *keyspace);

  virtual void set_column_family(const char *cfname)
  {
    column_family.assign(cfname); 
  }

  virtual bool insert(NameAndValue *fields);
  virtual bool get_slice(char *key, size_t key_len, NameAndValue *row, bool *found);
   

  /* Functions to enumerate ColumnFamily's DDL data */
  bool setup_ddl_checks();
  void first_ddl_column();
  bool next_ddl_column(char **name, int *name_len, char **value, int *value_len);
};


Cassandra_se_interface *get_cassandra_se()
{
  return new Cassandra_se_impl;
}

#define CASS_TRY(x) try { \
     x;  \
  }catch(TTransportException te){ \
    print_error("%s [%d]", te.what(), te.getType()); \
  }catch(InvalidRequestException ire){ \
    print_error("%s [%s]", ire.what(), ire.why.c_str()); \
  }catch(NotFoundException nfe){ \
    print_error("%s", nfe.what()); \
  } catch(...) { \
    print_error("Unknown Exception"); \
  }

bool Cassandra_se_impl::connect(const char *host, const char *keyspace_arg)
{
  bool res= true;
  
  keyspace.assign(keyspace_arg);
  
  try {
    boost::shared_ptr<TTransport> socket = 
      boost::shared_ptr<TSocket>(new TSocket(host, 9160));
    boost::shared_ptr<TTransport> tr = 
      boost::shared_ptr<TFramedTransport>(new TFramedTransport (socket));
    boost::shared_ptr<TProtocol> p = 
      boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(tr));
    
    cass= new CassandraClient(p);
    tr->open();
    cass->set_keyspace(keyspace_arg);

    res= false; // success
  }catch(TTransportException te){
    print_error("%s [%d]", te.what(), te.getType());
  }catch(InvalidRequestException ire){
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  }catch(NotFoundException nfe){
    print_error("%s", nfe.what());
  }
  catch(...) {
    print_error("Unknown Exception");
  }

  // For now:
  cur_consistency_level= ConsistencyLevel::ONE;

  if (setup_ddl_checks())
    res= true;
  return res;
}


bool Cassandra_se_impl::setup_ddl_checks()
{
  try {
    cass->describe_keyspace(ks_def, keyspace);

    std::vector<CfDef>::iterator it;
    for (it= ks_def.cf_defs.begin(); it < ks_def.cf_defs.end(); it++)
    {
      cf_def= *it;
      if (!cf_def.name.compare(column_family))
        return false;
    }

    print_error("describe_keyspace() didn't return our column family");

  } catch (InvalidRequestException ire) {
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  } catch (NotFoundException nfe) {
    print_error("keyspace not found: %s", nfe.what());
  }
  return true;
}


void Cassandra_se_impl::first_ddl_column()
{
  column_ddl_it= cf_def.column_metadata.begin();
}


bool Cassandra_se_impl::next_ddl_column(char **name, int *name_len, 
                                        char **type, int *type_len)
{
  if (column_ddl_it == cf_def.column_metadata.end())
    return true;

  *name= (char*)(*column_ddl_it).name.c_str();
  *name_len= (*column_ddl_it).name.length();

  *type= (char*)(*column_ddl_it).validation_class.c_str();
  *type_len= (*column_ddl_it).validation_class.length();

  column_ddl_it++;
  return false;
}


bool Cassandra_se_impl::insert(NameAndValue *fields)
{
  ColumnParent cparent;
  cparent.column_family= column_family;
  
  Column c;
  struct timeval td;
  gettimeofday(&td, NULL);
  int64_t ms = td.tv_sec;
  ms = ms * 1000;
  int64_t usec = td.tv_usec;
  usec = usec / 1000;
  ms += usec;
  c.timestamp = ms;
  c.__isset.timestamp = true;

  std::string key;
  key.assign(fields->value, fields->value_len);
  fields++;

  bool res= false;
  try {
    /* TODO: switch to batch_mutate(). Or, even to CQL? */

    // TODO: what should INSERT table (co1, col2) VALUES ('foo', 'bar') mean?
    //       in SQL, it sets all columns.. what should it mean here? can we have
    //       it to work only for specified columns? (if yes, what do for
    //       VALUES()?)    
    c.__isset.value= true;
    for(;fields->name; fields++)
    {
      c.name.assign(fields->name);
      c.value.assign(fields->value, fields->value_len);
      cass->insert(key, cparent, c, ConsistencyLevel::ONE);
    }

  } catch (...) {
   res= true;
  }
  return res;
}


bool Cassandra_se_impl::get_slice(char *key, size_t key_len, NameAndValue *row, bool *found)
{
  ColumnParent cparent;
  cparent.column_family= column_family;

  std::string rowkey_str;
  rowkey_str.assign(key, key_len);

  SlicePredicate slice_pred;
  SliceRange sr;
  sr.start = "";
  sr.finish = "";
  slice_pred.__set_slice_range(sr);

  try {
    std::vector<ColumnOrSuperColumn> &res= col_supercol_vec;
    cass->get_slice(res, rowkey_str, cparent, slice_pred, ConsistencyLevel::ONE);
    *found= true;

    std::vector<ColumnOrSuperColumn>::iterator it;
    if (res.size() == 0)
    {
      /* 
        No columns found. Cassandra doesn't allow records without any column =>
        this means the seach key doesn't exist
      */
      *found= false;
      return false;
    }
    for (it= res.begin(); it < res.end(); it++)
    {
      ColumnOrSuperColumn cs= *it;
      if (!cs.__isset.column)
        return true;
      row->name= (char*)cs.column.name.c_str();
      row->value= (char*)cs.column.value.c_str();
      row->value_len= cs.column.value.length();
      row++;
    }
    row->name= NULL;
  } catch (InvalidRequestException ire) {
    return true;
  } catch (UnavailableException ue) {
    return true;
  } catch (TimedOutException te) {
    return true;
  }
  return false;
}


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
  
  /* DDL data */
  KsDef ks_def; /* KeySpace we're using (TODO: put this in table->share) */
  CfDef cf_def; /* Column family we're using (TODO: put in table->share)*/
  std::vector<ColumnDef>::iterator column_ddl_it;

  /* The list that was returned by the last key lookup */
  std::vector<ColumnOrSuperColumn> column_data_vec;
  std::vector<ColumnOrSuperColumn>::iterator column_data_it;

  /* Insert preparation */
  typedef std::map<std::string, std::vector<Mutation> > ColumnFamilyToMutation;
  typedef std::map<std::string,  ColumnFamilyToMutation> KeyToCfMutationMap;
   
  KeyToCfMutationMap batch_mutation; /* Prepare operation here */
  std::string key_to_insert;
  int64_t insert_timestamp;
  std::vector<Mutation>* insert_list;
   
  /* Resultset we're reading */
  std::vector<KeySlice> key_slice_vec;
  std::vector<KeySlice>::iterator key_slice_it;

  SlicePredicate slice_pred;
public:
  Cassandra_se_impl() : cass(NULL) {}
  virtual ~Cassandra_se_impl(){ delete cass; }
  
  /* Connection and DDL checks */
  bool connect(const char *host, const char *keyspace);
  void set_column_family(const char *cfname) { column_family.assign(cfname); }

  bool setup_ddl_checks();
  void first_ddl_column();
  bool next_ddl_column(char **name, int *name_len, char **value, int *value_len);

  /* Writes */
  void start_prepare_insert(const char *key, int key_len);
  void add_insert_column(const char *name, const char *value, int value_len);
  bool do_insert();

  /* Reads, point lookups */
  bool get_slice(char *key, size_t key_len, bool *found);
  bool get_next_read_column(char **name, char **value, int *value_len);

  /* Reads, multi-row scans */
  bool get_range_slices();
  void finish_reading_range_slices();
  bool get_next_range_slice_row();

  /* Setup that's necessary before a multi-row read. (todo: use it before point lookups, too) */
  void clear_read_columns();
  void add_read_column(const char *name);
};


/////////////////////////////////////////////////////////////////////////////
// Connection and setup
/////////////////////////////////////////////////////////////////////////////
Cassandra_se_interface *get_cassandra_se()
{
  return new Cassandra_se_impl;
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

  cur_consistency_level= ConsistencyLevel::ONE;

  if (!res && setup_ddl_checks())
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

/////////////////////////////////////////////////////////////////////////////
// Data writes
/////////////////////////////////////////////////////////////////////////////

void Cassandra_se_impl::start_prepare_insert(const char *key, int key_len)
{
  key_to_insert.assign(key, key_len);
  batch_mutation.clear();
  batch_mutation[key_to_insert]= ColumnFamilyToMutation();
  ColumnFamilyToMutation& cf_mut= batch_mutation[key_to_insert];

  cf_mut[column_family]= std::vector<Mutation>();
  insert_list= &cf_mut[column_family];

  struct timeval td;
  gettimeofday(&td, NULL);
  int64_t ms = td.tv_sec;
  ms = ms * 1000;
  int64_t usec = td.tv_usec;
  usec = usec / 1000;
  ms += usec;
  insert_timestamp= ms;
}


void Cassandra_se_impl::add_insert_column(const char *name, const char *value, 
                                          int value_len)
{
  Mutation mut;
  mut.__isset.column_or_supercolumn= true;
  mut.column_or_supercolumn.__isset.column= true;

  Column& col=mut.column_or_supercolumn.column;
  col.name.assign(name);
  col.value.assign(value, value_len);
  col.timestamp= insert_timestamp;
  col.__isset.value= true;
  col.__isset.timestamp= true;
  insert_list->push_back(mut);
}


bool Cassandra_se_impl::do_insert()
{
  bool res= true;
  try {
    
    cass->batch_mutate(batch_mutation, cur_consistency_level);
    res= false;

  } catch (InvalidRequestException ire) {
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  } catch (UnavailableException ue) {
    print_error("UnavailableException: %s", ue.what());
  } catch (TimedOutException te) {
    print_error("TimedOutException: %s", te.what());
  }

  return res;
}


/////////////////////////////////////////////////////////////////////////////
// Reading data
/////////////////////////////////////////////////////////////////////////////

/*
  Make one key lookup. If the record is found, the result is stored locally and 
  the caller should iterate over it.
*/

bool Cassandra_se_impl::get_slice(char *key, size_t key_len, bool *found)
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
    cass->get_slice(column_data_vec, rowkey_str, cparent, slice_pred, 
                    cur_consistency_level);

    if (column_data_vec.size() == 0)
    {
      /*
        No columns found. Cassandra doesn't allow records without any column =>
        this means the seach key doesn't exist
      */
      *found= false;
      return false;
    }
    *found= true;

  } catch (InvalidRequestException ire) {
    print_error("%s [%s]", ire.what(), ire.why.c_str());
    return true;
  } catch (UnavailableException ue) {
    print_error("UnavailableException: %s", ue.what());
    return true;
  } catch (TimedOutException te) {
    print_error("TimedOutException: %s", te.what());
    return true;
  }

  column_data_it= column_data_vec.begin();
  return false;
}


bool Cassandra_se_impl::get_next_read_column(char **name, char **value, 
                                             int *value_len)
{
  while (1)
  {
    if (column_data_it == column_data_vec.end())
      return true;

    if (((*column_data_it).__isset.column))
      break; /* Ok it's a real column. Should be always the case. */

    column_data_it++;
  }

  ColumnOrSuperColumn& cs= *column_data_it;
  *name= (char*)cs.column.name.c_str();
  *value= (char*)cs.column.value.c_str();
  *value_len= cs.column.value.length();

  column_data_it++;
  return false;
}


bool Cassandra_se_impl::get_range_slices() //todo: start_range/end_range as parameters
{
  bool res= true;
  
  ColumnParent cparent;
  cparent.column_family= column_family;
  
  /* SlicePredicate can be used to limit columns we will retrieve */
   // Try passing nothing...

  KeyRange key_range; // Try passing nothing, too.
  key_range.__isset.start_key=true;
  key_range.__isset.end_key=true;
  key_range.start_key.assign("", 0);
  key_range.end_key.assign("", 0);

  try {
  
    cass->get_range_slices(key_slice_vec,
                           cparent, slice_pred, key_range, 
                           cur_consistency_level);
    res= false;

  } catch (InvalidRequestException ire) {
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  } catch (UnavailableException ue) {
    print_error("UnavailableException: %s", ue.what());
  } catch (TimedOutException te) {
    print_error("TimedOutException: %s", te.what());
  }

  key_slice_it= key_slice_vec.begin();
  return res;
}


bool Cassandra_se_impl::get_next_range_slice_row()
{
  if (key_slice_it == key_slice_vec.end())
    return true;
  
  column_data_vec= key_slice_it->columns;
  column_data_it= column_data_vec.begin();
  key_slice_it++;
  return false;
}


void Cassandra_se_impl::finish_reading_range_slices()
{
  key_slice_vec.clear();
}


void Cassandra_se_impl::clear_read_columns()
{
  slice_pred.column_names.clear();
}


void Cassandra_se_impl::add_read_column(const char *name_arg)
{
  std::string name(name_arg);
  slice_pred.__isset.column_names= true;
  slice_pred.column_names.push_back(name);
}


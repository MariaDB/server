
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
  int64_t insert_timestamp;
  std::vector<Mutation>* insert_list;
   
  /* Resultset we're reading */
  std::vector<KeySlice> key_slice_vec;
  std::vector<KeySlice>::iterator key_slice_it;
  
  std::string rowkey; /* key of the record we're returning now */

  SlicePredicate slice_pred;
  bool get_slices_returned_less;
public:
  Cassandra_se_impl() : cass(NULL) {}
  virtual ~Cassandra_se_impl(){ delete cass; }
  
  /* Connection and DDL checks */
  bool connect(const char *host, const char *keyspace);
  void set_column_family(const char *cfname) { column_family.assign(cfname); }

  bool setup_ddl_checks();
  void first_ddl_column();
  bool next_ddl_column(char **name, int *name_len, char **value, int *value_len);
  void get_rowkey_type(char **name, char **type);

  /* Writes */
  void clear_insert_buffer();
  void start_row_insert(const char *key, int key_len);
  void add_insert_column(const char *name, const char *value, int value_len);
  bool do_insert();

  /* Reads, point lookups */
  bool get_slice(char *key, size_t key_len, bool *found);
  bool get_next_read_column(char **name, char **value, int *value_len);
  void get_read_rowkey(char **value, int *value_len);

  /* Reads, multi-row scans */
  bool get_range_slices(bool last_key_as_start_key);
  void finish_reading_range_slices();
  bool get_next_range_slice_row(bool *eof);

  /* Setup that's necessary before a multi-row read. (todo: use it before point lookups, too) */
  void clear_read_columns();
  void add_read_column(const char *name);
 
  /* Reads, MRR scans */
  void new_lookup_keys();
  int  add_lookup_key(const char *key, size_t key_len);
  bool multiget_slice();

  std::vector<std::string> mrr_keys; /* TODO: can we use allocator to put them onto MRR buffer? */
  std::map<std::string, std::vector<ColumnOrSuperColumn> > mrr_result;
  std::map<std::string, std::vector<ColumnOrSuperColumn> >::iterator mrr_result_it;

  bool get_next_multiget_row();

  bool truncate();
  bool remove_row();

  /* Non-inherited utility functions: */
  int64_t get_i64_timestamp();
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


void Cassandra_se_impl::get_rowkey_type(char **name, char **type)
{
  if (cf_def.__isset.key_validation_class)
    *type= (char*)cf_def.key_validation_class.c_str();
  else
    *type= NULL;

  if (cf_def.__isset.key_alias)
    *name= (char*)cf_def.key_alias.c_str();
  else
    *name= NULL;
}


/////////////////////////////////////////////////////////////////////////////
// Data writes
/////////////////////////////////////////////////////////////////////////////
int64_t Cassandra_se_impl::get_i64_timestamp()
{
  struct timeval td;
  gettimeofday(&td, NULL);
  int64_t ms = td.tv_sec;
  ms = ms * 1000;
  int64_t usec = td.tv_usec;
  usec = usec / 1000;
  ms += usec;
  
  return ms;
}


void Cassandra_se_impl::clear_insert_buffer()
{
  batch_mutation.clear();
}


void Cassandra_se_impl::start_row_insert(const char *key, int key_len)
{
  std::string key_to_insert;
  key_to_insert.assign(key, key_len);
  batch_mutation[key_to_insert]= ColumnFamilyToMutation();
  ColumnFamilyToMutation& cf_mut= batch_mutation[key_to_insert];

  cf_mut[column_family]= std::vector<Mutation>();
  insert_list= &cf_mut[column_family];

  insert_timestamp= get_i64_timestamp();
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

    cassandra_counters.row_inserts+= batch_mutation.size();
    cassandra_counters.row_insert_batches++;

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

  rowkey.assign(key, key_len);

  SlicePredicate slice_pred;
  SliceRange sr;
  sr.start = "";
  sr.finish = "";
  slice_pred.__set_slice_range(sr);

  try {
    cass->get_slice(column_data_vec, rowkey, cparent, slice_pred, 
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


/* Return the rowkey for the record that was read */

void Cassandra_se_impl::get_read_rowkey(char **value, int *value_len)
{
  *value= (char*)rowkey.c_str();
  *value_len= rowkey.length();
}


bool Cassandra_se_impl::get_range_slices(bool last_key_as_start_key)
{
  bool res= true;
  
  ColumnParent cparent;
  cparent.column_family= column_family;
  
  /* SlicePredicate can be used to limit columns we will retrieve */
   // Try passing nothing...

  KeyRange key_range; // Try passing nothing, too.
  key_range.__isset.start_key= true;
  key_range.__isset.end_key= true;

  if (last_key_as_start_key)
    key_range.start_key= rowkey;
  else
    key_range.start_key.assign("", 0);

  key_range.end_key.assign("", 0);
  key_range.count= read_batch_size;
  try {
  
    cass->get_range_slices(key_slice_vec,
                           cparent, slice_pred, key_range, 
                           cur_consistency_level);
    res= false;

    if (key_slice_vec.size() < (uint)read_batch_size)
      get_slices_returned_less= true;
    else
      get_slices_returned_less= false;

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


/* Switch to next row. This may produce an error */
bool Cassandra_se_impl::get_next_range_slice_row(bool *eof)
{
  if (key_slice_it == key_slice_vec.end())
  {
    if (get_slices_returned_less)
    {
      *eof= true;
      return false;
    }

    /*
      We have read through all columns in this batch. Try getting the next
      batch.
    */
    if (get_range_slices(true))
      return true;

    if (key_slice_vec.empty())
    {
      *eof= true;
      return false;
    }
  }
 
  *eof= false;
  column_data_vec= key_slice_it->columns;
  rowkey= key_slice_it->key;
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


bool Cassandra_se_impl::truncate()
{
  bool res= true;
  try {
    
    cass->truncate(column_family);
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

bool Cassandra_se_impl::remove_row()
{
  bool res= true;

  ColumnPath column_path;
  column_path.column_family= column_family;

  try {
    
    cass->remove(rowkey, column_path, get_i64_timestamp(), cur_consistency_level);
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
// MRR reads
/////////////////////////////////////////////////////////////////////////////

void Cassandra_se_impl::new_lookup_keys()
{
  mrr_keys.clear();
}


int Cassandra_se_impl::add_lookup_key(const char *key, size_t key_len)
{
  mrr_keys.push_back(std::string(key, key_len));
  return mrr_keys.size();
}


bool Cassandra_se_impl::multiget_slice()
{
  ColumnParent cparent;
  cparent.column_family= column_family;

  SlicePredicate slice_pred;
  SliceRange sr;
  sr.start = "";
  sr.finish = "";
  slice_pred.__set_slice_range(sr);

  bool res= true;

  try {
    
    cassandra_counters.multiget_reads++;
    cassandra_counters.multiget_keys_scanned += mrr_keys.size();

    cass->multiget_slice(mrr_result, mrr_keys, cparent, slice_pred, 
                         cur_consistency_level);

    cassandra_counters.multiget_rows_read += mrr_result.size();
    
    res= false;
    mrr_result_it= mrr_result.begin();

  } catch (InvalidRequestException ire) {
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  } catch (UnavailableException ue) {
    print_error("UnavailableException: %s", ue.what());
  } catch (TimedOutException te) {
    print_error("TimedOutException: %s", te.what());
  }

  return res;
}


bool Cassandra_se_impl::get_next_multiget_row()
{
  if (mrr_result_it == mrr_result.end())
    return true; /* EOF */

  column_data_vec= mrr_result_it->second;
  rowkey= mrr_result_it->first;

  column_data_it= column_data_vec.begin();
  mrr_result_it++;
  return false;
}


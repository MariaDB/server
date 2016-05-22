
// Cassandra includes:
#include <inttypes.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>

#include "thrift/Thrift.h"
#include "thrift/transport/TSocket.h"
#include "thrift/transport/TTransport.h"
#include "thrift/transport/TBufferTransports.h"
#include "thrift/protocol/TProtocol.h"
#include "thrift/protocol/TBinaryProtocol.h"
#include "gen-cpp/Cassandra.h"
// cassandra includes end

#include "cassandra_se.h"

struct st_mysql_lex_string
{
  char *str;
  size_t length;
};

using namespace std;
using namespace apache::thrift;
using namespace apache::thrift::transport;
using namespace apache::thrift::protocol;
using namespace org::apache::cassandra;


/*
  Implementation of connection to one Cassandra column family (ie., table)
*/
class Cassandra_se_impl: public Cassandra_se_interface
{
  CassandraClient *cass; /* Connection to cassandra */

  std::string column_family;
  std::string keyspace;

  ConsistencyLevel::type write_consistency;
  ConsistencyLevel::type read_consistency;
  
  /* Connection data */
  std::string host;
  int port;
  /* How many times to retry an operation before giving up */
  int thrift_call_retries_to_do;

  bool inside_try_operation;

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
  SliceRange slice_pred_sr;
  bool get_slices_returned_less;
  bool get_slice_found_rows;

  bool reconnect();
public:
  Cassandra_se_impl() : cass(NULL),
                        write_consistency(ConsistencyLevel::ONE),
                        read_consistency(ConsistencyLevel::ONE),
                        thrift_call_retries_to_do(1),
                        inside_try_operation(false) 
                        {}
  virtual ~Cassandra_se_impl(){ delete cass; }

  /* Connection and DDL checks */
  bool connect(const char *host_arg, int port_arg, const char *keyspace);
  void set_column_family(const char *cfname) { column_family.assign(cfname); }

  bool setup_ddl_checks();
  void first_ddl_column();
  bool next_ddl_column(char **name, int *name_len, char **value, int *value_len);
  void get_rowkey_type(char **name, char **type);
  size_t get_ddl_size();
  const char* get_default_validator();

  /* Settings */
  void set_consistency_levels(unsigned long read_cons_level, unsigned long write_cons_level);
  virtual void set_n_retries(uint retries_arg) {
    thrift_call_retries_to_do= retries_arg;
  }

  /* Writes */
  void clear_insert_buffer();
  void start_row_insert(const char *key, int key_len);
  void add_insert_column(const char *name, int name_len,
                         const char *value, int value_len);
  void add_insert_delete_column(const char *name, int name_len);
  void add_row_deletion(const char *key, int key_len,
                        Column_name_enumerator *col_names,
                        LEX_STRING *names, uint nnames);

  bool do_insert();

  /* Reads, point lookups */
  bool get_slice(char *key, size_t key_len, bool *found);
  bool get_next_read_column(char **name, int *name_len,
                            char **value, int *value_len );
  void get_read_rowkey(char **value, int *value_len);

  /* Reads, multi-row scans */
private:
  bool have_rowkey_to_skip;
  std::string rowkey_to_skip;

  bool get_range_slices_param_last_key_as_start_key;
public:
  bool get_range_slices(bool last_key_as_start_key);
  void finish_reading_range_slices();
  bool get_next_range_slice_row(bool *eof);

  /* Setup that's necessary before a multi-row read. (todo: use it before point lookups, too) */
  void clear_read_columns();
  void clear_read_all_columns();
  void add_read_column(const char *name);

  /* Reads, MRR scans */
  void new_lookup_keys();
  int  add_lookup_key(const char *key, size_t key_len);
  bool multiget_slice();

  bool get_next_multiget_row();

  bool truncate();

  bool remove_row();

private:
  bool retryable_truncate();
  bool retryable_do_insert();
  bool retryable_remove_row();
  bool retryable_setup_ddl_checks();
  bool retryable_multiget_slice();
  bool retryable_get_range_slices();
  bool retryable_get_slice();

  std::vector<std::string> mrr_keys; /* can we use allocator to put these into MRR buffer? */
  std::map<std::string, std::vector<ColumnOrSuperColumn> > mrr_result;
  std::map<std::string, std::vector<ColumnOrSuperColumn> >::iterator mrr_result_it;

  /* Non-inherited utility functions: */
  int64_t get_i64_timestamp();

  typedef bool (Cassandra_se_impl::*retryable_func_t)();
  bool try_operation(retryable_func_t func);
};


/////////////////////////////////////////////////////////////////////////////
// Connection and setup
/////////////////////////////////////////////////////////////////////////////
Cassandra_se_interface *create_cassandra_se()
{
  return new Cassandra_se_impl;
}


bool Cassandra_se_impl::connect(const char *host_arg, int port_arg, const char *keyspace_arg)
{
  keyspace.assign(keyspace_arg);
  host.assign(host_arg);
  port= port_arg;
  return reconnect();
}


bool Cassandra_se_impl::reconnect()
{

  delete cass;
  cass= NULL;

  bool res= true;
  try {
    boost::shared_ptr<TTransport> socket =
      boost::shared_ptr<TSocket>(new TSocket(host.c_str(), port));
    boost::shared_ptr<TTransport> tr =
      boost::shared_ptr<TFramedTransport>(new TFramedTransport (socket));
    boost::shared_ptr<TProtocol> p =
      boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(tr));

    cass= new CassandraClient(p);
    tr->open();
    cass->set_keyspace(keyspace.c_str());

    res= false; // success
  }catch(TTransportException te){
    print_error("%s [%d]", te.what(), te.getType());
  }catch(InvalidRequestException ire){
    print_error("%s [%s]", ire.what(), ire.why.c_str());
  }catch(NotFoundException nfe){
    print_error("%s", nfe.what());
  }catch(TException e){
    print_error("Thrift exception: %s", e.what());
  }catch (...) {
    print_error("Unknown exception");
  }

  if (!res && setup_ddl_checks())
    res= true;
  return res;
}


void Cassandra_se_impl::set_consistency_levels(unsigned long read_cons_level,
                                               unsigned long write_cons_level)
{
  write_consistency= (ConsistencyLevel::type)(write_cons_level + 1);
  read_consistency=  (ConsistencyLevel::type)(read_cons_level + 1);
}


bool Cassandra_se_impl::retryable_setup_ddl_checks()
{
  try {

    cass->describe_keyspace(ks_def, keyspace);

  } catch (NotFoundException nfe) {
    print_error("keyspace `%s` not found: %s", keyspace.c_str(), nfe.what());
    return true;
  }

  std::vector<CfDef>::iterator it;
  for (it= ks_def.cf_defs.begin(); it < ks_def.cf_defs.end(); it++)
  {
    cf_def= *it;
    if (!cf_def.name.compare(column_family))
      return false;
  }

  print_error("Column family %s not found in keyspace %s",
               column_family.c_str(),
               keyspace.c_str());
  return true;
}

bool Cassandra_se_impl::setup_ddl_checks()
{
  return try_operation(&Cassandra_se_impl::retryable_setup_ddl_checks);
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

size_t Cassandra_se_impl::get_ddl_size()
{
  return cf_def.column_metadata.size();
}

const char* Cassandra_se_impl::get_default_validator()
{
  return cf_def.default_validation_class.c_str();
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


void Cassandra_se_impl::add_row_deletion(const char *key, int key_len,
                                         Column_name_enumerator *col_names,
                                         LEX_STRING *names, uint nnames)
{
  std::string key_to_delete;
  key_to_delete.assign(key, key_len);

  batch_mutation[key_to_delete]= ColumnFamilyToMutation();
  ColumnFamilyToMutation& cf_mut= batch_mutation[key_to_delete];

  cf_mut[column_family]= std::vector<Mutation>();
  std::vector<Mutation> &mutation_list= cf_mut[column_family];

  Mutation mut;
  mut.__isset.deletion= true;
  mut.deletion.__isset.timestamp= true;
  mut.deletion.timestamp= get_i64_timestamp();
  mut.deletion.__isset.predicate= true;

  /*
    Attempting to delete columns with SliceRange causes exception with message
    "Deletion does not yet support SliceRange predicates".

    Delete all columns individually.
  */
  SlicePredicate slice_pred;
  slice_pred.__isset.column_names= true;
  const char *col_name;
  while ((col_name= col_names->get_next_name()))
    slice_pred.column_names.push_back(std::string(col_name));
  for (uint i= 0; i < nnames; i++)
    slice_pred.column_names.push_back(std::string(names[i].str,
                                                  names[i].length));

  mut.deletion.predicate= slice_pred;

  mutation_list.push_back(mut);
}


void Cassandra_se_impl::add_insert_column(const char *name,
                                          int name_len,
                                          const char *value,
                                          int value_len)
{
  Mutation mut;
  mut.__isset.column_or_supercolumn= true;
  mut.column_or_supercolumn.__isset.column= true;

  Column& col=mut.column_or_supercolumn.column;
  if (name_len)
    col.name.assign(name, name_len);
  else
    col.name.assign(name);
  col.value.assign(value, value_len);
  col.timestamp= insert_timestamp;
  col.__isset.value= true;
  col.__isset.timestamp= true;
  insert_list->push_back(mut);
}

void Cassandra_se_impl::add_insert_delete_column(const char *name,
                                                 int name_len)
{
  Mutation mut;
  mut.__isset.deletion= true;
  mut.deletion.__isset.timestamp= true;
  mut.deletion.timestamp= insert_timestamp;
  mut.deletion.__isset.predicate= true;

  SlicePredicate slice_pred;
  slice_pred.__isset.column_names= true;
  slice_pred.column_names.push_back(std::string(name, name_len));
  mut.deletion.predicate= slice_pred;

  insert_list->push_back(mut);
}


bool Cassandra_se_impl::retryable_do_insert()
{
  cass->batch_mutate(batch_mutation, write_consistency);

  cassandra_counters.row_inserts+= batch_mutation.size();
  cassandra_counters.row_insert_batches++;

  clear_insert_buffer();
  return 0;
}


bool Cassandra_se_impl::do_insert()
{
  /*
    zero-size mutations are allowed by Cassandra's batch_mutate but lets not
    do them (we may attempt to do it if there is a bulk insert that stores
    exactly @@cassandra_insert_batch_size*n elements.
  */
  if (batch_mutation.empty())
    return false;

  return try_operation(&Cassandra_se_impl::retryable_do_insert);
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
  bool res;
  rowkey.assign(key, key_len);

  if (!(res= try_operation(&Cassandra_se_impl::retryable_get_slice)))
    *found= get_slice_found_rows;
  return res;
}


bool Cassandra_se_impl::retryable_get_slice()
{
  ColumnParent cparent;
  cparent.column_family= column_family;

  SlicePredicate slice_pred;
  SliceRange sr;
  sr.start = "";
  sr.finish = "";
  slice_pred.__set_slice_range(sr);

  cass->get_slice(column_data_vec, rowkey, cparent, slice_pred,
                  read_consistency);

  if (column_data_vec.size() == 0)
  {
    /*
      No columns found. Cassandra doesn't allow records without any column =>
      this means the seach key doesn't exist
    */
    get_slice_found_rows= false;
    return false;
  }
  get_slice_found_rows= true;

  column_data_it= column_data_vec.begin();
  return false;
}


bool Cassandra_se_impl::get_next_read_column(char **name, int *name_len,
                                             char **value, int *value_len)
{
  bool use_counter=false;
  while (1)
  {
    if (column_data_it == column_data_vec.end())
      return true;

    if ((*column_data_it).__isset.column)
      break; /* Ok it's a real column. Should be always the case. */

    if ((*column_data_it).__isset.counter_column)
    {
      use_counter= true;
      break;
    }

    column_data_it++;
  }

  ColumnOrSuperColumn& cs= *column_data_it;
  if (use_counter)
  {
    *name_len= cs.counter_column.name.size();
    *name= (char*)cs.counter_column.name.c_str();
    *value= (char*)&cs.counter_column.value;
    *value_len= sizeof(cs.counter_column.value);
  }
  else
  {
    *name_len= cs.column.name.size();
    *name= (char*)cs.column.name.c_str();
    *value= (char*)cs.column.value.c_str();
    *value_len= cs.column.value.length();
  }

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
  get_range_slices_param_last_key_as_start_key= last_key_as_start_key;

  return try_operation(&Cassandra_se_impl::retryable_get_range_slices);
}


bool Cassandra_se_impl::retryable_get_range_slices()
{
  bool last_key_as_start_key= get_range_slices_param_last_key_as_start_key;

  ColumnParent cparent;
  cparent.column_family= column_family;

  /* SlicePredicate can be used to limit columns we will retrieve */

  KeyRange key_range;
  key_range.__isset.start_key= true;
  key_range.__isset.end_key= true;

  if (last_key_as_start_key)
  {
    key_range.start_key= rowkey;

    have_rowkey_to_skip= true;
    rowkey_to_skip= rowkey;
  }
  else
  {
    have_rowkey_to_skip= false;
    key_range.start_key.assign("", 0);
  }

  key_range.end_key.assign("", 0);
  key_range.count= read_batch_size;

  cass->get_range_slices(key_slice_vec, cparent, slice_pred, key_range,
                         read_consistency);

  if (key_slice_vec.size() < (uint)read_batch_size)
    get_slices_returned_less= true;
  else
    get_slices_returned_less= false;

  key_slice_it= key_slice_vec.begin();
  return false;
}


/* Switch to next row. This may produce an error */
bool Cassandra_se_impl::get_next_range_slice_row(bool *eof)
{
restart:
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

  /*
    (1) - skip the last row that we have read in the previous batch.
    (2) - Rows that were deleted show up as rows without any columns. Skip
          them, like CQL does.
  */
  if ((have_rowkey_to_skip && !rowkey_to_skip.compare(key_slice_it->key)) || // (1)
      key_slice_it->columns.size() == 0) // (2)
  {
    key_slice_it++;
    goto restart;
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

void Cassandra_se_impl::clear_read_all_columns()
{
  slice_pred_sr.start = "";
  slice_pred_sr.finish = "";
  slice_pred.__set_slice_range(slice_pred_sr);
}


void Cassandra_se_impl::add_read_column(const char *name_arg)
{
  std::string name(name_arg);
  slice_pred.__isset.column_names= true;
  slice_pred.column_names.push_back(name);
}


bool Cassandra_se_impl::truncate()
{
  return try_operation(&Cassandra_se_impl::retryable_truncate);
}


bool Cassandra_se_impl::retryable_truncate()
{
  cass->truncate(column_family);
  return 0;
}


bool Cassandra_se_impl::remove_row()
{
  return try_operation(&Cassandra_se_impl::retryable_remove_row);
}


bool Cassandra_se_impl::retryable_remove_row()
{
  ColumnPath column_path;
  column_path.column_family= column_family;
  cass->remove(rowkey, column_path, get_i64_timestamp(), write_consistency);
  return 0;
}

/*
  Try calling a function, catching possible Cassandra errors, and re-trying
   for "transient" errors.
*/
bool Cassandra_se_impl::try_operation(retryable_func_t func_to_call)
{
  bool res;
  int n_attempts= thrift_call_retries_to_do;
  
  bool was_inside_try_operation= inside_try_operation;
  inside_try_operation= true;

  do
  {
    res= true;

    try {

      if ((res= (this->*func_to_call)()))
      {
        /*
          The function call was made successfully (without timeouts, etc),
          but something inside it returned 'true'.
          This is supposedly a failure (or "not found" or other negative
          result). We need to return this to the caller.
        */
        n_attempts= 0;
      }

    } catch (InvalidRequestException ire) {
      n_attempts= 0; /* there is no point in retrying this operation */
      print_error("%s [%s]", ire.what(), ire.why.c_str());
    } catch (UnavailableException ue) {
      cassandra_counters.unavailable_exceptions++;
      if (!--n_attempts)
        print_error("UnavailableException: %s", ue.what());
    } catch (TimedOutException te) {
      /* 
        Note: this is a timeout generated *inside Cassandra cluster*.
        Connection between us and the cluster is ok, but something went wrong
        within the cluster.
      */
      cassandra_counters.timeout_exceptions++;
      if (!--n_attempts)
        print_error("TimedOutException: %s", te.what());
    } catch (TTransportException tte) {
      /* Something went wrong in communication between us and Cassandra */
      cassandra_counters.network_exceptions++;

      switch (tte.getType())
      {
        case TTransportException::NOT_OPEN:
        case TTransportException::TIMED_OUT:
        case TTransportException::END_OF_FILE:
        case TTransportException::INTERRUPTED:
        {
          if (!was_inside_try_operation && reconnect())
          {
            /* Failed to reconnect, no point to retry the operation */
            n_attempts= 0;
            print_error("%s", tte.what());
          }
          else
          {
            n_attempts--;
          }
          break;
        }
        default:
        {
          /* 
            We assume it doesn't make sense to retry for 
            unknown kinds of TTransportException-s 
          */
          n_attempts= 0;
          print_error("%s", tte.what());
        }
      }
    }catch(TException e){
      /* todo: we may use retry for certain kinds of Thrift errors */
      n_attempts= 0;
      print_error("Thrift exception: %s", e.what());
    } catch (...) {
      n_attempts= 0; /* Don't retry */
      print_error("Unknown exception");
    }

  } while (res && n_attempts > 0);
  
  inside_try_operation= was_inside_try_operation;
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
  return try_operation(&Cassandra_se_impl::retryable_multiget_slice);
}


bool Cassandra_se_impl::retryable_multiget_slice()
{
  ColumnParent cparent;
  cparent.column_family= column_family;

  SlicePredicate slice_pred;
  SliceRange sr;
  sr.start = "";
  sr.finish = "";
  slice_pred.__set_slice_range(sr);

  cassandra_counters.multiget_reads++;
  cassandra_counters.multiget_keys_scanned += mrr_keys.size();
  cass->multiget_slice(mrr_result, mrr_keys, cparent, slice_pred,
                       read_consistency);

  cassandra_counters.multiget_rows_read += mrr_result.size();
  mrr_result_it= mrr_result.begin();

  return false;
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




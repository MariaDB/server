/* Copyright (c) 2001, 2010, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Function to handle quick removal of duplicates
  This code is used when doing multi-table deletes to find the rows in
  reference tables that needs to be deleted.

  The basic idea is as follows:

  Store first all strings in a binary tree, ignoring duplicates.
  When the tree uses more memory than 'max_heap_table_size',
  write the tree (in sorted order) out to disk and start with a new tree.
  When all data has been generated, merge the trees (removing any found
  duplicates).

  The unique entries will be returned in sort order, to ensure that we do the
  deletes in disk order.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_sort.h"
#include "queues.h"                             // QUEUE
#include "my_tree.h"                            // element_count
#include "uniques.h"	                        // Unique
#include "sql_sort.h"

int unique_write_to_file(uchar* key, element_count count, Unique_impl *unique)
{
  return unique->write_record_to_file(key) ? 1 : 0;
}

int unique_write_to_file_with_count(uchar* key, element_count count, Unique_impl *unique)
{
  return unique_write_to_file(key, count, unique) ||
         my_b_write(&unique->file, (uchar*)&count, sizeof(element_count)) ? 1 : 0;
}

int unique_write_to_ptrs(uchar* key, element_count count, Unique_impl *unique)
{
  memcpy(unique->sort.record_pointers, key, unique->size);
  unique->sort.record_pointers+=unique->size;
  return 0;
}

int unique_intersect_write_to_ptrs(uchar* key, element_count count, Unique_impl *unique)
{
  if (count >= unique->min_dupl_count)
  {
    memcpy(unique->sort.record_pointers, key, unique->size);
    unique->sort.record_pointers+=unique->size;
  }
  else
    unique->filtered_out_elems++;
  return 0;
}


Unique_impl::Unique_impl(qsort_cmp2 comp_func, void * comp_func_fixed_arg,
	             uint size_arg, size_t max_in_memory_size_arg,
               uint min_dupl_count_arg, Descriptor *desc)
  :max_in_memory_size(max_in_memory_size_arg),
   size(size_arg),
   memory_used(0),
   elements(0)
{
  my_b_clear(&file);
  min_dupl_count= min_dupl_count_arg;
  full_size= size;
  if (min_dupl_count_arg)
    full_size+= sizeof(element_count);
  with_counters= MY_TEST(min_dupl_count_arg);
  init_tree(&tree, (max_in_memory_size / 16), 0, 0, comp_func,
            NULL, comp_func_fixed_arg, MYF(MY_THREAD_SPECIFIC));
  /* If the following fail's the next add will also fail */
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &file_ptrs, sizeof(Merge_chunk), 16,
                        16, MYF(MY_THREAD_SPECIFIC));
  /*
    If you change the following, change it in get_max_elements function, too.
  */
  max_elements= (ulong) (max_in_memory_size /
                         ALIGN_SIZE(sizeof(TREE_ELEMENT)+size));
  if (!max_elements)
  {
    max_elements= 1;
    /*
      Need to ensure that we have memory to store atleast one record
      in the Unique tree
    */
    max_in_memory_size= sizeof(TREE_ELEMENT) + size;
  }

  (void) open_cached_file(&file, mysql_tmpdir,TEMP_PREFIX, DISK_BUFFER_SIZE,
                          MYF(MY_WME));
  m_descriptor= desc;
}


/*
  Calculate log2(n!)

  NOTES
    Stirling's approximate formula is used:

      n! ~= sqrt(2*M_PI*n) * (n/M_E)^n

    Derivation of formula used for calculations is as follows:

    log2(n!) = log(n!)/log(2) = log(sqrt(2*M_PI*n)*(n/M_E)^n) / log(2) =

      = (log(2*M_PI*n)/2 + n*log(n/M_E)) / log(2).
*/

inline double log2_n_fact(double x)
{
  return (log(2*M_PI*x)/2 + x*log(x/M_E)) / M_LN2;
}


/*
  Calculate cost of merge_buffers function call for given sequence of
  input stream lengths and store the number of rows in result stream in *last.

  SYNOPSIS
    get_merge_buffers_cost()
      buff_elems  Array of #s of elements in buffers
      elem_size   Size of element stored in buffer
      first       Pointer to first merged element size
      last        Pointer to last merged element size

  RETURN
    Cost of merge_buffers operation in disk seeks.

  NOTES
    It is assumed that no rows are eliminated during merge.
    The cost is calculated as

      cost(read_and_write) + cost(merge_comparisons).

    All bytes in the sequences is read and written back during merge so cost
    of disk io is 2*elem_size*total_buf_elems/IO_SIZE (2 is for read + write)

    For comparisons cost calculations we assume that all merged sequences have
    the same length, so each of total_buf_size elements will be added to a sort
    heap with (n_buffers-1) elements. This gives the comparison cost:

      total_buf_elems* log2(n_buffers) / TIME_FOR_COMPARE_ROWID;
*/

static double get_merge_buffers_cost(uint *buff_elems, uint elem_size,
                                     uint *first, uint *last,
                                     double compare_factor)
{
  uint total_buf_elems= 0;
  for (uint *pbuf= first; pbuf <= last; pbuf++)
    total_buf_elems+= *pbuf;
  *last= total_buf_elems;

  size_t n_buffers= last - first + 1;

  /* Using log2(n)=log(n)/log(2) formula */
  return 2*((double)total_buf_elems*elem_size) / IO_SIZE +
     total_buf_elems*log((double) n_buffers) / (compare_factor * M_LN2);
}


/*
  Calculate cost of merging buffers into one in Unique::get, i.e. calculate
  how long (in terms of disk seeks) the two calls
    merge_many_buffs(...);
    merge_buffers(...);
  will take.

  SYNOPSIS
    get_merge_many_buffs_cost()
      buffer        buffer space for temporary data, at least
                    Unique::get_cost_calc_buff_size bytes
      maxbuffer     # of full buffers
      max_n_elems   # of elements in first maxbuffer buffers
      last_n_elems  # of elements in last buffer
      elem_size     size of buffer element

  NOTES
    maxbuffer+1 buffers are merged, where first maxbuffer buffers contain
    max_n_elems elements each and last buffer contains last_n_elems elements.

    The current implementation does a dumb simulation of merge_many_buffs
    function actions.

  RETURN
    Cost of merge in disk seeks.
*/

static double get_merge_many_buffs_cost(uint *buffer,
                                        uint maxbuffer, uint max_n_elems,
                                        uint last_n_elems, int elem_size,
                                        double compare_factor)
{
  int i;
  double total_cost= 0.0;
  uint *buff_elems= buffer; /* #s of elements in each of merged sequences */

  /*
    Set initial state: first maxbuffer sequences contain max_n_elems elements
    each, last sequence contains last_n_elems elements.
  */
  for (i = 0; i < (int)maxbuffer; i++)
    buff_elems[i]= max_n_elems;
  buff_elems[maxbuffer]= last_n_elems;

  /*
    Do it exactly as merge_many_buff function does, calling
    get_merge_buffers_cost to get cost of merge_buffers.
  */
  if (maxbuffer >= MERGEBUFF2)
  {
    while (maxbuffer >= MERGEBUFF2)
    {
      uint lastbuff= 0;
      for (i = 0; i <= (int) maxbuffer - MERGEBUFF*3/2; i += MERGEBUFF)
      {
        total_cost+=get_merge_buffers_cost(buff_elems, elem_size,
                                           buff_elems + i,
                                           buff_elems + i + MERGEBUFF-1,
                                           compare_factor);
	lastbuff++;
      }
      total_cost+=get_merge_buffers_cost(buff_elems, elem_size,
                                         buff_elems + i,
                                         buff_elems + maxbuffer,
                                         compare_factor);
      maxbuffer= lastbuff;
    }
  }

  /* Simulate final merge_buff call. */
  total_cost += get_merge_buffers_cost(buff_elems, elem_size,
                                       buff_elems, buff_elems + maxbuffer,
                                       compare_factor);
  return total_cost;
}


/*
  Calculate cost of using Unique for processing nkeys elements of size
  key_size using max_in_memory_size memory.

  SYNOPSIS
    Unique::get_use_cost()
      buffer    space for temporary data, use Unique::get_cost_calc_buff_size
                to get # bytes needed.
      nkeys     #of elements in Unique
      key_size  size of each elements in bytes
      max_in_memory_size   amount of memory Unique will be allowed to use
      compare_factor   used to calculate cost of one comparison
      write_fl  if the result must be saved written to disk
      in_memory_elems  OUT estimate of the number of elements in memory
                           if disk is not used  

  RETURN
    Cost in disk seeks.

  NOTES
    cost(using_unqiue) =
      cost(create_trees) +  (see #1)
      cost(merge) +         (see #2)
      cost(read_result)     (see #3)

    1. Cost of trees creation
      For each Unique::put operation there will be 2*log2(n+1) elements
      comparisons, where n runs from 1 tree_size (we assume that all added
      elements are different). Together this gives:

      n_compares = 2*(log2(2) + log2(3) + ... + log2(N+1)) = 2*log2((N+1)!)

      then cost(tree_creation) = n_compares*ROWID_COMPARE_COST;

      Total cost of creating trees:
      (n_trees - 1)*max_size_tree_cost + non_max_size_tree_cost.

      Approximate value of log2(N!) is calculated by log2_n_fact function.

    2. Cost of merging.
      If only one tree is created by Unique no merging will be necessary.
      Otherwise, we model execution of merge_many_buff function and count
      #of merges. (The reason behind this is that number of buffers is small,
      while size of buffers is big and we don't want to loose precision with
      O(x)-style formula)

    3. If only one tree is created by Unique no disk io will happen.
      Otherwise, ceil(key_len*n_keys) disk seeks are necessary. We assume
      these will be random seeks.
*/

double Unique_impl::get_use_cost(uint *buffer, size_t nkeys, uint key_size,
                            size_t max_in_memory_size,
                            double compare_factor,
                            bool intersect_fl, bool *in_memory)
{
  size_t max_elements_in_tree;
  size_t last_tree_elems;
  size_t   n_full_trees; /* number of trees in unique - 1 */
  double result;

  max_elements_in_tree= ((size_t) max_in_memory_size /
                         ALIGN_SIZE(sizeof(TREE_ELEMENT)+key_size));

  if (max_elements_in_tree == 0)
    max_elements_in_tree= 1;

  n_full_trees=    nkeys / max_elements_in_tree;
  last_tree_elems= nkeys % max_elements_in_tree;

  /* Calculate cost of creating trees */
  result= 2*log2_n_fact(last_tree_elems + 1.0);
  if (n_full_trees)
    result+= n_full_trees * log2_n_fact(max_elements_in_tree + 1.0);
  result /= compare_factor;

  DBUG_PRINT("info",("unique trees sizes: %u=%u*%u + %u", (uint)nkeys,
                     (uint)n_full_trees, 
                     (uint)(n_full_trees?max_elements_in_tree:0),
                     (uint)last_tree_elems));

  if (in_memory)
    *in_memory= !n_full_trees;

  if (!n_full_trees)
    return result;

  /*
    There is more then one tree and merging is necessary.
    First, add cost of writing all trees to disk, assuming that all disk
    writes are sequential.
  */
  result += DISK_SEEK_BASE_COST * n_full_trees *
              ceil(((double) key_size)*max_elements_in_tree / IO_SIZE);
  result += DISK_SEEK_BASE_COST * ceil(((double) key_size)*last_tree_elems / IO_SIZE);

  /* Cost of merge */
  if (intersect_fl)
    key_size+= sizeof(element_count);
  double merge_cost= get_merge_many_buffs_cost(buffer, (uint)n_full_trees,
                                               (uint)max_elements_in_tree,
                                               (uint)last_tree_elems, key_size,
                                               compare_factor);
  result += merge_cost;
  /*
    Add cost of reading the resulting sequence, assuming there were no
    duplicate elements.
  */
  result += ceil((double)key_size*nkeys/IO_SIZE);

  return result;
}

Unique_impl::~Unique_impl()
{
  close_cached_file(&file);
  delete_tree(&tree, 0);
  delete_dynamic(&file_ptrs);
  delete m_descriptor;
}


    /* Write tree to disk; clear tree */
bool Unique_impl::flush()
{
  Merge_chunk file_ptr;
  elements+= tree.elements_in_tree;
  file_ptr.set_rowcount(tree.elements_in_tree);
  file_ptr.set_file_position(my_b_tell(&file));

  tree_walk_action action= min_dupl_count ?
		           (tree_walk_action) unique_write_to_file_with_count :
		           (tree_walk_action) unique_write_to_file;
  if (tree_walk(&tree, action,
		(void*) this, left_root_right) ||
      insert_dynamic(&file_ptrs, (uchar*) &file_ptr))
    return 1;
  /**
    the tree gets reset so make sure the memory used is reset too
  */
  memory_used= 0;
  delete_tree(&tree, 0);
  return 0;
}


/*
  Clear the tree and the file.
  You must call reset() if you want to reuse Unique after walk().
*/

void
Unique_impl::reset()
{
  reset_tree(&tree);
  /*
    If elements != 0, some trees were stored in the file (see how
    flush() works). Note, that we can not count on my_b_tell(&file) == 0
    here, because it can return 0 right after walk(), and walk() does not
    reset any Unique member.
  */
  if (elements)
  {
    reset_dynamic(&file_ptrs);
    reinit_io_cache(&file, WRITE_CACHE, 0L, 0, 1);
  }
  my_free(sort.record_pointers);
  elements= 0;
  tree.flag= 0;
  sort.record_pointers= 0;
}

/*
  The comparison function, passed to queue_init() in merge_walk() and in
  merge_buffers() when the latter is called from Uniques::get() must
  use comparison function of Uniques::tree, but compare members of struct
  BUFFPEK.
*/

C_MODE_START

static int buffpek_compare(void *arg, uchar *key_ptr1, uchar *key_ptr2)
{
  BUFFPEK_COMPARE_CONTEXT *ctx= (BUFFPEK_COMPARE_CONTEXT *) arg;
  return ctx->key_compare(ctx->key_compare_arg,
                          *((uchar **) key_ptr1), *((uchar **)key_ptr2));
}

C_MODE_END


inline
element_count get_counter_from_merged_element(void *ptr, uint ofs)
{
  element_count cnt;
  memcpy((uchar *) &cnt, (uchar *) ptr + ofs, sizeof(element_count));
  return cnt;
}


inline
void put_counter_into_merged_element(void *ptr, uint ofs, element_count cnt)
{
  memcpy((uchar *) ptr + ofs, (uchar *) &cnt, sizeof(element_count));
}


/*
  DESCRIPTION

    Function is very similar to merge_buffers, but instead of writing sorted
    unique keys to the output file, it invokes walk_action for each key.
    This saves I/O if you need to pass through all unique keys only once.

  SYNOPSIS
    merge_walk()
  All params are 'IN' (but see comment for begin, end):
    merge_buffer       buffer to perform cached piece-by-piece loading
                       of trees; initially the buffer is empty
    merge_buffer_size  size of merge_buffer. Must be aligned with
                       key_length
    key_length         size of tree element; key_length * (end - begin)
                       must be less or equal than merge_buffer_size.
    begin              pointer to BUFFPEK struct for the first tree.
    end                pointer to BUFFPEK struct for the last tree;
                       end > begin and [begin, end) form a consecutive
                       range. BUFFPEKs structs in that range are used and
                       overwritten in merge_walk().
    walk_action        element visitor. Action is called for each unique
                       key.
    walk_action_arg    argument to walk action. Passed to it on each call.
    compare            elements comparison function
    compare_arg        comparison function argument
    file               file with all trees dumped. Trees in the file
                       must contain sorted unique values. Cache must be
                       initialized in read mode.
    with counters      take into account counters for equal merged
                       elements
  RETURN VALUE
    0     ok
    <> 0  error
*/

static bool merge_walk(uchar *merge_buffer, size_t merge_buffer_size,
                       uint key_length, Merge_chunk *begin, Merge_chunk *end,
                       tree_walk_action walk_action, void *walk_action_arg,
                       qsort_cmp2 compare, void *compare_arg,
                       IO_CACHE *file, bool with_counters,
                       uint min_dupl_count, bool packed)
{
  BUFFPEK_COMPARE_CONTEXT compare_context = { compare, compare_arg };
  QUEUE queue;
  if (end <= begin ||
      merge_buffer_size < (size_t) (key_length * (end - begin + 1)) ||
      init_queue(&queue, (uint) (end - begin),
                 offsetof(Merge_chunk, m_current_key), 0,
                 buffpek_compare, &compare_context, 0, 0))
    return 1;
  /* we need space for one key when a piece of merge buffer is re-read */
  merge_buffer_size-= key_length;
  uchar *save_key_buff= merge_buffer + merge_buffer_size;
  uint max_key_count_per_piece= (uint) (merge_buffer_size/(end-begin) /
                                        key_length);
  /* if piece_size is aligned reuse_freed_buffer will always hit */
  uint piece_size= max_key_count_per_piece * key_length;
  ulong bytes_read;               /* to hold return value of read_to_buffer */
  Merge_chunk *top;
  int res= 1;
  uint cnt_ofs= key_length - (with_counters ? sizeof(element_count) : 0);
  element_count cnt;

  // read_to_buffer() needs only rec_length.
  Sort_param sort_param;
  sort_param.rec_length= key_length;
  sort_param.sort_length= key_length;
  sort_param.min_dupl_count= min_dupl_count;
  DBUG_ASSERT(sort_param.res_length  == 0);
  DBUG_ASSERT(!sort_param.using_addon_fields());
  sort_param.set_using_packed_keys(packed);
  uint size_of_dupl_count= min_dupl_count ? sizeof(element_count) : 0;

  /*
    Invariant: queue must contain top element from each tree, until a tree
    is not completely walked through.
    Here we're forcing the invariant, inserting one element from each tree
    to the queue.
  */
  for (top= begin; top != end; ++top)
  {
    top->set_buffer(merge_buffer + (top - begin) * piece_size,
                    merge_buffer + (top - begin) * piece_size + piece_size);
    top->set_max_keys(max_key_count_per_piece);
    bytes_read= read_to_buffer(file, top, &sort_param, packed);
    if (unlikely(bytes_read == (ulong) -1))
      goto end;
    DBUG_ASSERT(bytes_read);
    queue_insert(&queue, (uchar *) top);
  }
  top= (Merge_chunk *) queue_top(&queue);
  while (queue.elements > 1)
  {
    /*
      Every iteration one element is removed from the queue, and one is
      inserted by the rules of the invariant. If two adjacent elements on
      the top of the queue are not equal, biggest one is unique, because all
      elements in each tree are unique. Action is applied only to unique
      elements.
    */
    void *old_key= top->current_key();
    /*
      read next key from the cache or from the file and push it to the
      queue; this gives new top.
    */
    key_length= sort_param.get_key_length_for_unique((uchar*)old_key,
                                                     size_of_dupl_count);

    cnt_ofs= key_length - (with_counters ? sizeof(element_count) : 0);
    top->advance_current_key(key_length);
    top->decrement_mem_count();
    if (top->mem_count())
      queue_replace_top(&queue);
    else /* next piece should be read */
    {
      /* save old_key not to overwrite it in read_to_buffer */
      memcpy(save_key_buff, old_key, key_length);
      old_key= save_key_buff;
      bytes_read= read_to_buffer(file, top, &sort_param, packed);
      if (unlikely(bytes_read == (ulong) -1))
        goto end;
      else if (bytes_read)      /* top->key, top->mem_count are reset */
        queue_replace_top(&queue);             /* in read_to_buffer */
      else
      {
        /*
          Tree for old 'top' element is empty: remove it from the queue and
          give all its memory to the nearest tree.
        */
        queue_remove_top(&queue);
        reuse_freed_buff(&queue, top, key_length);
      }
    }
    top= (Merge_chunk *) queue_top(&queue);
    /* new top has been obtained; if old top is unique, apply the action */
    if (compare(compare_arg, old_key, top->current_key()))
    {
      cnt= with_counters ?
           get_counter_from_merged_element(old_key, cnt_ofs) : 1;
      if (walk_action(old_key, cnt, walk_action_arg))
        goto end;
    }
    else if (with_counters)
    {
      cnt= get_counter_from_merged_element(top->current_key(), cnt_ofs);
      cnt+= get_counter_from_merged_element(old_key, cnt_ofs);
      put_counter_into_merged_element(top->current_key(), cnt_ofs, cnt);
    }
  }
  /*
    Applying walk_action to the tail of the last tree: this is safe because
    either we had only one tree in the beginning, either we work with the
    last tree in the queue.
  */
  do
  {
    do
    {
      key_length= sort_param.get_key_length_for_unique(top->current_key(),
                                                       size_of_dupl_count);
      cnt_ofs= key_length - (with_counters ? sizeof(element_count) : 0);
      cnt= with_counters ?
           get_counter_from_merged_element(top->current_key(), cnt_ofs) : 1;
      if (walk_action(top->current_key(), cnt, walk_action_arg))
        goto end;
      top->advance_current_key(key_length);
    }
    while (top->decrement_mem_count());
    bytes_read= read_to_buffer(file, top, &sort_param, packed);
    if (unlikely(bytes_read == (ulong) -1))
      goto end;
  }
  while (bytes_read);
  res= 0;
end:
  delete_queue(&queue);
  return res;
}


/*
  DESCRIPTION
    Walks consecutively through all unique elements:
    if all elements are in memory, then it simply invokes 'tree_walk', else
    all flushed trees are loaded to memory piece-by-piece, pieces are
    sorted, and action is called for each unique value.
    Note: so as merging resets file_ptrs state, this method can change
    internal Unique state to undefined: if you want to reuse Unique after
    walk() you must call reset() first!
  SYNOPSIS
    Unique:walk()
  All params are 'IN':
    table   parameter for the call of the merge method
    action  function-visitor, typed in include/my_tree.h
            function is called for each unique element
    arg     argument for visitor, which is passed to it on each call
  RETURN VALUE
    0    OK
    <> 0 error
 */

bool Unique_impl::walk(TABLE *table, tree_walk_action action, void *walk_action_arg)
{
  int res= 0;
  uchar *merge_buffer;

  if (elements == 0)                       /* the whole tree is in memory */
    return tree_walk(&tree, action, walk_action_arg, left_root_right);

  sort.return_rows= elements+tree.elements_in_tree;
  /* flush current tree to the file to have some memory for merge buffer */
  if (flush())
    return 1;
  if (flush_io_cache(&file) || reinit_io_cache(&file, READ_CACHE, 0L, 0, 0))
    return 1;
  /*
    merge_buffer must fit at least MERGEBUFF2 + 1 keys, because
    merge_index() can merge that many BUFFPEKs at once. The extra space for one key 
    is needed when a piece of merge buffer is re-read, see merge_walk()
  */
  size_t buff_sz= MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;
  if (!(merge_buffer = (uchar *)my_malloc(key_memory_Unique_merge_buffer,
                                     buff_sz, MYF(MY_THREAD_SPECIFIC|MY_WME))))
    return 1;
  if (buff_sz < full_size * (file_ptrs.elements + 1UL))
    res= merge(table, merge_buffer, buff_sz,
               buff_sz >= full_size * MERGEBUFF2) ;

  if (!res)
  {
    res= merge_walk(merge_buffer, buff_sz, full_size,
                    (Merge_chunk *) file_ptrs.buffer,
                    (Merge_chunk *) file_ptrs.buffer + file_ptrs.elements,
                    action, walk_action_arg,
                    tree.compare, tree.custom_arg, &file, with_counters,
                    min_dupl_count, is_variable_sized());
  }
  my_free(merge_buffer);
  return res;
}


/*
  DESCRIPTION

  Perform multi-pass sort merge of the elements using the buffer buff as
  the merge buffer. The last pass is not performed if without_last_merge is
  TRUE.

  SYNOPSIS
    Unique_impl::merge()
  All params are 'IN':
    table               the parameter to access sort context
    buff                merge buffer
    buff_size           size of merge buffer
    without_last_merge  TRUE <=> do not perform the last merge
  RETURN VALUE
    0    OK
    <> 0 error
 */

bool Unique_impl::merge(TABLE *table, uchar *buff, size_t buff_size,
                   bool without_last_merge)
{
  IO_CACHE *outfile= &sort.io_cache;
  Merge_chunk *file_ptr= (Merge_chunk*) file_ptrs.buffer;
  uint maxbuffer= file_ptrs.elements - 1;
  my_off_t save_pos;
  bool error= 1;
  Sort_param sort_param; 

  /* Open cached file for table records if it isn't open */
  if (! my_b_inited(outfile) &&
      open_cached_file(outfile,mysql_tmpdir,TEMP_PREFIX,READ_RECORD_BUFFER,
                       MYF(MY_WME)))
    return 1;

  bzero((char*) &sort_param,sizeof(sort_param));
  sort_param.max_rows= elements;
  sort_param.sort_form= table;
  sort_param.rec_length= sort_param.sort_length= sort_param.ref_length=
   full_size;
  sort_param.min_dupl_count= min_dupl_count;
  sort_param.res_length= 0;
  sort_param.max_keys_per_buffer=
    (uint) MY_MAX((max_in_memory_size / sort_param.sort_length), MERGEBUFF2);
  sort_param.not_killable= 1;
  sort_param.set_using_packed_keys(is_variable_sized());
  sort_param.set_packed_format(is_variable_sized());

  sort_param.unique_buff= buff +(sort_param.max_keys_per_buffer *
                                 sort_param.sort_length);

  sort_param.compare= (qsort2_cmp) buffpek_compare;
  sort_param.cmp_context.key_compare= tree.compare;
  sort_param.cmp_context.key_compare_arg= tree.custom_arg;

  /*
    We need to remove the size allocated for the unique buffer.
    The sort_buffer_size is:
      MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;
  */
  buff_size-= full_size;

  /* Merge the buffers to one file, removing duplicates */
  if (merge_many_buff(&sort_param,
                      Bounds_checked_array<uchar>(buff, buff_size),
                      file_ptr,&maxbuffer,&file))
    goto err;
  if (flush_io_cache(&file) ||
      reinit_io_cache(&file,READ_CACHE,0L,0,0))
    goto err;
  sort_param.res_length= sort_param.rec_length-
                         (min_dupl_count ? sizeof(min_dupl_count) : 0);
  if (without_last_merge)
  {
    file_ptrs.elements= maxbuffer+1;
    return 0;
  }
  if (merge_index(&sort_param,
                  Bounds_checked_array<uchar>(buff, buff_size),
                  file_ptr, maxbuffer, &file, outfile))
    goto err;
  error= 0;
err:
  if (flush_io_cache(outfile))
    error= 1;

  /* Setup io_cache for reading */
  save_pos= outfile->pos_in_file;
  if (reinit_io_cache(outfile,READ_CACHE,0L,0,0))
    error= 1;
  outfile->end_of_file=save_pos;
  return error;
}


/*
  Allocate memory that can be used with init_records() so that
  rows will be read in priority order.
*/

bool Unique_impl::get(TABLE *table)
{
  bool rc= 1;
  uchar *sort_buffer= NULL;
  sort.return_rows= elements+tree.elements_in_tree;
  DBUG_ENTER("Unique_impl::get");

  DBUG_ASSERT(is_variable_sized() == FALSE);

  if (my_b_tell(&file) == 0)
  {
    /* Whole tree is in memory;  Don't use disk if you don't need to */
    if ((sort.record_pointers= (uchar*)
	 my_malloc(key_memory_Filesort_info_record_pointers,
                   size * tree.elements_in_tree, MYF(MY_THREAD_SPECIFIC))))
    {
      uchar *save_record_pointers= sort.record_pointers;
      tree_walk_action action= min_dupl_count ?
		         (tree_walk_action) unique_intersect_write_to_ptrs :
		         (tree_walk_action) unique_write_to_ptrs;
      filtered_out_elems= 0;
      (void) tree_walk(&tree, action,
		       this, left_root_right);
      /* Restore record_pointers that was changed in by 'action' above */
      sort.record_pointers= save_record_pointers;
      sort.return_rows-= filtered_out_elems;
      DBUG_RETURN(0);
    }
  }
  /* Not enough memory; Save the result to file && free memory used by tree */
  if (flush())
    DBUG_RETURN(1);
  /*
    merge_buffer must fit at least MERGEBUFF2 + 1 keys, because
    merge_index() can merge that many BUFFPEKs at once. The extra space for
    one key for Sort_param::unique_buff
  */
  size_t buff_sz= MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;

  if (!(sort_buffer= (uchar*) my_malloc(key_memory_Unique_sort_buffer, buff_sz,
                                        MYF(MY_THREAD_SPECIFIC|MY_WME))))
    DBUG_RETURN(1);

  if (merge(table, sort_buffer, buff_sz,  FALSE))
    goto err;
  rc= 0;  

err:  
  my_free(sort_buffer);  
  DBUG_RETURN(rc);
}


/*
  @brief
    Write an intermediate unique record to the file

  @param key                  key to be written

  @retval
    >0   Error
    =0   Record successfully written
*/

int Unique_impl::write_record_to_file(uchar *key)
{
  return my_b_write(get_file(), key, m_descriptor->get_length_of_key(key));
}


/*                VARIABLE SIZE KEYS DESCRIPTOR                             */


Variable_size_keys_descriptor::Variable_size_keys_descriptor(uint length)
{
  max_length= length;
  flags= (1 << VARIABLE_SIZED_KEYS);
  sort_keys= NULL;
  sortorder= NULL;
}


/*
  @brief
    Setup the structures that are used when Unique stores packed values

  @param thd                   thread structure
  @param item                  item of aggregate function
  @param non_const_args        number of non constant arguments
  @param arg_count             total number of arguments

  @note
    This implementation is used by GROUP_CONCAT and COUNT_DISTINCT
    as it can have more than one arguments in the argument list.

  @retval
    TRUE  error
    FALSE setup successful
*/

bool
Variable_size_keys_descriptor::setup_for_item(THD *thd, Item_sum *item,
                                              uint non_const_args,
                                              uint arg_count)
{
  SORT_FIELD *pos;
  if (init(thd, non_const_args))
    return true;
  pos= sortorder;

  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg= item->get_arg(i);
    if (arg->const_item())
      continue;

    if (arg->type() == Item::FIELD_ITEM)
    {
      Field *field= ((Item_field*)arg)->field;
      pos->setup_key_part_for_variable_size_key(field);
    }
    else
      pos->setup_key_part_for_variable_size_key(arg);
    pos++;
  }
  return false;
}


/*
  @brief
    Setup the structures that are used when Unique stores packed values

  @param thd                   thread structure
  @param field                 field structure

  @retval
    TRUE  error
    FALSE setup successful
*/

bool Variable_size_keys_descriptor::setup_for_field(THD *thd, Field *field)
{
  SORT_FIELD *pos;
  if (init(thd, 1))
    return true;
  pos= sortorder;
  pos->setup_key_part_for_variable_size_key(field);
  return false;
}


/*
  @brief
    Compare two packed keys inside the Unique tree

  @param a_ptr             packed sort key
  @param b_ptr             packed sort key

  @retval
    >0   key a_ptr greater than b_ptr
    =0   key a_ptr equal to b_ptr
    <0   key a_ptr less than b_ptr

*/

int Variable_size_composite_key_desc::compare_keys(uchar *a_ptr,
                                                uchar *b_ptr)
{
  uchar *a= a_ptr + Variable_size_keys_descriptor::size_of_length_field;
  uchar *b= b_ptr + Variable_size_keys_descriptor::size_of_length_field;
  int retval= 0;
  size_t a_len, b_len;
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    retval= sort_field->is_variable_sized() ?
            sort_field->compare_packed_varstrings(a, &a_len, b, &b_len) :
            sort_field->compare_packed_fixed_size_vals(a, &a_len, b, &b_len);

    if (retval)
      return sort_field->reverse ? -retval : retval;

    a+= a_len;
    b+= b_len;
  }
  return retval;
}


int Variable_size_composite_key_desc_for_gconcat::compare_keys(uchar *a_ptr,
                                                               uchar *b_ptr)
{
  uchar *a= a_ptr + Variable_size_keys_descriptor::size_of_length_field;
  uchar *b= b_ptr + Variable_size_keys_descriptor::size_of_length_field;
  int retval= 0;
  size_t a_len, b_len;
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    retval= sort_field->is_variable_sized() ?
            sort_field->compare_packed_varstrings(a, &a_len, b, &b_len) :
            sort_field->compare_fixed_size_vals(a, &a_len, b, &b_len);

    if (retval)
      return sort_field->reverse ? -retval : retval;

    a+= a_len;
    b+= b_len;
  }
  return retval;
}


int Variable_size_keys_simple::compare_keys(uchar *a, uchar *b)
{
  return sort_keys->compare_keys_for_single_arg(a + size_of_length_field,
                                                b + size_of_length_field);
}


/*
  @brief
    Create the sortorder and Sort keys structures for a descriptor

  @param thd                   THD structure
  @param count                 Number of key parts to be allocated

  @retval
    TRUE                       ERROR
    FALSE                      structures successfully created
*/

bool Descriptor::init(THD *thd, uint count)
{
  if (sortorder)
    return false;
  DBUG_ASSERT(sort_keys == NULL);
  sortorder= (SORT_FIELD*) thd->alloc(sizeof(SORT_FIELD) * count);
  if (!sortorder)
    return true;  // OOM
  sort_keys= new Sort_keys(sortorder, count);
  if (!sort_keys)
    return true;  // OOM
  return false;
}


bool
Variable_size_composite_key_desc_for_gconcat::setup_for_item(THD *thd,
                                                          Item_sum *item,
                                                          uint non_const_args,
                                                          uint arg_count)
{
  if (init(thd, non_const_args))
    return true;
  SORT_FIELD *pos;
  pos= sortorder;

  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg= item->get_arg(i);
    if (arg->const_item())
      continue;

    Field *field= arg->get_tmp_table_field();
    pos->setup_key_part_for_variable_size_key(field);
    pos++;
  }
  return false;
}


/*                   FIXED SIZE KEYS DESCRIPTOR                             */


Fixed_size_keys_descriptor::Fixed_size_keys_descriptor(uint length)
{
  max_length= length;
  flags= (1 << FIXED_SIZED_KEYS);
  sort_keys= NULL;
  sortorder= NULL;
}


int Fixed_size_keys_descriptor::compare_keys(uchar *a, uchar *b)
{
  DBUG_ASSERT(sort_keys);
  SORT_FIELD *sort_field= sort_keys->begin();
  DBUG_ASSERT(sort_field->field);
  return sort_field->field->cmp(a, b);
}


bool
Fixed_size_keys_descriptor::setup_for_item(THD *thd, Item_sum *item,
                                           uint non_const_args,
                                           uint arg_count)
{
  SORT_FIELD *pos;
  if (Descriptor::init(thd, non_const_args))
    return true;
  pos= sortorder;

  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg= item->get_arg(i);
    if (arg->const_item())
      continue;

    Field *field= arg->get_tmp_table_field();

    DBUG_ASSERT(field);
    pos->setup_key_part_for_fixed_size_key(field);
    pos++;
  }
  return false;
}


bool
Fixed_size_keys_descriptor::setup_for_field(THD *thd, Field *field)
{
  SORT_FIELD *pos;
  if (Descriptor::init(thd, 1))
    return true;
  pos= sortorder;
  pos->setup_key_part_for_fixed_size_key(field);

  return false;
}


int Fixed_size_keys_mem_comparable::compare_keys(uchar *key1, uchar *key2)
{
  return memcmp(key1, key2, max_length);
}


int
Fixed_size_composite_keys_descriptor::compare_keys(uchar *key1, uchar *key2)
{
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    Field *field= sort_field->field;
    int res = field->cmp(key1, key2);
    if (res)
      return res;
    key1 += sort_field->length;
    key2 += sort_field->length;
  }
  return 0;
}


int Fixed_size_keys_for_rowids::compare_keys(uchar *key1, uchar *key2)
{
  return file->cmp_ref(key1, key2);
}


int
Fixed_size_keys_descriptor_with_nulls::compare_keys(uchar *key1_arg,
                                                    uchar *key2_arg)
{

  /*
    We have to use get_tmp_table_field() instead of
    real_item()->get_tmp_table_field() because we want the field in
    the temporary table, not the original field
  */
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    Field *field= sort_field->field;
    if (field->is_null_in_record(key1_arg) &&
        field->is_null_in_record(key2_arg))
      return 0;

    if (field->is_null_in_record(key1_arg))
      return -1;

    if (field->is_null_in_record(key2_arg))
      return 1;

    uchar *key1= (uchar*)key1_arg + field->table->s->null_bytes;
    uchar *key2= (uchar*)key2_arg + field->table->s->null_bytes;

    uint offset= (field->offset(field->table->record[0]) -
                  field->table->s->null_bytes);
    int res= field->cmp(key1 + offset, key2 + offset);
    if (res)
      return res;
  }
  return 0;
}


int Fixed_size_keys_for_group_concat::compare_keys(uchar *key1, uchar *key2)
{
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    Field *field= sort_field->field;
    uint offset= (field->offset(field->table->record[0]) -
                  field->table->s->null_bytes);
    int res= field->cmp(key1 + offset, key2 + offset);
    if (res)
      return res;
  }
  return 0;
}


bool Key_encoder::init(uint length)
{
  if (tmp_buffer.alloc(length))
    return true;
  rec_ptr= (uchar *)my_malloc(PSI_INSTRUMENT_ME,
                              length,
                              MYF(MY_WME | MY_THREAD_SPECIFIC));
  return rec_ptr == NULL;
}


Key_encoder::~Key_encoder()
{
  my_free(rec_ptr);
}


/*
  @brief
    Make a record with packed values for a key

  @retval
    0         NULL value
    >0        length of the packed record
*/

uchar* Key_encoder_for_variable_size_key::make_record(Sort_keys *sort_keys,
                                                      bool exclude_nulls)
{
  Field *field;
  SORT_FIELD *sort_field;
  uint length;
  uchar *orig_to, *to;

  orig_to= to= rec_ptr;
  to+= Variable_size_keys_descriptor::size_of_length_field;

  for (sort_field=sort_keys->begin() ;
       sort_field != sort_keys->end() ;
       sort_field++)
  {
    bool maybe_null=0;
    if ((field=sort_field->field))
    {
      // Field
      length= field->make_packed_sort_key_part(to, sort_field);
    }
    else
    {           // Item
      Item *item= sort_field->item;
      length= item->type_handler()->make_packed_sort_key_part(to, item,
                                                              sort_field,
                                                              &tmp_buffer);
    }

    if ((maybe_null= sort_field->maybe_null))
    {
      if (exclude_nulls && length == 0)  // rejecting NULLS
        return NULL;
      to++;
    }
    to+= length;
  }

  length= static_cast<uint>(to - orig_to);
  Variable_size_keys_descriptor::store_packed_length(orig_to, length);
  return rec_ptr;
}


uchar* Key_encoder_for_group_concat::make_record(Sort_keys *sort_keys,
                                                 bool exclude_nulls)
{
  Field *field;
  SORT_FIELD *sort_field;
  uint length;
  uchar *orig_to, *to;

  orig_to= to= rec_ptr;
  to+= Variable_size_keys_descriptor::size_of_length_field;

  for (sort_field=sort_keys->begin() ;
       sort_field != sort_keys->end() ;
       sort_field++)
  {
    bool maybe_null=0;
    DBUG_ASSERT(sort_field->field);
    field=sort_field->field;
    length= field->make_packed_key_part(to, sort_field);

    if ((maybe_null= sort_field->maybe_null))
    {
      if (exclude_nulls && length == 0)  // rejecting NULLS
        return NULL;
      to++;
    }
    to+= length;
  }

  length= static_cast<uint>(to - orig_to);
  Variable_size_keys_descriptor::store_packed_length(orig_to, length);
  return rec_ptr;
}

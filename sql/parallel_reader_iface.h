#pragma once

/** Interface for implementation of parallel scans for different engines */

namespace Parallel_scan
{

// OLEGS: is this class needed?
class Coordinator
{
public:
  Coordinator() = default;
  virtual ~Coordinator() = default;

  virtual int initialize(size_t n_threads) = 0;
  //virtual int prepare_full_table_scan() = 0;
  // TODO: virtual int prepare_range_scan() = 0;

  //virtual Worker_context *get_worker_context(size_t worker_idx) = 0;
  virtual void cleanup() = 0;
};

/* 
  Opaque data structure that allows the coordinator to identify different workers.
  Each worker thread gets one when being initialized for parallel scan.
  Engine-specific implementation should inherit from this structure and add
  necessary fields
*/
struct Worker_ctx
{
  virtual ~Worker_ctx() = default;
};

} // End of namespace Parallel_scan
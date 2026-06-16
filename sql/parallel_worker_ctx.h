#pragma once

/* 
  Opaque data structure that allows the engine to identify different workers.
  Each worker thread gets one when being initialized for parallel scan.
  Engine-specific implementation should inherit from this structure and
  add necessary fields
*/
struct Parallel_worker_ctx
{
  virtual ~Parallel_worker_ctx() = default;
};

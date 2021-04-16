#include <tpool.h>

namespace tpool
{
static thread_local tpool::thread_pool* tls_thread_pool;

extern "C" void set_tls_pool(tpool::thread_pool* pool)
{
  tls_thread_pool = pool;
}

extern "C" void tpool_wait_begin()
{
  if (tls_thread_pool)
    tls_thread_pool->wait_begin();
}


extern "C" void tpool_wait_end()
{
  if (tls_thread_pool)
    tls_thread_pool->wait_end();
}

}

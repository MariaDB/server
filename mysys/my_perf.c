#include "my_global.h"

#include "my_perf.h"


static my_bool my_perf_inited= FALSE;


my_bool
my_perf_init(void)
{
  int ret;

  if (my_perf_inited)
    return FALSE;
  ret= pfm_initialize();
  if (ret != PFM_SUCCESS)
    return TRUE;

  my_perf_inited= TRUE;
  return FALSE;
}


void
my_perf_deinit(void)
{
  if (my_perf_inited)
  {
    pfm_terminate();
    my_perf_inited= FALSE;
  }
}

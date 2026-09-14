#include "my_global.h"

#include "my_perf.h"


static my_bool my_perf_inited= FALSE;


static const char *event_names[]= {
  "cpu-cycles",
  "instructions"
};

static perf_event_attr_t perf_attr[2];
static int perf_fds[2];
/* Note: one extra buffer for event count. */
static uint64_t perf_read_buf[1+2];

my_bool
my_perf_init(void)
{
  int ret;
  int i;

  if (my_perf_inited)
    return FALSE;
  memset(perf_attr, 0, sizeof(perf_attr));
  ret= pfm_initialize();
  if (ret != PFM_SUCCESS)
    return TRUE;

  for (i= 0; i < 2; ++i)
  {
    ret= pfm_get_perf_event_encoding(event_names[i], PFM_PLM3,
                                     &perf_attr[i], NULL, NULL);
    if (ret != PFM_SUCCESS)
    {
      pfm_terminate();
      return TRUE;
    }
    perf_attr[i].size= sizeof(perf_attr[i]);
    perf_attr[i].read_format= PERF_FORMAT_GROUP;
    perf_attr[i].disabled= (i == 0);
    perf_attr[i].pinned= (i == 0);
  }

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

my_bool
my_perf_start_record(void)
{
  int ret;
  int i;

  for (i= 0; i < 2; ++i)
  {
    perf_fds[i]= perf_event_open(&perf_attr[i], 0, -1,
                                 (i ? perf_fds[0] : -1), 0);
    /* ToDo: Not to leave some fds open in case of error. */
    if (unlikely(perf_fds[i]) < 0)
    {
      /* ToDo: Something other than fprintf(stderr, ...) for messages. */
      fprintf(stderr, "perf_event_open(%d) failed: ret=%d errno=%d (%s)\n",
              i, perf_fds[i], errno, strerror(errno));
      return TRUE;
    }
    if (unlikely((ret= ioctl(perf_fds[i], PERF_EVENT_IOC_RESET, 0))))
    {
      fprintf(stderr, "ioctl(%d, PERF_EVENT_IOC_RESET) error, ret=%d errno=%d\n",
              i, ret, errno);
      return TRUE;
    }
  }
  if (unlikely((ret= ioctl(perf_fds[0], PERF_EVENT_IOC_ENABLE, 0))))
  {
    fprintf(stderr, "ioctl(%d, PERF_EVENT_IOC_ENABLE) error, ret=%d errno=%d\n",
            i, ret, errno);
    return TRUE;
  }
  return FALSE;
}

void
my_perf_end_record(void)
{
  int ret;
  int i;

  if (unlikely((ret= ioctl(perf_fds[0], PERF_EVENT_IOC_DISABLE, 0))))
    fprintf(stderr, "WARNING: ioctl(PERF_EVENT_IOC_DISABLE) error, ret=%d errno=%d\n",
            ret, errno);

  i= read(perf_fds[0], &perf_read_buf[0], sizeof(perf_read_buf));
  if (i != sizeof(perf_read_buf))
    fprintf(stderr, "WARNING: read() on perf event returned short %d expected %d; errno=%d (%s)\n",
            i, (int)(sizeof(perf_read_buf)), errno, strerror(errno));
  else if (perf_read_buf[0] != 2)
    fprintf(stderr, "WARNING: read() on perf event returned events %d expected %d\n",
            (int)(perf_read_buf[0]), 2);
  else
  {
    for (i= 0; i < 2; ++i)
      fprintf(stderr, "INFO: counter %s=%llu\n", event_names[i], (ulonglong)perf_read_buf[i+1]);
  }
  for (i= 0; i < 2; ++i)
  {
    close(perf_fds[i]);
    perf_fds[i]= -1;
  }
}

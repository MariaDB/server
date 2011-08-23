/*
  TODO: MP AB Copyright
*/

/*
  Design
  - Mutex-guarded request queue (it belongs to the target), which can be enabled/
    disabled (when empty).

  - After the request has been put into queue, the requestor waits for request
    to be satisfied. The worker satisifes the request and signals the
    requestor.
*/

/*
  Target for asynchronous calls.
*/
class Apc_target
{
public:
  Apc_target() : enabled(0), apc_calls(NULL) /*, call_queue_size(0)*/ {} 
  ~Apc_target() { DBUG_ASSERT(!enabled && !apc_calls);}

  /* 
    Initialize the target. This must be called before anything else. Right
    after initialization, the target is disabled.
  */
  void init();

  /* 
    Destroy the target. The target must be disabled when this call is made.
  */
  void destroy();
  
  /* 
    Enter into state where this target will be serving APC requests
  */
  void enable();

  /* 
    Leave the state where we could serve APC requests (will serve all already 
    enqueued requests)
  */
  void disable();
  
  /*
    This should be called periodically to serve observation requests.
  */
  void process_apc_requests();

  typedef void (*apc_func_t)(void *arg);
  
  /*
    Make an APC call: schedule it for execution and wait until the target
    thread has executed it. This function must not be called from a thread
    that's different from the target thread.

    @retval FALSE - Ok, the call has been made
    @retval TRUE  - Call wasnt made (either the target is in disabled state or
                    timeout occured)
  */
  bool make_apc_call(apc_func_t func, void *func_arg, 
                     int timeout_sec, bool *timed_out);

private:
  class Call_request;
  int enabled;

  Call_request *apc_calls;
  pthread_mutex_t LOCK_apc_queue;
  //int call_queue_size;

  class Call_request
  {
  public:
    apc_func_t func;
    void *func_arg;
    bool done;

    pthread_mutex_t LOCK_request;
    pthread_cond_t COND_request;

    Call_request *next;
    Call_request *prev;
    
    const char *what;
  };

  void enqueue_request(Call_request *qe);
  void dequeue_request(Call_request *qe);
  Call_request *get_first_in_queue()
  { 
    return apc_calls;
  }
};

///////////////////////////////////////////////////////////////////////


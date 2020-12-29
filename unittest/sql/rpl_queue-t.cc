#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tap.h>
#include <my_global.h>
#include <queue.h>
#include <rpl_queue.h>

class dummy_Log_event
{
  public:
  char arr[20];
  dummy_Log_event(char data)
  {
    for(uint i= 0; i< 20; i++)
      arr[i]= data;

  };
  static uint32 get_size()
  {
    return 20;
  }
};
void enqueue(circular_buffer_queue_events *queue, char c)
{
  void *memory= queue->enqueue_1(dummy_Log_event::get_size());
  dummy_Log_event * _debug __attribute__((unused))= new(memory) dummy_Log_event(c) ;
  queue->enqueue_2(dummy_Log_event::get_size());
}
void dequeue(circular_buffer_queue_events *queue)
{
  dummy_Log_event *dl= static_cast<dummy_Log_event *>(queue->dequeue_1(dummy_Log_event::get_size()));
  fwrite(dl->arr, sizeof(char), 20, stdout);
  printf("\n");
}
int main(int argc __attribute__((unused)),char *argv[])
{
  plan(1);
  circular_buffer_queue_events *queue = new circular_buffer_queue_events();
  queue->init(90);
  for(int i = 0; i < 4; i++)
  {
    enqueue(queue, i+65);
  }
  //this one will not wrap arround
  dequeue(queue);
  dequeue(queue);
  enqueue(queue, 69);
  for(int i = 0; i < 3; i++)
  {
    dequeue(queue);
  }
  
  queue->destroy();
  ok(true," ");
  return exit_status();
};

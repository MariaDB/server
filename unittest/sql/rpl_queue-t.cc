#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tap.h>
#include <my_global.h>
#include <queue.h>
#include <rpl_queue.h>

uchar* create_dummy_event(char c, int size)
{
  uchar *data= (uchar *)malloc(size);
  memset(data, c, size);
  int4store(data+EVENT_LEN_OFFSET, size);
  return data;
}


class dummy_queue:public circular_buffer_queue<slave_queue_element>
{

};
int main(int argc __attribute__((unused)),char *argv[])
{
  dummy_queue *queue = new dummy_queue();
  int counter= 0;
  queue->init(80);
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('A', 25)))? 0 : 1;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('B', 26)))? 0 : 1;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('C', 25)))? 0 : 1;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('D', 25)))? 0 : 1;
  for(int i = 0; i < counter; i++)
  {
    slave_queue_element *el= queue->dequeue();
    if (el)
    {
      fwrite(el->event, sizeof(char), el->total_length, stdout);
      el->~slave_queue_element();
    }
  }

  counter= 0;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('A', 25)))? 0 : 1;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('B', 26)))? 0 : 1;
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('C', 25)))? 0 : 1;
  queue->dequeue();
  counter += queue->enqueue(new slave_queue_element(create_dummy_event('D', 25)))? 0 : 1;
  for(int i = 0; i < counter; i++)
  {
    slave_queue_element *el= queue->dequeue();
    if (el)
    {
      fwrite(el->event, sizeof(char), el->total_length, stdout);
      el->~slave_queue_element();
    }
  }
  plan(1);
  ok(true," ");
  return exit_status();
}

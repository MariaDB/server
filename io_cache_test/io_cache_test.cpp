#include "my_sys.h"
#include "pthread.h"
#include "time.h"
#include "ring_buffer.hpp"
#include <fstream>

RingBuffer *cache;
uchar *buff_from;
uchar *buff_to;

void *read_to_cache(void *) {
  for (int i= 0; i < 32; ++i)
    cache->read(buff_to + (i * 255), 255);
  return NULL;
}

void *write_to_cache(void *args) {
  int *v_args= (int *) args;
  for (int i= v_args[0]; i < v_args[1]; ++i)
    cache->write(buff_from + (i * 255), 255);
  return NULL;
}

void *write_to_cache_one(void*) {
  for (int i= 0; i < 32; ++i)
    cache->write_slot(buff_from + (i * 255), 255);
  return NULL;
}

int main() {

  {
    std::ofstream off;
    off.open("tandom.txt");
    char c;
    int r;
    buff_from = (uchar*) malloc(10000);

    srand (0);    // initialize the random number generator
    for (int i=0; i<8160; i++)
    {
      r = i % 4;   // generate a random number
      if(r == 3) {
        buff_from[i] = '\n';
        continue;
      }
      c = 'a' + rand() % 26;            // Convert to a character from a-z
      buff_from[i] = c;
    }
    off << buff_from;
  }

  remove("cache_file.txt");
  pthread_t thr_read;
  pthread_t *thr_write= (pthread_t *) malloc(8 * sizeof(pthread_t));
  //int args[8];

  clock_t tss, tee;
  buff_to= (uchar *) malloc(sizeof(uchar) * 10000);
  tss= clock();

  cache= new RingBuffer((char *) "cache_file.txt", 4096);
/*
  for (int i= 0; i < 4; ++i) {
    args[i * 2]= i * 8;
    args[(i * 2) + 1]= (i + 1) * 8;
    pthread_create(&thr_write[i], NULL, write_to_cache, (void *) &args[i * 2]);
  }
*/

  pthread_create(&thr_write[0], NULL, write_to_cache_one, NULL);
  pthread_join(thr_write[0], NULL);

  pthread_create(&thr_read, NULL, read_to_cache, NULL);
  /*
  for (int i= 0; i < 4; ++i)
    pthread_join(thr_write[i], NULL);
  */
  pthread_join(thr_read, NULL);

  delete cache;

  tee= clock();
  printf("Time: %lld\n", (long long) tee - tss);
  std::ofstream of;
  of.open("test_out.txt", std::ios_base::out);
  of << buff_to;
  return 0;
}

#include <mysql/psi/mysql_file.h>
#include <mysys_priv.h>
#include <semaphore.h>
#include <atomic>

class RingBuffer
{
public:
  enum WriteState{
    SUCSECC,
    ERR_FLUSH,
    ERR_FILE_WRITE
  };

  RingBuffer(char* filename, size_t cachesize);
  int read(uchar *To, size_t Count);
  WriteState write(uchar *From, size_t Count);
  int write_slot(uchar *From, size_t Count);
  int read_slot(uchar *To, size_t Count);
  ~RingBuffer();

private:

  struct cache_slot {
    std::atomic<bool> vacant{true};
    mysql_mutex_t vacant_lock;
    volatile bool finished = false;
    volatile int next = -1;
    uchar* pos_write_first = nullptr;
    uchar* pos_write_second = nullptr;
    uchar* pos_end = nullptr;

    size_t count_first = 0;
    size_t count_second = 0;

    cache_slot() {
      mysql_mutex_init(key_IO_CACHE_append_buffer_lock,
                       &vacant_lock, MY_MUTEX_INIT_FAST);
    }
    ~cache_slot() {
      mysql_mutex_destroy(&vacant_lock);
    }
  };
  static const int count_thread_for_slots = 4;
  cache_slot _slots[count_thread_for_slots];

  sem_t semaphore;

  int last_slot = -1;

  mysql_rwlock_t flush_rw_lock;

  int slot_acquire(uchar*& From, size_t& Count) {
    sem_wait(&semaphore);
    int i;
    mysql_mutex_lock(&_buffer_lock);
    for (i = 0; i < count_thread_for_slots; ++i) {
      auto &vacant= _slots[i].vacant;
      if(vacant.load(std::memory_order_relaxed)
          && vacant.exchange(false, std::memory_order_acquire))
        break;
    }



    if(Count > (_buffer_length - _total_size)) {
      size_t rest_length_to_right_border = _write_end - _write_new_pos;
      memcpy(_write_new_pos, From, rest_length_to_right_border);
      _total_size += rest_length_to_right_border;
      Count -= rest_length_to_right_border;
      From += rest_length_to_right_border;
      _write_pos += rest_length_to_right_border;

      size_t rest_length_to_read_border = _append_read_pos - _write_buffer;
      memcpy(_write_buffer, From, rest_length_to_read_border);
      _total_size += rest_length_to_read_border;
      Count -= rest_length_to_read_border;
      From += rest_length_to_read_border;
      _write_pos = _write_buffer + rest_length_to_read_border;

      mysql_rwlock_wrlock(&flush_rw_lock);
      _flush_io_buffer(i);
      mysql_rwlock_unlock(&flush_rw_lock);
    }

    mysql_rwlock_rdlock(&flush_rw_lock);


    if(last_slot != -1)
      _slots[last_slot].next = i;
    last_slot = i;

    _slots[i].pos_write_first= _write_new_pos;

    size_t rest_length_to_right_border = _write_end - _write_new_pos;
    if(Count > rest_length_to_right_border) {
      _slots[i].count_first = rest_length_to_right_border;
      _slots[i].pos_write_second = _write_buffer;
      _slots[i].count_second = Count - rest_length_to_right_border;
      _slots[i].pos_end = (_write_buffer + (Count - rest_length_to_right_border));
      _write_new_pos = _slots[i].pos_end;
    }
    else {
      _slots[i].count_first = Count;
      _slots[i].pos_end = (_write_new_pos += Count);
    }
    _total_size += Count;
    mysql_mutex_unlock(&_buffer_lock);
    return i;
  }

  bool slot_release(int slot_id) {

    _slots[slot_id].finished = true;
    mysql_rwlock_unlock(&flush_rw_lock);
    mysql_mutex_lock(&_buffer_lock);
    if(last_slot != -1 && _write_pos == _slots[slot_id].pos_write_first) {
      do {
        _write_pos = _slots[slot_id].pos_end;

        int tmp_id = slot_id;
        slot_id = _slots[slot_id].next;

        _slots[tmp_id].next = -1;
        _slots[tmp_id].finished = false;
        assert(_slots[tmp_id].pos_write_first != nullptr);
        assert(_slots[tmp_id].pos_end != nullptr);
        _slots[tmp_id].pos_write_first = _slots[tmp_id].pos_write_second
            = _slots[tmp_id].pos_end = nullptr;
        assert(!_slots[tmp_id].vacant);
        _slots[tmp_id].vacant = true;
        sem_post(&semaphore);
      }
      while(slot_id != -1 && _slots[slot_id].finished);
      if(slot_id == -1) last_slot = -1;
    }

    mysql_mutex_unlock(&_buffer_lock);

    return true;
  }

  size_t _total_size;

  File _file;

  /* buffer writes */
  uchar *_write_buffer;
  /* Points to the current read position in the write buffer. */
  uchar *_append_read_pos;

  /* Points to current write position in the write buffer */
  uchar *_write_pos;

  /* The non-inclusive boundary of the valid write area */
  uchar *_write_end;

  /* Offset in file corresponding to the first byte of uchar* buffer. */
  my_off_t _pos_in_file;
  /*
    Maximum of the actual end of file and
    the position represented by read_end.
  */
  my_off_t _end_of_file;
  /* Points to current read position in the buffer */
  uchar *_read_pos;
  /* the non-inclusive boundary in the buffer for the currently valid read */
  uchar *_read_end;

  /* read buffer */
  uchar *_buffer;

  int _seek_not_done;

  size_t _alloced_buffer;

  size_t _buffer_length;

  uchar *_write_new_pos;

  mysql_mutex_t _buffer_lock;

  /* For a synchronized writer. */
  mysql_cond_t _cond_writer;

  /* To sync on writers into buffer. */
  mysql_mutex_t _mutex_writer;

  /* For single reader */
  mysql_mutex_t _read_lock;

  size_t _read_length;

  int _error;

  int _flush_io_buffer(int not_release);

  int _read_append(uchar* To, size_t Count, my_off_t pos_in_file);
};

RingBuffer::WriteState RingBuffer::write(uchar *From, size_t Count) {
  size_t rest_length;
  const uchar* saved_buffer;
  uchar* saved_write_pos;

  mysql_mutex_lock(&_buffer_lock);
  saved_buffer = From;
  rest_length= (size_t) (_write_end - _write_pos);
  if(Count <= rest_length)
    goto end;

  From += rest_length;
  Count -= rest_length;
  saved_write_pos = _write_pos;
  _write_pos += rest_length;
  mysql_mutex_unlock(&_buffer_lock);
  memcpy(saved_write_pos, saved_buffer, rest_length);

/*
  if(_flush_io_buffer())
    return ERR_FLUSH;
*/
  mysql_mutex_lock(&_buffer_lock);
  if (Count >= _buffer_length)
  {
    if (mysql_file_write(_file, From, Count, MY_NABP))
    {
      mysql_mutex_unlock(&_buffer_lock);
      _error= -1;
      return ERR_FILE_WRITE;
    }

    From+=Count;
    ;
    _end_of_file+=Count;
  }
  saved_buffer = From;
  end:
  saved_write_pos = _write_new_pos;
  _write_new_pos+=Count;
  mysql_mutex_unlock(&_buffer_lock);
  memcpy(saved_write_pos, saved_buffer, Count);


  mysql_mutex_lock(&_mutex_writer);
  while(saved_write_pos != _write_pos)
    mysql_cond_wait(&_cond_writer, &_mutex_writer);
  mysql_mutex_unlock(&_mutex_writer);

  mysql_mutex_lock(&_buffer_lock);
  _write_pos = _write_new_pos;
  mysql_mutex_unlock(&_buffer_lock);
  mysql_cond_signal(&_cond_writer);

  return SUCSECC;
}

int RingBuffer::write_slot(uchar* From, size_t Count) {
  int slot_id = slot_acquire(From, Count);

  memcpy(_slots[slot_id].pos_write_first, From, _slots[slot_id].count_first);
  if(_slots[slot_id].pos_write_second)
    memcpy(_slots[slot_id].pos_write_second, From + _slots[slot_id].count_first,
           _slots[slot_id].count_second);

  slot_release(slot_id);
  return 0;
}

int RingBuffer::_read_append(uchar* To, size_t Count, my_off_t pos_in_file){
  size_t len_in_buff, copy_len, transfer_len;
  uchar* save_append_read_pos;
  mysql_mutex_lock(&_buffer_lock);

  len_in_buff = (size_t) (_write_pos - _append_read_pos);
  DBUG_ASSERT(_append_read_pos <= _write_pos);
  copy_len=MY_MIN(Count, len_in_buff);
  save_append_read_pos = _append_read_pos;
  _append_read_pos += copy_len;
  mysql_mutex_unlock(&_buffer_lock);


  memcpy(To, save_append_read_pos, copy_len);
  /*
  Count -= copy_len;
  if (Count)
    info->error= (int) (save_count - Count);
  */

  /* Fill read buffer with data from write buffer */
  memcpy(_buffer, _append_read_pos,
  (size_t) (transfer_len=len_in_buff - copy_len));
  mysql_mutex_lock(&_buffer_lock);
  _read_pos= _buffer;
  _read_end= _buffer+transfer_len;
  _append_read_pos=_write_pos;
  _pos_in_file=pos_in_file+copy_len;
  _end_of_file+=len_in_buff;
  mysql_mutex_unlock(&_buffer_lock);
  return 0;
}


int RingBuffer::read(uchar *To, size_t Count) {
  size_t left_length = 0, /*diff_length,*/ length, max_length;
  my_off_t pos_in_file;
  int error;

  if (_read_pos + Count <= _read_end)
  {
    memcpy(To, _read_pos, Count);
    _read_pos+= Count;
    return 0;
  }

  if(_read_pos != _read_end)
  {
    left_length= (size_t) (_read_end - _read_pos);
    DBUG_ASSERT(Count > left_length);
    memcpy(To, _read_pos, left_length);
    To+=left_length;
    Count-=left_length;
  }

  mysql_mutex_lock(&_buffer_lock);
  if ((pos_in_file=_pos_in_file +
                   (size_t) (_read_end - _buffer)) < _end_of_file)
  {
    error= mysql_file_seek(_file, pos_in_file, MY_SEEK_SET, MYF(0)) ==
           MY_FILEPOS_ERROR;

    if (!error)
    {
      _seek_not_done= 0;

      //diff_length= (size_t) (pos_in_file & (IO_SIZE - 1));
      /*
      if (Count >= (size_t) (IO_SIZE + (IO_SIZE - diff_length)))
      {
        // Fill first intern buffer
        size_t read_length;

        length= _round_to_block(Count) - diff_length;

        read_length=
            mysql_file_read(_file, To, length, MYF(0));
        if (read_length != (size_t) -1)
        {
          Count-= read_length;
          To+= read_length;
          pos_in_file+= read_length;

          if (read_length != length)
          {
            mysql_mutex_unlock(&_buffer_lock);
            return _read_append(To, Count);
          }
        }
        left_length+= length;
        diff_length= 0;
      }
      */
      max_length= _read_length;
      if (max_length > (_end_of_file - pos_in_file))
        max_length= (size_t) (_end_of_file - pos_in_file);

      if (max_length)
      {
        length= mysql_file_read(_file, _buffer, max_length,
                                MYF(0));
        if (length >= Count)
        {
          _read_pos=_buffer+Count;
          _read_end=_buffer+length;
          _pos_in_file=pos_in_file;
          mysql_mutex_unlock(&_buffer_lock);
          memcpy(To,_buffer,(size_t) Count);
          return 0;
        }
        memcpy(To, _buffer, length);
        Count-= length;
        To+= length;

        pos_in_file+= length;
      }
      else
      {
        if (Count)
          // goto read_append_buffer;
          length= 0;
      }
    }
  }



  mysql_mutex_unlock(&_buffer_lock);

  return _read_append(To, Count, pos_in_file);
}

RingBuffer::RingBuffer(char* filename, size_t cachesize)
{
  _total_size = 0;
  sem_init(&semaphore, 0, count_thread_for_slots);
  mysql_rwlock_init(0, &flush_rw_lock);
  _file = my_open(filename,O_CREAT | O_RDWR,MYF(MY_WME));
  if (_file >= 0)
  {
    my_off_t pos;
    pos= mysql_file_tell(_file, MYF(0));
    assert(pos != (my_off_t) -1);
  }


  // Calculate end of file to avoid allocating oversized buffers
  _end_of_file= mysql_file_seek(_file, 0L, MY_SEEK_END, MYF(0));
  // Need to reset seek_not_done now that we just did a seek.
  _seek_not_done= 0;


  // Retry allocating memory in smaller blocks until we get one
  for (;;)
  {
    size_t buffer_block;

    buffer_block= cachesize * 2;

    if ((_buffer= (uchar*) my_malloc(key_memory_IO_CACHE, buffer_block, (myf) MY_WME)) != 0)
    {
      _write_buffer= _buffer + cachesize;
      _alloced_buffer= buffer_block;
      break;					// Enough memory found
    }
    // Try with less memory
    cachesize= (cachesize*3/4);
  }
  _read_length = cachesize;

  _buffer_length = cachesize;
  _read_pos = _buffer;
  _append_read_pos = _write_pos = _write_buffer;
  _write_end = _write_buffer + _buffer_length;

  _read_end = _buffer;

  _write_new_pos = _write_pos;

  _error = 0;

  _pos_in_file = 0;
  mysql_mutex_init(key_IO_CACHE_append_buffer_lock, &_read_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_IO_CACHE_append_buffer_lock,
                   &_buffer_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_IO_CACHE_SHARE_cond_writer, &_cond_writer, 0);
  mysql_mutex_init(key_IO_CACHE_SHARE_mutex, &_mutex_writer, MY_MUTEX_INIT_FAST);
}
RingBuffer::~RingBuffer() {
  sem_destroy(&semaphore);
  mysql_rwlock_destroy(&flush_rw_lock);
  if (_file != -1) /* File doesn't exist */
  {
    _flush_io_buffer(-1);
  }
  my_free(_buffer);
  mysql_mutex_destroy(&_read_lock);
  mysql_mutex_destroy(&_buffer_lock);
  mysql_cond_destroy(&_cond_writer);
  mysql_mutex_destroy(&_mutex_writer);
  my_close(_file, MYF(MY_WME));
}

int RingBuffer::_flush_io_buffer(int not_released) {
  size_t length;


  if (_file == -1)
    return _error= -1;

  //mysql_mutex_lock(&_buffer_lock);

  if (_total_size)
  {
    if(_write_pos <= _append_read_pos) {
      length = _write_end - _append_read_pos;
      if (mysql_file_write(_file, _append_read_pos, length, MY_NABP))
        _error= -1;
      length = _write_pos - _write_buffer;
      if (mysql_file_write(_file, _write_buffer, length, MY_NABP))
        _error= -1;
    }
    else {
      length = _write_pos - _append_read_pos;
      if (mysql_file_write(_file, _append_read_pos, length, MY_NABP))
        _error= -1;
    }


    _end_of_file+= _total_size;
    _write_new_pos = _append_read_pos= _write_buffer;

    DBUG_ASSERT(_end_of_file == mysql_file_tell(_file, MYF(0)));

    _write_pos= _write_buffer;
    _total_size = 0;
    //++info->disk_writes;
    //mysql_mutex_unlock(&_buffer_lock);

    for (int i = 0; i < count_thread_for_slots; i++)
    {
      if(i == not_released)
        continue;
      if(!_slots[i].vacant)
        sem_post(&semaphore);
      _slots[i].finished= false;
      _slots[i].vacant= true;
      _slots[i].next= -1;
      _slots[i].pos_write_first= nullptr;
      _slots[i].pos_write_second= nullptr;
      _slots[i].pos_end= nullptr;
    }
    last_slot = -1;

    return _error;
  }
  //mysql_mutex_unlock(&_buffer_lock);

  return 0;
}
int RingBuffer::read_slot(uchar *To, size_t Count) {
  return 0;
}

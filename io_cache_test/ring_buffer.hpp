
#include <mysql/psi/mysql_file.h>
#include <mysys_priv.h>
#include <semaphore.h>
#include <atomic>
#include <array>

class RingBuffer {
public:
  RingBuffer(char* filename, size_t cachesize);
  int write_slot(uchar *From, size_t Count);
  int read_slot(uchar *To, size_t Count);
  ~RingBuffer();

private:

  struct cache_slot_t {
    volatile bool vacant= true;

    std::atomic<bool> finished {false};

    volatile int next = -1;

    uchar* pos_write_first = nullptr;

    /** For wrapping case in curricular write buffer */
    uchar* pos_write_second = nullptr;

    uchar* pos_end = nullptr;

    volatile size_t count_first = 0;

    volatile size_t count_second = 0;

    /**
      Each time the buffer wraps, its version increases. Then it's compared
      with slot version to avoid the race when the slot was cleared by other
      thread and then re-occupied by a new writer (i.e. vacant == false)
      in the same place (i.e. write_pos and lenth are the same)
    */
    volatile longlong wrap_version;
  };

  static const int count_thread_for_slots = 4;

  cache_slot_t _slots[count_thread_for_slots];

  /** Last used slot */
  volatile int last_slot= -1;

  volatile longlong version= 1;

  /** This semaphore prevents slots overflow */
  sem_t semaphore;

  mysql_rwlock_t flush_rw_lock;

  int _slot_acquire(uchar*& From, size_t& Count);

  bool _slot_release(int slot_id);

  /* Size of allocated space in slots */
  size_t _total_size;

  /* File descriptor */
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

    Is protected by flush_rw_lock
  */
  my_off_t _end_of_file;

  /* Points to current read position in the buffer */
  uchar *_read_pos;

  /* the non-inclusive boundary in the buffer for the currently valid read */
  uchar *_read_end;

  /* read buffer */
  uchar *_buffer;

  /* Length of read and write buffers */
  size_t _alloced_buffer;

  /* Length of buffer (write or read) */
  size_t _buffer_length;

  /* Point to place for new readers, while previous not done */
  uchar *_write_new_pos;

  /* Main buffer lock */
  mysql_mutex_t _buffer_lock;

  /* For single reader */
  mysql_mutex_t _read_lock;

  size_t _read_length;

  int _error;

  int _flush_io_buffer(int not_release);

  int _read_append_slot(uchar* To, size_t Count);

  int _fill_read_buffer_from_append();
};

RingBuffer::RingBuffer(char* filename, size_t cachesize) {
  _total_size = 0;
  sem_init(&semaphore, 0, count_thread_for_slots-1);
  mysql_rwlock_init(0, &flush_rw_lock);
  _file = my_open(filename,O_CREAT | O_RDWR,MYF(MY_WME));
  if (_file >= 0) {
    my_off_t pos;
    pos= mysql_file_tell(_file, MYF(0));
    assert(pos != (my_off_t) -1);
  }


  // Calculate end of file to avoid allocating oversized buffers
  _end_of_file= mysql_file_seek(_file, 0L, MY_SEEK_END, MYF(0));
  // Need to reset seek_not_done now that we just did a seek.


  // Retry allocating memory in smaller blocks until we get one
  for (;;) {
    size_t buffer_block;

    buffer_block= cachesize * 2;

    if ((_buffer= (uchar*) my_malloc(key_memory_IO_CACHE,
                                      buffer_block, (myf) MY_WME)) != 0) {
      _write_buffer= _buffer + cachesize;
      _alloced_buffer= buffer_block;
      break;	// Enough memory found
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
  mysql_mutex_init(key_IO_CACHE_append_buffer_lock,
                   &_read_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_IO_CACHE_append_buffer_lock,
                   &_buffer_lock, MY_MUTEX_INIT_FAST);
}

RingBuffer::~RingBuffer() {
  sem_destroy(&semaphore);
  mysql_rwlock_destroy(&flush_rw_lock);
  if (_file != -1) /* File doesn't exist */
    _flush_io_buffer(-1);

  my_free(_buffer);
  mysql_mutex_destroy(&_read_lock);
  mysql_mutex_destroy(&_buffer_lock);
  my_close(_file, MYF(MY_WME));
}

int RingBuffer::_slot_acquire(uchar *&From, size_t &Count) {
  sem_wait(&semaphore);
  int i;
  mysql_mutex_lock(&_buffer_lock);
  for (i= 0; i < count_thread_for_slots; ++i)
  {
    auto &vacant= _slots[i].vacant;
    if (vacant)
    {
      vacant= false;
      _slots[i].wrap_version= version;
      break;
    }
  }
  assert(i != count_thread_for_slots);

  if(Count > (_buffer_length - _total_size)) {
    /*
      Buffer is full, flush to disk.
      1. Wait for all writes finished by wlock(flush_rw_lock)
      2. Re-initialize slots and increase version
      3. Unlock buffer_lock to allow other writers exit (or else they'll wait
         in release() until flushing is done).
      4. Fill out the rest of buffer under exclusive lock and write to file.
    */

    mysql_rwlock_wrlock(&flush_rw_lock);

    uchar *save_write_new_pos= _write_new_pos;

    DBUG_ASSERT(_append_read_pos)
    _write_new_pos = _write_buffer;
    _write_pos= _write_buffer;
    _total_size = 0;

    for (int j = 0; j < count_thread_for_slots; j++) {
      if(j == i)
        continue;
      if(!_slots[j].vacant)
        sem_post(&semaphore);
      _slots[j].finished= false;
      _slots[j].vacant= true;
      _slots[j].next= -1;
      _slots[j].pos_write_first= nullptr;
      _slots[j].pos_write_second= nullptr;
      _slots[j].pos_end= nullptr;
    }
    last_slot = -1;

    if(save_write_new_pos < _append_read_pos) {
      size_t rest_length = _append_read_pos - save_write_new_pos;
      memcpy(save_write_new_pos, From, rest_length);
      _total_size += rest_length;
      Count -= rest_length;
      From += rest_length;
      _write_pos = save_write_new_pos + rest_length;
    }
    else {
      size_t rest_length_to_right_border = _write_end - save_write_new_pos;
      memcpy(save_write_new_pos, From, rest_length_to_right_border);
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
    }
    _flush_io_buffer(i);
    _append_read_pos= _write_buffer;
    mysql_rwlock_unlock(&flush_rw_lock);
  }

  mysql_rwlock_rdlock(&flush_rw_lock);


  if(last_slot != -1)
    _slots[last_slot].next = i;
  last_slot = i;

  _slots[i].pos_write_first= _write_new_pos;

  size_t rest_length_to_right_border;
  if(_write_new_pos < _append_read_pos)
    rest_length_to_right_border = _append_read_pos - _write_new_pos;
  else
    rest_length_to_right_border = _write_end - _write_new_pos;

  if(Count > rest_length_to_right_border) {
    version++;
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

bool RingBuffer::_slot_release(int slot_id) {

  auto version_here= _slots[slot_id].wrap_version;
  _slots[slot_id].finished = true;
  mysql_rwlock_unlock(&flush_rw_lock);
  DEBUG_SYNC(nullptr, "slot_release");
  mysql_mutex_lock(&_buffer_lock);
  if (last_slot != -1 && version_here == _slots[slot_id].wrap_version
      && _write_pos == _slots[slot_id].pos_write_first) {
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

int RingBuffer::write_slot(uchar* From, size_t Count) {
  int slot_id = _slot_acquire(From, Count);

  memcpy(_slots[slot_id].pos_write_first, From, _slots[slot_id].count_first);
  if(_slots[slot_id].pos_write_second)
    memcpy(_slots[slot_id].pos_write_second, From + _slots[slot_id].count_first,
           _slots[slot_id].count_second);

  _slot_release(slot_id);
  return 0;
}

int RingBuffer::_flush_io_buffer(int not_released) {
  size_t length;

  if (_file == -1)
    return _error= -1;

  if (_total_size) {
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
    DBUG_ASSERT(_end_of_file == mysql_file_tell(_file, MYF(0)));

    return _error;
  }

  return 0;
}
int RingBuffer::read_slot(uchar *To, size_t Count) {
  size_t left_length, length, read_file_length;
  int error;

  if (_read_pos + Count <= _read_end) {
    memcpy(To, _read_pos, Count);
    _read_pos+= Count;
    return 0;
  }

  if(_read_pos != _read_end) {
    left_length= (size_t) (_read_end - _read_pos);
    DBUG_ASSERT(Count > left_length);
    memcpy(To, _read_pos, left_length);
    To+=left_length;
    Count-=left_length;
  }

  mysql_mutex_lock(&_read_lock);
  mysql_rwlock_rdlock(&flush_rw_lock);
  length= _end_of_file - _pos_in_file;
  if (!length) {
    mysql_rwlock_unlock(&flush_rw_lock);
    error = _read_append_slot(To, Count);

    mysql_mutex_unlock(&_read_lock);
    return error;
  }
  if(mysql_file_seek(_file, _pos_in_file, MY_SEEK_SET, MYF(0)) ==
        MY_FILEPOS_ERROR){
    mysql_rwlock_unlock(&flush_rw_lock);
    mysql_mutex_unlock(&_read_lock);
    return -1; // error while seeking
  }

  read_file_length= length < _read_length? length : _read_length;

  length = mysql_file_read(_file, _buffer, read_file_length, MYF(0));

  if(length >= Count) {
    mysql_rwlock_unlock(&flush_rw_lock);
    _read_pos=_buffer+Count;
    _read_end=_buffer+length;
    memcpy(To, _buffer, Count);
    _pos_in_file += length;
    mysql_mutex_unlock(&_read_lock);
    return 0;
  }

  memcpy(To, _buffer, Count);
  Count -= length;
  To += length;
  _pos_in_file += length;
  mysql_rwlock_unlock(&flush_rw_lock);
  _read_append_slot(To, Count);

  mysql_mutex_unlock(&_read_lock);
  return 0;
}
int RingBuffer::_read_append_slot(uchar *To, size_t Count) {
  size_t length;
  mysql_mutex_lock(&_buffer_lock);
  if(Count > _total_size) {
    mysql_mutex_unlock(&_buffer_lock);
    return -1; // error: buffer size less than needed
  }
  if(_write_pos > _append_read_pos || (Count <= (size_t) (_write_end - _append_read_pos))) {
    memcpy(To, _append_read_pos, Count);
    _append_read_pos += Count;
    assert(_total_size >= Count);
    _total_size -= Count;
    _fill_read_buffer_from_append();
  }
  else {
    length = _write_end - _append_read_pos;
    memcpy(To, _append_read_pos, length);
    To += length;
    assert(_total_size >= length);
    _total_size -= length;

    length = _write_pos - _write_buffer;
    memcpy(To, _write_buffer, length);
    assert(_total_size >= length);
    _total_size -= length;
    _append_read_pos = _write_buffer + length;
    _fill_read_buffer_from_append();
  }
  mysql_mutex_unlock(&_buffer_lock);
  return 0;
}
int RingBuffer::_fill_read_buffer_from_append() {
  size_t transfer_len, length;
  if(!_total_size || _write_pos == _append_read_pos)
    return -1; // error?

  if(_write_pos > _append_read_pos) {
    /* _total_size is updated before memcpy() completed in write method, so we can't use this value, need calculate actual */
    memcpy(_buffer, _append_read_pos,
           transfer_len = (size_t) (_write_pos - _append_read_pos));
  }
  else {
    length = _write_end - _append_read_pos;
    memcpy(_buffer, _append_read_pos, length);
    transfer_len = length;

    length = _write_pos - _write_buffer;
    memcpy(_buffer + transfer_len, _write_buffer, length);
    transfer_len += length;
  }

  _read_pos = _buffer;
  _read_end = _buffer + transfer_len;
  _append_read_pos = _write_pos;
  assert(_total_size >= transfer_len);
  _total_size -= transfer_len;

  return 0;
}


#include <mysql/psi/mysql_file.h>
#include <mysys_priv.h>

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
  ~RingBuffer();

private:


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

  size_t _read_length;

  int _error;

  int _flush_io_buffer();

  int _read_append(uchar* To, size_t Count);
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


  if(_flush_io_buffer())
    return ERR_FLUSH;

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
    saved_buffer = From;
    _end_of_file+=Count;
  }

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

int RingBuffer::_read_append(uchar* To, size_t Count){
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
  _end_of_file+=len_in_buff;
  mysql_mutex_unlock(&_buffer_lock);
  return 0;
}


int RingBuffer::read(uchar *To, size_t Count) {
  size_t left_length = 0, diff_length, length, max_length;
  my_off_t pos_in_file;
  int error;

  if (_read_pos + Count <= _read_end)
  {
    memcpy(To, _read_pos, Count);
    _read_pos+= Count;
    return 0;
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

      diff_length= (size_t) (pos_in_file & (IO_SIZE - 1));
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
      max_length= _read_length - diff_length;
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
  if(_read_pos != _read_end)
  {
    left_length= (size_t) (_read_end - _read_pos);
    DBUG_ASSERT(Count > left_length);
    memcpy(To, _read_pos, left_length);
    To+=left_length;
    Count-=left_length;
  }
  return _read_append(To, Count);
}

RingBuffer::RingBuffer(char* filename, size_t cachesize)
{
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
  mysql_mutex_init(key_IO_CACHE_append_buffer_lock,
                   &_buffer_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_IO_CACHE_SHARE_cond_writer, &_cond_writer, 0);
  mysql_mutex_init(key_IO_CACHE_SHARE_mutex, &_mutex_writer, MY_MUTEX_INIT_FAST);
}
RingBuffer::~RingBuffer() {
  if (_file != -1) /* File doesn't exist */
  {
    _flush_io_buffer();
  }
  my_free(_buffer);
  mysql_mutex_destroy(&_buffer_lock);
  mysql_cond_destroy(&_cond_writer);
  mysql_mutex_destroy(&_mutex_writer);
  my_close(_file, MYF(MY_WME));
}

int RingBuffer::_flush_io_buffer() {
  size_t length;


  if (_file == -1)
    return _error= -1;

  mysql_mutex_lock(&_buffer_lock);

  if ((length=(size_t) (_write_pos - _write_buffer)))
  {

    if (mysql_file_write(_file, _write_buffer, length, MY_NABP))
      _error= -1;

    _end_of_file+= _write_pos - _append_read_pos;
    _write_new_pos = _append_read_pos= _write_buffer;

    DBUG_ASSERT(_end_of_file == mysql_file_tell(_file, MYF(0)));

    _write_end= (_write_buffer +_buffer_length);
    _write_pos= _write_buffer;
    //++info->disk_writes;
    mysql_mutex_unlock(&_buffer_lock);
    return _error;
  }

  mysql_mutex_unlock(&_buffer_lock);
  return 0;
}


#include <mysql/psi/mysql_file.h>
class RingBuffer
{
public:
  explicit RingBuffer(File file);
  int read(uchar *To, size_t Count);
  int write(uchar *From, size_t Count);
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

  uchar *_write_new_pos;

  mysql_mutex_t _buffer_lock;

  /* For a synchronized writer. */
  mysql_cond_t _cond_writer;

  /* To sync on writers into buffer. */
  mysql_mutex_t _mutex_writer;
};

int RingBuffer::write(uchar *From, size_t Count) { return 0; }
int RingBuffer::read(uchar *To, size_t Count) { return 0; }
RingBuffer::RingBuffer(File file) : _file(file)
{

}
RingBuffer::~RingBuffer() {}

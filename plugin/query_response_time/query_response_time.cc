#include "mysql_version.h"
#include "my_global.h"
#ifdef HAVE_RESPONSE_TIME_DISTRIBUTION
#include "mysql_com.h"
#include "rpl_tblmap.h"
#include "table.h"
#include "field.h"
#include "sql_show.h"
#include "query_response_time.h"

#define TIME_STRING_POSITIVE_POWER_LENGTH QRT_TIME_STRING_POSITIVE_POWER_LENGTH
#define TIME_STRING_NEGATIVE_POWER_LENGTH 6
#define TOTAL_STRING_POSITIVE_POWER_LENGTH QRT_TOTAL_STRING_POSITIVE_POWER_LENGTH
#define TOTAL_STRING_NEGATIVE_POWER_LENGTH 6
#define MINIMUM_BASE 2
#define MAXIMUM_BASE QRT_MAXIMUM_BASE
#define POSITIVE_POWER_FILLER QRT_POSITIVE_POWER_FILLER
#define NEGATIVE_POWER_FILLER QRT_NEGATIVE_POWER_FILLER
#define TIME_OVERFLOW   QRT_TIME_OVERFLOW
#define DEFAULT_BASE    QRT_DEFAULT_BASE

#define do_xstr(s) do_str(s)
#define do_str(s) #s
#define do_format(filler,width) "%" filler width "lld"
/*
  Format strings for snprintf. Generate from:
  POSITIVE_POWER_FILLER and TIME_STRING_POSITIVE_POWER_LENGTH
  NEFATIVE_POWER_FILLER and TIME_STRING_NEGATIVE_POWER_LENGTH
*/
#define TIME_STRING_POSITIVE_POWER_FORMAT do_format(POSITIVE_POWER_FILLER,do_xstr(TIME_STRING_POSITIVE_POWER_LENGTH))
#define TIME_STRING_NEGATIVE_POWER_FORMAT do_format(NEGATIVE_POWER_FILLER,do_xstr(TIME_STRING_NEGATIVE_POWER_LENGTH))
#define TIME_STRING_FORMAT		      TIME_STRING_POSITIVE_POWER_FORMAT "." TIME_STRING_NEGATIVE_POWER_FORMAT

#define TOTAL_STRING_POSITIVE_POWER_FORMAT do_format(POSITIVE_POWER_FILLER,do_xstr(TOTAL_STRING_POSITIVE_POWER_LENGTH))
#define TOTAL_STRING_NEGATIVE_POWER_FORMAT do_format(NEGATIVE_POWER_FILLER,do_xstr(TOTAL_STRING_NEGATIVE_POWER_LENGTH))
#define TOTAL_STRING_FORMAT		      TOTAL_STRING_POSITIVE_POWER_FORMAT "." TOTAL_STRING_NEGATIVE_POWER_FORMAT

#define TIME_STRING_LENGTH	QRT_TIME_STRING_LENGTH
#define TIME_STRING_BUFFER_LENGTH	(TIME_STRING_LENGTH + 1 /* '\0' */)

#define TOTAL_STRING_LENGTH	QRT_TOTAL_STRING_LENGTH
#define TOTAL_STRING_BUFFER_LENGTH	(TOTAL_STRING_LENGTH + 1 /* '\0' */)

/*
  Calculate length of "log linear"
  1)
  (MINIMUM_BASE ^ result) <= (10 ^ STRING_POWER_LENGTH) < (MINIMUM_BASE ^ (result + 1))

  2)
  (MINIMUM_BASE ^ result) <= (10 ^ STRING_POWER_LENGTH)
  and
  (MINIMUM_BASE ^ (result + 1)) > (10 ^ STRING_POWER_LENGTH)

  3)
  result     <= LOG(MINIMUM_BASE, 10 ^ STRING_POWER_LENGTH)= STRING_POWER_LENGTH * LOG(MINIMUM_BASE,10)
  result + 1 >  LOG(MINIMUM_BASE, 10 ^ STRING_POWER_LENGTH)= STRING_POWER_LENGTH * LOG(MINIMUM_BASE,10)

  4) STRING_POWER_LENGTH * LOG(MINIMUM_BASE,10) - 1 < result <= STRING_POWER_LENGTH * LOG(MINIMUM_BASE,10)

  MINIMUM_BASE= 2 always, LOG(MINIMUM_BASE,10)= 3.3219280948873626, result= (int)3.3219280948873626 * STRING_POWER_LENGTH

  Last counter always use for time overflow
*/
#define POSITIVE_POWER_COUNT ((int)(3.32192809 * TIME_STRING_POSITIVE_POWER_LENGTH))
#define NEGATIVE_POWER_COUNT ((int)(3.32192809 * TIME_STRING_NEGATIVE_POWER_LENGTH))
#define OVERALL_POWER_COUNT (NEGATIVE_POWER_COUNT + 1 + POSITIVE_POWER_COUNT)

#define MILLION ((unsigned long)1000 * 1000)

namespace query_response_time
{

class utility
{
public:
  utility() : m_base(0)
  {
    m_max_dec_value= MILLION;
    for(int i= 0; TIME_STRING_POSITIVE_POWER_LENGTH > i; ++i)
      m_max_dec_value *= 10;
    setup(DEFAULT_BASE);
  }
public:
  uint      base()            const { return m_base; }
  uint      negative_count()  const { return m_negative_count; }
  uint      positive_count()  const { return m_positive_count; }
  uint      bound_count()     const { return m_bound_count; }
  ulonglong max_dec_value()   const { return m_max_dec_value; }
  ulonglong bound(uint index) const { return m_bound[ index ]; }
public:
  void setup(uint base)
  {
    if(base != m_base)
    {
      m_base= base;

      const ulonglong million= 1000 * 1000;
      ulonglong value= million;
      m_negative_count= 0;
      while(value > 0)
      {
	m_negative_count += 1;
	value /= m_base;
      }
      m_negative_count -= 1;

      value= million;
      m_positive_count= 0;
      while(value < m_max_dec_value)
      {
	m_positive_count += 1;
	value *= m_base;
      }
      m_bound_count= m_negative_count + m_positive_count;

      value= million;
      for(uint i= 0; i < m_negative_count; ++i)
      {
	value /= m_base;
	m_bound[m_negative_count - i - 1]= value;
      }
      value= million;
      for(uint i= 0; i < m_positive_count;  ++i)
      {
	m_bound[m_negative_count + i]= value;
	value *= m_base;
      }
    }
  }
private:
  uint      m_base;
  uint      m_negative_count;
  uint      m_positive_count;
  uint      m_bound_count;
  ulonglong m_max_dec_value; /* for TIME_STRING_POSITIVE_POWER_LENGTH=7 is 10000000 */
  ulonglong m_bound[OVERALL_POWER_COUNT];
};

static
void print_time(char* buffer, std::size_t buffer_size, const char* format,
                uint64 value)
{
  ulonglong second=      (value / MILLION);
  ulonglong microsecond= (value % MILLION);
  my_snprintf(buffer, buffer_size, format, second, microsecond);
}

class time_collector
{
  utility *m_utility;
  Atomic_counter<uint32_t> m_count[OVERALL_POWER_COUNT + 1];
  Atomic_counter<uint64_t> m_total[OVERALL_POWER_COUNT + 1];

public:
  time_collector(utility& u): m_utility(&u) { flush(); }
  ~time_collector() { }
  uint32_t count(uint index) { return m_count[index]; }
  uint64_t total(uint index) { return m_total[index]; }
  void flush()
  {
    for (auto i= 0; i < OVERALL_POWER_COUNT + 1; i++)
    {
      m_count[i]= 0;
      m_total[i]= 0;
    }
  }
  void collect(uint64_t time)
  {
    int i= 0;
    for(int count= m_utility->bound_count(); count > i; ++i)
    {
      if(m_utility->bound(i) > time)
      {
        m_count[i]++;
        m_total[i]+= time;
        break;
      }
    }
  }
};

class collector
{
public:
  collector() : m_time(m_utility)
  {
    m_utility.setup(DEFAULT_BASE);
  }
public:
  void flush()
  {
    m_utility.setup(opt_query_response_time_range_base);
    m_time.flush();
  }
  int fill(THD* thd, TABLE_LIST *tables, COND *cond)
  {
    DBUG_ENTER("fill_schema_query_response_time");
    TABLE        *table= static_cast<TABLE*>(tables->table);
    Field        **fields= table->field;
    for(uint i= 0, count= bound_count() + 1 /* with overflow */; count > i; ++i)
    {
      char time[TIME_STRING_BUFFER_LENGTH];
      char total[TOTAL_STRING_BUFFER_LENGTH];
      if(i == bound_count())
      {
        assert(sizeof(TIME_OVERFLOW) <= TIME_STRING_BUFFER_LENGTH);
        assert(sizeof(TIME_OVERFLOW) <= TOTAL_STRING_BUFFER_LENGTH);
        memcpy(time,TIME_OVERFLOW,sizeof(TIME_OVERFLOW));
        memcpy(total,TIME_OVERFLOW,sizeof(TIME_OVERFLOW));
      }
      else
      {
        print_time(time, sizeof(time), TIME_STRING_FORMAT, this->bound(i));
        print_time(total, sizeof(total), TOTAL_STRING_FORMAT, this->total(i));
      }
      fields[0]->store(time,strlen(time),system_charset_info);
      fields[1]->store((longlong)this->count(i),true);
      fields[2]->store(total,strlen(total),system_charset_info);
      if (schema_table_store_record(thd, table))
      {
	DBUG_RETURN(1);
      }
    }
    DBUG_RETURN(0);
  }
  void collect(ulonglong time)
  {
    m_time.collect(time);
  }
  uint bound_count() const
  {
    return m_utility.bound_count();
  }
  ulonglong bound(uint index)
  {
    return m_utility.bound(index);
  }
  ulonglong count(uint index)
  {
    return m_time.count(index);
  }
  ulonglong total(uint index)
  {
    return m_time.total(index);
  }
private:
  utility          m_utility;
  time_collector   m_time;
};

static collector g_collector;

} // namespace query_response_time

void query_response_time_init()
{
}

void query_response_time_free()
{
  query_response_time::g_collector.flush();
}

int query_response_time_flush()
{
  query_response_time::g_collector.flush();
  return 0;
}
void query_response_time_collect(ulonglong query_time)
{
  query_response_time::g_collector.collect(query_time);
}

int query_response_time_fill(THD* thd, TABLE_LIST *tables, COND *cond)
{
  return query_response_time::g_collector.fill(thd,tables,cond);
}
#endif // HAVE_RESPONSE_TIME_DISTRIBUTION

if (!`SELECT count(*) FROM performance_schema.performance_timers WHERE timer_name='CYCLE' AND timer_frequency IS NOT NULL`)
{
  Skip Need performance timer CYCLE;
}

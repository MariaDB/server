if (!`SELECT COUNT(*) FROM information_schema.ENGINES
      WHERE (SUPPORT='YES' OR SUPPORT='DEFAULT') AND ENGINE='VIDEX'`)
{
  skip Need VIDEX engine;
}

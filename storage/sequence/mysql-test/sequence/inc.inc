if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'sequence' AND support='YES'`)
{
  --skip Test requires sequence engine
}

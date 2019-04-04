let $HAVE_TRIGGER= `SELECT COUNT(*) FROM information_schema.tables
  WHERE TABLE_SCHEMA = 'information_schema' AND TABLE_NAME = 'TRIGGERS'`;

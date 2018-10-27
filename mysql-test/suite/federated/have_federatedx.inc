if (!`SELECT count(*) FROM information_schema.plugins WHERE
      plugin_name = 'federated' AND plugin_status = 'active' AND
      plugin_description LIKE '%transactions%'`){
  skip Need FederatedX engine;
}

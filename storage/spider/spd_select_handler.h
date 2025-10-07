class spider_select_handler: public select_handler
{
  spider_fields *fields;
  int store_error;
public:
  spider_select_handler(THD *, SELECT_LEX *, spider_fields *);
  ~spider_select_handler();
  int init_scan() override;
  int next_row() override;
  int end_scan() override;
};

select_handler *spider_create_select_handler(THD *, SELECT_LEX *, SELECT_LEX_UNIT *);

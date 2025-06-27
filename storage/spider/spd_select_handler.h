class spider_select_handler: public select_handler
{
  bool first;
public:
  spider_select_handler(THD *, SELECT_LEX *);
  int init_scan() override;
  int next_row() override;
  int end_scan() override;
};

select_handler *spider_create_select_handler(THD *, SELECT_LEX *, SELECT_LEX_UNIT *);

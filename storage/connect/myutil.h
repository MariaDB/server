/***********************************************************************/
/*  Prototypes of Functions used externally.                           */
/***********************************************************************/
enum enum_field_types PLGtoMYSQL(int type, bool dbf);
const char *PLGtoMYSQLtype(int type, bool dbf);
int   MYSQLtoPLG(char *typname);
int   MYSQLtoPLG(int mytype);
char *MyDateFmt(int mytype);
char *MyDateFmt(char *typname);

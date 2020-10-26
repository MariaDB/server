/***********************************************************************/
/*  Prototypes of Functions used externally.                           */
/***********************************************************************/
#ifndef __MYUTIL__H
#define  __MYUTIL__H

enum enum_field_types PLGtoMYSQL(int type, bool dbf, char var = 0);
const char *PLGtoMYSQLtype(int type, bool dbf, char var = 0);
int  MYSQLtoPLG(char *typname, char *var);
int  MYSQLtoPLG(int mytype, char *var);
PCSZ MyDateFmt(int mytype);
PCSZ MyDateFmt(char *typname);

#endif // __MYUTIL__H

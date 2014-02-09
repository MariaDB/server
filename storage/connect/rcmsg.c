/**************** RCMsg C Program Source Code File (.C) ****************/
/* PROGRAM NAME: RCMSG                                                 */
/* -------------                                                       */
/*  Version 1.1                                                        */
/*                                                                     */
/* COPYRIGHT                                                           */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND:  2005 - 2013         */
/*                                                                     */
/* WHAT THIS PROGRAM DOES                                              */
/* -----------------------                                             */
/*  This program simulates LoadString for Unix and Linux.              */
/*                                                                     */
/***********************************************************************/
#include <stdio.h>
#include "resource.h"
#include "rcmsg.h"

char *GetMsgid(int id)
  {
  char *p = NULL;

  switch (id) {
    case IDS_00:     p = "%s";                break;
#if defined(FRENCH)
    case IDS_01:     p = "%s: erreur d'allocation du buffer de communication de %d octets"; break;
    case IDS_02:     p = "%s: erreur d'allocation mémoire tampon pour %d colonnes";         break;
    case IDS_03:     p = "%s: Commande spéciale invalide";                                   break;
    case IDS_04:     p = "%s: Wrong number of arguments %d";                                break;
    case IDS_05:     p = "%s";                                                               break;
    case IDS_06:     p = "%s: Commande dépassant la taille du buffer interne (%d octets)";  break;
    case IDS_07:     p = "%s: Données (%d octets) tronquées à la taille du buffer";         break;
    case IDS_08:     p = "%s: Résultat dépassant la taille du buffer interne (%d octets)";  break;
    case IDS_09:     p = "Erreur dans %s: %s";                                               break;
    case IDS_10:     p = "%s: erreur d'allocating mémoire de %d octets";                    break;
    case IDS_11:     p = "%s: mauvaise clé de connexion %d";                                break;
    case IDS_12:     p = "%s: Pas plus de %d connexions autorisées pour un programme";      break;
    case IDS_13:     p = "%s: clé de connexion invalide %d";                                break;
    case IDS_14:     p = "SafeDB: %s rc=%d";                                                break;
    case IDS_15:     p = "Mauvaise Dll de communication appelée par le moteur %s";           break;
    case IDS_TAB_01: p = "Catalogue";         break;
    case IDS_TAB_02: p = "Schéma";            break;
    case IDS_TAB_03: p = "Nom";               break;
    case IDS_TAB_04: p = "Type";              break;
    case IDS_TAB_05: p = "Remarque";          break;
    case IDS_COL_01: p = "Cat_Table";         break;
    case IDS_COL_02: p = "Schem_Table";       break;
    case IDS_COL_03: p = "Nom_Table";         break;
    case IDS_COL_04: p = "Nom_Colonne";       break;
    case IDS_COL_05: p = "Type_Données";      break;
    case IDS_COL_06: p = "Nom_Type";          break;
    case IDS_COL_07: p = "Précision";         break;
    case IDS_COL_08: p = "Longueur";          break;
    case IDS_COL_09: p = "Echelle";           break;
    case IDS_COL_10: p = "Base";              break;
    case IDS_COL_11: p = "Nullifiable";       break;
    case IDS_COL_12: p = "Remarques";         break;
    case IDS_INF_01: p = "Nom_Type";          break;
    case IDS_INF_02: p = "Type_Données";      break;
    case IDS_INF_03: p = "Précision";         break;
    case IDS_INF_04: p = "Préfixe_Litéral";   break;
    case IDS_INF_05: p = "Suffixe_Litéral";   break;
    case IDS_INF_06: p = "Création_Params";   break;
    case IDS_INF_07: p = "Nullifiable";       break;
    case IDS_INF_08: p = "Maj_Minuscule";     break;
    case IDS_INF_09: p = "Localisable";       break;
    case IDS_INF_10: p = "Valeur_Absolue";    break;
    case IDS_INF_11: p = "Monnaie";           break;
    case IDS_INF_12: p = "Auto_Incrément";    break;
    case IDS_INF_13: p = "Nom_Type_Local";    break;
    case IDS_INF_14: p = "Echelle_Minimum";   break;
    case IDS_INF_15: p = "Echelle_Maximum";   break;
    case IDS_PKY_01: p = "Cat_Table";         break;
    case IDS_PKY_02: p = "Schem_Table";       break;
    case IDS_PKY_03: p = "Nom_Table";         break;
    case IDS_PKY_04: p = "Nom_Colonne";       break;
    case IDS_PKY_05: p = "Numéro_Clé";        break;
    case IDS_PKY_06: p = "Nom_Clé";           break;
    case IDS_FKY_01: p = "PKTable_Catalog";   break;
    case IDS_FKY_02: p = "PKTable_Schema";    break;
    case IDS_FKY_03: p = "PKTable_Name";      break;
    case IDS_FKY_04: p = "PKColumn_Name";     break;
    case IDS_FKY_05: p = "FKTable_Catalog";   break;
    case IDS_FKY_06: p = "FKTable_Schema";    break;
    case IDS_FKY_07: p = "FKTable_Name";      break;
    case IDS_FKY_08: p = "FKColumn_Name";     break;
    case IDS_FKY_09: p = "Key_Seq";           break;
    case IDS_FKY_10: p = "Update_Rule";       break;
    case IDS_FKY_11: p = "Delete_Rule";       break;
    case IDS_FKY_12: p = "FK_Name";           break;
    case IDS_FKY_13: p = "PK_Name";           break;
    case IDS_STA_01: p = "Table_Catalog";     break;
    case IDS_STA_02: p = "Table_Schema";      break;
    case IDS_STA_03: p = "Table_Name";        break;
    case IDS_STA_04: p = "Non_Unique";        break;
    case IDS_STA_05: p = "Index_Qualifier";   break;
    case IDS_STA_06: p = "Index_Name";        break;
    case IDS_STA_07: p = "Type";              break;
    case IDS_STA_08: p = "Seq_in_Index";      break;
    case IDS_STA_09: p = "Column_Name";       break;
    case IDS_STA_10: p = "Collation";         break;
    case IDS_STA_11: p = "Cardinality";       break;
    case IDS_STA_12: p = "Pages";             break;
    case IDS_STA_13: p = "Filter_Condition";  break;
    case IDS_SPC_01: p = "Champ";             break;
    case IDS_SPC_02: p = "Nom_Colonne";       break;
    case IDS_SPC_03: p = "Type_Données";      break;
    case IDS_SPC_04: p = "Nom_Type";          break;
    case IDS_SPC_05: p = "Précision";         break;
    case IDS_SPC_06: p = "Longueur";          break;
    case IDS_SPC_07: p = "Echelle";           break;
    case IDS_SPC_08: p = "Pseudo_Colonne";    break;
    case IDS_DRV_01: p = "Description";       break;
    case IDS_DRV_02: p = "Attributs";         break;
    case IDS_DSC_01: p = "Nom";               break;
    case IDS_DSC_02: p = "Description";       break;
#else    // English
    case IDS_01:     p = "%s: error allocating communication buffer of %d bytes";        break;
    case IDS_02:     p = "%s: error allocating parser memory for %d columns";            break;
    case IDS_03:     p = "%s: Invalid special command";                                   break;
    case IDS_04:     p = "%s: Wrong number of arguments %d";                             break;
    case IDS_05:     p = "%s";                                                            break;
    case IDS_06:     p = "%s: Command bigger than internal buffer of size = %d";         break;
    case IDS_07:     p = "%s: Data truncated to buffer size, actual length is %d bytes"; break;
    case IDS_08:     p = "%s: Result bigger than internal buffer of size = %d";          break;
    case IDS_09:     p = "Error in %s: %s";                                               break;
    case IDS_10:     p = "%s: error allocating instance memory of %d bytes";             break;
    case IDS_11:     p = "%s: wrong connection key value %d";                            break;
    case IDS_12:     p = "%s: No more than %d connections allowed from one process";     break;
    case IDS_13:     p = "%s: invalid connection key value %d";                          break;
    case IDS_14:     p = "SafeDB: %s rc=%d";                                             break;
    case IDS_15:     p = "Wrong communication Dll called for engine %s";                  break;
    case IDS_TAB_01: p = "Table_Cat";          break;
    case IDS_TAB_02: p = "Table_Schema";       break;
    case IDS_TAB_03: p = "Table_Name";         break;
    case IDS_TAB_04: p = "Table_Type";         break;
    case IDS_TAB_05: p = "Remark";             break;
    case IDS_COL_01: p = "Table_Cat";          break;
    case IDS_COL_02: p = "Table_Schema";       break;
    case IDS_COL_03: p = "Table_Name";         break;
    case IDS_COL_04: p = "Column_Name";        break;
    case IDS_COL_05: p = "Data_Type";          break;
    case IDS_COL_06: p = "Type_Name";          break;
    case IDS_COL_07: p = "Column_Size";        break;
    case IDS_COL_08: p = "Buffer_Length";      break;
    case IDS_COL_09: p = "Decimal_Digits";     break;
    case IDS_COL_10: p = "Radix";              break;
    case IDS_COL_11: p = "Nullable";           break;
    case IDS_COL_12: p = "Remarks";            break;
    case IDS_INF_01: p = "Type_Name";          break;
    case IDS_INF_02: p = "Data_Type";          break;
    case IDS_INF_03: p = "Precision";          break;
    case IDS_INF_04: p = "Literal_Prefix";     break;
    case IDS_INF_05: p = "Literal_Suffix";     break;
    case IDS_INF_06: p = "Create_Params";      break;
    case IDS_INF_07: p = "Nullable";           break;
    case IDS_INF_08: p = "Case_Sensitive";     break;
    case IDS_INF_09: p = "Searchable";         break;
    case IDS_INF_10: p = "Unsigned_Attribute"; break;
    case IDS_INF_11: p = "Money";              break;
    case IDS_INF_12: p = "Auto_Increment";     break;
    case IDS_INF_13: p = "Local_Type_Name";    break;
    case IDS_INF_14: p = "Minimum_Scale";      break;
    case IDS_INF_15: p = "Maximum_Scale";      break;
    case IDS_PKY_01: p = "Table_Catalog";      break;
    case IDS_PKY_02: p = "Table_Schema";       break;
    case IDS_PKY_03: p = "Table_Name";         break;
    case IDS_PKY_04: p = "Column_Name";        break;
    case IDS_PKY_05: p = "Key_Seq";            break;
    case IDS_PKY_06: p = "Pk_Name";            break;
    case IDS_FKY_01: p = "PKTable_Catalog";    break;
    case IDS_FKY_02: p = "PKTable_Schema";     break;
    case IDS_FKY_03: p = "PKTable_Name";       break;
    case IDS_FKY_04: p = "PKColumn_Name";      break;
    case IDS_FKY_05: p = "FKTable_Catalog";    break;
    case IDS_FKY_06: p = "FKTable_Schema";     break;
    case IDS_FKY_07: p = "FKTable_Name";       break;
    case IDS_FKY_08: p = "FKColumn_Name";      break;
    case IDS_FKY_09: p = "Key_Seq";            break;
    case IDS_FKY_10: p = "Update_Rule";        break;
    case IDS_FKY_11: p = "Delete_Rule";        break;
    case IDS_FKY_12: p = "FK_Name";            break;
    case IDS_FKY_13: p = "PK_Name";            break;
    case IDS_STA_01: p = "Table_Catalog";      break;
    case IDS_STA_02: p = "Table_Schema";       break;
    case IDS_STA_03: p = "Table_Name";         break;
    case IDS_STA_04: p = "Non_Unique";         break;
    case IDS_STA_05: p = "Index_Qualifier";    break;
    case IDS_STA_06: p = "Index_Name";         break;
    case IDS_STA_07: p = "Type";               break;
    case IDS_STA_08: p = "Seq_in_Index";       break;
    case IDS_STA_09: p = "Column_Name";        break;
    case IDS_STA_10: p = "Collation";          break;
    case IDS_STA_11: p = "Cardinality";        break;
    case IDS_STA_12: p = "Pages";              break;
    case IDS_STA_13: p = "Filter_Condition";   break;
    case IDS_SPC_01: p = "Scope";              break;
    case IDS_SPC_02: p = "Column_Name";        break;
    case IDS_SPC_03: p = "Data_Type";          break;
    case IDS_SPC_04: p = "Type_Name";          break;
    case IDS_SPC_05: p = "Precision";          break;
    case IDS_SPC_06: p = "Length";             break;
    case IDS_SPC_07: p = "Scale";              break;
    case IDS_SPC_08: p = "Pseudo_Column";      break;
    case IDS_DRV_01: p = "Description";        break;
    case IDS_DRV_02: p = "Attributes";         break;
    case IDS_DSC_01: p = "Name";               break;
    case IDS_DSC_02: p = "Description";        break;
#endif   // English
    } // endswitch(id)

  return p;
  } // end of GetMsgid

int GetRcString(int id, char *buf, int bufsize)
  {
  char *p = NULL, msg[32];

  if (!(p = GetMsgid(id))) {
    sprintf(msg, "ID=%d unknown", id);
    p = msg;
    } // endif p

  return sprintf(buf, "%.*s", bufsize-1, p);
  } // end of GetRcString

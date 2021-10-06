/****************** bsonudf C++ Program Source Code File (.CPP) ******************/
/*  PROGRAM NAME: bsonudf     Version 1.0                                        */
/*  (C) Copyright to the author Olivier BERTRAND          2020 - 2021            */
/*  This program are the BSON User Defined Functions.                            */
/*********************************************************************************/

/*********************************************************************************/
/*  Include relevant sections of the MariaDB header file.                        */
/*********************************************************************************/
#include <my_global.h>
#include <mysqld.h>
#include <mysql.h>
#include <sql_error.h>
#include <stdio.h>
#include <cassert>

#include "bsonudf.h"

#if defined(UNIX) || defined(UNIV_LINUX)
#define _O_RDONLY O_RDONLY
#endif

#define MEMFIX  4096
#if defined(connect_EXPORTS)
#define PUSH_WARNING(M) push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, M)
#else
#define PUSH_WARNING(M) htrc(M)
#endif
#define M 6

int  JsonDefPrec = -1;
int  GetDefaultPrec(void);
int  IsArgJson(UDF_ARGS* args, uint i);
void SetChanged(PBSON bsp);
int  GetJsonDefPrec(void);

static PBSON BbinAlloc(PGLOBAL g, ulong len, PBVAL jsp);

/* --------------------------------- JSON UDF ---------------------------------- */

/*********************************************************************************/
/*  Replaces GetJsonGrpSize not usable when CONNECT is not installed.            */
/*********************************************************************************/
int  GetJsonDefPrec(void)	{
	return (JsonDefPrec < 0) ? GetDefaultPrec() : JsonDefPrec;
}	/* end of GetJsonDefPrec */

/*********************************************************************************/
/*  Program for saving the status of the memory pools.                           */
/*********************************************************************************/
inline void JsonMemSave(PGLOBAL g) {
	g->Saved_Size = ((PPOOLHEADER)g->Sarea)->To_Free;							 
} /* end of JsonMemSave */

/*********************************************************************************/
/*  Program for freeing the memory pools.                                        */
/*********************************************************************************/
inline void JsonFreeMem(PGLOBAL g) {
	g->Activityp = NULL;
	g = PlugExit(g);
} /* end of JsonFreeMem */

/*********************************************************************************/
/*  Allocate and initialize a BSON structure.                                    */
/*********************************************************************************/
static PBSON BbinAlloc(PGLOBAL g, ulong len, PBVAL jsp)
{
	PBSON bsp = (PBSON)PlgDBSubAlloc(g, NULL, sizeof(BSON));

	if (bsp) {
		strcpy(bsp->Msg, "Binary Json");
		bsp->Msg[BMX] = 0;
		bsp->Filename = NULL;
		bsp->G = g;
		bsp->Pretty = 2;
		bsp->Reslen = len;
		bsp->Changed = false;
		bsp->Top = bsp->Jsp = (PJSON)jsp;
		bsp->Bsp = NULL;
	} else
		PUSH_WARNING(g->Message);

	return bsp;
} /* end of BbinAlloc */

/* --------------------------- New Testing BJSON Stuff --------------------------*/

/*********************************************************************************/
/*  SubAlloc a new BJNX class with protection against memory exhaustion.         */
/*********************************************************************************/
static PBJNX BjnxNew(PGLOBAL g, PBVAL vlp, int type, int len)
{
	PBJNX bjnx;

	try {
		bjnx = new(g) BJNX(g, vlp, type, len);
	} catch (...) {
		if (trace(1023))
			htrc("%s\n", g->Message);

		PUSH_WARNING(g->Message);
		bjnx = NULL;
	}	// end try/catch

	return bjnx;
} /* end of BjnxNew */

/* ----------------------------------- BSNX ------------------------------------ */

/*********************************************************************************/
/*  BSNX public constructor.                                                     */
/*********************************************************************************/
BJNX::BJNX(PGLOBAL g) : BDOC(g)
{
	Row = NULL;
	Bvalp = NULL;
	Jpnp = NULL;
	Jp = NULL;
	Nodes = NULL;
	Value = NULL;
	//MulVal = NULL;
	Jpath = NULL;
	Buf_Type = TYPE_STRING;
	Long = len;
	Prec = 0;
	Nod = 0;
	Xnod = -1;
	K = 0;
	I = -1;
	Imax = 9;
	B = 0;
	Xpd = false;
	Parsed = false;
	Found = false;
	Wr = false;
	Jb = false;
	Changed = false;
	Throw = false;
} // end of BJNX constructor

/*********************************************************************************/
/*  BSNX public constructor.                                                     */
/*********************************************************************************/
BJNX::BJNX(PGLOBAL g, PBVAL row, int type, int len, int prec, my_bool wr) : BDOC(g)
{
	Row = row;
	Bvalp = NULL;
	Jpnp = NULL;
	Jp = NULL;
	Nodes = NULL;
	Value = AllocateValue(g, type, len, prec);
	//MulVal = NULL;
	Jpath = NULL;
	Buf_Type = type;
	Long = len;
	Prec = prec;
	Nod = 0;
	Xnod = -1;
	K = 0;
	I = -1;
	Imax = 9;
	B = 0;
	Xpd = false;
	Parsed = false;
	Found = false;
	Wr = wr;
	Jb = false;
	Changed = false;
	Throw = false;
} // end of BJNX constructor

/*********************************************************************************/
/*  SetJpath: set and parse the json path.                                       */
/*********************************************************************************/
my_bool BJNX::SetJpath(PGLOBAL g, char* path, my_bool jb)
{
	// Check Value was allocated
	if (Value)
		Value->SetNullable(true);

	Jpath = path;

	// Parse the json path
	Parsed = false;
	Nod = 0;
	Jb = jb;
	return ParseJpath(g);
} // end of SetJpath

/*********************************************************************************/
/*  Analyse array processing options.                                            */
/*********************************************************************************/
my_bool BJNX::SetArrayOptions(PGLOBAL g, char* p, int i, PSZ nm)
{
	int     n = (int)strlen(p);
	my_bool dg = true, b = false;
	PJNODE  jnp = &Nodes[i];

	if (*p) {
		if (p[n - 1] == ']') {
			p[--n] = 0;
		} else if (!IsNum(p)) {
			// Wrong array specification
			sprintf(g->Message, "Invalid array specification %s", p);
			return true;
		} // endif p

	} else
		b = true;

	// To check whether a numeric Rank was specified
	dg = IsNum(p);

	if (!n) {
		// Default specifications
		if (jnp->Op != OP_EXP) {
			if (Wr) {
				// Force append
				jnp->Rank = INT_MAX32;
				jnp->Op = OP_LE;
			} else if (Jb) {
				// Return a Json item
				jnp->Op = OP_XX;
			} else if (b) {
				// Return 1st value (B is the index base)
				jnp->Rank = B;
				jnp->Op = OP_LE;
			} else if (!Value->IsTypeNum()) {
				jnp->CncVal = AllocateValue(g, PlugDup(g, ", "), TYPE_STRING);
				jnp->Op = OP_CNC;
			} else
				jnp->Op = OP_ADD;

		} // endif OP

	} else if (dg) {
		// Return nth value
		jnp->Rank = atoi(p) - B;
		jnp->Op = OP_EQ;
	} else if (Wr) {
		sprintf(g->Message, "Invalid specification %s in a write path", p);
		return true;
	} else if (n == 1) {
		// Set the Op value;
		switch (*p) {
		case '+': jnp->Op = OP_ADD;  break;
		case 'x': jnp->Op = OP_MULT; break;
		case '>': jnp->Op = OP_MAX;  break;
		case '<': jnp->Op = OP_MIN;  break;
		case '!': jnp->Op = OP_SEP;  break; // Average
		case '#': jnp->Op = OP_NUM;  break;
		case '*': jnp->Op = OP_EXP;  break;
		default:
			sprintf(g->Message, "Invalid function specification %c", *p);
			return true;
		} // endswitch *p

	} else if (*p == '"' && p[n - 1] == '"') {
		// This is a concat specification
		jnp->Op = OP_CNC;

		if (n > 2) {
			// Set concat intermediate string
			p[n - 1] = 0;

			if (trace(1))
				htrc("Concat string=%s\n", p + 1);

			jnp->CncVal = AllocateValue(g, p + 1, TYPE_STRING);
		} // endif n

	} else {
		strcpy(g->Message, "Wrong array specification");
		return true;
	} // endif's

	return false;
} // end of SetArrayOptions

/*********************************************************************************/
/*  Parse the eventual passed Jpath information.                                 */
/*  This information can be specified in the Fieldfmt column option when         */
/*  creating the table. It permits to indicate the position of the node          */
/*  corresponding to that column.                                                */
/*********************************************************************************/
my_bool BJNX::ParseJpath(PGLOBAL g)
{
	char* p, * p1 = NULL, * p2 = NULL, * pbuf = NULL;
	int     i;
	my_bool a, mul = false;

	if (Parsed)
		return false;                       // Already done
	else if (!Jpath)
		//	Jpath = Name;
		return true;

	if (trace(1))
		htrc("ParseJpath %s\n", SVP(Jpath));

	if (!(pbuf = PlgDBDup(g, Jpath)))
		return true;

	if (*pbuf == '$') pbuf++;
	if (*pbuf == '.') pbuf++;
	if (*pbuf == '[') p1 = pbuf++;

	// Estimate the required number of nodes
	for (i = 0, p = pbuf; (p = NextChr(p, '.')); i++, p++)
		Nod++;                         // One path node found

	if (!(Nodes = (PJNODE)PlgDBSubAlloc(g, NULL, (++Nod) * sizeof(JNODE))))
		return true;

	memset(Nodes, 0, (Nod) * sizeof(JNODE));

	// Analyze the Jpath for this column
	for (i = 0, p = pbuf; p && i < Nod; i++, p = (p2 ? p2 : NULL)) {
		a = (p1 != NULL);
		p1 = strchr(p, '[');
		p2 = strchr(p, '.');

		if (!p2)
			p2 = p1;
		else if (p1) {
			if (p1 < p2)
				p2 = p1;
			else if (p1 == p2 + 1)
				*p2++ = 0;		 // Old syntax .[
			else
				p1 = NULL;

		}	// endif p1

		if (p2)
			*p2++ = 0;

		// Jpath must be explicit
		if (a || *p == 0 || *p == '[' || IsNum(p)) {
			// Analyse intermediate array processing
			if (SetArrayOptions(g, p, i, Nodes[i - 1].Key))
				return true;

		} else if (*p == '*') {
			if (Wr) {
				sprintf(g->Message, "Invalid specification %c in a write path", *p);
				return true;
			} else     			// Return JSON
				Nodes[i].Op = OP_XX;

		} else {
			Nodes[i].Key = p;
			Nodes[i].Op = OP_EXIST;
		} // endif's

	} // endfor i, p

	Nod = i;
//MulVal = AllocateValue(g, Value);

	if (trace(1))
		for (i = 0; i < Nod; i++)
			htrc("Node(%d) Key=%s Op=%d Rank=%d\n",
				i, SVP(Nodes[i].Key), Nodes[i].Op, Nodes[i].Rank);

	Parsed = true;
	return false;
} // end of ParseJpath

/*********************************************************************************/
/*  Make a valid key from the passed argument.                                   */
/*********************************************************************************/
PSZ BJNX::MakeKey(UDF_ARGS *args, int i)
{
	if (args->arg_count > (unsigned)i) {
		int     j = 0, n = args->attribute_lengths[i];
		my_bool b;  // true if attribute is zero terminated
		PSZ     p;
		PCSZ    s = args->attributes[i];

		if (s && *s && (n || *s == '\'')) {
			if ((b = (!n || !s[n])))
				n = strlen(s);

			if (IsArgJson(args, i))
				j = (int)(strchr(s, '_') - s + 1);

			if (j && n > j) {
				s += j;
				n -= j;
			} else if (*s == '\'' && s[n-1] == '\'') {
				s++;
				n -= 2;
				b = false;
			} // endif *s

			if (n < 1)
				return NewStr((PSZ)"Key");

			if (!b) {
				p = (PSZ)BsonSubAlloc(n + 1);
				memcpy(p, s, n);
				p[n] = 0;
				return p;
			} // endif b

		} // endif s

		return NewStr((PSZ)s);
	} // endif count

	return NewStr((PSZ)"Key");
} // end of MakeKey

/*********************************************************************************/
/*  MakeJson: Make the Json tree to serialize.                                   */
/*********************************************************************************/
PBVAL BJNX::MakeJson(PGLOBAL g, PBVAL bvp, int n)
{
	PBVAL vlp, jvp = bvp;

	Jb = false;

	if (n < Nod -1) {
		if (bvp->Type == TYPE_JAR) {
			int    ars = GetArraySize(bvp);
			PJNODE jnp = &Nodes[n];

			jvp = NewVal(TYPE_JAR);
			jnp->Op = OP_EQ;

			for (int i = 0; i < ars; i++) {
				jnp->Rank = i;
				vlp = GetRowValue(g, bvp, n);
				AddArrayValue(jvp, DupVal(vlp));
			} // endfor i

			jnp->Op = OP_XX;
			jnp->Rank = 0;
		} else if(bvp->Type == TYPE_JOB) {
			jvp = NewVal(TYPE_JOB);

			for (PBPR prp = GetObject(bvp); prp; prp = GetNext(prp)) {
				vlp = GetRowValue(g, GetVlp(prp), n + 1);
				SetKeyValue(jvp, vlp, MZP(prp->Key));
			}	// endfor prp

		} // endif Type

	} // endif n

	Jb = true;
	return jvp;
} // end of MakeJson

/*********************************************************************************/
/*  SetValue: Set a value from a BVALUE contains.                                */
/*********************************************************************************/
void BJNX::SetJsonValue(PGLOBAL g, PVAL vp, PBVAL vlp)
{
	if (vlp) {
		vp->SetNull(false);

		if (Jb) {
			vp->SetValue_psz(Serialize(g, vlp, NULL, 0));
			Jb = false;
		} else switch (vlp->Type) {
		case TYPE_DTM:
		case TYPE_STRG:
			vp->SetValue_psz(GetString(vlp));
			break;
		case TYPE_INTG:
			vp->SetValue(GetInteger(vlp));
			break;
		case TYPE_BINT:
			vp->SetValue(GetBigint(vlp));
			break;
		case TYPE_DBL:
		case TYPE_FLOAT:
			if (vp->IsTypeNum())
				vp->SetValue(GetDouble(vlp));
			else // Get the proper number of decimals
				vp->SetValue_psz(GetString(vlp));

			break;
		case TYPE_BOOL:
			if (vp->IsTypeNum())
				vp->SetValue(GetInteger(vlp) ? 1 : 0);
			else
				vp->SetValue_psz(GetString(vlp));

			break;
		case TYPE_JAR:
			vp->SetValue_psz(GetArrayText(g, vlp, NULL));
			break;
		case TYPE_JOB:
			vp->SetValue_psz(GetObjectText(g, vlp, NULL));
			break;
		case TYPE_NULL:
			vp->SetNull(true);
		default:
			vp->Reset();
		} // endswitch Type

	} else {
		vp->SetNull(true);
		vp->Reset();
	} // endif val

} // end of SetJsonValue

/*********************************************************************************/
/*  GetJson:                                                                     */
/*********************************************************************************/
PBVAL BJNX::GetJson(PGLOBAL g)
{
	return GetRowValue(g, Row, 0);
} // end of GetJson

/*********************************************************************************/
/*  ReadValue:                                                                   */
/*********************************************************************************/
void BJNX::ReadValue(PGLOBAL g)
{
	Value->SetValue_pval(GetColumnValue(g, Row, 0));
} // end of ReadValue

/*********************************************************************************/
/*  GetColumnValue:                                                              */
/*********************************************************************************/
PVAL BJNX::GetColumnValue(PGLOBAL g, PBVAL row, int i)
{
	PBVAL vlp = GetRowValue(g, row, i);

	SetJsonValue(g, Value, vlp);
	return Value;
} // end of GetColumnValue

/*********************************************************************************/
/*  GetRowValue:                                                                 */
/*********************************************************************************/
PBVAL BJNX::GetRowValue(PGLOBAL g, PBVAL row, int i)
{
	my_bool expd = false;
	PBVAL   bap;
	PBVAL   vlp = NULL;

	for (; i < Nod && row; i++) {
		if (Nodes[i].Op == OP_NUM) {
			Value->SetValue(row->Type == TYPE_JAR ? GetArraySize(row) : 1);
			vlp = NewVal(Value);
			return vlp;
		} else if (Nodes[i].Op == OP_XX) {
			return MakeJson(g, row, i);
		} else if (Nodes[i].Op == OP_EXP) {
			PUSH_WARNING("Expand not supported by this function");
			return NULL;
		} else switch (row->Type) {
		case TYPE_JOB:
			if (!Nodes[i].Key) {
				// Expected Array was not there
				if (Nodes[i].Op == OP_LE) {
					if (i < Nod - 1)
						continue;
					else
						vlp = row;  // DupVal(g, row) ???

				} else {
					strcpy(g->Message, "Unexpected object");
					vlp = NULL;
				} //endif Op

			} else
				vlp = GetKeyValue(row, Nodes[i].Key);

			break;
		case TYPE_JAR:
			bap = row;

			if (!Nodes[i].Key) {
				if (Nodes[i].Op == OP_EQ || Nodes[i].Op == OP_LE)
					vlp = GetArrayValue(bap, Nodes[i].Rank);
				else if (Nodes[i].Op == OP_EXP)
					return (PBVAL)ExpandArray(g, bap, i);
				else
					return NewVal(CalculateArray(g, bap, i));

			} else {
				// Unexpected array, unwrap it as [0]
				vlp = GetArrayValue(bap, 0);
				i--;
			}	// endif's

			break;
		case TYPE_JVAL:
			vlp = row;
			break;
		default:
			sprintf(g->Message, "Invalid row JSON type %d", row->Type);
			vlp = NULL;
		} // endswitch Type

		row = vlp;
	} // endfor i

	return vlp;
} // end of GetRowValue

/*********************************************************************************/
/*  ExpandArray:                                                                 */
/*********************************************************************************/
PVAL BJNX::ExpandArray(PGLOBAL g, PBVAL arp, int n)
{
	strcpy(g->Message, "Expand cannot be done by this function");
	return NULL;
} // end of ExpandArray

/*********************************************************************************/
/*  Get the value used for calculating the array.                                */
/*********************************************************************************/
PVAL BJNX::GetCalcValue(PGLOBAL g, PBVAL bap, int n)
{
	// For calculated arrays, a local Value must be used
	int     lng = 0;
	short   type = 0, prec = 0;
	bool    b = n < Nod - 1;
	PVAL    valp;
	PBVAL   vlp, vp;
	OPVAL   op = Nodes[n].Op;

	switch (op) {
		case OP_NUM:
			type = TYPE_INT;
			break;
		case OP_ADD:
		case OP_MULT:
			if (!IsTypeNum(Buf_Type)) {
				type = TYPE_INT;
				prec = 0;

				for (vlp = GetArray(bap); vlp; vlp = GetNext(vlp)) {
					vp = (b && IsJson(vlp)) ? GetRowValue(g, vlp, n + 1) : vlp;

					switch (vp->Type) {
						case TYPE_BINT:
							if (type == TYPE_INT)
								type = TYPE_BIGINT;

							break;
						case TYPE_DBL:
						case TYPE_FLOAT:
							type = TYPE_DOUBLE;
							prec = MY_MAX(prec, vp->Nd);
							break;
						default:
							break;
					}	// endswitch Type

				} // endfor vlp

			} else {
				type = Buf_Type;
				prec = GetPrecision();
			} // endif Buf_Type

			break;
		case OP_SEP:
			if (IsTypeChar(Buf_Type)) {
				type = TYPE_DOUBLE;
				prec = 2;
			} else {
				type = Buf_Type;
				prec = GetPrecision();
			} // endif Buf_Type

			break;
		case OP_MIN:
		case OP_MAX:
			type = Buf_Type;
			lng = Long;
			prec = GetPrecision();
			break;
		case OP_CNC:
			type = TYPE_STRING;

			if (IsTypeChar(Buf_Type)) {
				lng = (Long) ? Long : 512;
				prec = GetPrecision();
			} else
				lng = 512;

			break;
		default:
			DBUG_ASSERT(!"Implement new op type support.");
	} // endswitch Op

	return valp = AllocateValue(g, type, lng, prec);
} // end of GetCalcValue

/*********************************************************************************/
/*  CalculateArray                                                               */
/*********************************************************************************/
PVAL BJNX::CalculateArray(PGLOBAL g, PBVAL bap, int n)
{
	int     i, ars = GetArraySize(bap), nv = 0;
	bool    err;
	OPVAL   op = Nodes[n].Op;
	PVAL    val[2], vp = GetCalcValue(g, bap, n);
	PVAL    mulval = AllocateValue(g, vp);
	PBVAL   bvrp, bvp;
	BVAL    bval;

	vp->Reset();
	xtrc(1, "CalculateArray size=%d op=%d\n", ars, op);

	try {
		for (i = 0; i < ars; i++) {
			bvrp = GetArrayValue(bap, i);
			xtrc(1, "i=%d nv=%d\n", i, nv);

			if (!IsValueNull(bvrp) || (op == OP_CNC && GetJsonNull())) {
				if (IsValueNull(bvrp)) {
					SetString(bvrp, NewStr(GetJsonNull()), 0);
					bvp = bvrp;
				} else if (n < Nod - 1 && IsJson(bvrp)) {
					SetValue(&bval, GetColumnValue(g, bvrp, n + 1));
					bvp = &bval;
				} else
					bvp = bvrp;

				if (trace(1))
					htrc("bvp=%s null=%d\n",
						GetString(bvp), IsValueNull(bvp) ? 1 : 0);

				if (!nv++) {
					SetJsonValue(g, vp, bvp);
					continue;
				} else
					SetJsonValue(g, mulval, bvp);

				if (!mulval->IsNull()) {
					switch (op) {
						case OP_CNC:
							if (Nodes[n].CncVal) {
								val[0] = Nodes[n].CncVal;
								err = vp->Compute(g, val, 1, op);
							} // endif CncVal

							val[0] = mulval;
							err = vp->Compute(g, val, 1, op);
							break;
							// case OP_NUM:
						case OP_SEP:
							val[0] = vp;
							val[1] = mulval;
							err = vp->Compute(g, val, 2, OP_ADD);
							break;
						default:
							val[0] = vp;
							val[1] = mulval;
							err = vp->Compute(g, val, 2, op);
					} // endswitch Op

					if (err)
						vp->Reset();

					if (trace(1)) {
						char buf(32);

						htrc("vp='%s' err=%d\n",
							vp->GetCharString(&buf), err ? 1 : 0);
					} // endif trace

				} // endif Zero

			}	// endif jvrp

		} // endfor i

		if (op == OP_SEP) {
			// Calculate average
			mulval->SetValue(nv);
			val[0] = vp;
			val[1] = mulval;

			if (vp->Compute(g, val, 2, OP_DIV))
				vp->Reset();

		} // endif Op

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
	} catch (const char* msg) {
		strcpy(g->Message, msg);
	} // end catch

	return vp;
} // end of CalculateArray

/***********************************************************************/
/*  GetRow: Set the complete path of the object to be set.             */
/***********************************************************************/
PBVAL BJNX::GetRow(PGLOBAL g)
{
	PBVAL val = NULL;
	PBVAL arp;
	PBVAL nwr, row = Row;

	for (int i = 0; i < Nod - 1 && row; i++) {
		if (Nodes[i].Op == OP_XX)
			break;
		else if (Nodes[i].Op == OP_EXP) {
			PUSH_WARNING("Expand not supported by this function");
			return NULL;
		} else switch (row->Type) {
		case TYPE_JOB:
			if (!Nodes[i].Key)
				// Expected Array was not there, wrap the value
				continue;

			val = GetKeyValue(row, Nodes[i].Key);
			break;
		case TYPE_JAR:
			arp = row;

			if (!Nodes[i].Key) {
				if (Nodes[i].Op == OP_EQ)
					val = GetArrayValue(arp, Nodes[i].Rank);
				else
					val = GetArrayValue(arp, Nodes[i].Rx);

			} else {
				// Unexpected array, unwrap it as [0]
				val = GetArrayValue(arp, 0);
				i--;
			} // endif Nodes

			break;
		case TYPE_JVAL:
			val = MVP(row->To_Val);
			break;
		default:
			sprintf(g->Message, "Invalid row JSON type %d", row->Type);
			val = NULL;
		} // endswitch Type

		if (val) {
			row = val;
		} else {
			// Construct missing objects
			for (i++; row && i < Nod; i++) {
				if (Nodes[i].Op == OP_XX)
					break;

				// Construct new row
				nwr = NewVal();

				if (row->Type == TYPE_JOB) {
					SetKeyValue(row, MOF(nwr), Nodes[i - 1].Key);
				} else if (row->Type == TYPE_JAR) {
					AddArrayValue(row, MOF(nwr));
				} else {
					strcpy(g->Message, "Wrong type when writing new row");
					nwr = NULL;
				} // endif's

				row = nwr;
			} // endfor i

			break;
		} // endelse

	} // endfor i

	return row;
} // end of GetRow

/***********************************************************************/
/*  WriteValue:                                                        */
/***********************************************************************/
my_bool BJNX::WriteValue(PGLOBAL g, PBVAL jvalp)
{
	PBVAL objp = NULL;
	PBVAL arp = NULL;
	PBVAL jvp = NULL;
	PBVAL row = GetRow(g);

	if (!row)
		return true;

	switch (row->Type) {
	case TYPE_JOB:  objp = row; break;
	case TYPE_JAR:  arp = row;  break;
	case TYPE_JVAL: jvp = MVP(row->To_Val);  break;
	default:
		strcpy(g->Message, "Invalid target type");
		return true;
	} // endswitch Type

	if (arp) {
		if (!Nodes[Nod - 1].Key) {
			if (Nodes[Nod - 1].Op == OP_EQ)
				SetArrayValue(arp, jvalp, Nodes[Nod - 1].Rank);
			else
				AddArrayValue(arp, MOF(jvalp));

		}	// endif Key

	} else if (objp) {
		if (Nodes[Nod - 1].Key)
			SetKeyValue(objp, MOF(jvalp), Nodes[Nod - 1].Key);

	} else if (jvp)
		SetValueVal(jvp, jvalp);

	return false;
} // end of WriteValue

/*********************************************************************************/
/*  GetRowValue:                                                                 */
/*********************************************************************************/
my_bool BJNX::DeleteItem(PGLOBAL g, PBVAL row)
{
	int     n = -1;
	my_bool b = false;
	bool    loop;
	PBVAL   vlp, pvp, rwp;

	do {
		loop = false;
		vlp = NULL;
		pvp = rwp = row;

		for (int i = 0; i < Nod && rwp; i++) {
			if (Nodes[i].Op == OP_XX)
				break;
			else switch (rwp->Type) {
				case TYPE_JOB:
					if (!Nodes[i].Key) {
						vlp = NULL;
					} else
						vlp = GetKeyValue(rwp, Nodes[i].Key);

					break;
				case TYPE_JAR:
					if (!Nodes[i].Key) {
						if (Nodes[i].Op == OP_EXP) {
							if (loop) {
								PUSH_WARNING("Only one expand can be handled");
								return b;
							} // endif loop

							n++;
						} else
							n = Nodes[i].Rank;

						vlp = GetArrayValue(rwp, n);

						if (GetNext(vlp) && Nodes[i].Op == OP_EXP)
							loop = true;

					} else
						vlp = NULL;

					break;
				case TYPE_JVAL:
					vlp = rwp;
					break;
				default:
					vlp = NULL;
			} // endswitch Type

			pvp = rwp;
			rwp = vlp;
			vlp = NULL;
		} // endfor i

		if (rwp) {
			if (Nodes[Nod - 1].Op == OP_XX) {
				if (!IsJson(rwp))
					rwp->Type = TYPE_NULL;

				rwp->To_Val = 0;
			} else switch (pvp->Type) {
				case TYPE_JOB:
					b = DeleteKey(pvp, Nodes[Nod - 1].Key);
					break;
				case TYPE_JAR:
					if (Nodes[Nod - 1].Op == OP_EXP) {
						pvp->To_Val = 0;
						loop = false;
					} else
						b = DeleteValue(pvp, n);

					break;
				default:
					break;
			} // endswitch Type

		} // endif rwp

	} while (loop);

	return b;
} // end of DeleteItem

/*********************************************************************************/
/* CheckPath: Checks whether the path exists in the document.                    */
/*********************************************************************************/
my_bool BJNX::CheckPath(PGLOBAL g)
{
	PBVAL   val = NULL;
	PBVAL   row = Row;

	for (int i = 0; i < Nod && row; i++) {
		val = NULL;

		if (Nodes[i].Op == OP_NUM || Nodes[i].Op == OP_XX) {
		} else switch (row->Type) {
			case TYPE_JOB:
				if (Nodes[i].Key)
					val = GetKeyValue(row, Nodes[i].Key);

				break;
			case TYPE_JAR:
				if (!Nodes[i].Key)
					if (Nodes[i].Op == OP_EQ || Nodes[i].Op == OP_LE)
						val = GetArrayValue(row, Nodes[i].Rank);

				break;
			case TYPE_JVAL:
				val = row;
				break;
			default:
				sprintf(g->Message, "Invalid row JSON type %d", row->Type);
		} // endswitch Type

		if (i < Nod-1)
			if (!(row = (IsJson(val)) ? val : NULL))
				val = NULL;

	} // endfor i

	return (val != NULL);
} // end of CheckPath

/*********************************************************************************/
/*  Check if a path was specified and set jvp according to it.                   */
/*********************************************************************************/
my_bool BJNX::CheckPath(PGLOBAL g, UDF_ARGS *args, PBVAL jsp, PBVAL& jvp, int n)
{
	for (uint i = n; i < args->arg_count; i++)
		if (args->arg_type[i] == STRING_RESULT && args->args[i]) {
			// A path to a subset of the json tree is given
			char *path = MakePSZ(g, args, i);

			if (path) {
				Row = jsp;

				if (SetJpath(g, path))
					return true;

				if (!(jvp = GetJson(g))) {
					sprintf(g->Message, "No sub-item at '%s'", path);
					return true;
				} else
					return false;

			} else {
				strcpy(g->Message, "Path argument is null");
				return true;
			} // endif path

		}	// endif type

	jvp = jsp;
	return false;
} // end of CheckPath

/*********************************************************************************/
/*  Locate a value in a JSON tree:                                               */
/*********************************************************************************/
PSZ BJNX::Locate(PGLOBAL g, PBVAL jsp, PBVAL jvp, int k)
{
	PSZ     str = NULL;
	my_bool b = false, err = true;

	g->Message[0] = 0;

	if (!jsp) {
		strcpy(g->Message, "Null json tree");
		return NULL;
	} // endif jsp

	try {
		// Write to the path string
		Jp = new(g) JOUTSTR(g);
		Jp->WriteChr('$');
		Bvalp = jvp;
		K = k;

		switch (jsp->Type) {
		case TYPE_JAR:
			err = LocateArray(g, jsp);
			break;
		case TYPE_JOB:
			err = LocateObject(g, jsp);
			break;
		case TYPE_JVAL:
			err = LocateValue(g, MVP(jsp->To_Val));
			break;
		default:
			err = true;
		} // endswitch Type

		if (err) {
			if (!g->Message[0])
				strcpy(g->Message, "Invalid json tree");

		} else if (Found) {
			Jp->WriteChr('\0');
			PlugSubAlloc(g, NULL, Jp->N);
			str = Jp->Strp;
		} // endif's

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
	} catch (const char* msg) {
		strcpy(g->Message, msg);
	} // end catch

	return str;
} // end of Locate

/*********************************************************************************/
/*  Locate in a JSON Array.                                                      */
/*********************************************************************************/
my_bool BJNX::LocateArray(PGLOBAL g, PBVAL jarp)
{
	char   s[16];
	int    n = GetArraySize(jarp);
	size_t m = Jp->N;

	for (int i = 0; i < n && !Found; i++) {
		Jp->N = m;
		sprintf(s, "[%d]", i + B);

		if (Jp->WriteStr(s))
			return true;

		if (LocateValue(g, GetArrayValue(jarp, i)))
			return true;

	} // endfor i

	return false;
} // end of LocateArray

/*********************************************************************************/
/*  Locate in a JSON Object.                                                     */
/*********************************************************************************/
my_bool BJNX::LocateObject(PGLOBAL g, PBVAL jobp)
{
	size_t m;

	if (Jp->WriteChr('.'))
		return true;

	m = Jp->N;

	for (PBPR pair = GetObject(jobp); pair && !Found; pair = GetNext(pair)) {
		Jp->N = m;

		if (Jp->WriteStr(MZP(pair->Key)))
			return true;

		if (LocateValue(g, GetVlp(pair)))
			return true;

	} // endfor i

	return false;
} // end of LocateObject

/*********************************************************************************/
/*  Locate a JSON Value.                                                         */
/*********************************************************************************/
my_bool BJNX::LocateValue(PGLOBAL g, PBVAL jvp)
{
	if (CompareTree(g, Bvalp, jvp))
		Found = (--K == 0);
	else if (jvp->Type == TYPE_JAR)
		return LocateArray(g, jvp);
	else if (jvp->Type == TYPE_JOB)
		return LocateObject(g, jvp);

	return false;
} // end of LocateValue

/*********************************************************************************/
/*  Locate all occurrences of a value in a JSON tree:                            */
/*********************************************************************************/
PSZ BJNX::LocateAll(PGLOBAL g, PBVAL jsp, PBVAL bvp, int mx)
{
	PSZ     str = NULL;
	my_bool b = false, err = true;
	PJPN    jnp;

	if (!jsp) {
		strcpy(g->Message, "Null json tree");
		return NULL;
	} // endif jsp

	try {
		jnp = (PJPN)PlugSubAlloc(g, NULL, sizeof(JPN) * mx);
		memset(jnp, 0, sizeof(JPN) * mx);
		g->Message[0] = 0;

		// Write to the path string
		Jp = new(g)JOUTSTR(g);
		Bvalp = bvp;
		Imax = mx - 1;
		Jpnp = jnp;
		Jp->WriteChr('[');

		switch (jsp->Type) {
		case TYPE_JAR:
			err = LocateArrayAll(g, jsp);
			break;
		case TYPE_JOB:
			err = LocateObjectAll(g, jsp);
			break;
		case TYPE_JVAL:
			err = LocateValueAll(g, MVP(jsp->To_Val));
			break;
		default:
			err = LocateValueAll(g, jsp);
		} // endswitch Type

		if (!err) {
			if (Jp->N > 1)
				Jp->N--;

			Jp->WriteChr(']');
			Jp->WriteChr('\0');
			PlugSubAlloc(g, NULL, Jp->N);
			str = Jp->Strp;
		} else if (!g->Message[0])
			strcpy(g->Message, "Invalid json tree");

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
	} catch (const char* msg) {
		strcpy(g->Message, msg);
	} // end catch

	return str;
} // end of LocateAll

/*********************************************************************************/
/*  Locate in a JSON Array.                                                      */
/*********************************************************************************/
my_bool BJNX::LocateArrayAll(PGLOBAL g, PBVAL jarp)
{
	int i = 0;

	if (I < Imax) {
		Jpnp[++I].Type = TYPE_JAR;

		for (PBVAL vp = GetArray(jarp); vp; vp = GetNext(vp)) {
			Jpnp[I].N = i;

			if (LocateValueAll(g, GetArrayValue(jarp, i)))
				return true;

			i++;
		} // endfor i

		I--;
	} // endif I

	return false;
} // end of LocateArrayAll

/*********************************************************************************/
/*  Locate in a JSON Object.                                                     */
/*********************************************************************************/
my_bool BJNX::LocateObjectAll(PGLOBAL g, PBVAL jobp)
{
	if (I < Imax) {
		Jpnp[++I].Type = TYPE_JOB;

		for (PBPR pair = GetObject(jobp); pair; pair = GetNext(pair)) {
			Jpnp[I].Key = MZP(pair->Key);

			if (LocateValueAll(g, GetVlp(pair)))
				return true;

		} // endfor i

		I--;
	} // endif I

	return false;
} // end of LocateObjectAll

/*********************************************************************************/
/*  Locate a JSON Value.                                                         */
/*********************************************************************************/
my_bool BJNX::LocateValueAll(PGLOBAL g, PBVAL jvp)
{
	if (CompareTree(g, Bvalp, jvp))
		return AddPath();
	else if (jvp->Type == TYPE_JAR)
		return LocateArrayAll(g, jvp);
	else if (jvp->Type == TYPE_JOB)
		return LocateObjectAll(g, jvp);

	return false;
} // end of LocateValueAll

/*********************************************************************************/
/*  Compare two JSON trees.                                                      */
/*********************************************************************************/
my_bool BJNX::CompareTree(PGLOBAL g, PBVAL jp1, PBVAL jp2)
{
	if (!jp1 || !jp2 || jp1->Type != jp2->Type || GetSize(jp1) != GetSize(jp2))
		return false;

	my_bool found = true;

	if (jp1->Type == TYPE_JAR) {
		for (int i = 0; found && i < GetArraySize(jp1); i++)
			found = (CompareValues(g, GetArrayValue(jp1, i), GetArrayValue(jp2, i)));

	} else if (jp1->Type == TYPE_JOB) {
		PBPR p1 = GetObject(jp1), p2 = GetObject(jp2);

		// Keys can be differently ordered
		for (; found && p1 && p2; p1 = GetNext(p1))
			found = CompareValues(g, GetVlp(p1), GetKeyValue(jp2, GetKey(p1)));

	} else if (jp1->Type == TYPE_JVAL) {
		found = CompareTree(g, MVP(jp1->To_Val), (MVP(jp2->To_Val)));
	} else
		found = CompareValues(g, jp1, jp2);

	return found;
} // end of CompareTree

/*********************************************************************************/
/*  Compare two VAL values and return true if they are equal.                    */
/*********************************************************************************/
my_bool BJNX::CompareValues(PGLOBAL g, PBVAL v1, PBVAL v2)
{
	my_bool b = false;

	if (v1 && v2)
		switch (v1->Type) {
		case TYPE_JAR:
		case TYPE_JOB:
			if (v2->Type == v1->Type)
				b = CompareTree(g, v1, v2);

			break;
		case TYPE_STRG:
			if (v2->Type == TYPE_STRG) {
				if (v1->Nd || v2->Nd)		// Case insensitive
					b = (!stricmp(MZP(v1->To_Val), MZP(v2->To_Val)));
				else
					b = (!strcmp(MZP(v1->To_Val), MZP(v2->To_Val)));

			} // endif Type

			break;
		case TYPE_DTM:
			if (v2->Type == TYPE_DTM)
				b = (!strcmp(MZP(v1->To_Val), MZP(v2->To_Val)));

			break;
		case TYPE_INTG:
			if (v2->Type == TYPE_INTG)
				b = (v1->N == v2->N);
			else if (v2->Type == TYPE_BINT)
				b = ((longlong)v1->N == LLN(v2->To_Val));

			break;
		case TYPE_BINT:
			if (v2->Type == TYPE_INTG)
				b = (LLN(v1->To_Val) == (longlong)v2->N);
			else if (v2->Type == TYPE_BINT)
				b = (LLN(v1->To_Val) == LLN(v2->To_Val));

			break;
		case TYPE_FLOAT:
			if (v2->Type == TYPE_FLOAT)
				b = (v1->F == v2->F);
			else if (v2->Type == TYPE_DBL)
				b = ((double)v1->F == DBL(v2->To_Val));

			break;
		case TYPE_DBL:
			if (v2->Type == TYPE_DBL)
				b = (DBL(v1->To_Val) == DBL(v2->To_Val));
			else if (v2->Type == TYPE_FLOAT)
				b = (DBL(v1->To_Val) == (double)v2->F);

			break;
		case TYPE_BOOL:
			if (v2->Type == TYPE_BOOL)
				b = (v1->B == v2->B);

			break;
		case TYPE_NULL:
			b = (v2->Type == TYPE_NULL);
			break;
		default:
			break;
		}	// endswitch Type

	else
		b = (!v1 && !v2);

	return b;
} // end of CompareValues

/*********************************************************************************/
/*  Add the found path to the list.                                              */
/*********************************************************************************/
my_bool BJNX::AddPath(void)
{
	char s[16];

	if (Jp->WriteStr("\"$"))
		return true;

	for (int i = 0; i <= I; i++) {
		if (Jpnp[i].Type == TYPE_JAR) {
			sprintf(s, "[%d]", Jpnp[i].N + B);

			if (Jp->WriteStr(s))
				return true;

		} else {
			if (Jp->WriteChr('.'))
				return true;

			if (Jp->WriteStr(Jpnp[i].Key))
				return true;

		}	// endif's

	}	// endfor i

	if (Jp->WriteStr("\","))
		return true;

	return false;
}	// end of AddPath

/*********************************************************************************/
/*  Make a JSON value from the passed argument.                                  */
/*********************************************************************************/
PBVAL BJNX::MakeValue(UDF_ARGS *args, uint i, bool b, PBVAL *top)
{
	char *sap = (args->arg_count > i) ? args->args[i] : NULL;
	int   n, len;
	int   ci;
	long long bigint;
	PGLOBAL& g = G;
	PBVAL jvp = NewVal();

	if (top)
		*top = NULL;

	if (sap) switch (args->arg_type[i]) {
		case STRING_RESULT:
			if ((len = args->lengths[i])) {
				if ((n = IsArgJson(args, i)) < 3)
					sap = MakePSZ(g, args, i);

				if (n) {
					if (n == 3) {
						PBSON bsp = (PBSON)sap;

						if (i == 0) {
							if (top)
								*top = (PBVAL)bsp->Top;

							jvp = (PBVAL)bsp->Jsp;
							G = bsp->G;
							Base = G->Sarea;
						} else {
							BJNX bnx(bsp->G);

							jvp = MoveJson(&bnx, (PBVAL)bsp->Jsp);
						} // endelse i

					} else {
						if (n == 2) {
							if (!(sap = GetJsonFile(g, sap))) {
								PUSH_WARNING(g->Message);
								return jvp;
							} // endif sap

							len = strlen(sap);
						} // endif n

						if (!(jvp = ParseJson(g, sap, strlen(sap))))
							PUSH_WARNING(g->Message);
						else if (top)
							*top = jvp;

					} // endif's n

				} else {
					PBVAL bp = NULL;

					if (b) {
						if (strchr("[{ \t\r\n", *sap)) {
							// Check whether this string is a valid json string
							JsonMemSave(g);

							if (!(bp = ParseJson(g, sap, strlen(sap))))
								JsonSubSet(g);			// Recover suballocated memory

							g->Saved_Size = 0;
						}	else {
							// Perhaps a file name
							char* s = GetJsonFile(g, sap);
							
							if (s)
								bp = ParseJson(g, s, strlen(s));

						}	// endif's

					}	// endif b
					
					if (!bp) {
						ci = (strnicmp(args->attributes[i], "ci", 2)) ? 0 : 1;
						SetString(jvp, sap, ci);
					}	else {
						if (top)
							*top = bp;

						jvp = bp;
					}	// endif bp

				}	// endif n

			} // endif len

			break;
		case INT_RESULT:
			bigint = *(long long*)sap;

			if ((bigint == 0LL && !strcmp(args->attributes[i], "FALSE")) ||
				(bigint == 1LL && !strcmp(args->attributes[i], "TRUE")))
				SetBool(jvp, (char)bigint);
			else
				SetBigint(jvp, bigint);

			break;
		case REAL_RESULT:
			SetFloat(jvp, *(double*)sap);
			break;
		case DECIMAL_RESULT:
			SetFloat(jvp, MakePSZ(g, args, i));
			break;
		case TIME_RESULT:
		case ROW_RESULT:
		default:
			break;
	} // endswitch arg_type

	return jvp;
} // end of MakeValue

/*********************************************************************************/
/*  Try making a JSON value of the passed type from the passed argument.         */
/*********************************************************************************/
PBVAL BJNX::MakeTypedValue(PGLOBAL g, UDF_ARGS *args, uint i, JTYP type, PBVAL *top)
{
	char *sap;
	PBVAL jsp;
	PBVAL jvp = MakeValue(args, i, false, top);

	//if (type == TYPE_JSON) {
	//	if (jvp->GetValType() >= TYPE_JSON)
	//		return jvp;

	//} else if (jvp->GetValType() == type)
	//	return jvp;

	if (jvp->Type == TYPE_STRG) {
		sap = GetString(jvp);

		if ((jsp = ParseJson(g, sap, strlen(sap)))) {
			if ((type == TYPE_JSON && jsp->Type != TYPE_JVAL) || jsp->Type == type) {
				if (top)
					*top = jvp;

				SetValueVal(jvp, jsp);
			} // endif Type

		} // endif jsp

	} // endif Type

	return jvp;
} // end of MakeTypedValue

/*********************************************************************************/
/*  Parse a json file.                                                           */
/*********************************************************************************/
PBVAL BJNX::ParseJsonFile(PGLOBAL g, char *fn, int& pty, size_t& len)
{
	char   *memory;
	HANDLE  hFile;
	MEMMAP  mm;
	PBVAL   jsp;

	// Create the mapping file object
	hFile = CreateFileMap(g, fn, &mm, MODE_READ, false);

	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD rc = GetLastError();

		if (!(*g->Message))
			sprintf(g->Message, MSG(OPEN_MODE_ERROR), "map", (int)rc, fn);

		return NULL;
	} // endif hFile

		// Get the file size
	len = (size_t)mm.lenL;

	if (mm.lenH)
		len += mm.lenH;

	memory = (char *)mm.memory;

	if (!len) {              // Empty or deleted file
		CloseFileHandle(hFile);
		return NULL;
	} // endif len

	if (!memory) {
		CloseFileHandle(hFile);
		sprintf(g->Message, MSG(MAP_VIEW_ERROR), fn, GetLastError());
		return NULL;
	} // endif Memory

	CloseFileHandle(hFile);  // Not used anymore

	// Parse the json file and allocate its tree structure
	g->Message[0] = 0;
	jsp = ParseJson(g, memory, len);
	pty = pretty;
	CloseMemMap(memory, len);
	return jsp;
} // end of ParseJsonFile

/*********************************************************************************/
/*  Make the result according to the first argument type.                        */
/*********************************************************************************/
char *BJNX::MakeResult(UDF_ARGS *args, PBVAL top, uint n)
{
	char    *str = NULL;
	PGLOBAL& g = G;

	if (IsArgJson(args, 0) == 2) {
		// Make the change in the json file
		PSZ fn = MakePSZ(g, args, 0);

		if (Changed) {
			int pretty = 2;

			for (uint i = n; i < args->arg_count; i++)
				if (args->arg_type[i] == INT_RESULT) {
					pretty = (int)*(longlong*)args->args[i];
					break;
				} // endif type

			if (!Serialize(g, top, fn, pretty))
				PUSH_WARNING(g->Message);

			Changed = false;
		}	// endif Changed

		str = fn;
	} else if (IsArgJson(args, 0) == 3) {
		PBSON bsp = (PBSON)args->args[0];

		if (bsp->Filename) {
			if (Changed) {
				// Make the change in the json file
				if (!Serialize(g, (PBVAL)top, bsp->Filename, bsp->Pretty))
					PUSH_WARNING(g->Message);

				Changed = false;
			}	// endif Changed

			str = bsp->Filename;
		} else if (!(str = Serialize(g, (PBVAL)top, NULL, 0)))
			PUSH_WARNING(g->Message);

	} else if (!(str = Serialize(g, top, NULL, 0)))
		PUSH_WARNING(g->Message);

	return str;
} // end of MakeResult

/*********************************************************************************/
/*  Make the binary result according to the first argument type.                 */
/*********************************************************************************/
PBSON BJNX::MakeBinResult(UDF_ARGS *args, PBVAL top, ulong len, int n)
{
	char* filename = NULL;
	int   pretty = 2;
	PBSON bnp = NULL;

	if (IsArgJson(args, 0) == 3) {
		bnp = (PBSON)args->args[0];

		if (bnp->Top != (PJSON)top)
			bnp->Top = bnp->Jsp = (PJSON)top;

		return bnp;
	}	// endif 3

	if (IsArgJson(args, 0) == 2) {
		for (uint i = n; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT) {
				pretty = (int)*(longlong*)args->args[i];
				break;
			} // endif type

		filename = (char*)args->args[0];
	} // endif 2

	if ((bnp = BbinAlloc(G, len, top))) {
		bnp->Filename = filename;
		bnp->Pretty = pretty;
		strcpy(bnp->Msg, "Json Binary item");
	} //endif bnp

	return bnp;
} // end of MakeBinResult

/***********************************************************************/
/* Move a Json val block from one area to the current area.            */
/***********************************************************************/
PBVAL BJNX::MoveVal(PBVAL vlp)
{
	PBVAL nvp = NewVal(vlp->Type);

	nvp->Nd = vlp->Nd;
	return nvp;
}	// end of MovedVal

/***********************************************************************/
/* Move a Json tree from one area to current area.                     */
/***********************************************************************/
PBVAL BJNX::MoveJson(PBJNX bxp, PBVAL jvp)
{
	PBVAL res = NULL;

	if (jvp)
		switch (jvp->Type) {
			case TYPE_JAR:
				res = MoveArray(bxp, jvp);
				break;
			case TYPE_JOB:
				res = MoveObject(bxp, jvp);
				break;
			default:
				res = MoveValue(bxp, jvp);
				break;
		} // endswitch Type

	return res;
} // end of MoveJson

/***********************************************************************/
/* Move an array.                                                      */
/***********************************************************************/
PBVAL BJNX::MoveArray(PBJNX bxp, PBVAL jap)
{
	PBVAL vlp, vmp, jvp = NULL, jarp = MoveVal(jap);

	for (vlp = bxp->GetArray(jap); vlp; vlp = bxp->GetNext(vlp)) {
		vmp = MoveJson(bxp, vlp);

		if (jvp)
			jvp->Next = MOF(vmp);
		else
			jarp->To_Val = MOF(vmp);

		jvp = vmp;
	}	// endfor vlp

	return jarp;
} // end of MoveArray

/***********************************************************************/
/* Replace all object pointers by offsets.                             */
/***********************************************************************/
PBVAL BJNX::MoveObject(PBJNX bxp, PBVAL jop)
{
	PBPR   mpp, prp, ppp = NULL;
	PBVAL  vmp, jobp = MoveVal(jop);

	for (prp = bxp->GetObject(jop); prp; prp = bxp->GetNext(prp)) {
		vmp = MoveJson(bxp, GetVlp(prp));
		mpp = NewPair(DupStr(bxp->MZP(prp->Key)));
		SetPairValue(mpp, vmp);

		if (ppp)
			ppp->Vlp.Next = MOF(mpp);
		else
			jobp->To_Val = MOF(mpp);

		ppp = mpp;
	}	// endfor vlp

	return jobp;
} // end of MoffObject

/***********************************************************************/
/* Move a non json value.                                              */
/***********************************************************************/
PBVAL BJNX::MoveValue(PBJNX bxp, PBVAL jvp)
{
	double *dp;
	PBVAL   nvp = MoveVal(jvp);

	switch (jvp->Type) {
		case TYPE_STRG:
		case TYPE_DTM:
			nvp->To_Val = DupStr(bxp->MZP(jvp->To_Val));
			break;
		case TYPE_DBL:
			dp = (double*)BsonSubAlloc(sizeof(double));
			*dp = bxp->DBL(jvp->To_Val);
			nvp->To_Val = MOF(dp);
			break;
		case TYPE_JVAL:
			nvp->To_Val = MOF(MoveJson(bxp, bxp->MVP(jvp->To_Val)));
			break;
		default:
			nvp->To_Val = jvp->To_Val;
			break;
	}	// endswith Type

	return nvp;
} // end of MoveValue
	
/* -----------------------------Utility functions ------------------------------ */

/*********************************************************************************/
/*  Returns a pointer to the first integer argument found from the nth argument. */
/*********************************************************************************/
static int *GetIntArgPtr(PGLOBAL g, UDF_ARGS *args, uint& n)
{
	int *x = NULL;

	for (uint i = n; i < args->arg_count; i++)
		if (args->arg_type[i] == INT_RESULT) {
			if (args->args[i]) {
				if ((x = (int*)PlgDBSubAlloc(g, NULL, sizeof(int))))
					*x = (int)*(longlong*)args->args[i];
				else
					PUSH_WARNING(g->Message);

			} // endif args

			n = i + 1;
			break;
		}	// endif arg_type

	return x;
} // end of GetIntArgPtr

/*********************************************************************************/
/*  Returns not 0 if the argument is a JSON item or file name.                   */
/*********************************************************************************/
int IsArgJson(UDF_ARGS *args, uint i)
{
	char *pat = args->attributes[i];
	int   n = 0;

	if (*pat == '@') {
		pat++;

		if (*pat == '\'' || *pat == '"')
			pat++;

	} // endif pat

	if (i >= args->arg_count || args->arg_type[i] != STRING_RESULT) {
	} else if (!strnicmp(pat, "Bson_", 5) || !strnicmp(pat, "Json_", 5)) {
		if (!args->args[i] || strchr("[{ \t\r\n", *args->args[i]))
			n = 1;					 // arg should be is a json item
//	else
//		n = 2;           // A file name may have been returned

	} else if (!strnicmp(pat, "Bbin_", 5)) {
		if (args->lengths[i] == sizeof(BSON))
			n = 3;					 //	arg is a binary json item
//	else
//		n = 2;           // A file name may have been returned

	} else if (!strnicmp(pat, "Bfile_", 6) || !strnicmp(pat, "Jfile_", 6)) {
		n = 2;					   //	arg is a json file name
#if 0
	} else if (args->lengths[i]) {
		PGLOBAL g = PlugInit(NULL, (size_t)args->lengths[i] * M + 1024);
		char   *sap = MakePSZ(g, args, i);

		if (ParseJson(g, sap, strlen(sap)))
			n = 4;

		JsonFreeMem(g);
#endif // 0
	}	// endif's

	return n;
} // end of IsArgJson

/*********************************************************************************/
/*  GetFileLength: returns file size in number of bytes.                         */
/*********************************************************************************/
static long GetFileLength(char *fn)
{
	int  h;
	long len;

	h= open(fn, _O_RDONLY);

	if (h != -1) {
		if ((len = _filelength(h)) < 0)
			len = 0;

		close(h);
	} else
		len = 0;

	return len;
} // end of GetFileLength

/* ------------------------- Now the new Bin UDF's ----------------------------- */

/*********************************************************************************/
/*  Make a Json value containing the parameter.                                  */
/*********************************************************************************/
my_bool bsonvalue_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
{
	unsigned long reslen, memlen;

	if (args->arg_count > 1) {
		strcpy(message, "Cannot accept more than 1 argument");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bsonvalue_init

char* bsonvalue(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char*, char*)
{
	char   *str;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, 1, false)) {
			BJNX  bnx(g);
			PBVAL bvp = bnx.MakeValue(args, 0, true);

			if (!(str = bnx.Serialize(g, bvp, NULL, 0)))
				str = strcpy(result, g->Message);

		} else
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bsonValue

void bsonvalue_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsonvalue_deinit

/*********************************************************************************/
/*  Make a Json array containing all the parameters.                             */
/*  Note: jvp must be set before arp because it can be a binary argument.        */
/*********************************************************************************/
my_bool bson_make_array_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
{
	unsigned long reslen, memlen;

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bson_make_array_init

char* bson_make_array(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char*, char*)
{
	char* str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			BJNX  bnx(g);
			PBVAL jvp = bnx.MakeValue(args, 0);
			PBVAL arp = bnx.NewVal(TYPE_JAR);

			for (uint i = 0; i < args->arg_count;) {
				bnx.AddArrayValue(arp, jvp);
				jvp = bnx.MakeValue(args, ++i);
			} // endfor i

			if (!(str = bnx.Serialize(g, arp, NULL, 0)))
				str = strcpy(result, g->Message);

		} else
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bson_make_array

void bson_make_array_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_make_array_deinit

/*********************************************************************************/
/*  Add one or several values to a Bson array.                                   */
/*********************************************************************************/
my_bool bson_array_add_values_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
		//} else if (!IsArgJson(args, 0, true)) {
		//	strcpy(message, "First argument must be a valid json string or item");
		//	return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_array_add_values_init

char* bson_array_add_values(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char*) {
	char* str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			BJNX  bnx(g);
			PBVAL arp = bnx.MakeValue(args, 0, true);

			if (arp->Type != TYPE_JAR) {
				PUSH_WARNING("First argument is not an array");
				goto fin;
			} // endif arp

			for (uint  i = 1; i < args->arg_count; i++)
				bnx.AddArrayValue(arp, bnx.MakeValue(args, i));

			bnx.SetChanged(true);
			str = bnx.MakeResult(args, arp, INT_MAX);
		} // endif CheckMemory

		if (!str) {
			PUSH_WARNING(g->Message);
			str = args->args[0];
		}	// endif str

		// Keep result of constant function
		g->Xchk = (g->N) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	fin:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_array_add_values

void bson_array_add_values_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_array_add_values_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool bson_array_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
		//} else if (!IsArgJson(args, 0, true)) {
		//	strcpy(message, "First argument is not a valid Json item");
		//	return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_array_add_init

char *bson_array_add(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		int  *x;
		uint	n = 2;
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL jsp, top;
		PBVAL arp, jvp = bnx.MakeValue(args, 0, true, &top);

		jsp = jvp;
		x = GetIntArgPtr(g, args, n);

		if (bnx.CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp) {
			if (jvp->Type != TYPE_JAR) {
				if ((arp = bnx.NewVal(TYPE_JAR))) {
					bnx.AddArrayValue(arp, jvp);

					if (!top)
						top = arp;

				}	// endif arp

			} else
				arp = jvp;

			if (arp) {
				bnx.AddArrayValue(arp, bnx.MakeValue(args, 1), x);
				bnx.SetChanged(true);
				str = bnx.MakeResult(args, top, n);
			}	else
				PUSH_WARNING(g->Message);

		} else {
			PUSH_WARNING("Target is not an array");
			//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

		// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
		*error = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_array_add

void bson_array_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_array_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool bson_array_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_array_delete_init

char *bson_array_delete(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, false, true)) {
		int  *x;
		uint	n = 1;
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL arp, top;
		PBVAL jvp = bnx.MakeValue(args, 0, true, &top);

		if (!(x = GetIntArgPtr(g, args, n)))
			PUSH_WARNING("Missing or null array index");
		else if (bnx.CheckPath(g, args, jvp, arp, 1))
			PUSH_WARNING(g->Message);
		else if (arp && arp->Type == TYPE_JAR) {
			bnx.DeleteValue(arp, *x);
			bnx.SetChanged(true);
			str = bnx.MakeResult(args, top, n);
		} else {
			PUSH_WARNING("First argument target is not an array");
			//		if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

		// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_array_delete

void bson_array_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_array_delete_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the parameters.                            */
/*********************************************************************************/
my_bool bson_make_object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bson_make_object_init

char *bson_make_object(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, false, true)) {
			BJNX  bnx(g);
			PBVAL objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i++)
					bnx.SetKeyValue(objp, bnx.MakeValue(args, i), bnx.MakeKey(args, i));

				str = bnx.Serialize(g, objp, NULL, 0);
			}	// endif objp

		} // endif CheckMemory

		if (!str)
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bson_make_object

void bson_make_object_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_make_object_deinit

/*********************************************************************************/
/*  Make a Json Object containing all not null parameters.                       */
/*********************************************************************************/
my_bool bson_object_nonull_init(UDF_INIT *initid, UDF_ARGS *args,
	char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bson_object_nonull_init

char *bson_object_nonull(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			BJNX  bnx(g);
			PBVAL jvp, objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i++)
					if (!bnx.IsValueNull(jvp = bnx.MakeValue(args, i)))
						bnx.SetKeyValue(objp, jvp, bnx.MakeKey(args, i));

				str = bnx.Serialize(g, objp, NULL, 0);
			}	// endif objp

		} // endif CheckMemory

		if (!str)
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bson_object_nonull

void bson_object_nonull_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_nonull_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the key/value parameters.                  */
/*********************************************************************************/
my_bool bson_object_key_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count % 2) {
		strcpy(message, "This function must have an even number of arguments");
		return true;
	} // endif arg_count

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bson_object_key_init

char *bson_object_key(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			BJNX  bnx(g);
			PBVAL objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i += 2)
					bnx.SetKeyValue(objp, bnx.MakeValue(args, i + 1), MakePSZ(g, args, i));

				str = bnx.Serialize(g, objp, NULL, 0);
			}	// endif objp

		} // endif CheckMemory

		if (!str)
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bson_object_key

void bson_object_key_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_key_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool bson_object_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!IsArgJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_object_add_init

char *bson_object_add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PSZ     key;
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, true, true)) {
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL jvp, objp;
		PBVAL jsp, top;

		jsp = bnx.MakeValue(args, 0, true, &top);

		if (bnx.CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->Type == TYPE_JOB) {
			objp = jvp;
			jvp = bnx.MakeValue(args, 1);
			key = bnx.MakeKey(args, 1);
			bnx.SetKeyValue(objp, jvp, key);
			bnx.SetChanged(true);
			str = bnx.MakeResult(args, top);
		} else {
			PUSH_WARNING("First argument target is not an object");
			//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

		// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_object_add

void bson_object_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool bson_object_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have 2 or 3 arguments");
		return true;
	} else if (!IsArgJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument must be a key string");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_object_delete_init

char *bson_object_delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, true, true)) {
		bool  chg;
		BJNX  bnx(g, NULL, TYPE_STRG);
		PSZ   key;
		PBVAL jsp, objp, top;
		PBVAL jvp = bnx.MakeValue(args, 0, false, &top);

		jsp = jvp;

		if (bnx.CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->Type == TYPE_JOB) {
			key = bnx.MakeKey(args, 1);
			objp = jvp;
			chg = bnx.DeleteKey(objp, key);
			bnx.SetChanged(chg);
			str = bnx.MakeResult(args, top);
		} else {
			PUSH_WARNING("First argument target is not an object");
			//		if (g->Mrr) *error = 1;					(only if no path)
		} // endif jvp

	} // endif CheckMemory

		// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_object_delete

void bson_object_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool bson_object_list_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "This function must have 1 argument");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bson_object_list_init

char *bson_object_list(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->N) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			BJNX  bnx(g);
			PBVAL jarp;
			PBVAL jsp = bnx.MakeValue(args, 0, true);

			if (jsp->Type == TYPE_JOB) {
				jarp = bnx.GetKeyList(jsp);

				if (!(str = bnx.Serialize(g, jarp, NULL, 0)))
					PUSH_WARNING(g->Message);

			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jvp

		} // endif CheckMemory

		if (initid->const_item) {
			// Keep result of constant function
			g->Xchk = str;
			g->N = 1;			// str can be NULL
		} // endif const_item

	} else
		str = (char*)g->Xchk;

	if (!str) {
		*is_null = 1;
		*res_length = 0;
	}	else
		*res_length = strlen(str);

	return str;
} // end of bson_object_list

void bson_object_list_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_list_deinit

/*********************************************************************************/
/*  Returns an array of the Json object values.                                  */
/*********************************************************************************/
my_bool bson_object_values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "This function must have 1 argument");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Argument must be a json object");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bson_object_values_init

char *bson_object_values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->N) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			BJNX  bnx(g);
			char *p;
			PBVAL jsp, jarp;
			PBVAL jvp = bnx.MakeValue(args, 0);

			if ((p = bnx.GetString(jvp))) {
				if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					return NULL;
				} // endif jsp

			} else
				jsp = jvp;

			if (jsp->Type == TYPE_JOB) {
				jarp = bnx.GetObjectValList(jsp);

				if (!(str = bnx.Serialize(g, jarp, NULL, 0)))
					PUSH_WARNING(g->Message);

			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jvp

		} // endif CheckMemory

		if (initid->const_item) {
			// Keep result of constant function
			g->Xchk = str;
			g->N = 1;			// str can be NULL
		} // endif const_item

	} else
		str = (char*)g->Xchk;

	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_object_values

void bson_object_values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_values_deinit

/*********************************************************************************/
/*  Set the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool bsonset_def_prec_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT) {
		strcpy(message, "This function must have 1 integer argument");
		return true;
	} else
		return false;

} // end of bsonset_def_prec_init

long long bsonset_def_prec(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	long long n = *(long long*)args->args[0];

	JsonDefPrec = (int)n;
	return (long long)GetJsonDefPrec();
} // end of bsonset_def_prec

/*********************************************************************************/
/*  Get the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool bsonget_def_prec_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 0) {
		strcpy(message, "This function must have no arguments");
		return true;
	} else
		return false;

} // end of bsonget_def_prec_init

long long bsonget_def_prec(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	return (long long)GetJsonDefPrec();
} // end of bsonget_def_prec

/*********************************************************************************/
/*  Set the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool bsonset_grp_size_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT) {
		strcpy(message, "This function must have 1 integer argument");
		return true;
	} else
		return false;

} // end of bsonset_grp_size_init

long long bsonset_grp_size(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	long long n = *(long long*)args->args[0];

	JsonGrpSize = (uint)n;
	return (long long)GetJsonGroupSize();
} // end of bsonset_grp_size

/*********************************************************************************/
/*  Get the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool bsonget_grp_size_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 0) {
		strcpy(message, "This function must have no arguments");
		return true;
	} else
		return false;

} // end of bsonget_grp_size_init

long long bsonget_grp_size(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	return (long long)GetJsonGroupSize();
} // end of bsonget_grp_size

/*********************************************************************************/
/*  Make a Json array from values coming from rows.                              */
/*********************************************************************************/
my_bool bson_array_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, n = GetJsonGroupSize();

	if (args->arg_count != 1) {
		strcpy(message, "This function can only accept 1 argument");
		return true;
	} else if (IsArgJson(args, 0) == 3) {
		strcpy(message, "This function does not support Jbin arguments");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	reslen *= n;
	memlen += ((memlen - MEMFIX) * (n - 1));

	if (JsonInit(initid, args, message, false, reslen, memlen))
		return true;

	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = new(g) BJNX(g);

	JsonMemSave(g);
	return false;
} // end of bson_array_grp_init

void bson_array_grp_clear(UDF_INIT *initid, char*, char*)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));

	JsonSubSet(g);
	g->Activityp = (PACTIVITY)bxp->NewVal(TYPE_JAR);
	g->N = GetJsonGroupSize();
} // end of bson_array_grp_clear

void bson_array_grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));
	PBVAL   arp = (PBVAL)g->Activityp;

	if (arp && g->N-- > 0)
		bxp->AddArrayValue(arp, bxp->MakeValue(args, 0));

} // end of bson_array_grp_add

char *bson_array_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
	unsigned long *res_length, char *, char *)
{
	char   *str;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));
	PBVAL   arp = (PBVAL)g->Activityp;

	if (g->N < 0)
		PUSH_WARNING("Result truncated to json_grp_size values");

	if (!arp || !(str = bxp->Serialize(g, arp, NULL, 0)))
		str = strcpy(result, g->Message);

	*res_length = strlen(str);
	return str;
} // end of bson_array_grp

void bson_array_grp_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_array_grp_deinit

/*********************************************************************************/
/*  Make a Json object from values coming from rows.                             */
/*********************************************************************************/
my_bool bson_object_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, n = GetJsonGroupSize();

	if (args->arg_count != 2) {
		strcpy(message, "This function requires 2 arguments (key, value)");
		return true;
	} else if (IsArgJson(args, 0) == 3) {
		strcpy(message, "This function does not support Jbin arguments");
		return true;
	} else
		CalcLen(args, true, reslen, memlen);

	reslen *= n;
	memlen += ((memlen - MEMFIX) * (n - 1));

	if (JsonInit(initid, args, message, false, reslen, memlen))
		return true;

	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = new(g) BJNX(g);

	JsonMemSave(g);
	return false;
} // end of bson_object_grp_init

void bson_object_grp_clear(UDF_INIT *initid, char*, char*)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));

	JsonSubSet(g);
	g->Activityp = (PACTIVITY)bxp->NewVal(TYPE_JOB);
	g->N = GetJsonGroupSize();
} // end of bson_object_grp_clear

void bson_object_grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));
	PBVAL   bop = (PBVAL)g->Activityp;

	if (g->N-- > 0)
		bxp->SetKeyValue(bop, bxp->MakeValue(args, 1), MakePSZ(g, args, 0));

} // end of bson_object_grp_add

char *bson_object_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
	unsigned long *res_length, char *, char *)
{
	char   *str;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBJNX   bxp = (PBJNX)((char*)g->Sarea + sizeof(POOLHEADER));
	PBVAL   bop = (PBVAL)g->Activityp;

	if (g->N < 0)
		PUSH_WARNING("Result truncated to json_grp_size values");

	if (!bop || !(str = bxp->Serialize(g, bop, NULL, 0)))
		str = strcpy(result, g->Message);

	*res_length = strlen(str);
	return str;
} // end of bson_object_grp

void bson_object_grp_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_object_grp_deinit

/*********************************************************************************/
/*  Test BJSON parse and serialize.                                              */
/*********************************************************************************/
my_bool bson_test_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count == 0) {
		strcpy(message, "At least 1 argument required (json)");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bson_test_init

char* bson_test(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char* str = NULL, * sap = NULL, * fn = NULL;
	int     pretty = 1;
	PBVAL   bvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Activityp;
		goto err;
	} else if (initid->const_item)
		g->N = 1;

	try {
		BJNX  bnx(g);

		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, !g->Xchk)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else		// Sarea may have been reallocated
				bnx.Reset();

			bvp = bnx.MakeValue(args, 0, true);

			if (bvp->Type == TYPE_NULL) {
				PUSH_WARNING(g->Message);
				goto err;
			}	// endif bvp

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = bvp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			bvp = (PBVAL)g->Xchk;

		for (uint i = 1; i < args->arg_count; i++)
			if (args->arg_type[i] == STRING_RESULT)
				fn = args->args[i];
			else if (args->arg_type[i] == INT_RESULT)
				pretty = (int)*(longlong*)args->args[i];

		// Serialize the parse tree
		str = bnx.Serialize(g, bvp, fn, pretty);

		if (initid->const_item)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)str;

	} catch (int n) {
		xtrc(1, "json_test_bson: error %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
		*error = 1;
		str = NULL;
	} catch (const char* msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		*error = 1;
		str = NULL;
	} // end catch

err:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_test

void bson_test_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_test_deinit

/*********************************************************************************/
/*  Locate a value in a Json tree.                                               */
/*********************************************************************************/
my_bool bsonlocate_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (rank)");
		return true;
	} // endifs args

	CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	if (IsArgJson(args, 0) == 3)
		more = 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsonlocate_init

char* bsonlocate(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char   *path = NULL;
	int     k;
	PBVAL   bvp, bvp2;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (g->Activityp) {
			path = (char*)g->Activityp;
			*res_length = strlen(path);
			return path;
		} else {
			*res_length = 0;
			*is_null = 1;
			return NULL;
		}	// endif Activityp

	} else if (initid->const_item)
		g->N = 1;

	try {
		BJNX  bnx(g);

		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, !g->Xchk)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else {
				bnx.Reset();		// Sarea may have been re-allocated
				bvp = bnx.MakeValue(args, 0, true);

				if (!bvp) {
					bnx.GetMsg(g);
					PUSH_WARNING(g->Message);
					goto err;
				} else if (bvp->Type == TYPE_NULL) {
					PUSH_WARNING("First argument is not a valid JSON item");
					goto err;
				}	// endif bvp

				if (g->Mrr) {			 // First argument is a constant
					g->Xchk = bvp;
					JsonMemSave(g);
				} // endif Mrr

			} // endif CheckMemory

		} else
			bvp = (PBVAL)g->Xchk;

		// The item to locate
		bvp2 = bnx.MakeValue(args, 1, true);

		if (bvp2->Type == TYPE_NULL) {
			PUSH_WARNING("Invalid second argument");
			goto err;
		}	// endif bvp

		k = (args->arg_count > 2) ? (int)*(long long*)args->args[2] : 1;

//	bnxp = new(g) BJNX(g, bvp, TYPE_STRING);
		path = bnx.Locate(g, bvp, bvp2, k);

		if (initid->const_item)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)path;

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} catch (const char* msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} // end catch

err:
	if (!path) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(path);

	return path;
} // end of bsonlocate

void bsonlocate_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsonlocate_deinit

/*********************************************************************************/
/*  Locate all occurences of a value in a Json tree.                             */
/*********************************************************************************/
my_bool bson_locate_all_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (Depth)");
		return true;
	} // endifs

	CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	if (IsArgJson(args, 0) == 3)
		more = 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bson_locate_all_init

char* bson_locate_all(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char* path = NULL;
	int     mx = 10;
	PBVAL   bvp, bvp2;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (g->Activityp) {
			path = (char*)g->Activityp;
			*res_length = strlen(path);
			return path;
		} else {
			*error = 1;
			*res_length = 0;
			*is_null = 1;
			return NULL;
		}	// endif Activityp

	} else if (initid->const_item)
		g->N = 1;

	try {
		BJNX  bnx(g);

		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else
				bnx.Reset();

			bvp = bnx.MakeValue(args, 0, true);

			if (bvp->Type == TYPE_NULL) {
				PUSH_WARNING("First argument is not a valid JSON item");
				goto err;
			}	// endif bvp

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = bvp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			bvp = (PBVAL)g->Xchk;

		// The item to locate
		bvp2 = bnx.MakeValue(args, 1, true);

		if (bvp2->Type == TYPE_NULL) {
			PUSH_WARNING("Invalid second argument");
			goto err;
		}	// endif bvp

		if (args->arg_count > 2)
			mx = (int)*(long long*)args->args[2];

//	bnxp = new(g) BJNX(g, bvp, TYPE_STRING);
		path = bnx.LocateAll(g, bvp, bvp2, mx);

		if (initid->const_item)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)path;

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} catch (const char* msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} // end catch

err:
	if (!path) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(path);

	return path;
} // end of bson_locate_all

void bson_locate_all_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_locate_all_deinit

/*********************************************************************************/
/*  Check whether the document contains a value or item.                         */
/*********************************************************************************/
my_bool bson_contains_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsArgJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (index)");
		return true;
	} else if (args->arg_count > 3) {
		if (args->arg_type[3] == INT_RESULT && args->args[3])
			more += (unsigned long)*(long long*)args->args[3];
		else
			strcpy(message, "Fourth argument is not an integer (memory)");

	}	// endif's

	CalcLen(args, false, reslen, memlen);
	//memlen += more;

	// TODO: calculate this
	more += (IsArgJson(args, 0) != 3 ? 1000 : 0);

	return JsonInit(initid, args, message, false, reslen, memlen, more);
} // end of bson contains_init

long long bson_contains(UDF_INIT *initid, UDF_ARGS *args, char *, char *error)
{
	char          isn, res[256];
	unsigned long reslen;

	isn = 0;
	bsonlocate(initid, args, res, &reslen, &isn, error);
	return (isn) ? 0LL : 1LL;
} // end of bson_contains

void bson_contains_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_contains_deinit

/*********************************************************************************/
/*  Check whether the document contains a path.                                  */
/*********************************************************************************/
my_bool bsoncontains_path_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsArgJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (path)");
		return true;
	} else if (args->arg_count > 2) {
		if (args->arg_type[2] == INT_RESULT && args->args[2])
			more += (unsigned long)*(long long*)args->args[2];
		else
			strcpy(message, "Third argument is not an integer (memory)");

	}	// endif's

	CalcLen(args, false, reslen, memlen);
	//memlen += more;

	// TODO: calculate this
	more += (IsArgJson(args, 0) != 3 ? 1000 : 0);

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsoncontains_path_init

long long bsoncontains_path(UDF_INIT *initid, UDF_ARGS *args, char *, char *error)
{
	char   *p, *path;
	long long n;
	PBVAL   jsp;
	PBVAL   jvp;
	PBJNX		bxp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (!g->Activityp) {
			return 0LL;
		} else
			return *(long long*)g->Activityp;

	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			goto err;
		} else {
			BJNX bnx(g);

			jvp = bnx.MakeValue(args, 0);

			if ((p = bnx.GetString(jvp))) {
				if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					goto err;
				} // endif jsp

			} else
				jsp = jvp;

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		}	// endelse CheckMemory

	} else
		jsp = (PBVAL)g->Xchk;

	bxp = new(g) BJNX(g, jsp, TYPE_BIGINT);
	path = MakePSZ(g, args, 1);

	if (bxp->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		goto err;
	} // endif SetJpath

	n = (bxp->CheckPath(g)) ? 1LL : 0LL;

	if (initid->const_item) {
		// Keep result of constant function
		long long *np = (long long*)PlgDBSubAlloc(g, NULL, sizeof(long long));

		if (np) {
			*np = n;
			g->Activityp = (PACTIVITY)np;
		}	else
			PUSH_WARNING(g->Message);

	} // endif const_item

	return n;

err:
	if (g->Mrr) *error = 1;
	return 0LL;
} // end of bsoncontains_path

void bsoncontains_path_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsoncontains_path_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool bson_item_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else for (int i = 0; i < 2; i++)
		if (!IsArgJson(args, i) && args->arg_type[i] != STRING_RESULT) {
			sprintf(message, "Argument %d must be a json item", i);
			return true;
		}	// endif type

	CalcLen(args, false, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_item_merge_init

char *bson_item_merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		JTYP  type;
		BJNX  bnx(g);
		PBVAL jvp, top = NULL;
		PBVAL jsp[2] = {NULL, NULL};

		for (int i = 0; i < 2; i++) {
			jvp = bnx.MakeValue(args, i, true);

			if (i) {
				if (jvp->Type != type) {
					PUSH_WARNING("Argument types mismatch");
					goto fin;
				}	// endif type

			} else {
				type = (JTYP)jvp->Type;

				if (type != TYPE_JAR && type != TYPE_JOB) {
					PUSH_WARNING("First argument is not an array or object");
					goto fin;
				} else
					top = jvp;

			}	// endif i

			jsp[i] = jvp;
		} // endfor i

		if (type == TYPE_JAR)
			bnx.MergeArray(jsp[0], jsp[1]);
		else
			bnx.MergeObject(jsp[0], jsp[1]);

		bnx.SetChanged(true);
		str = bnx.MakeResult(args, top);
	} // endif CheckMemory

	// In case of error or file, return unchanged first argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*res_length = 0;
		*error = 1;
		*is_null = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_item_merge

void bson_item_merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_item_merge_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool bson_get_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more;
	int n = IsArgJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (jpath)");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (n == 2 && args->args[0]) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		more = fl * 3;
	} else if (n != 3) {
		more = args->lengths[0] * 3;
	} else
		more = 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bson_get_item_init

char *bson_get_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *path, *str = NULL;
	PBVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	BJNX    bnx(g, NULL, TYPE_STRING, initid->max_length);

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true, true)) {
			PUSH_WARNING("CheckMemory error");
			goto fin;
		} else {
			bnx.Reset();
			jvp = bnx.MakeValue(args, 0, true);

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jvp;
				JsonMemSave(g);
			} // endif Mrr

		}	// endelse CheckMemory

	} else
		jvp = (PBVAL)g->Xchk;

	path = MakePSZ(g, args, 1);

	if (bnx.SetJpath(g, path, true)) {
		goto fin;
	}	else
		jvp = bnx.GetRowValue(g, jvp, 0);

	if (!bnx.IsJson(jvp)) {
		strcpy(g->Message, "Not a Json item");
	}	else
		str = bnx.Serialize(g, jvp, NULL, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)str;

fin:
	if (!str) {
		PUSH_WARNING(g->Message);
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_get_item

void bson_get_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_get_item_deinit

/*********************************************************************************/
/*  Get a string value from a Json item.                                         */
/*********************************************************************************/
my_bool bsonget_string_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsArgJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (jpath)");
		return true;
	} else if (args->arg_count > 2) {
		if (args->arg_type[2] == INT_RESULT && args->args[2])
			more += (unsigned long)*(long long*)args->args[2];
		else
			strcpy(message, "Third argument is not an integer (memory)");

	}	// endif's

	CalcLen(args, false, reslen, memlen);
	//memlen += more;

	if (n == 2 && args->args[0]) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		more += fl * 3;
	} else if (n != 3)
		more += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsonget_string_init

char *bsonget_string(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *p, *path, *str = NULL;
	PBVAL   jsp, jvp;
	PBJNX   bxp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Activityp;
		goto err;
	} else if (initid->const_item)
		g->N = 1;

	try {
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true)) {
				PUSH_WARNING("CheckMemory error");
				goto err;
			} else {
				BJNX bnx(g);

				jvp = bnx.MakeValue(args, 0);

				if ((p = bnx.GetString(jvp))) {
					if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
						PUSH_WARNING(g->Message);
						goto err;
					} // endif jsp

				} else
					jsp = jvp;

				if (g->Mrr) {			 // First argument is a constant
					g->Xchk = jsp;
					JsonMemSave(g);
				} // endif Mrr

			}	// endelse CheckMemory

		} else
			jsp = (PBVAL)g->Xchk;

		path = MakePSZ(g, args, 1);
		bxp = new(g) BJNX(g, jsp, TYPE_STRING, initid->max_length);

		if (bxp->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			goto err;
		}	else
			bxp->ReadValue(g);

		if (!bxp->GetValue()->IsNull())
			str = bxp->GetValue()->GetCharValue();

		if (initid->const_item)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)str;

	} catch (int n) {
		if (trace(1))
			htrc("Exception %d: %s\n", n, g->Message);

		PUSH_WARNING(g->Message);
		str = NULL;
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		str = NULL;
	} // end catch

err:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bsonget_string

void bsonget_string_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsonget_string_deinit

/*********************************************************************************/
/*  Get an integer value from a Json item.                                       */
/*********************************************************************************/
my_bool bsonget_int_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more;

	if (args->arg_count != 2) {
		strcpy(message, "This function must have 2 arguments");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	more = (IsArgJson(args, 0) != 3) ? 1000 : 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsonget_int_init

long long bsonget_int(UDF_INIT *initid, UDF_ARGS *args,
	char *is_null, char *error)
{
	char   *p, *path;
	long long n;
	PBVAL   jsp, jvp;
	PBJNX   bxp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (!g->Activityp) {
			*is_null = 1;
			return 0LL;
		} else
			return *(long long*)g->Activityp;

	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			if (g->Mrr) *error = 1;
			*is_null = 1;
			return 0LL;
		} else {
			BJNX bnx(g);

			jvp = bnx.MakeValue(args, 0);

			if ((p = bnx.GetString(jvp))) {
				if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					if (g->Mrr) *error = 1;
					*is_null = 1;
					return 0;
				} // endif jsp

			} else
				jsp = jvp;

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		}	// endelse CheckMemory

	} else
		jsp = (PBVAL)g->Xchk;

	path = MakePSZ(g, args, 1);
	bxp = new(g) BJNX(g, jsp, TYPE_BIGINT);

	if (bxp->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		*is_null = 1;
		return 0;
	} else
		bxp->ReadValue(g);

	if (bxp->GetValue()->IsNull()) {
		*is_null = 1;
		return 0;
	}	// endif IsNull

	n = bxp->GetValue()->GetBigintValue();

	if (initid->const_item) {
		// Keep result of constant function
		long long *np = (long long*)PlgDBSubAlloc(g, NULL, sizeof(long long));

		if (np) {
			*np = n;
			g->Activityp = (PACTIVITY)np;
		}	else
			PUSH_WARNING(g->Message);

	} // endif const_item

	return n;
} // end of bsonget_int

void bsonget_int_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsonget_int_deinit

/*********************************************************************************/
/*  Get a double value from a Json item.                                         */
/*********************************************************************************/
my_bool bsonget_real_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more;

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else if (args->arg_count > 2) {
		if (args->arg_type[2] != INT_RESULT) {
			strcpy(message, "Third argument is not an integer (decimals)");
			return true;
		} else
			initid->decimals = (uint)*(longlong*)args->args[2];

	} else
		initid->decimals = 15;

	CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	more = (IsArgJson(args, 0) != 3) ? 1000 : 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsonget_real_init

double bsonget_real(UDF_INIT *initid, UDF_ARGS *args,
	char *is_null, char *error)
{
	char   *p, *path;
	double  d;
	PBVAL   jsp, jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	BJNX    bnx(g);

	if (g->N) {
		if (!g->Activityp) {
			*is_null = 1;
			return 0.0;
		} else
			return *(double*)g->Activityp;

	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			if (g->Mrr) *error = 1;
			*is_null = 1;
			return 0.0;
		} else {
			bnx.Reset();
			jvp = bnx.MakeValue(args, 0);

			if ((p = bnx.GetString(jvp))) {
				if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					*is_null = 1;
					return 0.0;
				} // endif jsp

			} else
				jsp = jvp;

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr
		}	// endelse CheckMemory

	} else
		jsp = (PBVAL)g->Xchk;

	path = MakePSZ(g, args, 1);
//bxp = new(g) BJNX(g, jsp, TYPE_DOUBLE, 32, jsp->Nd);

	if (bnx.SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		*is_null = 1;
		return 0.0;
	}	else
		jvp = bnx.GetRowValue(g, jsp, 0);

	if (!jvp || bnx.IsValueNull(jvp)) {
		*is_null = 1;
		return 0.0;
	}	else if (args->arg_count == 2) {
		d = atof(bnx.GetString(jvp));
	} else
		d = bnx.GetDouble(jvp);

	if (initid->const_item) {
		// Keep result of constant function
		double *dp;

		if ((dp = (double*)PlgDBSubAlloc(g, NULL, sizeof(double)))) {
			*dp = d;
			g->Activityp = (PACTIVITY)dp;
		} else {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return 0.0;
		}	// endif dp

	} // endif const_item

	return d;
} // end of jsonget_real

void bsonget_real_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bsonget_real_deinit

/*********************************************************************************/
/*  Delete items from a Json document.                                           */
/*********************************************************************************/
my_bool bson_delete_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		if (IsArgJson(args, 0) != 3) {
			strcpy(message, "This function must have at least 2 arguments or one binary");
			return true;
		} // endif args

	} // endif count

	CalcLen(args, false, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// Is this a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bson_delete_item_init

char *bson_delete_item(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	char   *path, *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, false, true)) {
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL top, jar = NULL;
		PBVAL jvp = bnx.MakeValue(args, 0, true, &top);

		if (args->arg_count == 1) {
			// This	should be coming from bbin_locate_all
			jar = jvp;		 // This is the array of paths
			jvp = top;		 // And this is the document
		}	else if(!bnx.IsJson(jvp)) {
			PUSH_WARNING("First argument is not a JSON document");
			goto fin;
		}	else if (args->arg_count == 2) {
			// Check whether this is an array of paths 
			jar = bnx.MakeValue(args, 1, true);

			if (jar && jar->Type != TYPE_JAR)
				jar = NULL;

		}	// endif arg_count

		if (jar) {
			// Do the deletion in reverse order
			for(int i = bnx.GetArraySize(jar) - 1;	i >= 0; i--) {
				path = bnx.GetString(bnx.GetArrayValue(jar, i));

				if (bnx.SetJpath(g, path, false)) {
					PUSH_WARNING(g->Message);
					continue;
				}	// endif SetJpath

				bnx.SetChanged(bnx.DeleteItem(g, jvp));
			}	// endfor i

		}	else for (uint i = 1; i < args->arg_count; i++) {
			path = MakePSZ(g, args, i);

			if (bnx.SetJpath(g, path, false)) {
				PUSH_WARNING(g->Message);
				continue;
			}	// endif SetJpath

			bnx.SetChanged(bnx.DeleteItem(g, jvp));
		} // endfor i

		str = bnx.MakeResult(args, top, INT_MAX);
	} // endif CheckMemory

	if (g->N)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_delete_item

void bson_delete_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_delete_item_deinit

/*********************************************************************************/
/*  This function is used by the json_set/insert/update_item functions.          */
/*********************************************************************************/
static char *bson_handle_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *path, *str = NULL;
	int     w;
	my_bool b = true;
	PBJNX   bxp;
	PBVAL   jsp, jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Alchecked) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (g->N)
		g->Alchecked = 1;

	if (!strcmp(result, "$set"))
		w = 0;
	else if (!strcmp(result, "$insert"))
		w = 1;
	else if (!strcmp(result, "$update"))
		w = 2;
	else {
		PUSH_WARNING("Logical error, please contact CONNECT developer");
		goto fin;
	}	// endelse

	try {
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true, false, true)) {
				PUSH_WARNING("CheckMemory error");
				throw 1;
			} else {
				BJNX bnx(g);

				jsp = bnx.MakeValue(args, 0, true);

				if (g->Mrr) {			 // First argument is a constant
					g->Xchk = jsp;
					JsonMemSave(g);
				} // endif Mrr

			}	// endif CheckMemory

		} else
			jsp = (PBVAL)g->Xchk;

		bxp = new(g)BJNX(g, jsp, TYPE_STRING, initid->max_length, 0, true);

		for (uint i = 1; i + 1 < args->arg_count; i += 2) {
			jvp = bxp->MakeValue(args, i);
			path = MakePSZ(g, args, i + 1);

			if (bxp->SetJpath(g, path, false)) {
				PUSH_WARNING(g->Message);
				continue;
			}	// endif SetJpath

			if (w) {
				bxp->ReadValue(g);
				b = bxp->GetValue()->IsNull();
				b = (w == 1) ? b : !b;
			}	// endif w

			if (b && bxp->WriteValue(g, jvp)) {
				PUSH_WARNING(g->Message);
				continue;
			}	// endif SetJpath

			bxp->SetChanged(true);
		} // endfor i

		// In case of error or file, return unchanged argument
		if (!(str = bxp->MakeResult(args, jsp, INT_MAX32)))
			str = MakePSZ(g, args, 0);

		if (g->N)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)str;

	} catch (int n) {
		if (trace(1))
			htrc("Exception %d: %s\n", n, g->Message);

		PUSH_WARNING(g->Message);
		str = NULL;
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		str = NULL;
	} // end catch

fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of bson_handle_item

/*********************************************************************************/
/*  Set Json items of a Json document according to path.                         */
/*********************************************************************************/
my_bool bson_set_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 0;
	int n = IsArgJson(args, 0);

	if (!(args->arg_count % 2)) {
		strcpy(message, "This function must have an odd number of arguments");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (n == 2 && args->args[0]) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		more += fl * 3;
	} else if (n != 3)
		more += args->lengths[0] * 3;

	if (!JsonInit(initid, args, message, true, reslen, memlen, more)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		g->Alchecked = 0;
		return false;
	} else
		return true;

} // end of bson_set_item_init

char *bson_set_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$set");
	return bson_handle_item(initid, args, result, res_length, is_null, p);
} // end of bson_set_item

void bson_set_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_set_item_deinit

/*********************************************************************************/
/*  Insert Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool bson_insert_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_set_item_init(initid, args, message);
} // end of bson_insert_item_init

char *bson_insert_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$insert");
	return bson_handle_item(initid, args, result, res_length, is_null, p);
} // end of bson_insert_item

void bson_insert_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_insert_item_deinit

/*********************************************************************************/
/*  Update Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool bson_update_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_set_item_init(initid, args, message);
} // end of bson_update_item_init

char *bson_update_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$update");
	return bson_handle_item(initid, args, result, res_length, is_null, p);
} // end of bson_update_item

void bson_update_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_update_item_deinit

/*********************************************************************************/
/*  Returns a json file as a json string.                                        */
/*********************************************************************************/
my_bool bson_file_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, fl, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 4) {
		strcpy(message, "This function only accepts 1 to 4 arguments");
		return true;
	} else if (args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a string (file name)");
		return true;
	} // endif's args[0]

	for (unsigned int i = 1; i < args->arg_count; i++) {
		if (!(args->arg_type[i] == INT_RESULT || args->arg_type[i] == STRING_RESULT)) {
			sprintf(message, "Argument %d is not an integer or a string (pretty or path)", i);
			return true;
		} // endif arg_type

			// Take care of eventual memory argument
		if (args->arg_type[i] == INT_RESULT && args->args[i])
			more += (ulong)*(longlong*)args->args[i];

	} // endfor i

	initid->maybe_null = 1;
	CalcLen(args, false, reslen, memlen);

	if (args->args[0])
		fl = GetFileLength(args->args[0]);
	else
		fl = 100;		 // What can be done here?

	reslen += fl;

	if (initid->const_item)
		more += fl;

	if (args->arg_count > 1) 
		more += fl * M;

	memlen += more;
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bson_file_init

char *bson_file(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *fn, *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	PlugSubSet(g->Sarea, g->Sarea_Size);
	fn = MakePSZ(g, args, 0);

	if (args->arg_count > 1) {
		int    pretty = 3, pty = 3;
		size_t len;
		PBVAL  jsp, jvp = NULL;
		BJNX   bnx(g);

		for (unsigned int i = 1; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT && *(longlong*)args->args[i] < 4) {
				pretty = (int) * (longlong*)args->args[i];
				break;
			} // endif type

		// Parse the json file and allocate its tree structure
		if (!(jsp = bnx.ParseJsonFile(g, fn, pty, len))) {
			PUSH_WARNING(g->Message);
			goto fin;
		} // endif jsp

		if (pty == 3)
			PUSH_WARNING("File pretty format cannot be determined");
		else if (pretty != 3 && pty != pretty)
			PUSH_WARNING("File pretty format doesn't match the specified pretty value");
		else if (pretty == 3)
			pretty = pty;

		// Check whether a path was specified
		if (bnx.CheckPath(g, args, jsp, jvp, 1)) {
			PUSH_WARNING(g->Message);
			goto fin;
		} else if (jvp)
			jsp = jvp;

		if (!(str = bnx.Serialize(g, jsp, NULL, 0)))
			PUSH_WARNING(g->Message);

	} else
		if (!(str = GetJsonFile(g, fn)))
			PUSH_WARNING(g->Message);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

fin:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
	}	else
		*res_length = strlen(str);

	return str;
} // end of bson_file

void bson_file_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_file_deinit

/*********************************************************************************/
/*  Make a json file from a json item.                                           */
/*********************************************************************************/
my_bool bfile_make_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 1 || args->arg_count > 3) {
		strcpy(message, "Wrong number of arguments");
		return true;
	} else if (!IsArgJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	}	// endif

	CalcLen(args, false, reslen, memlen);
	memlen = memlen + 5000;	 // To take care of not pretty files 
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bfile_make_init

char *bfile_make(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *p, *str = NULL, *fn = NULL;
	int     n, pretty = 2;
	PBVAL   jsp, jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	BJNX    bnx(g);

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if ((n = IsArgJson(args, 0)) == 3) {
		// Get default file name and pretty
		PBSON bsp = (PBSON)args->args[0];

		fn = bsp->Filename;
		pretty = bsp->Pretty;
	} else if ((n = IsArgJson(args, 0)) == 2)
		fn = args->args[0];

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			goto fin;
		} else
			bnx.Reset();

		jvp = bnx.MakeValue(args, 0);

		if (!n && (p = bnx.GetString(jvp))) {
			if (!strchr("[{ \t\r\n", *p)) {
				// Is this a file name?
				if (!(p = GetJsonFile(g, p))) {
					PUSH_WARNING(g->Message);
					goto fin;
				} else
					fn = bnx.GetString(jvp);

			} // endif p

			if (!(jsp = bnx.ParseJson(g, p, strlen(p)))) {
				PUSH_WARNING(g->Message);
				goto fin;
			} // endif jsp

			bnx.SetValueVal(jvp, jsp);
		} // endif p

		if (g->Mrr) {			 // First argument is a constant
			g->Xchk = jvp;
			JsonMemSave(g);
		} // endif Mrr

	} else
		jvp = (PBVAL)g->Xchk;

	for (uint i = 1; i < args->arg_count; i++)
		switch (args->arg_type[i]) {
			case STRING_RESULT:
				fn = MakePSZ(g, args, i);
				break;
			case INT_RESULT:
				pretty = (int)*(longlong*)args->args[i];
				break;
			default:
				PUSH_WARNING("Unexpected argument type in bfile_make");
		}	// endswitch arg_type

	if (fn) {
		if (!bnx.Serialize(g, jvp, fn, pretty))
			PUSH_WARNING(g->Message);
	} else
		PUSH_WARNING("Missing file name");

	str = fn;

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)str;

fin:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of bfile_make

void bfile_make_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bfile_make_deinit

/*********************************************************************************/
/*  Convert a prettiest Json file to Pretty=0.                                   */
/*********************************************************************************/
my_bool bfile_convert_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen;

	if (args->arg_count != 3) {
		strcpy(message, "This function must have 3 arguments");
		return true;
	} else if (args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third Argument must be an integer (LRECL)");
		return true;
	} else for (int i = 0; i < 2; i++)
		if (args->arg_type[i] != STRING_RESULT) {
			sprintf(message, "Arguments %d must be a string (file name)", i+1);
			return true;
		} // endif args

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bfile_convert_init

char *bfile_convert(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long *res_length, char *is_null, char *error) {
	char   *str, *fn, *ofn;
	int     lrecl = (int)*(longlong*)args->args[2];
	PGLOBAL g = (PGLOBAL)initid->ptr;

	PlugSubSet(g->Sarea, g->Sarea_Size);
	fn = MakePSZ(g, args, 0);
	ofn = MakePSZ(g, args, 1);

	if (!g->Xchk) {
		JUP* jup = new(g) JUP(g);

		str = jup->UnprettyJsonFile(g, fn, ofn, lrecl);
		g->Xchk = str;
	} else
		str = (char*)g->Xchk;

	if (!str) {
		PUSH_WARNING(*g->Message ? g->Message : "Unexpected error");
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else {
		strcpy(result, str);
		*res_length = strlen(str);
	}	// endif str

	return str;
} // end of bfile_convert

void bfile_convert_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bfile_convert_deinit

/*********************************************************************************/
/*  Convert a pretty=0 Json file to binary BJSON.                                */
/*********************************************************************************/
my_bool bfile_bjson_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen;

	if (args->arg_count != 2 && args->arg_count != 3) {
		strcpy(message, "This function must have 2 or 3 arguments");
		return true;
	} else if (args->arg_count == 3 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third Argument must be an integer (LRECL)");
		return true;
	} else for (int i = 0; i < 2; i++)
		if (args->arg_type[i] != STRING_RESULT) {
			sprintf(message, "Arguments %d must be a string (file name)", i + 1);
			return true;
		} // endif args

	CalcLen(args, false, reslen, memlen);
	memlen = memlen * M;
	memlen += (args->arg_count == 3) ? (ulong)*(longlong*)args->args[2] : 1024;
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bfile_bjson_init

char *bfile_bjson(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char*, char *error) {
	char   *buf, *str = NULL, fn[_MAX_PATH], ofn[_MAX_PATH];
	bool    loop;
	ssize_t len, newloc;
	size_t  lrecl, binszp;
	PBVAL		jsp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	BDOC    doc(g);

	strcpy(fn, MakePSZ(g, args, 0));
	strcpy(ofn, MakePSZ(g, args, 1));

	if (args->arg_count == 3)
		lrecl = (size_t)*(longlong*)args->args[2];
	else
		lrecl = 1024;

	if (!g->Xchk) {
		int 	msgid = MSGID_OPEN_MODE_STRERROR;
		FILE *fout = NULL;
		FILE *fin;

		if (!(fin = global_fopen(g, msgid, fn, "rt")))
			str = strcpy(result, g->Message);
		else if (!(fout = global_fopen(g, msgid, ofn, "wb")))
			str = strcpy(result, g->Message);
		else if ((buf = (char*)malloc(lrecl))) {
			try {
				do {
					loop = false;
					PlugSubSet(g->Sarea, g->Sarea_Size);

					if (!fgets(buf, lrecl, fin)) {
						if (!feof(fin)) {
							sprintf(g->Message, "Error %d reading %zd bytes from %s",
								errno, lrecl, fn);
							str = strcpy(result, g->Message);
						}	else
							str = strcpy(result, ofn);

					} else if ((len = strlen(buf))) {
						if ((jsp = doc.ParseJson(g, buf, len))) {
							newloc = (size_t)PlugSubAlloc(g, NULL, 0);
							binszp = newloc - (size_t)jsp;

							if (fwrite(&binszp, sizeof(binszp), 1, fout) != 1) {
								sprintf(g->Message, "Error %d writing %zd bytes to %s", 
									errno, sizeof(binszp), ofn);
								str = strcpy(result, g->Message);
							} else if (fwrite(jsp, binszp, 1, fout) != 1) {
								sprintf(g->Message, "Error %d writing %zd bytes to %s", 
									errno, binszp, ofn);
								str = strcpy(result, g->Message);
							} else
								loop = true;

						} else {
							str = strcpy(result, g->Message);
						}	// endif jsp

					} else
						loop = true;

				} while (loop);

			} catch (int) {
				str = strcpy(result, g->Message);
			} catch (const char* msg) {
				str = strcpy(result, msg);
			} // end catch

			free(buf);
		} else
			str = strcpy(result, "Buffer malloc failed");

		if (fin) fclose(fin);
		if (fout) fclose(fout);
		g->Xchk = str;
	} else
		str = (char*)g->Xchk;

	if (!str) {
		if (*g->Message)
			str = strcpy(result, g->Message);
		else
			str = strcpy(result, "Unexpected error");

	} // endif str

	*res_length = strlen(str);
	return str;
} // end of bfile_bjson

void bfile_bjson_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bfile_bjson_deinit

/*********************************************************************************/
/*  Serialize a Json document.                .                                  */
/*********************************************************************************/
my_bool bson_serialize_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->args[0] && IsArgJson(args, 0) != 3) {
		strcpy(message, "Argument must be a Jbin tree");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bson_serialize_init

char *bson_serialize(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	char   *str;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (IsArgJson(args, 0) == 3) {
			PBSON bsp = (PBSON)args->args[0];
			BJNX  bnx(bsp->G);
			PBVAL bvp = (args->arg_count == 1) ? (PBVAL)bsp->Jsp : (PBVAL)bsp->Top;

//		if (!(str = bnx.Serialize(g, bvp, bsp->Filename, bsp->Pretty)))
			if (!(str = bnx.Serialize(g, bvp, NULL, 0)))
				str = strcpy(result, g->Message);

			// Keep result of constant function
			g->Xchk = (initid->const_item) ? str : NULL;
		} else {
			// *error = 1;
			str = strcpy(result, "Argument is not a Jbin tree");
		} // endif

	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of bson_serialize

void bson_serialize_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bson_serialize_deinit

/*********************************************************************************/
/*  Make and return a binary Json array containing all the parameters.           */
/*  Note: jvp must be set before arp because it can be a binary argument.        */
/*********************************************************************************/
my_bool bbin_make_array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bbin_make_array_init

char *bbin_make_array(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			BJNX  bnx(g);
			PBVAL jvp = bnx.MakeValue(args, 0);
			PBVAL arp = bnx.NewVal(TYPE_JAR);

			for (uint i = 0; i < args->arg_count;) {
				bnx.AddArrayValue(arp, jvp);
				jvp = bnx.MakeValue(args, ++i);
			} // endfor i

			if ((bsp = BbinAlloc(bnx.G, initid->max_length, arp))) {
				strcat(bsp->Msg, " array");

				// Keep result of constant function
				g->Xchk = (initid->const_item) ? bsp : NULL;
			}	// endif bsp

		} // endif CheckMemory

	} else
		bsp = (PBSON)g->Xchk;

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_make_array

void bbin_make_array_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_make_array_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool bbin_array_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsArgJson(args, 0) > 1)
			initid->const_item = 0;

		return false;
	} else
		return true;

} // end of bbin_array_add_init

char *bbin_array_add(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} else if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		uint	n = 2;
		int* x = GetIntArgPtr(g, args, n);
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL jarp = nullptr, top = nullptr, jvp = nullptr;
		PBVAL jsp = bnx.MakeValue(args, 0, true, &top);

		if (bnx.CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->Type != TYPE_JAR) {
			if ((jarp = bnx.NewVal(TYPE_JAR))) {
				bnx.AddArrayValue(jarp, jvp);

				if (!top)
					top = jarp;

			}	// endif jarp

		} else
			jarp = jvp;

		if (jarp) {
			bnx.AddArrayValue(jarp, bnx.MakeValue(args, 1), x);
			bnx.SetChanged(true);
			bsp = bnx.MakeBinResult(args, top, initid->max_length);

			if (initid->const_item)
				// Keep result of constant function
				g->Xchk = bsp;

		} else
			PUSH_WARNING(g->Message);

	} // endif CheckMemory

	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
		*error = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_array_add

void bbin_array_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_array_add_deinit

/*********************************************************************************/
/*  Add one or several values to a Bson array.                                   */
/*********************************************************************************/
my_bool bbin_array_add_values_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
{
	return bson_array_add_values_init(initid, args, message);
} // end of bbin_array_add_values_init

char* bbin_array_add_values(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			uint  i = 0;
			BJNX  bnx(g);
			PBVAL arp, top, jvp = NULL;
			PBVAL bvp = bnx.MakeValue(args, 0, true, &top);

			if (bvp->Type == TYPE_JAR) {
				arp = bvp;
				i = 1;
			} else		// First argument is not an array
				arp = bnx.NewVal(TYPE_JAR);

			for (; i < args->arg_count; i++)
				bnx.AddArrayValue(arp, bnx.MakeValue(args, i));

			bnx.SetChanged(true);
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
		} // endif CheckMemory

		// Keep result of constant function
		g->Xchk = (g->N) ? bsp : NULL;
	} else
		bsp = (PBSON)g->Xchk;

	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
		*error = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_array_add_values

void bbin_array_add_values_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_array_add_values_deinit

/*********************************************************************************/
/*  Make a Json array from values coming from rows.                              */
/*********************************************************************************/
my_bool bbin_array_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_array_grp_init(initid, args, message);
} // end of bbin_array_grp_init

void bbin_array_grp_clear(UDF_INIT *initid, char *a, char *b)
{
	bson_array_grp_clear(initid, a, b);
} // end of bbin_array_grp_clear

void bbin_array_grp_add(UDF_INIT *initid, UDF_ARGS *args, char *a, char *b)
{
	bson_array_grp_add(initid, args, a, b);
} // end of bbin_array_grp_add

char *bbin_array_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBVAL   arp = (PBVAL)g->Activityp;

	if (g->N < 0)
		PUSH_WARNING("Result truncated to json_grp_size values");

	if (arp)
		if ((bsp = BbinAlloc(g, initid->max_length, arp)))
			strcat(bsp->Msg, " array");

	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
		*error = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_array_grp

void bbin_array_grp_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_array_grp_deinit

/*********************************************************************************/
/*  Make a Json object from values coming from rows.                             */
/*********************************************************************************/
my_bool bbin_object_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_object_grp_init(initid, args, message);
} // end of bbin_object_grp_init

void bbin_object_grp_clear(UDF_INIT *initid, char *a, char *b)
{
	bson_object_grp_clear(initid, a, b);
} // end of bbin_object_grp_clear

void bbin_object_grp_add(UDF_INIT *initid, UDF_ARGS *args, char *a, char *b)
{
	bson_object_grp_add(initid, args, a, b);
} // end of bbin_object_grp_add

char *bbin_object_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBVAL   bop = (PBVAL)g->Activityp;

	if (g->N < 0)
		PUSH_WARNING("Result truncated to json_grp_size values");

	if (bop)
		if ((bsp = BbinAlloc(g, initid->max_length, bop)))
			strcat(bsp->Msg, " object");

	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
		*error = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_grp

void bbin_object_grp_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_grp_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the parameters.                            */
/*********************************************************************************/
my_bool bbin_make_object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of bbin_make_object_init

char *bbin_make_object(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			BJNX  bnx(g);
			PBVAL objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i++)
					bnx.SetKeyValue(objp, bnx.MakeValue(args, i), bnx.MakeKey(args, i));

				if ((bsp = BbinAlloc(bnx.G, initid->max_length, objp))) {
					strcat(bsp->Msg, " object");

					// Keep result of constant function
					g->Xchk = (initid->const_item) ? bsp : NULL;
				}	// endif bsp

			} // endif objp																												 

		} // endif CheckMemory

	} // endif Xchk

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_make_object

void bbin_make_object_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_make_object_deinit

/*********************************************************************************/
/*  Make a Json Object containing all not null parameters.                       */
/*********************************************************************************/
my_bool bbin_object_nonull_init(UDF_INIT *initid, UDF_ARGS *args,	char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bbin_object_nonull_init

char *bbin_object_nonull(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			BJNX  bnx(g);
			PBVAL jvp, objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i++)
					if (!bnx.IsValueNull(jvp = bnx.MakeValue(args, i)))
						bnx.SetKeyValue(objp, jvp, bnx.MakeKey(args, i));

				if ((bsp = BbinAlloc(bnx.G, initid->max_length, objp)))	{
					strcat(bsp->Msg, " object");

					// Keep result of constant function
					g->Xchk = (initid->const_item) ? bsp : NULL;
				}	// endif bsp

			} // endif objp

		} // endif CheckMemory

	} // endif Xchk

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_nonull

void bbin_object_nonull_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_nonull_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the key/value parameters.                  */
/*********************************************************************************/
my_bool bbin_object_key_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count % 2) {
		strcpy(message, "This function must have an even number of arguments");
		return true;
	} // endif arg_count

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bbin_object_key_init

char *bbin_object_key(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			BJNX  bnx(g);
			PBVAL objp;

			if ((objp = bnx.NewVal(TYPE_JOB))) {
				for (uint i = 0; i < args->arg_count; i += 2)
					bnx.SetKeyValue(objp, bnx.MakeValue(args, i + 1), MakePSZ(g, args, i));

				if ((bsp = BbinAlloc(bnx.G, initid->max_length, objp))) {
					strcat(bsp->Msg, " object");

					// Keep result of constant function
					g->Xchk = (initid->const_item) ? bsp : NULL;
				}	// endif bsp

			} // endif objp

		} // endif CheckMemory

	} // endif Xchk

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_key

void bbin_object_key_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_key_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool bbin_object_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!IsArgJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bbin_object_add_init

char *bbin_object_add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} else if (!CheckMemory(g, initid, args, 2, false, true, true)) {
		PSZ   key;
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL top;
		PBVAL jobp = bnx.MakeValue(args, 0, true, &top);
		PBVAL jvp = jobp;

		if (bnx.CheckPath(g, args, jvp, jobp, 2))
			PUSH_WARNING(g->Message);
		else if (jobp && jobp->Type == TYPE_JOB) {
			jvp = bnx.MakeValue(args, 1);
			key = bnx.MakeKey(args, 1);
			bnx.SetKeyValue(jobp, jvp, key);
			bnx.SetChanged(true);
		} else {
			PUSH_WARNING("First argument target is not an object");
			//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jobp

		// In case of error unchanged argument will be returned
		bsp = bnx.MakeBinResult(args, top, initid->max_length);

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = bsp;

	} // endif CheckMemory

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_add

void bbin_object_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool bbin_array_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_array_delete_init(initid, args, message);
} // end of bbin_array_delete_init

char *bbin_array_delete(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
	} else if (!CheckMemory(g, initid, args, 1, false, false, true)) {
		int* x;
		uint	n = 1;
		BJNX  bnx(g);
		PBVAL arp, top;
		PBVAL jvp = bnx.MakeValue(args, 0, true, &top);

		if (!(x = GetIntArgPtr(g, args, n)))
			PUSH_WARNING("Missing or null array index");
		else if (bnx.CheckPath(g, args, jvp, arp, 1))
			PUSH_WARNING(g->Message);
		else if (arp && arp->Type == TYPE_JAR) {
			bnx.SetChanged(bnx.DeleteValue(arp, *x));
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
		} else {
			PUSH_WARNING("First argument target is not an array");
			//		if (g->Mrr) *error = 1;
		} // endif jvp

		if (g->N)
			// Keep result of constant function
			g->Xchk = bsp;

	} // endif CheckMemory

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_array_delete

void bbin_array_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_array_delete_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool bbin_object_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have 2 or 3 arguments");
		return true;
	} else if (!IsArgJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument must be a key string");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of bbin_object_delete_init

char *bbin_object_delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} else if (!CheckMemory(g, initid, args, 1, false, true, true)) {
		PCSZ  key;
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL top;
		PBVAL jobp = bnx.MakeValue(args, 0, true, &top);

		if (bnx.CheckPath(g, args, top, jobp, 2))
			PUSH_WARNING(g->Message);
		else if (jobp && jobp->Type == TYPE_JOB) {
			key = bnx.MakeKey(args, 1);
			bnx.SetChanged(bnx.DeleteKey(jobp, key));
		} else {
			PUSH_WARNING("First argument target is not an object");
			//		if (g->Mrr) *error = 1;					(only if no path)
		} // endif jvp

	  // In case of error unchanged argument will be returned
		bsp = bnx.MakeBinResult(args, top, initid->max_length);

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = bsp;

	} // endif CheckMemory

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_delete

void bbin_object_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool bbin_object_list_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_object_list_init(initid, args, message);
} // end of bbin_object_list_init

char *bbin_object_list(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			BJNX  bnx(g);
			PBVAL top, jarp = NULL;
			PBVAL jsp = bnx.MakeValue(args, 0, true, &top);

			if (jsp->Type == TYPE_JOB) {
				jarp = bnx.GetKeyList(jsp);
			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jsp type

			// In case of error unchanged argument will be returned
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
			bsp->Jsp = (PJSON)jarp;

		} // endif CheckMemory

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_list

void bbin_object_list_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_list_deinit

/*********************************************************************************/
/*  Returns an array of the Json object values.                                  */
/*********************************************************************************/
my_bool bbin_object_values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_object_values_init(initid, args, message);
} // end of bbin_object_values_init

char *bbin_object_values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			BJNX  bnx(g);
			PBVAL top, jarp = nullptr;
			PBVAL jvp = bnx.MakeValue(args, 0, true, &top);

			if (jvp->Type == TYPE_JOB) {
				jarp = bnx.GetObjectValList(jvp);
			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jvp

			// In case of error unchanged argument will be returned
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
			bsp->Jsp = (PJSON)jarp;

		} // endif CheckMemory

		if (initid->const_item) {
			// Keep result of constant function
			g->Xchk = bsp;
		} // endif const_item

	} // endif bsp

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_object_values

void bbin_object_values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_object_values_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool bbin_get_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_get_item_init(initid, args, message);
} // end of bbin_get_item_init																								           

char *bbin_get_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		bsp = (PBSON)g->Xchk;
	} else if (!CheckMemory(g, initid, args, 1, true, true)) {
		char *path = MakePSZ(g, args, 1);
		BJNX  bnx(g, NULL, TYPE_STRING, initid->max_length);
		PBVAL top, jvp = NULL;
		PBVAL jsp = bnx.MakeValue(args, 0, true, &top);

		if (bnx.CheckPath(g, args, jsp, jvp, 1))
			PUSH_WARNING(g->Message);
		else if (jvp) {
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
			bsp->Jsp = (PJSON)jvp;

			if (initid->const_item)
				// Keep result of constant function
				g->Xchk = bsp;

		} // endif jvp

	} else 
		PUSH_WARNING("CheckMemory error");

	if (!bsp) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_get_item

void bbin_get_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_get_item_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool bbin_item_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_item_merge_init(initid, args, message);
} // end of bbin_item_merge_init

char *bbin_item_merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		JTYP  type;
		BJNX  bnx(g);
		PBVAL jvp, top = NULL;
		PBVAL jsp[2] = {NULL, NULL};

		for (int i = 0; i < 2; i++) {
			if (i) {
				jvp = bnx.MakeValue(args, i, true);

				if (jvp->Type != type) {
					PUSH_WARNING("Argument types mismatch");
					goto fin;
				}	// endif type

			} else {
				jvp = bnx.MakeValue(args, i, true, &top);
				type = (JTYP)jvp->Type;

				if (type != TYPE_JAR && type != TYPE_JOB) {
					PUSH_WARNING("First argument is not an array or object");
					goto fin;
				} // endif type

			}	// endif i

			jsp[i] = jvp;
		} // endfor i

		if (type == TYPE_JAR)
			bnx.MergeArray(jsp[0], jsp[1]);
		else
			bnx.MergeObject(jsp[0], jsp[1]);

		bnx.SetChanged(true);
		bsp = bnx.MakeBinResult(args, top, initid->max_length);
	} // endif CheckMemory

	if (g->N)
		// Keep result of constant function
		g->Xchk = bsp;

fin:
	if (!bsp) {
		*res_length = 0;
		*error = 1;
		*is_null = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_item_merge

void bbin_item_merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_item_merge_deinit

/*********************************************************************************/
/*  This function is used by the jbin_set/insert/update_item functions.          */
/*********************************************************************************/
static char *bbin_handle_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *path;
	int     w;
	my_bool b = true;
	PBJNX   bxp;
	PBVAL   jsp, jvp, top;
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Alchecked) {
		bsp = (PBSON)g->Activityp;
		goto fin;
	} else if (g->N)
		g->Alchecked = 1;

	if (!strcmp(result, "$set"))
		w = 0;
	else if (!strcmp(result, "$insert"))
		w = 1;
	else if (!strcmp(result, "$update"))
		w = 2;
	else {
		PUSH_WARNING("Logical error, please contact CONNECT developer");
		goto fin;
	}	// endelse

	try {
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true, false, true)) {
				throw 1;
			} else {
				BJNX bnx(g);

				jsp = bnx.MakeValue(args, 0, true, &top);

				if (g->Mrr) {			 // First argument is a constant
					g->Xchk = jsp;
					g->More = (size_t)top;
					JsonMemSave(g);
				} // endif Mrr

			}	// endif CheckMemory

		} else {
			jsp = (PBVAL)g->Xchk;
			top = (PBVAL)g->More;
		}	// endif Xchk

		bxp = new(g)BJNX(g, jsp, TYPE_STRING, initid->max_length, 0, true);

		for (uint i = 1; i + 1 < args->arg_count; i += 2) {
			jvp = bxp->MakeValue(args, i);
			path = MakePSZ(g, args, i + 1);

			if (bxp->SetJpath(g, path, false))
				throw 2;

			if (w) {
				bxp->ReadValue(g);
				b = bxp->GetValue()->IsNull();
				b = (w == 1) ? b : !b;
			}	// endif w

			if (b && bxp->WriteValue(g, jvp))
				throw 3;

			bxp->SetChanged(true);
		} // endfor i

		if (!(bsp = bxp->MakeBinResult(args, top, initid->max_length)))
			throw 4;

		if (g->N)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)bsp;

	} catch (int n) {
		if (trace(1))
			htrc("Exception %d: %s\n", n, g->Message);

		PUSH_WARNING(g->Message);
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
	} // end catch

fin:
	if (!bsp) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_handle_item

/*********************************************************************************/
/*  Set Json items of a Json document according to path.                         */
/*********************************************************************************/
my_bool bbin_set_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_set_item_init(initid, args, message);
} // end of bbin_set_item_init

char *bbin_set_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$set");
	return bbin_handle_item(initid, args, result, res_length, is_null, p);
} // end of bbin_set_item

void bbin_set_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_set_item_deinit

/*********************************************************************************/
/*  Insert Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool bbin_insert_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_set_item_init(initid, args, message);
} // end of bbin_insert_item_init

char *bbin_insert_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$insert");
	return bbin_handle_item(initid, args, result, res_length, is_null, p);
} // end of bbin_insert_item

void bbin_insert_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_insert_item_deinit

/*********************************************************************************/
/*  Update Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool bbin_update_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_set_item_init(initid, args, message);
} // end of bbin_update_item_init

char *bbin_update_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$update");
	return bbin_handle_item(initid, args, result, res_length, is_null, p);
} // end of bbin_update_item

void bbin_update_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_update_item_deinit

/*********************************************************************************/
/*  Delete items from a Json document.                                           */
/*********************************************************************************/
my_bool bbin_delete_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_delete_item_init(initid, args, message);
} // end of bbin_delete_item_init

char *bbin_delete_item(UDF_INIT *initid, UDF_ARGS *args, char *result, 
	unsigned long *res_length, char *is_null, char *error)
{
	char   *path;
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, false, true)) {
		BJNX  bnx(g, NULL, TYPE_STRING);
		PBVAL top, jar = NULL;
		PBVAL jvp = bnx.MakeValue(args, 0, true, &top);

		if (args->arg_count == 1) {
			// This	should be coming from bbin_locate_all
			jar = jvp;		 // This is the array of paths
			jvp = top;		 // And this is the document
		}	else if(!bnx.IsJson(jvp)) {
			PUSH_WARNING("First argument is not a JSON document");
			goto fin;
		}	else if (args->arg_count == 2) {
			// Check whether this is an array of paths 
			jar = bnx.MakeValue(args, 1, true);

			if (jar && jar->Type != TYPE_JAR)
				jar = NULL;

		}	// endif arg_count

		if (jar) {
			// Do the deletion in reverse order
			for(int i = bnx.GetArraySize(jar) - 1;	i >= 0; i--) {
				path = bnx.GetString(bnx.GetArrayValue(jar, i));

				if (bnx.SetJpath(g, path, false)) {
					PUSH_WARNING(g->Message);
					continue;
				}	// endif SetJpath

				bnx.SetChanged(bnx.DeleteItem(g, jvp));
			}	// endfor i

		}	else for (uint i = 1; i < args->arg_count; i++) {
			path = MakePSZ(g, args, i);

			if (bnx.SetJpath(g, path, false)) {
				PUSH_WARNING(g->Message);
				continue;
			}	// endif SetJpath

			bnx.SetChanged(bnx.DeleteItem(g, jvp));
		} // endfor i

		bsp = bnx.MakeBinResult(args, top, initid->max_length);

		if (args->arg_count == 1)
			// Here Jsp was not a sub-item of top
			bsp->Jsp = (PJSON)top;

	} // endif CheckMemory

	if (g->N)
		// Keep result of constant function
		g->Xchk = bsp;

fin:
	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_delete_item

void bbin_delete_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_delete_item_deinit

/*********************************************************************************/
/*  Returns a json file as a json binary tree.                                   */
/*********************************************************************************/
my_bool bbin_file_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return bson_file_init(initid, args, message);
} // end of bbin_file_init

char *bbin_file(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *fn;
	int     pretty = 3;
	size_t  len = 0;
	PBVAL   jsp, jvp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	BJNX    bnx(g);
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp)
		goto fin;

	fn = MakePSZ(g, args, 0);

	for (unsigned int i = 1; i < args->arg_count; i++)
		if (args->arg_type[i] == INT_RESULT && *(longlong*)args->args[i] < 4) {
			pretty = (int) * (longlong*)args->args[i];
			break;
		} // endif type

	//  Parse the json file and allocate its tree structure
	if (!(jsp = bnx.ParseJsonFile(g, fn, pretty, len))) {
		PUSH_WARNING(g->Message);
		*error = 1;
		goto fin;
	} // endif jsp

//	if (pretty == 3)
//		PUSH_WARNING("File pretty format cannot be determined");
//	else if (pretty == 3)
//		pretty = pty;

	if ((bsp = BbinAlloc(bnx.G, len, jsp))) {
		strcat(bsp->Msg, " file");
		bsp->Filename = fn;
		bsp->Pretty = pretty;
	} else {
		*error = 1;
		goto fin;
	}	// endif bsp

	// Check whether a path was specified
	if (bnx.CheckPath(g, args, jsp, jvp, 1)) {
		PUSH_WARNING(g->Message);
		bsp = NULL;
		goto fin;
	} else if (jvp)
		bsp->Jsp = (PJSON)jvp;

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

fin:
	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_file

void bbin_file_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_file_deinit

/*********************************************************************************/
/*  Locate all occurences of a value in a Json tree.                             */
/*********************************************************************************/
my_bool bbin_locate_all_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	return bson_locate_all_init(initid, args, message);
} // end of bbin_locate_all_init

char* bbin_locate_all(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char   *path = NULL;
	int     mx = 10;
	PBVAL   bvp, bvp2;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->N) {
		if (g->Activityp) {
			bsp = (PBSON)g->Activityp;
			*res_length = sizeof(BSON);
			return (char*)bsp;
		} else {
			*error = 1;
			*res_length = 0;
			*is_null = 1;
			return NULL;
		}	// endif Activityp

	} else if (initid->const_item)
		g->N = 1;

	try {
		PBVAL top = NULL;
		BJNX  bnx(g);

		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else
				bnx.Reset();

			bvp = bnx.MakeValue(args, 0, true, &top);

			if (bvp->Type == TYPE_NULL) {
				PUSH_WARNING("First argument is not a valid JSON item");
				goto err;
			}	// endif bvp

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = bvp;
				g->More = (size_t)top;
				JsonMemSave(g);
			} // endif Mrr

		} else {
			bvp = (PBVAL)g->Xchk;
			top = (PBVAL)g->More;
		}	// endif Xchk

		// The item to locate
		bvp2 = bnx.MakeValue(args, 1, true);

		if (bvp2->Type == TYPE_NULL) {
			PUSH_WARNING("Invalid second argument");
			goto err;
		}	// endif bvp2

		if (args->arg_count > 2)
			mx = (int)*(long long*)args->args[2];

		if ((path = bnx.LocateAll(g, bvp, bvp2, mx))) {
			bsp = bnx.MakeBinResult(args, top, initid->max_length);
			bsp->Jsp = (PJSON)bnx.ParseJson(g, path, strlen(path));
		}	// endif path

		if (initid->const_item)
			// Keep result of constant function
			g->Activityp = (PACTIVITY)bsp;

	} catch (int n) {
		xtrc(1, "Exception %d: %s\n", n, g->Message);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} catch (const char* msg) {
		strcpy(g->Message, msg);
		PUSH_WARNING(g->Message);
		*error = 1;
		path = NULL;
	} // end catch

err:
	if (!bsp) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of bbin_locate_all

void bbin_locate_all_deinit(UDF_INIT* initid) {
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of bbin_locate_all_deinit



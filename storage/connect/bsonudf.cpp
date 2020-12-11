/****************** bsonudf C++ Program Source Code File (.CPP) ******************/
/*  PROGRAM NAME: bsonudf     Version 1.0                                        */
/*  (C) Copyright to the author Olivier BERTRAND          2020                   */
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

/* --------------------------------- JSON UDF ---------------------------------- */

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
BJNX::BJNX(PGLOBAL g, PBVAL row, int type, int len, int prec, my_bool wr) : BDOC(g)
{
	Row = row;
	Bvalp = NULL;
	Jpnp = NULL;
	Jp = NULL;
	Nodes = NULL;
	Value = AllocateValue(g, type, len, prec);
	MulVal = NULL;
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
} // end of BJNX constructor

/*********************************************************************************/
/*  SetJpath: set and parse the json path.                                       */
/*********************************************************************************/
my_bool BJNX::SetJpath(PGLOBAL g, char* path, my_bool jb)
{
	// Check Value was allocated
	if (!Value)
		return true;

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
		case '*': // Expand this array
			strcpy(g->Message, "Expand not supported by this function");
			return true;
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

	// For calculated arrays, a local Value must be used
	switch (jnp->Op) {
	case OP_NUM:
		jnp->Valp = AllocateValue(g, TYPE_INT);
		break;
	case OP_ADD:
	case OP_MULT:
	case OP_SEP:
		if (!IsTypeChar(Buf_Type))
			jnp->Valp = AllocateValue(g, Buf_Type, 0, GetPrecision());
		else
			jnp->Valp = AllocateValue(g, TYPE_DOUBLE, 0, 2);

		break;
	case OP_MIN:
	case OP_MAX:
		jnp->Valp = AllocateValue(g, Buf_Type, Long, GetPrecision());
		break;
	case OP_CNC:
		if (IsTypeChar(Buf_Type))
			jnp->Valp = AllocateValue(g, TYPE_STRING, Long, GetPrecision());
		else
			jnp->Valp = AllocateValue(g, TYPE_STRING, 512);

		break;
	default:
		break;
	} // endswitch Op

	if (jnp->Valp)
		MulVal = AllocateValue(g, jnp->Valp);

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
	MulVal = AllocateValue(g, Value);

	if (trace(1))
		for (i = 0; i < Nod; i++)
			htrc("Node(%d) Key=%s Op=%d Rank=%d\n",
				i, SVP(Nodes[i].Key), Nodes[i].Op, Nodes[i].Rank);

	Parsed = true;
	return false;
} // end of ParseJpath

/*********************************************************************************/
/*  MakeJson: Serialize the json item and set value to it.                       */
/*********************************************************************************/
PVAL BJNX::MakeJson(PGLOBAL g, PBVAL bvp)
{
	if (Value->IsTypeNum()) {
		strcpy(g->Message, "Cannot make Json for a numeric value");
		Value->Reset();
	} else if (bvp->Type != TYPE_JAR && bvp->Type != TYPE_JOB) {
		strcpy(g->Message, "Target is not an array or object");
		Value->Reset();
	} else
		Value->SetValue_psz(Serialize(g, bvp, NULL, 0));

	return Value;
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
		} else switch (vlp->Type) {
		case TYPE_DTM:
		case TYPE_STRG:
			vp->SetValue_psz(GetString(vlp));
			break;
		case TYPE_INTG:
		case TYPE_BINT:
			vp->SetValue(GetInteger(vlp));
			break;
		case TYPE_DBL:
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
PBVAL BJNX::GetRowValue(PGLOBAL g, PBVAL row, int i, my_bool b)
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
			Jb = b;
			//		return DupVal(g, row);
			return row;		 // or last line ???
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
/*  CalculateArray: NIY                                                          */
/*********************************************************************************/
PVAL BJNX::CalculateArray(PGLOBAL g, PBVAL bap, int n)
{
#if 0
	int     i, ars = GetArraySize(bap), nv = 0;
	bool    err;
	OPVAL   op = Nodes[n].Op;
	PVAL    val[2], vp = Nodes[n].Valp;
	PBVAL   bvrp, bvp;
	BVAL    bval;

	vp->Reset();
	xtrc(1, "CalculateArray size=%d op=%d\n", ars, op);

	for (i = 0; i < ars; i++) {
		bvrp = GetArrayValue(bap, i);
		xtrc(1, "i=%d nv=%d\n", i, nv);

		if (!IsValueNull(bvrp) || (op == OP_CNC && GetJsonNull())) {
			if (IsValueNull(bvrp)) {
				SetString(bvrp, GetJsonNull(), 0);
				bvp = bvrp;
			} else if (n < Nod - 1 && bvrp->GetJson()) {
				bval.SetValue(g, GetColumnValue(g, jvrp->GetJson(), n + 1));
				bvp = &bval;
			} else
				jvp = jvrp;

			if (trace(1))
				htrc("jvp=%s null=%d\n",
					jvp->GetString(g), jvp->IsNull() ? 1 : 0);

			if (!nv++) {
				SetJsonValue(g, vp, jvp);
				continue;
			} else
				SetJsonValue(g, MulVal, jvp);

			if (!MulVal->IsNull()) {
				switch (op) {
				case OP_CNC:
					if (Nodes[n].CncVal) {
						val[0] = Nodes[n].CncVal;
						err = vp->Compute(g, val, 1, op);
					} // endif CncVal

					val[0] = MulVal;
					err = vp->Compute(g, val, 1, op);
					break;
					//        case OP_NUM:
				case OP_SEP:
					val[0] = Nodes[n].Valp;
					val[1] = MulVal;
					err = vp->Compute(g, val, 2, OP_ADD);
					break;
				default:
					val[0] = Nodes[n].Valp;
					val[1] = MulVal;
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
		MulVal->SetValue(nv);
		val[0] = vp;
		val[1] = MulVal;

		if (vp->Compute(g, val, 2, OP_DIV))
			vp->Reset();

	} // endif Op

	return vp;
#else
	strcpy(g->Message, "Calculate array NIY");
	return NULL;
#endif
} // end of CalculateArray

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
			val = MVP(row->To_Val);
			break;
		default:
			sprintf(g->Message, "Invalid row JSON type %d", row->Type);
		} // endswitch Type

//		if (i < Nod - 1)
//			if (!(row = (val) ? val->GetJsp() : NULL))
//				val = NULL;

		row = val;
	} // endfor i

	return (val != NULL);
} // end of CheckPath

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
		else switch (row->Type) {
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

		if (LocateValue(g, MVP(pair->Vlp)))
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

			if (LocateValueAll(g, MVP(pair->Vlp)))
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
		for (; found && p1 && p2; p1 = MPP(p1->Next))
			found = CompareValues(g, MVP(p1->Vlp), GetKeyValue(jp2, MZP(p1->Key)));

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

/* -----------------------------Utility functions ------------------------------ */

/*********************************************************************************/
/*  Make a BVAL value from the passed argument.                                  */
/*********************************************************************************/
static PBVAL MakeBinValue(PGLOBAL g, UDF_ARGS* args, uint i)
{
	char* sap = (args->arg_count > i) ? args->args[i] : NULL;
	int   n, len;
	int   ci;
	longlong bigint;
	BDOC  doc(g);
	PBVAL bp, bvp = doc.NewVal();

	if (sap) {
		if (args->arg_type[i] == STRING_RESULT) {
			if ((len = args->lengths[i])) {
				if ((n = IsJson(args, i)) < 3)
					sap = MakePSZ(g, args, i);

				if (n) {
					if (n == 2) {
						if (!(sap = GetJsonFile(g, sap))) {
							PUSH_WARNING(g->Message);
							return NULL;
						} // endif sap

						len = strlen(sap);
					} // endif 2

					if (!(bp = doc.ParseJson(g, sap, strlen(sap)))) {
						PUSH_WARNING(g->Message);
						return NULL;
					} else
						bvp = bp;

				} else {
					// Check whether this string is a valid json string
					JsonMemSave(g);

					if (!(bp = doc.ParseJson(g, sap, strlen(sap)))) {
						// Recover suballocated memory
						JsonSubSet(g);
						ci = (strnicmp(args->attributes[i], "ci", 2)) ? 0 : 1;
						doc.SetString(bvp, sap, ci);
					} else
						bvp = bp;

					g->Saved_Size = 0;
				}	// endif n

			} // endif len

		} else switch (args->arg_type[i]) {
			case INT_RESULT:
				bigint = *(longlong*)sap;

				if ((bigint == 0LL && !strcmp(args->attributes[i], "FALSE")) ||
					  (bigint == 1LL && !strcmp(args->attributes[i], "TRUE")))
					doc.SetBool(bvp, (bool)bigint);
				else
					doc.SetBigint(bvp, bigint);

				break;
			case REAL_RESULT:
				doc.SetFloat(bvp, *(double*)sap);
				break;
			case DECIMAL_RESULT:
				doc.SetFloat(bvp, atof(MakePSZ(g, args, i)));
				break;
			case TIME_RESULT:
			case ROW_RESULT:
			default:
				bvp->Type = TYPE_UNKNOWN;
				break;
			} // endswitch arg_type

	} // endif sap

	return bvp;
} // end of MakeBinValue

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
			BDOC  doc(g);
			PBVAL bvp = MakeBinValue(g, args, 0);

			if (!(str = doc.Serialize(g, bvp, NULL, 0)))
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
/*  Make a Bson array containing all the parameters.                             */
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
			BDOC  doc(g);
			PBVAL bvp = NULL, arp = doc.NewVal(TYPE_JAR);

			for (uint i = 0; i < args->arg_count; i++)
				doc.AddArrayValue(arp, MakeBinValue(g, args, i));

			if (!(str = doc.Serialize(g, arp, NULL, 0)))
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
		//} else if (!IsJson(args, 0, true)) {
		//	strcpy(message, "First argument must be a valid json string or item");
		//	return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (!JsonInit(initid, args, message, true, reslen, memlen)) {
		PGLOBAL g = (PGLOBAL)initid->ptr;

		// This is a constant function
		g->N = (initid->const_item) ? 1 : 0;

		// This is to avoid double execution when using prepared statements
		if (IsJson(args, 0) > 1)
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
			uint  i = 0;
			BDOC  doc(g);
			PBVAL arp, bvp = MakeBinValue(g, args, 0);

			if (bvp->Type == TYPE_JAR) {
				arp = bvp;
				i = 1;
			} else		// First argument is not an array
				arp = doc.NewVal(TYPE_JAR);

			for (; i < args->arg_count; i++)
				doc.AddArrayValue(arp, MakeBinValue(g, args, i));

			str = doc.Serialize(g, arp, NULL, 0);
		} // endif CheckMemory

		if (!str) {
			PUSH_WARNING(g->Message);
			str = args->args[0];
		}	// endif str

		// Keep result of constant function
		g->Xchk = (g->N) ? str : NULL;
	} else
		str = (char*)g->Xchk;

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
/*  Test BJSON parse and serialize.                                              */
/*********************************************************************************/
my_bool bson_test_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count == 0) {
		strcpy(message, "At least 1 argument required (json)");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
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
	BDOC    doc(g);

	if (g->N) {
		str = (char*)g->Activityp;
		goto err;
	} else if (initid->const_item)
		g->N = 1;

	try {
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, !g->Xchk)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else if (!(bvp = MakeBinValue(g, args, 0))) {
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
		str = doc.Serialize(g, bvp, fn, pretty);

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
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (rank)");
		return true;
	} // endifs args

	CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	if (IsJson(args, 0) == 3)
		more = 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bsonlocate_init

char* bsonlocate(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char   *path = NULL;
	int     k;
	PBVAL   bvp, bvp2;
	PBJNX   bnxp;
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
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, !g->Xchk)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else
				bvp = MakeBinValue(g, args, 0);

			if (!bvp) {
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
		if (!(bvp2 = MakeBinValue(g, args, 1))) {
			PUSH_WARNING("Invalid second argument");
			goto err;
		}	// endif bvp

		k = (args->arg_count > 2) ? (int)*(long long*)args->args[2] : 1;

		bnxp = new(g) BJNX(g, bvp, TYPE_STRING);
		path = bnxp->Locate(g, bvp, bvp2, k);

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
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (Depth)");
		return true;
	} // endifs

	CalcLen(args, false, reslen, memlen);

	// TODO: calculate this
	if (IsJson(args, 0) == 3)
		more = 0;

	return JsonInit(initid, args, message, true, reslen, memlen, more);
} // end of bson_locate_all_init

char* bson_locate_all(UDF_INIT* initid, UDF_ARGS* args, char* result,
	unsigned long* res_length, char* is_null, char* error) {
	char* path = NULL;
	int     mx = 10;
	PBVAL   bvp, bvp2;
	PBJNX   bnxp;
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
		if (!g->Xchk) {
			if (CheckMemory(g, initid, args, 1, true)) {
				PUSH_WARNING("CheckMemory error");
				*error = 1;
				goto err;
			} else
				bvp = MakeBinValue(g, args, 0);

			if (!bvp) {
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
		if (!(bvp2 = MakeBinValue(g, args, 1))) {
			PUSH_WARNING("Invalid second argument");
			goto err;
		}	// endif bvp

		if (args->arg_count > 2)
			mx = (int)*(long long*)args->args[2];

		bnxp = new(g) BJNX(g, bvp, TYPE_STRING);
		path = bnxp->LocateAll(g, bvp, bvp2, mx);

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
		FILE *fout;
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
		if (g->Message)
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



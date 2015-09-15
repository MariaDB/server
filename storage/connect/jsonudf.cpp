/****************** jsonudf C++ Program Source Code File (.CPP) ******************/
/*  PROGRAM NAME: jsonudf     Version 1.3                                        */
/*  (C) Copyright to the author Olivier BERTRAND          2015                   */
/*  This program are the JSON User Defined Functions     .                       */
/*********************************************************************************/

/*********************************************************************************/
/*  Include relevant sections of the MariaDB header file.                        */
/*********************************************************************************/
#include <my_global.h>
#include <mysqld.h>
#include <mysql.h>
#include <sql_error.h>
#include <stdio.h>

#include "jsonudf.h"

#define MEMFIX  4096
#define PUSH_WARNING(M) \
push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, M)
#define M 7

uint GetJsonGrpSize(void);
static int IsJson(UDF_ARGS *args, uint i);
static PSZ MakePSZ(PGLOBAL g, UDF_ARGS *args, int i);

/* ----------------------------------- JSNX ------------------------------------ */

/*********************************************************************************/
/*  JSNX public constructor.                                                     */
/*********************************************************************************/
JSNX::JSNX(PGLOBAL g, PJSON row, int type, int len, int prec)
{
	Row = row;
	Jvalp = NULL;
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
} // end of JSNX constructor

/*********************************************************************************/
/*  SetJpath: set and parse the json path.                                       */
/*********************************************************************************/
my_bool JSNX::SetJpath(PGLOBAL g, char *path, my_bool jb)
{
	// Check Value was allocated
	if (!Value)
		return true;

	Value->SetNullable(true);

	if (jb) {																						 
		// Path must return a Json item
		size_t n = strlen(path);

		if (!n || path[n - 1] != '*') {
			Jpath = (char*)PlugSubAlloc(g, NULL, n + 3);
			strcat(strcpy(Jpath, path), (n) ? ":*" : "*");
		} else
			Jpath = path;

	} else
		Jpath = path;

	// Parse the json path
	return ParseJpath(g);
} // end of SetJpath

/*********************************************************************************/
/*  Analyse array processing options.                                            */
/*********************************************************************************/
my_bool JSNX::SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm)
{
	int     n = (int)strlen(p);
	my_bool dg = true, b = false;
	PJNODE  jnp = &Nodes[i];

	if (*p) {
		if (p[--n] == ']') {
			p[n--] = 0;
			p++;
		} else {
			// Wrong array specification
			sprintf(g->Message, "Invalid array specification %s", p);
			return true;
		} // endif p

	} else
		b = true;

	// To check whether a numeric Rank was specified
	for (int k = 0; dg && p[k]; k++)
		dg = isdigit(p[k]) > 0;

	if (!n) {
		// Default specifications
		if (jnp->Op != OP_EXP) {
			if (b) {
				// Return 1st value (B is the index base)
				jnp->Rank = B;
				jnp->Op = OP_EQ;
			} else if (!Value->IsTypeNum()) {
				jnp->CncVal = AllocateValue(g, (void*)", ", TYPE_STRING);
				jnp->Op = OP_CNC;
			} else
				jnp->Op = OP_ADD;

		} // endif OP

	} else if (dg) {
		// Return nth value
		jnp->Rank = atoi(p) - B;
		jnp->Op = OP_EQ;
	} else if (n == 1) {
		// Set the Op value;
		switch (*p) {
		case '+': jnp->Op = OP_ADD;  break;
		case '*': jnp->Op = OP_MULT; break;
		case '>': jnp->Op = OP_MAX;  break;
		case '<': jnp->Op = OP_MIN;  break;
		case '!': jnp->Op = OP_SEP;  break; // Average
		case '#': jnp->Op = OP_NUM;  break;
		case 'x':
		case 'X': // Expand this array
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
my_bool JSNX::ParseJpath(PGLOBAL g)
{
	char   *p, *p2 = NULL, *pbuf = NULL;
	int     i;
	my_bool mul = false;

	if (Parsed)
		return false;                       // Already done
	else if (!Jpath)
		//	Jpath = Name;
		return true;

	pbuf = PlugDup(g, Jpath);

	// The Jpath must be analyzed
	for (i = 0, p = pbuf; (p = strchr(p, ':')); i++, p++)
		Nod++;                         // One path node found

	Nodes = (PJNODE)PlugSubAlloc(g, NULL, (++Nod) * sizeof(JNODE));
	memset(Nodes, 0, (Nod)* sizeof(JNODE));

	// Analyze the Jpath for this column
	for (i = 0, p = pbuf; i < Nod; i++, p = (p2 ? p2 + 1 : p + strlen(p))) {
		if ((p2 = strchr(p, ':')))
			*p2 = 0;

		// Jpath must be explicit
		if (*p == 0 || *p == '[') {
			// Analyse intermediate array processing
			if (SetArrayOptions(g, p, i, Nodes[i-1].Key))
				return true;

		} else if (*p == '*') {
			// Return JSON
			Nodes[i].Op = OP_XX;
		} else {
			Nodes[i].Key = p;
			Nodes[i].Op = OP_EXIST;
		} // endif's

	} // endfor i, p

	MulVal = AllocateValue(g, Value);
	Parsed = true;
	return false;
} // end of ParseJpath

/*********************************************************************************/
/*  MakeJson: Serialize the json item and set value to it.                       */
/*********************************************************************************/
PVAL JSNX::MakeJson(PGLOBAL g, PJSON jsp)
{
	if (Value->IsTypeNum()) {
		strcpy(g->Message, "Cannot make Json for a numeric value");
		Value->Reset();
	} else
		Value->SetValue_psz(Serialize(g, jsp, NULL, 0));

	return Value;
} // end of MakeJson

/*********************************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                                */
/*********************************************************************************/
void JSNX::SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n)
{
	if (val) {
		switch (val->GetValType()) {
		case TYPE_STRG:
		case TYPE_INTG:
		case TYPE_BINT:
		case TYPE_DBL:
			vp->SetValue_pval(val->GetValue());
			break;
		case TYPE_BOOL:
			if (vp->IsTypeNum())
				vp->SetValue(val->GetInteger() ? 1 : 0);
			else
				vp->SetValue_psz((PSZ)(val->GetInteger() ? "true" : "false"));

			break;
		case TYPE_JAR:
			SetJsonValue(g, vp, val->GetArray()->GetValue(0), n);
			break;
		case TYPE_JOB:
			//      if (!vp->IsTypeNum() || !Strict) {
			vp->SetValue_psz(val->GetObject()->GetText(g, NULL));
			break;
			//        } // endif Type

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
PJVAL JSNX::GetJson(PGLOBAL g)
{
	return GetValue(g, Row, 0);
} // end of GetJson

/*********************************************************************************/
/*  ReadValue:                                                                   */
/*********************************************************************************/
void JSNX::ReadValue(PGLOBAL g)
{
	Value->SetValue_pval(GetColumnValue(g, Row, 0));
} // end of ReadValue

/*********************************************************************************/
/*  GetColumnValue:                                                              */
/*********************************************************************************/
PVAL JSNX::GetColumnValue(PGLOBAL g, PJSON row, int i)
{
	int   n = Nod - 1;
	PJVAL val = NULL;

	val = GetValue(g, row, i);
	SetJsonValue(g, Value, val, n);
	return Value;
} // end of GetColumnValue

/*********************************************************************************/
/*  GetValue:                                                                    */
/*********************************************************************************/
PJVAL JSNX::GetValue(PGLOBAL g, PJSON row, int i, my_bool b)
{
//int     n = Nod - 1;
	my_bool expd = false;
	PJAR    arp;
	PJVAL   val = NULL;

	for (; i < Nod && row; i++) {
		if (Nodes[i].Op == OP_NUM) {
			Value->SetValue(row->GetType() == TYPE_JAR ? row->size() : 1);
			val = new(g) JVALUE(g, Value);
			return val;
		} else if (Nodes[i].Op == OP_XX) {
			if (b)
				return new(g)JVALUE(g, MakeJson(g, row));
			else
				return new(g)JVALUE(row);

		} else switch (row->GetType()) {
			case TYPE_JOB:
				if (!Nodes[i].Key) {
					// Expected Array was not there
					if (i < Nod-1)
						continue;
					else
						val = new(g) JVALUE(row);

				} else
					val = ((PJOB)row)->GetValue(Nodes[i].Key);

				break;
			case TYPE_JAR:
				arp = (PJAR)row;

				if (!Nodes[i].Key) {
					if (Nodes[i].Op == OP_EQ)
						val = arp->GetValue(Nodes[i].Rank);
					else if (Nodes[i].Op == OP_EXP)
						return (PJVAL)ExpandArray(g, arp, i);
					else
						return new(g) JVALUE(g, CalculateArray(g, arp, i));

				} else if (i < Nod-1) {
					strcpy(g->Message, "Unexpected array");
					val = NULL;          // Not an expected array
				} else
					val = arp->GetValue(0);

				break;
			case TYPE_JVAL:
				val = (PJVAL)row;
				break;
			default:
				sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
				val = NULL;
			} // endswitch Type

		if (i < Nod-1)
			row = (val) ? val->GetJson() : NULL;

	} // endfor i

	// SetJsonValue(g, Value, val, n);
	return val;
} // end of GetValue

/*********************************************************************************/
/*  ExpandArray:                                                                 */
/*********************************************************************************/
PVAL JSNX::ExpandArray(PGLOBAL g, PJAR arp, int n)
{
	strcpy(g->Message, "Expand cannot be done by this function");
	return NULL;
} // end of ExpandArray

/*********************************************************************************/
/*  CalculateArray:                                                              */
/*********************************************************************************/
PVAL JSNX::CalculateArray(PGLOBAL g, PJAR arp, int n)
{
//int     i, ars, nv = 0, nextsame = Tjp->NextSame;
	int     i, ars, nv = 0, nextsame = 0;
	my_bool err;
	OPVAL   op = Nodes[n].Op;
	PVAL    val[2], vp = Nodes[n].Valp;
	PJVAL   jvrp, jvp;
	JVALUE  jval;

	vp->Reset();
//ars = MY_MIN(Tjp->Limit, arp->size());
	ars = arp->size();

	for (i = 0; i < ars; i++) {
		jvrp = arp->GetValue(i);

//	do {
			if (n < Nod - 1 && jvrp->GetJson()) {
//			Tjp->NextSame = nextsame;
				jval.SetValue(GetColumnValue(g, jvrp->GetJson(), n + 1));
				jvp = &jval;
			} else
				jvp = jvrp;

			if (!nv++) {
				SetJsonValue(g, vp, jvp, n);
				continue;
			} else
				SetJsonValue(g, MulVal, jvp, n);

			if (!MulVal->IsZero()) {
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

			} // endif Zero

//	} while (Tjp->NextSame > nextsame);

	} // endfor i

	if (op == OP_SEP) {
		// Calculate average
		MulVal->SetValue(nv);
		val[0] = vp;
		val[1] = MulVal;

		if (vp->Compute(g, val, 2, OP_DIV))
			vp->Reset();

	} // endif Op

//Tjp->NextSame = nextsame;
	return vp;
} // end of CalculateArray

/*********************************************************************************/
/*  Locate a value in a JSON tree:                                               */
/*********************************************************************************/
PSZ JSNX::Locate(PGLOBAL g, PJSON jsp, PJVAL jvp, int k)
{
	my_bool b = false, err = true;

	g->Message[0] = 0;

	if (!jsp) {
		strcpy(g->Message, "Null json tree");
		return NULL;
	} // endif jsp

	// Write to the path string
	Jp = new(g) JOUTSTR(g);
	Jvalp = jvp;
	K = k;

	switch (jsp->GetType()) {
		case TYPE_JAR:
			err = LocateArray((PJAR)jsp);
			break;
		case TYPE_JOB:
			err = LocateObject((PJOB)jsp);
			break;
		case TYPE_JVAL:
			err = LocateValue((PJVAL)jsp);
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
		return Jp->Strp;
	} // endif's

	return NULL;
} // end of Locate

/*********************************************************************************/
/*  Locate in a JSON Array.                                                      */
/*********************************************************************************/
my_bool JSNX::LocateArray(PJAR jarp)
{
	char   s[16];
	size_t m = Jp->N;

	for (int i = 0; i < jarp->size() && !Found; i++) {
		Jp->N = m;
		sprintf(s, "[%d]", i + B);

		if (Jp->WriteStr(s))
			return true;

		if (LocateValue(jarp->GetValue(i)))
			return true;

		} // endfor i

	return false;
} // end of LocateArray

/*********************************************************************************/
/*  Locate in a JSON Object.                                                     */
/*********************************************************************************/
my_bool JSNX::LocateObject(PJOB jobp)
{
	size_t m = Jp->N;

	for (PJPR pair = jobp->First; pair && !Found; pair = pair->Next) {
		Jp->N = m;

		if (Jp->WriteStr(pair->Key))
			return true;

		if (LocateValue(pair->Val))
			return true;

		} // endfor i

	return false;
} // end of LocateObject

/*********************************************************************************/
/*  Locate a JSON Value.                                                         */
/*********************************************************************************/
my_bool JSNX::LocateValue(PJVAL jvp)
{
	if (CompareTree(Jvalp, jvp)) {
		Found = (--K == 0);
	} else if (jvp->GetArray()) {
		if (Jp->WriteChr(':'))
			return true;

		return LocateArray(jvp->GetArray());
	} else if (jvp->GetObject()) {
		if (Jp->WriteChr(':'))
			return true;

		return LocateObject(jvp->GetObject());
	} // endif's

	return false;
} // end of LocateValue

/*********************************************************************************/
/*  Locate all occurrences of a value in a JSON tree:                            */
/*********************************************************************************/
PSZ JSNX::LocateAll(PGLOBAL g, PJSON jsp, PJVAL jvp, int mx)
{
	my_bool b = false, err = true;
	PJPN    jnp = (PJPN)PlugSubAlloc(g, NULL, sizeof(JPN) * mx);

	memset(jnp, 0, sizeof(JPN) * mx);
	g->Message[0] = 0;

	if (!jsp) {
		strcpy(g->Message, "Null json tree");
		return NULL;
	} // endif jsp

	// Write to the path string
	Jp = new(g)JOUTSTR(g);
	Jvalp = jvp;
	Imax = mx - 1;
	Jpnp = jnp;
	Jp->WriteChr('[');

	switch (jsp->GetType()) {
	case TYPE_JAR:
		err = LocateArrayAll((PJAR)jsp);
		break;
	case TYPE_JOB:
		err = LocateObjectAll((PJOB)jsp);
		break;
	case TYPE_JVAL:
		err = LocateValueAll((PJVAL)jsp);
		break;
	default:
		err = true;
	} // endswitch Type

	if (err) {
		if (!g->Message[0])
			strcpy(g->Message, "Invalid json tree");

	} else {
		if (Jp->N > 1)
			Jp->N--;

		Jp->WriteChr(']');
		Jp->WriteChr('\0');
		PlugSubAlloc(g, NULL, Jp->N);
		return Jp->Strp;
	} // endif's

	return NULL;
} // end of LocateAll

/*********************************************************************************/
/*  Locate in a JSON Array.                                                      */
/*********************************************************************************/
my_bool JSNX::LocateArrayAll(PJAR jarp)
{
	if (I < Imax) {
		Jpnp[++I].Type = TYPE_JAR;

		for (int i = 0; i < jarp->size(); i++) {
			Jpnp[I].N = i;

			if (LocateValueAll(jarp->GetValue(i)))
				return true;

		} // endfor i

		I--;
	} // endif I

	return false;
} // end of LocateArrayAll

/*********************************************************************************/
/*  Locate in a JSON Object.                                                     */
/*********************************************************************************/
my_bool JSNX::LocateObjectAll(PJOB jobp)
{
	if (I < Imax) {
		Jpnp[++I].Type = TYPE_JOB;

		for (PJPR pair = jobp->First; pair; pair = pair->Next) {
			Jpnp[I].Key = pair->Key;

			if (LocateValueAll(pair->Val))
				return true;

		} // endfor i

		I--;
	} // endif I

	return false;
} // end of LocateObjectAll

/*********************************************************************************/
/*  Locate a JSON Value.                                                         */
/*********************************************************************************/
my_bool JSNX::LocateValueAll(PJVAL jvp)
{
	if (CompareTree(Jvalp, jvp))
		return AddPath();
	else if (jvp->GetArray())
		return LocateArrayAll(jvp->GetArray());
	else if (jvp->GetObject())
		return LocateObjectAll(jvp->GetObject());

	return false;
} // end of LocateValueAll

/*********************************************************************************/
/*  Compare two JSON trees.                                                      */
/*********************************************************************************/
my_bool JSNX::CompareTree(PJSON jp1, PJSON jp2)
{
	if (!jp1 || !jp2 || jp1->GetType() != jp2->GetType()
		               || jp1->size() != jp2->size())
		return false;

	my_bool found = true;

	if (jp1->GetType() == TYPE_JVAL) {
		PVAL v1 = jp1->GetValue(), v2 = jp2->GetValue();

		if (v1 && v2) {
			if (v1->GetType() == v2->GetType())
				found = !v1->CompareValue(v2);
			else
				found = false;

		} else
			found = CompareTree(jp1->GetJsp(), jp2->GetJsp());

	} else if (jp1->GetType() == TYPE_JAR) {
		for (int i = 0; found && i < jp1->size(); i++)
			found = (CompareTree(jp1->GetValue(i), jp2->GetValue(i)));

	} else if (jp1->GetType() == TYPE_JOB) {
		PJPR p1 = jp1->GetFirst(), p2 = jp2->GetFirst();

		for (; found && p1 && p2; p1 = p1->Next, p2 = p2->Next)
			found = CompareTree(p1->Val, p2->Val);

	} else
		found = false;

	return found;
} // end of CompareTree

/*********************************************************************************/
/*  Add the found path to the list.                                              */
/*********************************************************************************/
my_bool JSNX::AddPath(void)
{
	char    s[16];
	my_bool b = false;

	if (Jp->WriteChr('"'))
		return true;

	for (int i = 0; i <= I; i++) {
		if (b) {
			if (Jp->WriteChr(':')) return true;
		} else
			b = true;

		if (Jpnp[i].Type == TYPE_JAR) {
			sprintf(s, "[%d]", Jpnp[i].N + B);

			if (Jp->WriteStr(s))
				return true;

		} else if (Jp->WriteStr(Jpnp[i].Key))
			return true;

	}	// endfor i

	if (Jp->WriteStr("\","))
		return true;

	return false;
}	// end of AddPath

/* --------------------------------- JSON UDF ---------------------------------- */

#define BMX (_MAX_PATH - 1)
typedef struct BSON *PBSON;

/*********************************************************************************/
/*  Structure used to return binary json.                                        */
/*********************************************************************************/
struct BSON {
	char    Msg[_MAX_PATH];
	char   *Filename;
	int     Pretty;
	ulong   Reslen;
	my_bool Changed;
	PJSON   Jsp;
	PBSON   Bsp;
}; // end of struct BSON

/*********************************************************************************/
/*  Allocate and initialize a BSON structure.                                    */
/*********************************************************************************/
static PBSON JbinAlloc(PGLOBAL g, UDF_ARGS *args, ulong len, PJSON jsp)
{
	PBSON bsp = (PBSON)PlugSubAlloc(g, NULL, sizeof(BSON));

	strcpy(bsp->Msg, "Binary Json");
	bsp->Msg[BMX] = 0;
	bsp->Filename = NULL;
	bsp->Pretty = 2;
	bsp->Reslen = len;
	bsp->Changed = false;
	bsp->Jsp = jsp;
	bsp->Bsp = (IsJson(args, 0) == 3) ? (PBSON)args->args[0] : NULL;
	return bsp;
} /* end of JbinAlloc */

/*********************************************************************************/
/*  Set the BSON chain as changed.                                               */
/*********************************************************************************/
static void SetChanged(PBSON bsp)
{
	if (bsp->Bsp)
		SetChanged(bsp->Bsp);

	bsp->Changed = true;
} /* end of SetChanged */

/*********************************************************************************/
/*  Program for SubSet re-initialization of the memory pool.                     */
/*********************************************************************************/
static my_bool JsonSubSet(PGLOBAL g)
{
	PPOOLHEADER pph = (PPOOLHEADER)g->Sarea;

	pph->To_Free = (OFFSET)((g->Createas) ? g->Createas : sizeof(POOLHEADER));
	pph->FreeBlk = g->Sarea_Size - pph->To_Free;
	return FALSE;
} /* end of JsonSubSet */

/*********************************************************************************/
/*  Program for saving the status of the memory pools.                           */
/*********************************************************************************/
inline void JsonMemSave(PGLOBAL g)
{
	g->Createas = (int)((PPOOLHEADER)g->Sarea)->To_Free;
} /* end of JsonMemSave */

/*********************************************************************************/
/*  Program for freeing the memory pools.                           */
/*********************************************************************************/
inline void JsonFreeMem(PGLOBAL g)
{
	PlugExit(g);
} /* end of JsonFreeMem */

/*********************************************************************************/
/*  Allocate and initialise the memory area.                                     */
/*********************************************************************************/
static my_bool JsonInit(UDF_INIT *initid, UDF_ARGS *args,
												char *message, my_bool mbn,
                        unsigned long reslen, unsigned long memlen)
{
  PGLOBAL g = PlugInit(NULL, memlen);

  if (!g) {
    strcpy(message, "Allocation error");
    return true;
  } else if (g->Sarea_Size == 0) {
		strcpy(message, g->Message);
		PlugExit(g);
		return true;
  } // endif g

	g->Mrr = (args->arg_count && args->args[0]) ? 1 : 0;
  initid->maybe_null = mbn;
  initid->max_length = reslen;
	initid->ptr = (char*)g;
	return false;
} // end of JsonInit

/*********************************************************************************/
/*  Check if a path was specified and set jvp according to it.                   */
/*********************************************************************************/
static my_bool CheckPath(PGLOBAL g, UDF_ARGS *args, PJSON top, PJVAL& jvp, int n)
{
	for (uint i = n; i < args->arg_count; i++)
		if (args->arg_type[i] == STRING_RESULT) {
			// A path to a subset of the json tree is given
			char *path = MakePSZ(g, args, i);
			PJSNX jsx = new(g)JSNX(g, top, TYPE_STRING);

			if (jsx->SetJpath(g, path))
				return true;

			if (!(jvp = jsx->GetJson(g))) {
				sprintf(g->Message, "No sub-item at '%s'", path);
				return true;
			} // endif jvp

			break;
		}	// endif type

	return false;
} // end of CheckPath

/*********************************************************************************/
/*  Make the result according to the first argument type.                        */
/*********************************************************************************/
static char *MakeResult(PGLOBAL g, UDF_ARGS *args, PJSON top, int n = 2)
{
	char *str;

	if (IsJson(args, 0) == 2) {
		// Make the change in the json file
		char *msg;
		int   pretty = 2;

		for (uint i = n; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT) {
				pretty = (int)*(longlong*)args->args[i];
				break;
			} // endif type

		if ((msg = Serialize(g, top, MakePSZ(g, args, 0), pretty)))
			PUSH_WARNING(msg);

		str = NULL;
	} else if (IsJson(args, 0) == 3) {
		PBSON bsp = (PBSON)args->args[0];

		if (bsp->Filename) {
			// Make the change in the json file
			char *msg;

			if ((msg = Serialize(g, top, bsp->Filename, bsp->Pretty)))
				PUSH_WARNING(msg);

			str = bsp->Filename;
		} else if (!(str = Serialize(g, top, NULL, 0)))
			PUSH_WARNING(g->Message);

		SetChanged(bsp);
	} else if (!(str = Serialize(g, top, NULL, 0)))
			PUSH_WARNING(g->Message);

	return str;
} // end of MakeResult

/*********************************************************************************/
/*  Make the binary result according to the first argument type.                 */
/*********************************************************************************/
static PBSON MakeBinResult(PGLOBAL g, UDF_ARGS *args, PJSON top, ulong len, int n = 2)
{
	PBSON bsnp = JbinAlloc(g, args, len, top);

	if (IsJson(args, 0) == 2) {
		int   pretty = 2;

		for (uint i = n; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT) {
				pretty = (int)*(longlong*)args->args[i];
				break;
			} // endif type

		bsnp->Pretty = pretty;
		bsnp->Filename = (char*)args->args[0];
		strncpy(bsnp->Msg, (char*)args->args[0], BMX);
	} else if (IsJson(args, 0) == 3) {
		PBSON bsp = (PBSON)args->args[0];

		if (bsp->Filename) {
			bsnp->Filename = bsp->Filename;
			strncpy(bsnp->Msg, bsp->Filename, BMX);
		} else
			strcpy(bsnp->Msg, "Json Binary item");

	} else
		strcpy(bsnp->Msg, "Json Binary item");

	return bsnp;
} // end of MakeBinResult

/*********************************************************************************/
/*  Returns not 0 if the argument is a JSON item or file name.                   */
/*********************************************************************************/
static int IsJson(UDF_ARGS *args, uint i)
{
	int n = 0;

	if (i >= args->arg_count || args->arg_type[i] != STRING_RESULT) {
	} else if (!strnicmp(args->attributes[i], "Json_", 5)) {
		if (!args->args[i] || *args->args[i] == '[' || *args->args[i] == '{')
			n = 1;					 // arg is a json item
		else
			n = 2;           // A file name may have been returned

	} else if (!strnicmp(args->attributes[i], "Jbin_", 5)) {
		if (args->lengths[i] == sizeof(BSON))
			n = 3;					 //	arg is a binary json item
		else
			n = 2;           // A file name may have been returned

	} else if (!strnicmp(args->attributes[i], "Jfile_", 6))
		n = 2;					   //	arg is a json file name

	return n;
} // end of IsJson

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

/*********************************************************************************/
/*  Calculate the reslen and memlen needed by a function.                        */
/*********************************************************************************/
static my_bool CalcLen(UDF_ARGS *args, my_bool obj,
                       unsigned long& reslen, unsigned long& memlen,
											 my_bool mod = false)
{
	char fn[_MAX_PATH];
  unsigned long i, k, m, n;
	long fl, j = -1;

  reslen = args->arg_count + 2;

  // Calculate the result max length
  for (i = 0; i < args->arg_count; i++) {
		n = IsJson(args, i);

    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      reslen += (k + 3);     // For quotes and :
      } // endif obj

    switch (args->arg_type[i]) {
      case STRING_RESULT:
				if (n == 2 && args->args[i]) {
					if (!mod) {
						m = MY_MIN(args->lengths[i], sizeof(fn) - 1);
						memcpy(fn, args->args[i], m);
						fn[m] = 0;
						j = i;
						fl = GetFileLength(fn);
						reslen += fl;
					} else
						reslen += args->lengths[i];

				} else if (n == 3 && args->args[i])
					reslen += ((PBSON)args->args[i])->Reslen;
				else if (n == 1)
          reslen += args->lengths[i];
				else
          reslen += (args->lengths[i] + 1) * 2;   // Pessimistic !
  
        break;
      case INT_RESULT:
        reslen += 20;
        break;
      case REAL_RESULT:
        reslen += 31;
        break;
      case DECIMAL_RESULT:
        reslen += (args->lengths[i] + 7);   // 6 decimals
        break;
      case TIME_RESULT:
      case ROW_RESULT:
      case IMPOSSIBLE_RESULT:
      default:
        // What should we do here ?
        break;
      } // endswitch arg_type

    } // endfor i

	// Calculate the amount of memory needed
	memlen = MEMFIX + sizeof(JOUTSTR) + reslen;

  for (i = 0; i < args->arg_count; i++) {
		n = IsJson(args, i);
		memlen += (args->lengths[i] + sizeof(JVALUE));

    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      memlen += (k + sizeof(JOBJECT) + sizeof(JPAIR));
    } else
      memlen += sizeof(JARRAY);

    switch (args->arg_type[i]) {
      case STRING_RESULT:
				if (n == 2 && args->args[i]) {
					if ((signed)i != j) {
						m = MY_MIN(args->lengths[i], sizeof(fn) - 1);
						memcpy(fn, args->args[i], m);
						fn[m] = 0;
						j = -1;
						fl = GetFileLength(fn);
					}	// endif i

					memlen += fl * M;
				} else if (IsJson(args, i) == 3)
					memlen += sizeof(JVALUE);
				else if (IsJson(args, i) == 1)
					memlen += args->lengths[i] * M;  // Estimate parse memory
  
        memlen += sizeof(TYPVAL<PSZ>);
        break;
      case INT_RESULT:
        memlen += sizeof(TYPVAL<int>);
        break;
      case REAL_RESULT:
      case DECIMAL_RESULT:
        memlen += sizeof(TYPVAL<double>);
        break;
      case TIME_RESULT:
      case ROW_RESULT:
      case IMPOSSIBLE_RESULT:
      default:
        // What should we do here ?
        break;
      } // endswitch arg_type

    } // endfor i

	return false;
} // end of CalcLen

/*********************************************************************************/
/*  Check if the calculated memory is enough.                                    */
/*********************************************************************************/
static my_bool CheckMemory(PGLOBAL g, UDF_INIT *initid, UDF_ARGS *args, 
	                         uint n, my_bool obj, my_bool mod = false)
{
	unsigned long rl, ml;

	n = MY_MIN(n, args->arg_count);

	for (uint i = 0; i < n; i++)
		if (IsJson(args, i) == 2) {
			if (CalcLen(args, obj, rl, ml, mod))
				return true;
			else if (ml > g->Sarea_Size) {
				free(g->Sarea);

				if (!(g->Sarea = PlugAllocMem(g, ml))) {
					char errmsg[256];

					sprintf(errmsg, MSG(WORK_AREA), g->Message);
					strcpy(g->Message, errmsg);
					g->Sarea_Size = 0;
					return true;
					} // endif Alloc

				g->Sarea_Size = ml;
				g->Createas = 0;
				g->Xchk = NULL;
				initid->max_length = rl;
			}	// endif Size

			break;
		} // endif IsJson

	JsonSubSet(g);
	return false;
} // end of CheckMemory

/*********************************************************************************/
/*  Make a zero terminated string from the passed argument.                      */
/*********************************************************************************/
static PSZ MakePSZ(PGLOBAL g, UDF_ARGS *args, int i)
{
	if (args->arg_count > (unsigned)i && args->args[i]) {
    int n = args->lengths[i];
    PSZ s = (PSZ)PlugSubAlloc(g, NULL, n + 1);

    memcpy(s, args->args[i], n);
    s[n] = 0;
    return s;
  } else
    return NULL;

} // end of MakePSZ

/*********************************************************************************/
/*  Make a valid key from the passed argument.                                   */
/*********************************************************************************/
static PSZ MakeKey(PGLOBAL g, UDF_ARGS *args, int i)
{
	if (args->arg_count > (unsigned)i) {
		int     j = 0, n = args->attribute_lengths[i];
		my_bool b;  // true if attribute is zero terminated
		PSZ     p, s = args->attributes[i];

		if (s && *s && (n || *s == '\'')) {
			if ((b = (!n || !s[n])))
				n = strlen(s);

			if (IsJson(args, i))
				j = strchr(s, '_') - s + 1;

			if (j && n > j) {
				s += j;
				n -= j;
			} else if (*s == '\'' && s[n-1] == '\'') {
				s++;
				n -= 2;
				b = false;
			} // endif *s

			if (n < 1)
				return "Key";

			if (!b) {
				p = (PSZ)PlugSubAlloc(g, NULL, n + 1);
				memcpy(p, s, n);
				p[n] = 0;
				s = p;
			} // endif b

		} // endif s

		return s;
	} // endif count

  return "Key";
} // end of MakeKey

/*********************************************************************************/
/*  Return a json file contains.                                                 */
/*********************************************************************************/
static char *GetJsonFile(PGLOBAL g, char *fn)
{
	char   *str;
	int     h, n, len;

	h= open(fn, _O_RDONLY, _O_TEXT);

	if (h == -1) {
		sprintf(g->Message, "Error %d opening %s", errno, fn);
		return NULL;
	} // endif h

	if ((len = _filelength(h)) < 0) {
		sprintf(g->Message, MSG(FILELEN_ERROR), "_filelength", fn);
		close(h);
		return NULL;
	} // endif len

	str = (char*)PlugSubAlloc(g, NULL, len + 1);
	
	if ((n = read(h, str, len)) < 0) {
		sprintf(g->Message, "Error %d reading %d bytes from %s", errno, len, fn);
		return NULL;
	} // endif n

	str[n] = 0;
	close(h);
	return str;
} // end of GetJsonFile

/*********************************************************************************/
/*  Make a JSON value from the passed argument.                                  */
/*********************************************************************************/
static PJVAL MakeValue(PGLOBAL g, UDF_ARGS *args, uint i)
{
	char *sap = (args->arg_count > i) ? args->args[i] : NULL;
	int   n, len;
	short c;
	long long bigint;
  PJSON jsp;
  PJVAL jvp = new(g) JVALUE;

  if (sap) switch (args->arg_type[i]) {
    case STRING_RESULT:
      if ((len = args->lengths[i])) {
				if ((n = IsJson(args, i)) < 3)
					sap = MakePSZ(g, args, i);

        if (n) {
					if (n == 3) {
						jsp = ((PBSON)sap)->Jsp;
					} else {
						if (n == 2) {
							if (!(sap = GetJsonFile(g, sap)))
								PUSH_WARNING(g->Message);

							len = (sap) ? strlen(sap) : 0;
						} // endif n

						if (!(jsp = ParseJson(g, sap, len, 3)))
							PUSH_WARNING(g->Message);
					} // endif's n

          if (jsp && jsp->GetType() == TYPE_JVAL)
            jvp = (PJVAL)jsp;
          else
            jvp->SetValue(jsp);

				} else {
					c = (strnicmp(args->attributes[i], "ci", 2)) ? 0 : 1;
					jvp->SetString(g, sap, c);
				}	// endif n

      } // endif len

      break;
    case INT_RESULT:
			bigint = *(long long*)sap;

			if (bigint > INT_MAX32 || bigint < INT_MIN32)
				jvp->SetFloat(g, (double)bigint);
			else
				jvp->SetInteger(g, (int)bigint);

      break;
    case REAL_RESULT:
      jvp->SetFloat(g, *(double*)sap);
      break;
    case DECIMAL_RESULT:
      jvp->SetFloat(g, atof(MakePSZ(g, args, i)));
      break;
    case TIME_RESULT:
    case ROW_RESULT:
    case IMPOSSIBLE_RESULT:
    default:
      break;
    } // endswitch arg_type

  return jvp;
} // end of MakeValue

/*********************************************************************************/
/*  Make a Json value containing the parameter.                                  */
/*********************************************************************************/
my_bool JsonValue_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  if (args->arg_count > 1) {
    strcpy(message, "JsonValue cannot accept more than 1 argument");
    return true;
  } else
    CalcLen(args, false, reslen, memlen);

  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of JsonValue_init

char *JsonValue(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                 unsigned long *res_length, char *, char *)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, 1, false)) {
			PJVAL jvp = MakeValue(g, args, 0);

			if (!(str = Serialize(g, jvp, NULL, 0)))
				str = strcpy(result, g->Message);

		} else
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of JsonValue

void JsonValue_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of JsonValue_deinit

/*********************************************************************************/
/*  Make a Json array containing all the parameters.                             */
/*********************************************************************************/
my_bool Json_Array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, false, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_Array_init

char *Json_Array(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                 unsigned long *res_length, char *, char *)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			PJAR arp = new(g)JARRAY;

			for (uint i = 0; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);

			if (!(str = Serialize(g, arp, NULL, 0)))
				str = strcpy(result, g->Message);

		} else
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Array

void Json_Array_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Array_deinit

/*********************************************************************************/
/*  Add one or several values to a Json array.                                   */
/*********************************************************************************/
my_bool Json_Array_Add_Values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Array_Add must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Array_Add first argument must be a json string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Array_Add_Values_init

char *Json_Array_Add_Values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			PJAR  arp;
			PJVAL jvp = MakeValue(g, args, 0);
			
			if (jvp->GetValType() != TYPE_JAR) {
				arp = new(g)JARRAY;
				arp->AddValue(g, jvp);
			} else
				arp = jvp->GetArray();

			for (uint i = 1; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);
			str = Serialize(g, arp, NULL, 0);
		} // endif CheckMemory

		if (!str) {
			PUSH_WARNING(g->Message);
			str = args->args[0];
		}	// endif str

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	if (!str) {
		*res_length = 0;
		*is_null = 1;
	}	else
		*res_length = strlen(str);

	return str;
} // end of Json_Array_Add_Values

void Json_Array_Add_Values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Array_Add_Values_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool Json_Array_Add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Array_Add must have at least 2 arguments");
    return true;
  } else if (!IsJson(args, 0)) {
    strcpy(message, "Json_Array_Add first argument must be a json item");
    return true;
	} else
    CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Array_Add_init

char *Json_Array_Add(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                     unsigned long *res_length, char *, char *error)
{
	char   *str = NULL;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		*res_length = strlen(str);
		return str;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		int  *x = NULL, n = 2;
		PJSON top;
		PJVAL jvp;
		PJAR  arp;

		jvp = MakeValue(g, args, 0);
		top = jvp->GetJson();

		if (args->arg_count > 2) {
			if (args->arg_type[2] == INT_RESULT) {
				x = (int*)PlugSubAlloc(g, NULL, sizeof(int));
				*x = (int)*(longlong*)args->args[2];
				n = 3;
			} else if (!args->args[2])
				n = 3;

		} // endif count

		if (CheckPath(g, args, top, jvp, n))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			arp = jvp->GetArray();
			arp->AddValue(g, MakeValue(g, args, 1), x);
			arp->InitArray(g);
			str = MakeResult(g, args, top, n);
		} else {
			PUSH_WARNING("First argument is not an array");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Array_Add

void Json_Array_Add_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Array_Add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool Json_Array_Delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

	if (args->arg_count < 2) {
    strcpy(message, "Json_Array_Delete must have at lest 2 arguments");
    return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Array_Delete first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != INT_RESULT) {
		strcpy(message, "Json_Array_Delete second argument is not an integer (index)");
		return true;
	} else
    CalcLen(args, false, reslen, memlen, true);

  return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Array_Delete_init

char *Json_Array_Delete(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                        unsigned long *res_length, char *, char *error)
{
	char   *str = NULL;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		*res_length = strlen(str);
		return str;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, true)) {
		int   n;
		PJAR  arp;
		PJVAL jvp = MakeValue(g, args, 0);
		PJSON top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			n = *(int*)args->args[1];
			arp = jvp->GetArray();
			arp->DeleteValue(n);
			arp->InitArray(g);
			str = MakeResult(g, args, top);
		} else {
			PUSH_WARNING("First argument is not an array");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Array_Delete

void Json_Array_Delete_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Array_Delete_deinit

/*********************************************************************************/
/*  Make a Json Oject containing all the parameters.                             */
/*********************************************************************************/
my_bool Json_Object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, true, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_Object_init

char *Json_Object(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                  unsigned long *res_length, char *, char *)
{
  char   *str = NULL;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			PJOB objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				objp->SetValue(g, MakeValue(g, args, i), MakeKey(g, args, i));

			str = Serialize(g, objp, NULL, 0);
		} // endif CheckMemory

		if (!str)
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Object

void Json_Object_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_deinit

/*********************************************************************************/
/*  Make a Json Oject containing all not null parameters.                        */
/*********************************************************************************/
my_bool Json_Object_Nonull_init(UDF_INIT *initid, UDF_ARGS *args,
                                char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, true, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_Object_Nonull_init

char *Json_Object_Nonull(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                         unsigned long *res_length, char *, char *)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			PJVAL jvp;
			PJOB  objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				if (!(jvp = MakeValue(g, args, i))->IsNull())
					objp->SetValue(g, jvp, MakeKey(g, args, i));

			str = Serialize(g, objp, NULL, 0);
		} // endif CheckMemory

		if (!str)
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Object_Nonull

void Json_Object_Nonull_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_nonull_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool Json_Object_Add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Object_Add must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Object_Add first argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Object_Add_init

char *Json_Object_Add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                    unsigned long *res_length, char *, char *error)
{
	char   *key, *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		*res_length = strlen(str);
		return str;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0);
		PJSON top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			jobp = jvp->GetObject();
			jvp = MakeValue(g, args, 1);
			key = MakeKey(g, args, 1);
			jobp->SetValue(g, jvp, key);
			str = MakeResult(g, args, top);
		} else {
			PUSH_WARNING("First argument is not an object");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Object_Add

void Json_Object_Add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_Add_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool Json_Object_Delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Object_Delete must have 2 or 3 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Object_Delete first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Json_Object_Delete second argument must be a key string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Object_Delete_init

char *Json_Object_Delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
												 unsigned long *res_length, char *, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		*res_length = strlen(str);
		return str;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 1, false, true)) {
		char *key;
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0);
		PJSON top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			key = MakeKey(g, args, 1);
			jobp = jvp->GetObject();
			jobp->DeleteKey(key);
			str = MakeResult(g, args, top);
		} else {
			PUSH_WARNING("First argument is not an object");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Object_Delete

void Json_Object_Delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_Delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool Json_Object_List_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "Json_Object_List must have 1 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Object_List argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Object_List_init

char *Json_Object_List(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->N) {
		if (!CheckMemory(g, initid, args, 1, false)) {
			PJVAL jvp = MakeValue(g, args, 0);

			if (jvp && jvp->GetValType() == TYPE_JOB) {
				PJOB jobp = jvp->GetObject();
				PJAR jarp = jobp->GetKeyList(g);

				if (!(str = Serialize(g, jarp, NULL, 0)))
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
} // end of Json_Object_List

void Json_Object_List_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_List_deinit

/*********************************************************************************/
/*  Make a Json array from values coming from rows.                              */
/*********************************************************************************/
my_bool Json_Array_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 1) {
    strcpy(message, "Json_Array_Grp can only accept 1 argument");
    return true;
	} else if (IsJson(args, 0) == 3) {
		strcpy(message, "Json_Array_Grp does not support Jbin argument");
		return true;
	} else
    CalcLen(args, false, reslen, memlen);
  
  reslen *= n;
  memlen += ((memlen - MEMFIX) * (n - 1));

  if (JsonInit(initid, args, message, false, reslen, memlen))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  g->N = (int)n;
  return false;
} // end of Json_Array_Grp_init

void Json_Array_Grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  if (g->N-- > 0)
    arp->AddValue(g, MakeValue(g, args, 0));

} // end of Json_Array_Grp_add

char *Json_Array_Grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
                     unsigned long *res_length, char *, char *)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  if (g->N < 0)
    PUSH_WARNING("Result truncated to json_grp_size values");

  arp->InitArray(g);

  if (!(str = Serialize(g, arp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Array_Grp

void Json_Array_Grp_clear(UDF_INIT *initid, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  g->N = GetJsonGrpSize();
} // end of Json_Array_Grp_clear

void Json_Array_Grp_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Array_Grp_deinit

/*********************************************************************************/
/*  Make a Json object from values coming from rows.                             */
/*********************************************************************************/
my_bool Json_Object_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 2) {
    strcpy(message, "Json_Object_Grp can only accept 2 arguments");
    return true;
	} else if (IsJson(args, 0) == 3) {
		strcpy(message, "Json_Object_Grp does not support Jbin arguments");
		return true;
	} else
    CalcLen(args, true, reslen, memlen);
  
  reslen *= n;
  memlen += ((memlen - MEMFIX) * (n - 1));

  if (JsonInit(initid, args, message, false, reslen, memlen))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  g->N = (int)n;
  return false;
} // end of Json_Object_Grp_init

void Json_Object_Grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

  if (g->N-- > 0)
    objp->SetValue(g, MakeValue(g, args, 0), MakePSZ(g, args, 1));

} // end of Json_Object_Grp_add

char *Json_Object_Grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
                      unsigned long *res_length, char *, char *)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

  if (g->N < 0)
    PUSH_WARNING("Result truncated to json_grp_size values");

  if (!(str = Serialize(g, objp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Object_Grp

void Json_Object_Grp_clear(UDF_INIT *initid, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  g->N = GetJsonGrpSize();
} // end of Json_Object_Grp_clear

void Json_Object_Grp_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Object_Grp_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool Json_Item_Merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Item_Merge must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Json_Item_Merge first argument must be a json item");
		return true;
	} else if (!IsJson(args, 1)) {
		strcpy(message, "Json_Item_Merge second argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Item_Merge_init

char *Json_Item_Merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		*res_length = strlen(str);
		return str;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		PJSON top;
		PJVAL jvp;
		PJSON jsp[2] = {NULL, NULL};

		for (int i = 0; i < 2; i++) {
			jvp = MakeValue(g, args, i);
			if (!i) top = jvp->GetJson();

			if (jvp->GetValType() != TYPE_JAR && jvp->GetValType() != TYPE_JOB) {
				sprintf(g->Message, "Argument %d is not an array or object", i);
				PUSH_WARNING(g->Message);
			} else
				jsp[i] = jvp->GetJsp();

		} // endfor i

		if (jsp[0]) {
			if (jsp[0]->Merge(g, jsp[1]))
				PUSH_WARNING(g->Message);
			else
				str = MakeResult(g, args, top);

		} // endif jsp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Item_Merge

void Json_Item_Merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Item_Merge_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool Json_Get_Item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;
	int n = IsJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "Json_Get_Item must have at least 2 arguments");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Json_Get_Item first argument must be a json item");
		return true;
  } else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (jpath)");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (n == 2) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Get_Item_init

char *Json_Get_Item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	             unsigned long *res_length, char *is_null, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p, *path;
		PJSON jsp;
		PJSNX jsx;
		PJVAL jvp;

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);

					return NULL;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
				} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		path = MakePSZ(g, args, 1);
		jsx = new(g)JSNX(g, jsp, TYPE_STRING, initid->max_length);

		if (jsx->SetJpath(g, path, true)) {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return NULL;
		}	// endif SetJpath

		jsx->ReadValue(g);

		if (!jsx->GetValue()->IsNull())
			str = jsx->GetValue()->GetCharValue();

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = str;

	} // endif CheckMemory

 fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of Json_Get_Item

void Json_Get_Item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Get_Item_deinit

/*********************************************************************************/
/*  Get a string value from a Json item.                                         */
/*********************************************************************************/
my_bool JsonGetString_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;
	int n = IsJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "JsonGetString must have at least 2 arguments");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "JsonGetString first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (jpath)");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (n == 2) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of JsonGetString_init

char *JsonGetString(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p, *path;
		PJSON jsp;
		PJSNX jsx;
		PJVAL jvp;

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);

					return NULL;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		path = MakePSZ(g, args, 1);
		jsx = new(g)JSNX(g, jsp, TYPE_STRING, initid->max_length);

		if (jsx->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return NULL;
		}	// endif SetJpath

		jsx->ReadValue(g);

		if (!jsx->GetValue()->IsNull())
			str = jsx->GetValue()->GetCharValue();

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = str;

	} // endif CheckMemory

fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of JsonGetString

void JsonGetString_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of JsonGetString_deinit

/*********************************************************************************/
/*  Get an integer value from a Json item.                                       */
/*********************************************************************************/
my_bool JsonGetInt_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 2) {
		strcpy(message, "JsonGetInt must have 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "JsonGetInt first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of JsonGetInt_init

long long JsonGetInt(UDF_INIT *initid, UDF_ARGS *args,
	                     char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (!g->Xchk) {
			*is_null = 1;
			return 0LL;
		} else
			return *(long long*)g->Xchk;

	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p, *path;
		long long n;
		PJSON jsp;
		PJSNX jsx;
		PJVAL jvp;

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					if (g->Mrr) *error = 1;
					*is_null = 1;
					return 0;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		path = MakePSZ(g, args, 1);
		jsx = new(g)JSNX(g, jsp, TYPE_BIGINT);

		if (jsx->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return 0;
		} // endif SetJpath

		jsx->ReadValue(g);

		if (jsx->GetValue()->IsNull()) {
			PUSH_WARNING("Value not found");
			*is_null = 1;
			return 0;
		}	// endif IsNull

		n = jsx->GetValue()->GetBigintValue();

		if (initid->const_item) {
			// Keep result of constant function
			long long *np = (long long*)PlugSubAlloc(g, NULL, sizeof(long long));
			*np = n;
			g->Xchk = np;
		} // endif const_item

		return n;
	} // endif CheckMemory

	if (g->Mrr) *error = 1;
	*is_null = 1;
	return 0LL;
} // end of JsonGetInt

void JsonGetInt_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of JsonGetInt_deinit

/*********************************************************************************/
/*  Get a double value from a Json item.                                         */
/*********************************************************************************/
my_bool JsonGetReal_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "JsonGetReal must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "JsonGetReal first argument must be a json item");
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

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of JsonGetReal_init

double JsonGetReal(UDF_INIT *initid, UDF_ARGS *args,
	                   char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (!g->Xchk) {
			*is_null = 1;
			return 0.0;
		} else
			return *(double*)g->Xchk;

	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char  *p, *path;
		double d;
		PJSON  jsp;
		PJSNX  jsx;
		PJVAL  jvp;

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					*is_null = 1;
					return 0.0;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		path = MakePSZ(g, args, 1);
		jsx = new(g)JSNX(g, jsp, TYPE_DOUBLE);

		if (jsx->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return 0.0;
		}	// endif SetJpath

		jsx->ReadValue(g);

		if (jsx->GetValue()->IsNull()) {
			PUSH_WARNING("Value not found");
			*is_null = 1;
			return 0.0;
		}	// endif IsNull

		d = jsx->GetValue()->GetFloatValue();

		if (initid->const_item) {
			// Keep result of constant function
			double *dp = (double*)PlugSubAlloc(g, NULL, sizeof(double));
			*dp = d;
			g->Xchk = dp;
		} // endif const_item

		return d;
	} // endif CheckMemory

	if (g->Mrr) *error = 1;
	*is_null = 1;
	return 0.0;
} // end of JsonGetReal

void JsonGetReal_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of JsonGetReal_deinit

/*********************************************************************************/
/*  Locate a value in a Json tree.                                               */
/*********************************************************************************/
my_bool JsonLocate_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Locate must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Json_Locate first argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (rank)");
		return true;
	} else if (args->arg_count > 3)
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more = (ulong)*(longlong*)args->args[2];

	CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of JsonLocate_init

char *JsonLocate(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                unsigned long *res_length, char *is_null, char *error)
{
	char   *path = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (g->Xchk) {
			path = (char*)g->Xchk;
			*res_length = strlen(path);
			return path;
		} else {
			*res_length = 0;
			*is_null = 1;
			return NULL;
		}	// endif Xchk

	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p;
		int   k, rc;
		PJVAL jvp, jvp2;
		PJSON jsp;
		PJSNX jsx;

		// Save stack and allocation environment and prepare error return
		if (g->jump_level == MAX_JUMP) {
			PUSH_WARNING(MSG(TOO_MANY_JUMPS));
			*error = 1;
			*is_null = 1;
			return NULL;
		} // endif jump_level

		if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
			PUSH_WARNING(g->Message);
			*error = 1;
			path = NULL;
			goto err;
		} // endif rc

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					goto err;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		// The item to locate
		jvp2 = MakeValue(g, args, 1);

		k = (args->arg_count > 2) ? (int)*(long long*)args->args[2] : 1;

		jsx = new(g)JSNX(g, jsp, TYPE_STRING);
		path = jsx->Locate(g, jsp, jvp2, k);

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = path;

	err:
		g->jump_level--;

		if (!path) {
			*res_length = 0;
			*is_null = 1;
		}	else
			*res_length = strlen(path);

		return path;
	} // endif CheckMemory

	*error = 1;
	*is_null = 1;
	return NULL;
} // end of JsonLocate

void Json_Locate_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of JsonLocate_deinit

/*********************************************************************************/
/*  Locate all occurences of a value in a Json tree.                             */
/*********************************************************************************/
my_bool Json_Locate_All_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Locate_All must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Json_Locate_All first argument must be a json item");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (Depth)");
		return true;
	} else if (args->arg_count > 3)
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more = (ulong)*(longlong*)args->args[2];

		CalcLen(args, false, reslen, memlen);

		if (IsJson(args, 0) != 3)
			memlen += 1000;       // TODO: calculate this

		return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Locate_All_init

char *Json_Locate_All(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *path = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		if (g->Xchk) {
			path = (char*)g->Xchk;
			*res_length = strlen(path);
			return path;
		} else {
			*error = 1;
			*res_length = 0;
			*is_null = 1;
			return NULL;
		}	// endif Xchk

	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p;
		int   rc, mx = 10;
		PJVAL jvp, jvp2;
		PJSON jsp;
		PJSNX jsx;

		// Save stack and allocation environment and prepare error return
		if (g->jump_level == MAX_JUMP) {
			PUSH_WARNING(MSG(TOO_MANY_JUMPS));
			*error = 1;
			*is_null = 1;
			return NULL;
		} // endif jump_level

		if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
			PUSH_WARNING(g->Message);
			*error = 1;
			path = NULL;
			goto err;
		} // endif rc

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					goto err;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		// The item to locate
		jvp2 = MakeValue(g, args, 1);

		if (args->arg_count > 2)
			mx = (int)*(long long*)args->args[2];

		jsx = new(g)JSNX(g, jsp, TYPE_STRING);
		path = jsx->LocateAll(g, jsp, jvp2, mx);

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = path;

	err:
		g->jump_level--;

		if (!path) {
			*res_length = 0;
			*is_null = 1;
		} else
			*res_length = strlen(path);

		return path;
	} // endif CheckMemory

	*error = 1;
	*is_null = 1;
	return NULL;
} // end of Json_Locate_All

void Json_Locate_All_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Locate_All_deinit

/*********************************************************************************/
/*  Returns a json file as a json string.                                        */
/*********************************************************************************/
my_bool Json_File_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, fl, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 4) {
		strcpy(message, "Json_File only accepts 1 to 4 arguments");
		return true;
	} else if (args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Json_File first argument must be a (string) file name");
		return true;
	} else if (args->arg_count > 1 && args->arg_type[1] != INT_RESULT) {
		strcpy(message, "Second argument is not an integer (check)");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (pretty)");
		return true;
	} else if (args->arg_count > 3) {
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more += (ulong)*(longlong*)args->args[2];

	} // endifs

	initid->maybe_null = 1;
	CalcLen(args, false, reslen, memlen);
	fl = GetFileLength(args->args[0]);

	if (initid->const_item)
		more += fl;

	if (args->arg_count > 1 && *(longlong*)args->args[1])
		more += fl * M;

  memlen += more;
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_File_init

char *Json_File(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str, *fn;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	PlugSubSet(g, g->Sarea, g->Sarea_Size);
	fn = MakePSZ(g, args, 0);

	if (args->arg_count > 1 && *(longlong*)args->args[1]) {
		char  *memory;
		int    len, pretty;
		HANDLE hFile;
		MEMMAP mm;
		PJSON  jsp;

		pretty = (args->arg_count > 2) ? (int)*(longlong*)args->args[2] : 3;

		/*****************************************************************************/
		/*  Create the mapping file object.                                          */
		/*****************************************************************************/
		hFile = CreateFileMap(g, fn, &mm, MODE_READ, false);

		if (hFile == INVALID_HANDLE_VALUE) {
			DWORD rc = GetLastError();

			if (!(*g->Message))
				sprintf(g->Message, MSG(OPEN_MODE_ERROR), "map", (int)rc, fn);

			PUSH_WARNING(g->Message);
			if (g->Mrr) *error = 1;
			*is_null = 1;
			return NULL;
		} // endif hFile

		/*****************************************************************************/
		/*  Get the file size (assuming file is smaller than 4 GB)                   */
		/*****************************************************************************/
		len = mm.lenL;
		memory = (char *)mm.memory;

		if (!len) {              // Empty or deleted file
			CloseFileHandle(hFile);
			*is_null = 1;
			return NULL;
		} // endif len

		if (!memory) {
			CloseFileHandle(hFile);
			sprintf(g->Message, MSG(MAP_VIEW_ERROR), fn, GetLastError());
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return NULL;
		} // endif Memory

		CloseFileHandle(hFile);                    // Not used anymore
		hFile = INVALID_HANDLE_VALUE;              // For Fblock

		/*******************************************************************************/
		/*  Parse the json file and allocate its tree structure.                       */
		/*******************************************************************************/
		g->Message[0] = 0;

		if (!(jsp = ParseJson(g, memory, len, pretty))) {
			PUSH_WARNING(g->Message);
			str = NULL;
		} // endif jsp

		CloseMemMap(memory, len);

		if (jsp && !(str = Serialize(g, jsp, NULL, 0)))
			PUSH_WARNING(g->Message);

	} else
		str = GetJsonFile(g, fn);

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
} // end of Json_File

void Json_File_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_File_deinit

/*********************************************************************************/
/*  Make a json file from a json item.                                           */
/*********************************************************************************/
my_bool Jfile_Make_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;

	if (args->arg_count < 2 || args->arg_count > 3) {
		strcpy(message, "JsonMakeFile only accepts 2 or 3 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "JsonMakeFile first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument must be a (string) file name");
		return true;
	} else if (args->arg_count > 2 && args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (pretty)");
		return true;
	} // endifs

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jfile_Make_init

char *Jfile_Make(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *str, *fn, *msg;
	int     pretty;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	PlugSubSet(g, g->Sarea, g->Sarea_Size);

	if (!g->Xchk) {
		jvp = MakeValue(g, args, 0);

		if (g->Mrr) {			 // First argument is a constant
			g->Xchk = jvp;
			JsonMemSave(g);
		} // endif Mrr

	} else
		jvp = (PJVAL)g->Xchk;

	fn = MakePSZ(g, args, 1);
	pretty = (args->arg_count > 2) ? (int)*(longlong*)args->args[2] : 2;

	if ((msg = Serialize(g, jvp->GetJson(), fn, pretty)))
		PUSH_WARNING(msg);

	str= fn;

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = str;

 fin:
	if (!str) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(str);

	return str;
} // end of Jfile_Make

void Jfile_Make_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jfile_Make_deinit

/*********************************************************************************/
/*  Make and return a binary Json array containing all the parameters.           */
/*********************************************************************************/
my_bool Jbin_Array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Jbin_Array_init

char *Jbin_Array(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			PJAR  arp = new(g)JARRAY;

			for (uint i = 0; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);
			bsp = JbinAlloc(g, args, initid->max_length, arp);
			strcat(bsp->Msg, " array");
		} else {
			bsp = JbinAlloc(g, args, initid->max_length, NULL);
			strncpy(bsp->Msg, g->Message, 139);
		}	// endif CheckMemory

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Array

void Jbin_Array_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Array_deinit

/*********************************************************************************/
/*  Add one or several values to a Json array.                                   */
/*********************************************************************************/
my_bool Jbin_Array_Add_Values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Array_Add must have at least 2 arguments");
		return true;
	} else if (IsJson(args, 0) != 1) {
		strcpy(message, "Jbin_Array_Add first argument must be a json string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Array_Add_Values_init

char *Jbin_Array_Add_Values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			PJAR  arp;
			PJVAL jvp = MakeValue(g, args, 0);

			if (jvp->GetValType() != TYPE_JAR) {
				arp = new(g)JARRAY;
				arp->AddValue(g, jvp);
			} else
				arp = jvp->GetArray();

			for (uint i = 1; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);
			bsp = JbinAlloc(g, args, initid->max_length, arp);
			strcat(bsp->Msg, " array");
		} else {
			bsp = JbinAlloc(g, args, initid->max_length, NULL);
			strncpy(bsp->Msg, g->Message, BMX);
		}	// endif CheckMemory

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Array_Add_Values

void Jbin_Array_Add_Values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Array_Add_Values_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool Jbin_Array_Add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Array_Add must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Array_Add first argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Array_Add_init

char *Jbin_Array_Add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	int     n = 2;
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		int  *x = NULL;
		PJVAL jvp;
		PJAR  arp;

		jvp = MakeValue(g, args, 0);
		top = jvp->GetJson();

		if (args->arg_count > 2) {
			if (args->arg_type[2] == INT_RESULT) {
				x = (int*)PlugSubAlloc(g, NULL, sizeof(int));
				*x = (int)*(longlong*)args->args[2];
				n = 3;
			} else if (!args->args[2])
				n = 3;

		} // endif count

		if (CheckPath(g, args, top, jvp, n))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			arp = jvp->GetArray();
			arp->AddValue(g, MakeValue(g, args, 1), x);
			arp->InitArray(g);
		} else {
			PUSH_WARNING("First argument is not an array");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length, n);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Array_Add

void Jbin_Array_Add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Array_Add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool Jbin_Array_Delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Array_Delete must have at lest 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Array_Delete first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != INT_RESULT) {
		strcpy(message, "Jbin_Array_Delete second argument is not an integer (index)");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Array_Delete_init

char *Jbin_Array_Delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 1, false, true)) {
		int   n;
		PJAR  arp;
		PJVAL jvp = MakeValue(g, args, 0);

		top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			n = *(int*)args->args[1];
			arp = jvp->GetArray();
			arp->DeleteValue(n);
			arp->InitArray(g);
		} else {
			PUSH_WARNING("First argument is not an array");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Array_Delete

void Jbin_Array_Delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Array_Delete_deinit

/*********************************************************************************/
/*  Make a Json Oject containing all the parameters.                             */
/*********************************************************************************/
my_bool Jbin_Object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Jbin_Object_init

char *Jbin_Object(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			PJOB objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				objp->SetValue(g, MakeValue(g, args, i), MakeKey(g, args, i));

			bsp = JbinAlloc(g, args, initid->max_length, objp);
			strcat(bsp->Msg, " object");
		} else {
			bsp = JbinAlloc(g, args, initid->max_length, NULL);
			strncpy(bsp->Msg, g->Message, BMX);
		} // endif CheckMemory

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Object

void Jbin_Object_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Object_deinit

/*********************************************************************************/
/*  Make a Json Oject containing all not null parameters.                        */
/*********************************************************************************/
my_bool Jbin_Object_Nonull_init(UDF_INIT *initid, UDF_ARGS *args,	char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Jbin_Object_Nonull_init

char *Jbin_Object_Nonull(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			PJVAL jvp;
			PJOB  objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				if (!(jvp = MakeValue(g, args, i))->IsNull())
					objp->SetValue(g, jvp, MakeKey(g, args, i));

			bsp = JbinAlloc(g, args, initid->max_length, objp);
			strcat(bsp->Msg, " object");
		} else {
			bsp = JbinAlloc(g, args, initid->max_length, NULL);
			strncpy(bsp->Msg, g->Message, BMX);
		} // endif CheckMemory

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Object_Nonull

void Jbin_Object_Nonull_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Object_nonull_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool Jbin_Object_Add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Object_Add must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Object_Add first argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Object_Add_init

char *Jbin_Object_Add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		char *key;
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0);

		top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			jobp = jvp->GetObject();
			jvp = MakeValue(g, args, 1);
			key = MakeKey(g, args, 1);
			jobp->SetValue(g, jvp, key);
		} else {
			PUSH_WARNING("First argument is not an object");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Object_Add

void Jbin_Object_Add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Object_Add_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool Jbin_Object_Delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Object_Delete must have 2 or 3 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Object_Delete first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Jbin_Object_Delete second argument must be a key string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Object_Delete_init

char *Jbin_Object_Delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		bsp = (PBSON)g->Xchk;
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 1, false, true)) {
		char *key;
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0);

		top = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			key = MakeKey(g, args, 1);
			jobp = jvp->GetObject();
			jobp->DeleteKey(key);
		} else {
			PUSH_WARNING("First argument is not an object");
			if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Object_Delete

void Jbin_Object_Delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Object_Delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool Jbin_Object_List_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "Jbin_Object_List must have 1 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Object_List argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Jbin_Object_List_init

char *Jbin_Object_List(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PJAR    jarp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, 1, false)) {
			PJVAL jvp = MakeValue(g, args, 0);

			if (jvp && jvp->GetValType() == TYPE_JOB) {
				PJOB jobp = jvp->GetObject();

				jarp = jobp->GetKeyList(g);
			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jvp

		} // endif CheckMemory

		bsp = JbinAlloc(g, args, initid->max_length, jarp);
		strcat(bsp->Msg, " array");

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? bsp : NULL;
	} // endif bsp

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Object_List

void Jbin_Object_List_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Object_List_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool Jbin_Get_Item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;
	int n = IsJson(args, 0);

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Get_Item must have at least 2 arguments");
		return true;
	} else if (!n && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Jbin_Get_Item first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (jpath)");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (n == 2) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Get_Item_init

char *Jbin_Get_Item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = NULL;

	if (g->N) {
		bsp = (PBSON)g->Xchk;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p, *path;
		PJSON jsp;
		PJSNX jsx;
		PJVAL jvp;

		if (!g->Xchk) {
			jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);

					return NULL;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (g->Mrr) {			 // First argument is a constant
				g->Xchk = jsp;
				JsonMemSave(g);
			} // endif Mrr

		} else
			jsp = (PJSON)g->Xchk;

		path = MakePSZ(g, args, 1);
		jsx = new(g)JSNX(g, jsp, TYPE_STRING, initid->max_length);

		if (jsx->SetJpath(g, path, true)) {
			PUSH_WARNING(g->Message);
			*is_null = 1;
			return NULL;
		}	// endif SetJpath

		// Get the json tree
		jvp = jsx->GetValue(g, jsp, 0, false);

		if (jvp->GetJsp()) {
			bsp = JbinAlloc(g, args, initid->max_length, jvp->GetJsp());
			strcat(bsp->Msg, " item");
		} // end of Jsp

		if (initid->const_item)
			// Keep result of constant function
			g->Xchk = bsp;

	} // endif CheckMemory

fin:
	if (!bsp) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of Jbin_Get_Item

void Jbin_Get_Item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Get_Item_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool Jbin_Item_Merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Jbin_Item_Merge must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "Jbin_Item_Merge first argument must be a json item");
		return true;
	} else if (!IsJson(args, 1)) {
		strcpy(message, "Jbin_Item_Merge second argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Jbin_Item_Merge_init

char *Jbin_Item_Merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 2, false, true)) {
		PJVAL jvp;
		PJSON jsp[2] ={ NULL, NULL };

		for (int i = 0; i < 2; i++) {
			jvp = MakeValue(g, args, i);
			if (!i) top = jvp->GetJson();

			if (jvp->GetValType() != TYPE_JAR && jvp->GetValType() != TYPE_JOB) {
				sprintf(g->Message, "Argument %d is not an array or object", i);
				PUSH_WARNING(g->Message);
			} else
				jsp[i] = jvp->GetJsp();

		} // endfor i

		if (jsp[0]) {
			if (jsp[0]->Merge(g, jsp[1]))
				PUSH_WARNING(g->Message);
			else
				MakeBinResult(g, args, top, initid->max_length);

		} // endif jsp

	} // endif CheckMemory

	// In case of error unchanged first argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	*res_length = sizeof(BSON);
	return (char*)bsp;
} // end of Jbin_Item_Merge

void Jbin_Item_Merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_Item_Merge_deinit

/*********************************************************************************/
/*  Returns a json file as a json item.                                          */
/*********************************************************************************/
my_bool Jbin_File_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, fl, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 3) {
		strcpy(message, "Jbin_File only accepts 1 to 3 arguments");
		return true;
	} else if (args->arg_type[0] != STRING_RESULT || !args->args[0]) {
		strcpy(message, "Jbin_File first argument must be a constant string (file name)");
		return true;
	} else if (args->arg_count > 1 && args->arg_type[1] != INT_RESULT) {
		strcpy(message, "Second argument is not an integer (pretty)");
		return true;
	} else if (args->arg_count > 2) {
		if (args->arg_type[2] != INT_RESULT) {
			strcpy(message, "Third argument is not an integer (memory)");
			return true;
		} else
			more += (ulong)*(longlong*)args->args[2];

	} // endifs

	initid->maybe_null = 1;
	CalcLen(args, false, reslen, memlen);
	fl = GetFileLength(args->args[0]);
	more += fl * M;
	memlen += more;
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Jbin_File_init

char *Jbin_File(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *fn, *memory;
	int     len, pretty;
	HANDLE  hFile;
	MEMMAP  mm;
	PJSON   jsp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed)
		goto fin;

	PlugSubSet(g, g->Sarea, g->Sarea_Size);
	g->Xchk = NULL;
	fn = MakePSZ(g, args, 0);
	pretty = (args->arg_count > 1) ? (int)*(longlong*)args->args[1] : 3;

	/*******************************************************************************/
	/*  Create the mapping file object.                                            */
	/*******************************************************************************/
	hFile = CreateFileMap(g, fn, &mm, MODE_READ, false);

	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD rc = GetLastError();

		if (!(*g->Message))
			sprintf(g->Message, MSG(OPEN_MODE_ERROR), "map", (int)rc, fn);

		PUSH_WARNING(g->Message);
		if (g->Mrr) *error = 1;
		*is_null = 1;
		return NULL;
	} // endif hFile

	/*******************************************************************************/
	/*  Get the file size (assuming file is smaller than 4 GB)                     */
	/*******************************************************************************/
	len = mm.lenL;
	memory = (char *)mm.memory;

	if (!len) {              // Empty or deleted file
		CloseFileHandle(hFile);
		*is_null = 1;
		return NULL;
	} // endif len

	if (!memory) {
		CloseFileHandle(hFile);
		sprintf(g->Message, MSG(MAP_VIEW_ERROR), fn, GetLastError());
		PUSH_WARNING(g->Message);
		*is_null = 1;
		if (g->Mrr) *error = 1;
		return NULL;
	} // endif Memory

	CloseFileHandle(hFile);                    // Not used anymore
	hFile = INVALID_HANDLE_VALUE;              // For Fblock

	/*********************************************************************************/
	/*  Parse the json file and allocate its tree structure.                         */
	/*********************************************************************************/
	g->Message[0] = 0;

	if (!(jsp = ParseJson(g, memory, len, pretty))) {
		PUSH_WARNING(g->Message);
		if (g->Mrr) *error = 1;
	}	// endif jsp

	CloseMemMap(memory, len);

	if (jsp) {
		bsp = JbinAlloc(g, args, len, jsp);
		strcat(bsp->Msg, " file");
		bsp->Filename = fn;
		bsp->Pretty = pretty;
	} else
		bsp = NULL;

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
} // end of Jbin_File

void Jbin_File_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Jbin_File_deinit

/*********************************************************************************/
/*  Serialize a Json document.                .                                  */
/*********************************************************************************/
my_bool Json_Serialize_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "Json_Serialize must have 1 argument");
		return true;
	} else if (IsJson(args, 0) != 3) {
		strcpy(message, "Json_Serialize argument must be a Jbin tree");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_Serialize_init

char *Json_Serialize(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	char   *str;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		PBSON bsp = (PBSON)args->args[0];

		JsonSubSet(g);

		if (!(str = Serialize(g, bsp->Jsp, NULL, 0)))
			str = strcpy(result, g->Message);

		// Keep result of constant function
		g->Xchk = (initid->const_item) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
	return str;
} // end of Json_Serialize

void Json_Serialize_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of Json_Serialize_deinit

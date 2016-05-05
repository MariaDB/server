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

#if defined(UNIX) || defined(UNIV_LINUX)
#define _O_RDONLY O_RDONLY
#endif

#define MEMFIX  4096
#if defined(connect_EXPORTS)
#define PUSH_WARNING(M) push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, M)
#else
#define PUSH_WARNING(M) htrc(M)
#endif
#define M 7

uint GetJsonGrpSize(void);
static int IsJson(UDF_ARGS *args, uint i);
static PSZ MakePSZ(PGLOBAL g, UDF_ARGS *args, int i);

static uint JsonGrpSize = 10;

/* ----------------------------------- JSNX ------------------------------------ */

/*********************************************************************************/
/*  JSNX public constructor.                                                     */
/*********************************************************************************/
JSNX::JSNX(PGLOBAL g, PJSON row, int type, int len, int prec, my_bool wr)
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
	Wr = wr;
	Jb = false;
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

#if 0
	if (jb) {																						 
		// Path must return a Json item
		size_t n = strlen(path);

		if (!n || path[n - 1] != '*') {
			Jpath = (char*)PlugSubAlloc(g, NULL, n + 3);
			strcat(strcpy(Jpath, path), (n) ? ":*" : "*");
		} else
			Jpath = path;

	} else
#endif // 0
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
				jnp->CncVal = AllocateValue(g, (void*)", ", TYPE_STRING);
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
			if (Wr) {
				sprintf(g->Message, "Invalid specification %c in a write path", *p);
				return true;
			}	else     			// Return JSON
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
	} else if (jsp->GetType() != TYPE_JAR && jsp->GetType() != TYPE_JOB) {
		strcpy(g->Message, "Target is not an array or object");
		Value->Reset();
	}	else
		Value->SetValue_psz(Serialize(g, jsp, NULL, 0));

	return Value;
} // end of MakeJson

/*********************************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                                */
/*********************************************************************************/
void JSNX::SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n)
{
	if (val) {
		if (Jb) {
			vp->SetValue_psz(Serialize(g, val->GetJsp(), NULL, 0));
		} else switch (val->GetValType()) {
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
	return GetRowValue(g, Row, 0);
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

	val = GetRowValue(g, row, i);
	SetJsonValue(g, Value, val, n);
	return Value;
} // end of GetColumnValue

/*********************************************************************************/
/*  GetRowValue:                                                                 */
/*********************************************************************************/
PJVAL JSNX::GetRowValue(PGLOBAL g, PJSON row, int i, my_bool b)
{
	my_bool expd = false;
	PJAR    arp;
	PJVAL   val = NULL;

	for (; i < Nod && row; i++) {
		if (Nodes[i].Op == OP_NUM) {
			Value->SetValue(row->GetType() == TYPE_JAR ? row->size() : 1);
			val = new(g) JVALUE(g, Value);
			return val;
		} else if (Nodes[i].Op == OP_XX) {
			Jb = b;
			return new(g)JVALUE(row);
		} else switch (row->GetType()) {
			case TYPE_JOB:
				if (!Nodes[i].Key) {
					// Expected Array was not there
					if (Nodes[i].Op == OP_LE) {
						if (i < Nod-1)
							continue;
						else
							val = new(g)JVALUE(row);

					} else {
						strcpy(g->Message, "Unexpected object");
						val = NULL;
					} //endif Op

				} else
					val = ((PJOB)row)->GetValue(Nodes[i].Key);

				break;
			case TYPE_JAR:
				arp = (PJAR)row;

				if (!Nodes[i].Key) {
					if (Nodes[i].Op == OP_EQ || Nodes[i].Op == OP_LE)
						val = arp->GetValue(Nodes[i].Rank);
					else if (Nodes[i].Op == OP_EXP)
						return (PJVAL)ExpandArray(g, arp, i);
					else
						return new(g) JVALUE(g, CalculateArray(g, arp, i));

				} else {
					// Unexpected array, unwrap it as [0]
					val = arp->GetValue(0);
					i--;
				}	// endif's

				break;
			case TYPE_JVAL:
				val = (PJVAL)row;
				break;
			default:
				sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
				val = NULL;
			} // endswitch Type

		if (i < Nod-1)
			if (!(row = (val) ? val->GetJsp() : NULL))
				val = NULL;
//		row = (val) ? val->GetJson() : NULL;

	} // endfor i

	// SetJsonValue(g, Value, val, n);
	return val;
} // end of GetRowValue

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
/* CheckPath: Checks whether the path exists in the document.                    */
/*********************************************************************************/
my_bool JSNX::CheckPath(PGLOBAL g)
{
	PJVAL   val= NULL;
	PJSON   row = Row;

	for (int i = 0; i < Nod && row; i++) {
		val = NULL;

		if (Nodes[i].Op == OP_NUM || Nodes[i].Op == OP_XX) {
		} else switch (row->GetType()) {
		case TYPE_JOB:
			if (Nodes[i].Key)
				val = ((PJOB)row)->GetValue(Nodes[i].Key);

			break;
		case TYPE_JAR:
			if (!Nodes[i].Key)
				if (Nodes[i].Op == OP_EQ || Nodes[i].Op == OP_LE)
					val = ((PJAR)row)->GetValue(Nodes[i].Rank);

			break;
		case TYPE_JVAL:
			val = (PJVAL)row;
			break;
		default:
			sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
		} // endswitch Type

		if (i < Nod-1)
			if (!(row = (val) ? val->GetJsp() : NULL))
				val = NULL;

	} // endfor i

	return (val != NULL);
} // end of CheckPath

/***********************************************************************/
/*  GetRow: Set the complete path of the object to be set.             */
/***********************************************************************/
PJSON JSNX::GetRow(PGLOBAL g)
{
	PJVAL val = NULL;
	PJAR  arp;
	PJSON nwr, row = Row;

	for (int i = 0; i < Nod - 1 && row; i++) {
		if (Nodes[i].Op == OP_XX)
			break;
		else switch (row->GetType()) {
		case TYPE_JOB:
			if (!Nodes[i].Key)
				// Expected Array was not there, wrap the value
				continue;

			val = ((PJOB)row)->GetValue(Nodes[i].Key);
			break;
		case TYPE_JAR:
			arp = (PJAR)row;

			if (!Nodes[i].Key) {
				if (Nodes[i].Op == OP_EQ)
					val = arp->GetValue(Nodes[i].Rank);
				else
					val = arp->GetValue(Nodes[i].Rx);

			} else {
				// Unexpected array, unwrap it as [0]
				val = arp->GetValue(0);
				i--;
			} // endif Nodes

			break;
		case TYPE_JVAL:
			val = (PJVAL)row;
			break;
		default:
			sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
			val = NULL;
		} // endswitch Type

		if (val) {
			row = val->GetJson();
		} else {
			// Construct missing objects
			for (i++; row && i < Nod; i++) {
				if (Nodes[i].Op == OP_XX)
					break;
				else if (!Nodes[i].Key)
					// Construct intermediate array
					nwr = new(g)JARRAY;
				else
					nwr = new(g)JOBJECT;

				if (row->GetType() == TYPE_JOB) {
					((PJOB)row)->SetValue(g, new(g)JVALUE(nwr), Nodes[i-1].Key);
				} else if (row->GetType() == TYPE_JAR) {
					((PJAR)row)->AddValue(g, new(g)JVALUE(nwr));
					((PJAR)row)->InitArray(g);
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
my_bool JSNX::WriteValue(PGLOBAL g, PJVAL jvalp)
{
	PJOB  objp = NULL;
	PJAR  arp = NULL;
	PJVAL jvp = NULL;
	PJSON row = GetRow(g);

	if (!row)
		return true;

	switch (row->GetType()) {
	case TYPE_JOB:  objp = (PJOB)row;  break;
	case TYPE_JAR:  arp  = (PJAR)row;  break;
	case TYPE_JVAL: jvp  = (PJVAL)row; break;
	default: 
		strcpy(g->Message, "Invalid target type");
		return true;
	} // endswitch Type

	if (arp) {
		if (!Nodes[Nod-1].Key) {
			if (Nodes[Nod-1].Op == OP_EQ)
				arp->SetValue(g, jvalp, Nodes[Nod-1].Rank);
			else
				arp->AddValue(g, jvalp);

			arp->InitArray(g);
		}	// endif Key

	} else if (objp) {
		if (Nodes[Nod-1].Key)
			objp->SetValue(g, jvalp, Nodes[Nod-1].Key);

	} else if (jvp)
		jvp->SetValue(jvalp);

	return false;
} // end of WriteValue

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

// BSON size should be equal on Linux and Windows
#define BMX 255
typedef struct BSON *PBSON;

/*********************************************************************************/
/*  Structure used to return binary json.                                        */
/*********************************************************************************/
struct BSON {
	char    Msg[BMX + 1];
	char   *Filename;
	PGLOBAL G;
	int     Pretty;
	ulong   Reslen;
	my_bool Changed;
	PJSON   Top;
	PJSON   Jsp;
	PBSON   Bsp;
}; // end of struct BSON

/*********************************************************************************/
/*  Allocate and initialize a BSON structure.                                    */
/*********************************************************************************/
static PBSON JbinAlloc(PGLOBAL g, UDF_ARGS *args, ulong len, PJSON jsp)
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
		bsp->Top = bsp->Jsp = jsp;
		bsp->Bsp = (IsJson(args, 0) == 3) ? (PBSON)args->args[0] : NULL;
	} else
		PUSH_WARNING(g->Message);

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
/*  Replaces GetJsonGrpSize not usable when CONNECT is not installed.            */
/*********************************************************************************/
static uint GetJsonGroupSize(void)
{
	return (JsonGrpSize) ? JsonGrpSize : GetJsonGrpSize();
} // end of GetJsonGroupSize

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
/*  Program for freeing the memory pools.                                        */
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
static my_bool CheckPath(PGLOBAL g, UDF_ARGS *args, PJSON jsp, PJVAL& jvp, int n)
{
	for (uint i = n; i < args->arg_count; i++)
		if (args->arg_type[i] == STRING_RESULT && args->args[i]) {
			// A path to a subset of the json tree is given
			char *path = MakePSZ(g, args, i);

			if (path) {
				PJSNX jsx = new(g)JSNX(g, jsp, TYPE_STRING);

				if (jsx->SetJpath(g, path))
					return true;

				if (!(jvp = jsx->GetJson(g))) {
					sprintf(g->Message, "No sub-item at '%s'", path);
					return true;
				} // endif jvp

			} else {
				strcpy(g->Message, "Path argument is null");
				return true;
			} // endif path

			break;
		}	// endif type

	return false;
} // end of CheckPath

/*********************************************************************************/
/*  Make the result according to the first argument type.                        */
/*********************************************************************************/
static char *MakeResult(PGLOBAL g, UDF_ARGS *args, PJSON top, uint n = 2)
{
	char *str;

	if (IsJson(args, 0) == 2) {
		// Make the change in the json file
		int pretty = 2;

		for (uint i = n; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT) {
				pretty = (int)*(longlong*)args->args[i];
				break;
			} // endif type

		if (!Serialize(g, top, MakePSZ(g, args, 0), pretty))
			PUSH_WARNING(g->Message);

		str = NULL;
	} else if (IsJson(args, 0) == 3) {
		PBSON bsp = (PBSON)args->args[0];

		if (bsp->Filename) {
			// Make the change in the json file
			if (!Serialize(g, top, bsp->Filename, bsp->Pretty))
				PUSH_WARNING(g->Message);

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

	if (!bsnp)
		return NULL;

	if (IsJson(args, 0) == 2) {
		int pretty = 0;

		for (uint i = n; i < args->arg_count; i++)
			if (args->arg_type[i] == INT_RESULT) {
				pretty = (int)*(longlong*)args->args[i];
				break;
			} // endif type

		bsnp->Pretty = pretty;

		if (bsnp->Filename = (char*)args->args[0]) {
			bsnp->Filename = MakePSZ(g, args, 0);
			strncpy(bsnp->Msg, bsnp->Filename, BMX);
		} else
			strncpy(bsnp->Msg, "null filename", BMX);

	} else if (IsJson(args, 0) == 3) {
		PBSON bsp = (PBSON)args->args[0];

		if (bsp->Filename) {
			bsnp->Filename = bsp->Filename;
			strncpy(bsnp->Msg, bsp->Filename, BMX);
			bsnp->Pretty = bsp->Pretty;
		} else
			strcpy(bsnp->Msg, "Json Binary item");

	} else
		strcpy(bsnp->Msg, "Json Binary item");

	return bsnp;
} // end of MakeBinResult

/*********************************************************************************/
/*  Returns a pointer to the first integer argument found from the nth argument. */
/*********************************************************************************/
static int *GetIntArgPtr(PGLOBAL g, UDF_ARGS *args, uint& n)
{
	int *x = NULL;

	for (uint i = n; i < args->arg_count; i++)
		if (args->arg_type[i] == INT_RESULT) {
			if (args->args[i]) {
				x = (int*)PlugSubAlloc(g, NULL, sizeof(int));
				*x = (int)*(longlong*)args->args[i];
			} // endif args

			n = i + 1;
			break;
		}	// endif arg_type

	return x;
} // end of GetIntArgPtr

/*********************************************************************************/
/*  Returns not 0 if the argument is a JSON item or file name.                   */
/*********************************************************************************/
static int IsJson(UDF_ARGS *args, uint i)
{
	int n = 0;

	if (i >= args->arg_count || args->arg_type[i] != STRING_RESULT) {
	} else if (!strnicmp(args->attributes[i], "Json_", 5)) {
		if (!args->args[i] || strchr("[{ \t\r\n", *args->args[i]))
			n = 1;					 // arg should be is a json item
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
/*  GetMemPtr: returns the memory pointer used by this argument.                 */
/*********************************************************************************/
static PGLOBAL GetMemPtr(PGLOBAL g, UDF_ARGS *args, uint i)
{
	return (IsJson(args, i) == 3) ? ((PBSON)args->args[i])->G : g;
} // end of GetMemPtr

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
	long fl= 0, j = -1;

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
				} else if (n == 1) {
					if (i == 0)
						memlen += sizeof(BSON);				 // For Jbin functions

					memlen += args->lengths[i] * M;  // Estimate parse memory
				} else if (n == 3)
					memlen += sizeof(JVALUE);

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
static my_bool CheckMemory(PGLOBAL g, UDF_INIT *initid, UDF_ARGS *args, uint n,
	                         my_bool m, my_bool obj = false, my_bool mod = false)
{
	unsigned long rl, ml;
	my_bool       b = false;

	n = MY_MIN(n, args->arg_count);																					  

	for (uint i = 0; i < n; i++)
		if (IsJson(args, i) == 2 ||
			 (b = (m && !i && args->arg_type[0] == STRING_RESULT && !IsJson(args, 0)))) {
			if (CalcLen(args, obj, rl, ml, mod))
				return true;
			else if (b) {
				ulong len;
				char *p = args->args[0];

				// Is this a file name?
				if (!strchr("[{ \t\r\n", *p) && (len = GetFileLength(p)))
					ml += len * (M + 1);
				else
					ml += args->lengths[0] * M;

			}	// endif b

			if (ml > g->Sarea_Size) {
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
/*  Parse a json file.                                                 */
/*********************************************************************************/
static PJSON ParseJsonFile(PGLOBAL g, char *fn, int *pretty, int& len)
{
	char   *memory;
	HANDLE  hFile;
	MEMMAP  mm;
	PJSON   jsp;

	/*******************************************************************************/
	/*  Create the mapping file object.                                            */
	/*******************************************************************************/
	hFile = CreateFileMap(g, fn, &mm, MODE_READ, false);

	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD rc = GetLastError();

		if (!(*g->Message))
			sprintf(g->Message, MSG(OPEN_MODE_ERROR), "map", (int)rc, fn);

		return NULL;
	} // endif hFile

	/*******************************************************************************/
	/*  Get the file size (assuming file is smaller than 4 GB)                     */
	/*******************************************************************************/
	len = mm.lenL;
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

	CloseFileHandle(hFile);                    // Not used anymore

	/*********************************************************************************/
	/*  Parse the json file and allocate its tree structure.                         */
	/*********************************************************************************/
	g->Message[0] = 0;
	jsp = ParseJson(g, memory, len, pretty);
	CloseMemMap(memory, len);
	return jsp;
} // end of ParseJsonFile

/*********************************************************************************/
/*  Return a json file contains.                                                 */
/*********************************************************************************/
static char *GetJsonFile(PGLOBAL g, char *fn)
{
	char   *str;
	int     h, n, len;

#if defined(UNIX) || defined(UNIV_LINUX)
	h= open(fn, O_RDONLY);
#else
	h= open(fn, _O_RDONLY, _O_TEXT);
#endif

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
static PJVAL MakeValue(PGLOBAL g, UDF_ARGS *args, uint i, PJSON *top = NULL)
{
	char *sap = (args->arg_count > i) ? args->args[i] : NULL;
	int   n, len;
	short c;
	long long bigint;
  PJSON jsp;
  PJVAL jvp = new(g) JVALUE;

	if (top)
		*top = NULL;

  if (sap) switch (args->arg_type[i]) {
    case STRING_RESULT:
      if ((len = args->lengths[i])) {
				if ((n = IsJson(args, i)) < 3)
					sap = MakePSZ(g, args, i);

        if (n) {
					if (n == 3) {
						if (top)
							*top = ((PBSON)sap)->Top;

						jsp = ((PBSON)sap)->Jsp;
					} else {
						if (n == 2) {
							if (!(sap = GetJsonFile(g, sap))) {
								PUSH_WARNING(g->Message);
								return jvp;
							} // endif sap

							len = strlen(sap);
						} // endif n

						if (!(jsp = ParseJson(g, sap, strlen(sap))))
							PUSH_WARNING(g->Message);
						else if (top)
							*top = jsp;

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

			if ((bigint == 0LL && !strcmp(args->attributes[i], "FALSE")) ||
					(bigint == 1LL && !strcmp(args->attributes[i], "TRUE")))
				jvp->SetTiny(g, (char)bigint);
			else
				jvp->SetBigint(g, bigint);

      break;
    case REAL_RESULT:
      jvp->SetFloat(g, *(double*)sap);
      break;
    case DECIMAL_RESULT:
      jvp->SetFloat(g, atof(MakePSZ(g, args, i)));
      break;
    case TIME_RESULT:
    case ROW_RESULT:
    default:
      break;
    } // endswitch arg_type

  return jvp;
} // end of MakeValue

/*********************************************************************************/
/*  Make a Json value containing the parameter.                                  */
/*********************************************************************************/
my_bool jsonvalue_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  if (args->arg_count > 1) {
    strcpy(message, "Cannot accept more than 1 argument");
    return true;
  } else
    CalcLen(args, false, reslen, memlen);

  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of jsonvalue_init

char *jsonvalue(UDF_INIT *initid, UDF_ARGS *args, char *result, 
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

void jsonvalue_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsonvalue_deinit

/*********************************************************************************/
/*  Make a Json array containing all the parameters.                             */
/*********************************************************************************/
my_bool json_array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, false, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of json_array_init

char *json_array(UDF_INIT *initid, UDF_ARGS *args, char *result, 
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
} // end of json_array

void json_array_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_array_deinit

/*********************************************************************************/
/*  Add one or several values to a Json array.                                   */
/*********************************************************************************/
my_bool json_array_add_values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json string or item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_array_add_values_init

char *json_array_add_values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			char *p;
			PJSON top;
			PJAR  arp;
			PJVAL jvp = MakeValue(g, args, 0, &top);
			
			if ((p = jvp->GetString())) {
				if (!(top = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					return NULL;
				} // endif jsp

				jvp->SetValue(top);
			} // endif p

			if (jvp->GetValType() != TYPE_JAR) {
				arp = new(g)JARRAY;
				arp->AddValue(g, jvp);
			} else
				arp = jvp->GetArray();

			for (uint i = 1; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);
//		str = Serialize(g, arp, NULL, 0);
			str = MakeResult(g, args, top, args->arg_count);
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
} // end of json_array_add_values

void json_array_add_values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_array_add_values_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool json_array_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
    return true;
  } else if (!IsJson(args, 0)) {
    strcpy(message, "First argument must be a json item");
    return true;
	} else
    CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_array_add_init

char *json_array_add(UDF_INIT *initid, UDF_ARGS *args, char *result, 
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
		PJSON jsp, top;
		PJVAL jvp;
		PJAR  arp;

		jvp = MakeValue(g, args, 0, &top);
		jsp = jvp->GetJson();
		x = GetIntArgPtr(g, args, n);

		if (CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			PGLOBAL gb = GetMemPtr(g, args, 0);

			arp = jvp->GetArray();
			arp->AddValue(gb, MakeValue(gb, args, 1), x);
			arp->InitArray(gb);
			str = MakeResult(g, args, top, n);
		} else {
			PUSH_WARNING("First argument target is not an array");
//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
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
} // end of json_array_add

void json_array_add_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_array_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool json_array_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

	if (args->arg_count < 2) {
    strcpy(message, "This function must have at least 2 arguments");
    return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
    CalcLen(args, false, reslen, memlen, true);

  return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_array_delete_init

char *json_array_delete(UDF_INIT *initid, UDF_ARGS *args, char *result, 
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
		PJSON top;
		PJAR  arp;
		PJVAL jvp = MakeValue(g, args, 0, &top);

		if (!(x = GetIntArgPtr(g, args, n)))
			PUSH_WARNING("Missing or null array index");
		else if (CheckPath(g, args, jvp->GetJson(), jvp, 1))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			arp = jvp->GetArray();
			arp->DeleteValue(*x);
			arp->InitArray(GetMemPtr(g, args, 0));
			str = MakeResult(g, args, top, n);
		} else {
			PUSH_WARNING("First argument target is not an array");
//		if (g->Mrr) *error = 1;
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
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
} // end of json_array_delete

void json_array_delete_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_array_delete_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the parameters.                            */
/*********************************************************************************/
my_bool json_object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, true, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of json_object_init

char *json_object(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                  unsigned long *res_length, char *, char *)
{
  char   *str = NULL;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, false, true)) {
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
} // end of json_object

void json_object_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_deinit

/*********************************************************************************/
/*  Make a Json Object containing all not null parameters.                       */
/*********************************************************************************/
my_bool json_object_nonull_init(UDF_INIT *initid, UDF_ARGS *args,
                                char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, true, reslen, memlen);
  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of json_object_nonull_init

char *json_object_nonull(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                         unsigned long *res_length, char *, char *)
{
  char   *str= 0;
  PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
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
} // end of json_object_nonull

void json_object_nonull_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_nonull_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the key/value parameters.                  */
/*********************************************************************************/
my_bool json_object_key_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count % 2) {
		strcpy(message, "This function must have an even number of arguments");
		return true;
	} // endif arg_count

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of json_object_key_init

char *json_object_key(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->Xchk) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			PJOB objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i += 2)
				objp->SetValue(g, MakeValue(g, args, i+1), MakePSZ(g, args, i));

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
} // end of json_object_key

void json_object_key_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_key_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool json_object_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_object_add_init

char *json_object_add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                    unsigned long *res_length, char *is_null, char *error)
{
	char   *key, *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->Xchk) {
		// This constant function was recalled
		str = (char*)g->Xchk;
		goto fin;
	} // endif Xchk

	if (!CheckMemory(g, initid, args, 2, false, true, true)) {
		PJOB    jobp;
		PJVAL   jvp;
		PJSON   jsp, top;
		PGLOBAL gb = GetMemPtr(g, args, 0);

		jvp = MakeValue(g, args, 0, &top);
		jsp = jvp->GetJson();

		if (CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			jobp = jvp->GetObject();
			jvp = MakeValue(gb, args, 1);
			key = MakeKey(gb, args, 1);
			jobp->SetValue(gb, jvp, key);
			str = MakeResult(g, args, top);
		} else {
			PUSH_WARNING("First argument target is not an object");
//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
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
} // end of json_object_add

void json_object_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool json_object_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have 2 or 3 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument must be a key string");
		return true;
	} else
		CalcLen(args, true, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_object_delete_init

char *json_object_delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
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
		char *key;
		PJOB  jobp;
		PJSON jsp, top;
		PJVAL jvp = MakeValue(g, args, 0, &top);

		jsp = jvp->GetJson();

		if (CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			key = MakeKey(GetMemPtr(g, args, 0), args, 1);
			jobp = jvp->GetObject();
			jobp->DeleteKey(key);
			str = MakeResult(g, args, top);
		} else {
			PUSH_WARNING("First argument target is not an object");
//		if (g->Mrr) *error = 1;					(only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
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
} // end of json_object_delete

void json_object_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool json_object_list_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "This function must have 1 argument");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_object_list_init

char *json_object_list(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (!g->N) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			char *p;
			PJSON jsp;
			PJVAL jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					return NULL;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (jsp->GetType() == TYPE_JOB) {
				PJAR jarp = ((PJOB)jsp)->GetKeyList(g);

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
} // end of json_object_list

void json_object_list_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_list_deinit

/*********************************************************************************/
/*  Set the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool jsonset_grp_size_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT) {
		strcpy(message, "This function must have 1 integer argument");
		return true;
	} else
		return false;

} // end of jsonset_grp_size_init

long long jsonset_grp_size(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	long long n = *(long long*)args->args[0];

	JsonGrpSize = (uint)n;
	return (long long)GetJsonGroupSize();
} // end of jsonset_grp_size

/*********************************************************************************/
/*  Get the value of JsonGrpSize.                                                */
/*********************************************************************************/
my_bool jsonget_grp_size_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (args->arg_count != 0) {
		strcpy(message, "This function must have no arguments");
		return true;
	} else
		return false;

} // end of jsonget_grp_size_init

long long jsonget_grp_size(UDF_INIT *initid, UDF_ARGS *args, char *, char *)
{
	return (long long)GetJsonGroupSize();
} // end of jsonget_grp_size

/*********************************************************************************/
/*  Make a Json array from values coming from rows.                              */
/*********************************************************************************/
my_bool json_array_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGroupSize();

  if (args->arg_count != 1) {
    strcpy(message, "This function can only accept 1 argument");
    return true;
	} else if (IsJson(args, 0) == 3) {
		strcpy(message, "This function does not support Jbin arguments");
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
} // end of json_array_grp_init

void json_array_grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  if (g->N-- > 0)
    arp->AddValue(g, MakeValue(g, args, 0));

} // end of json_array_grp_add

char *json_array_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
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
} // end of json_array_grp

void json_array_grp_clear(UDF_INIT *initid, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  g->N = GetJsonGroupSize();
} // end of json_array_grp_clear

void json_array_grp_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_array_grp_deinit

/*********************************************************************************/
/*  Make a Json object from values coming from rows.                             */
/*********************************************************************************/
my_bool json_object_grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGroupSize();

	if (args->arg_count != 2) {
    strcpy(message, "This function requires 2 arguments (key, value)");
    return true;
	} else if (IsJson(args, 0) == 3) {
		strcpy(message, "This function does not support Jbin arguments");
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
} // end of json_object_grp_init

void json_object_grp_add(UDF_INIT *initid, UDF_ARGS *args, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

	if (g->N-- > 0)
		objp->SetValue(g, MakeValue(g, args, 1), MakePSZ(g, args, 0));

} // end of json_object_grp_add

char *json_object_grp(UDF_INIT *initid, UDF_ARGS *, char *result, 
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
} // end of json_object_grp

void json_object_grp_clear(UDF_INIT *initid, char*, char*)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  g->N = GetJsonGroupSize();
} // end of json_object_grp_clear

void json_object_grp_deinit(UDF_INIT* initid)
{
  JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_object_grp_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool json_item_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "This function must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0)) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (!IsJson(args, 1)) {
		strcpy(message, "Second argument must be a json item");
		return true;
	} else
		CalcLen(args, false, reslen, memlen, true);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_item_merge_init

char *json_item_merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
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
		PJSON top= 0;
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
			if (jsp[0]->Merge(GetMemPtr(g, args, 0), jsp[1]))
				PUSH_WARNING(GetMemPtr(g, args, 0)->Message);
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

 fin:
	if (!str) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of json_item_merge

void json_item_merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_item_merge_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool json_get_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;
	int n = IsJson(args, 0);

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
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_get_item_init

char *json_get_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	             unsigned long *res_length, char *is_null, char *)
{
	char   *p, *path, *str = NULL;
	PJSON   jsp;
	PJVAL   jvp;
	PJSNX   jsx;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true, true)) {
			PUSH_WARNING("CheckMemory error");
			goto fin;
		} else
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
	jsx = new(g) JSNX(g, jsp, TYPE_STRING, initid->max_length);

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
		g->Activityp = (PACTIVITY)str;

 fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of json_get_item

void json_get_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_get_item_deinit

/*********************************************************************************/
/*  Get a string value from a Json item.                                         */
/*********************************************************************************/
my_bool jsonget_string_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsJson(args, 0);

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
	memlen += more;

	if (n == 2 && args->args[0]) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jsonget_string_init

char *jsonget_string(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *p, *path, *str = NULL;
	int     rc;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	// Save stack and allocation environment and prepare error return
	if (g->jump_level == MAX_JUMP) {
		PUSH_WARNING(MSG(TOO_MANY_JUMPS));
		*is_null = 1;
		return NULL;
	} // endif jump_level

	if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
		PUSH_WARNING(g->Message);
		str = NULL;
		goto err;
	} // endif rc

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			goto err;
		} else
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

	path = MakePSZ(g, args, 1);
	jsx = new(g) JSNX(g, jsp, TYPE_STRING, initid->max_length);

	if (jsx->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		goto err;
	}	// endif SetJpath

	jsx->ReadValue(g);

	if (!jsx->GetValue()->IsNull())
		str = jsx->GetValue()->GetCharValue();

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)str;

 err:
	g->jump_level--;

 fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of jsonget_string

void jsonget_string_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsonget_string_deinit

/*********************************************************************************/
/*  Get an integer value from a Json item.                                       */
/*********************************************************************************/
my_bool jsonget_int_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 2) {
		strcpy(message, "This function must have 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jsonget_int_init

long long jsonget_int(UDF_INIT *initid, UDF_ARGS *args,
	                     char *is_null, char *error)
{
	char   *p, *path;
	long long n;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
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
		} else
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
	jsx = new(g) JSNX(g, jsp, TYPE_BIGINT);

	if (jsx->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		*is_null = 1;
		return 0;
	} // endif SetJpath

	jsx->ReadValue(g);

	if (jsx->GetValue()->IsNull()) {
		*is_null = 1;
		return 0;
	}	// endif IsNull

	n = jsx->GetValue()->GetBigintValue();

	if (initid->const_item) {
		// Keep result of constant function
		long long *np = (long long*)PlugSubAlloc(g, NULL, sizeof(long long));
		*np = n;
		g->Activityp = (PACTIVITY)np;
	} // endif const_item

	return n;
} // end of jsonget_int

void jsonget_int_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsonget_int_deinit

/*********************************************************************************/
/*  Get a double value from a Json item.                                         */
/*********************************************************************************/
my_bool jsonget_real_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "At least 2 arguments required");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
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

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jsonget_real_init

double jsonget_real(UDF_INIT *initid, UDF_ARGS *args,
	                   char *is_null, char *error)
{
	char   *p, *path;
	double  d;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

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
		} else
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
	jsx = new(g) JSNX(g, jsp, TYPE_DOUBLE);

	if (jsx->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		*is_null = 1;
		return 0.0;
	}	// endif SetJpath

	jsx->ReadValue(g);

	if (jsx->GetValue()->IsNull()) {
		*is_null = 1;
		return 0.0;
	}	// endif IsNull

	d = jsx->GetValue()->GetFloatValue();

	if (initid->const_item) {
		// Keep result of constant function
		double *dp = (double*)PlugSubAlloc(g, NULL, sizeof(double));
		*dp = d;
		g->Activityp = (PACTIVITY)dp;
	} // endif const_item

	return d;
} // end of jsonget_real

void jsonget_real_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsonget_real_deinit

/*********************************************************************************/
/*  Locate a value in a Json tree.                                               */
/*********************************************************************************/
my_bool jsonlocate_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
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
	} else if (args->arg_count > 3)
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more += (ulong)*(longlong*)args->args[2];

	CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) != 3)
		memlen += more;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jsonlocate_init

char *jsonlocate(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path = NULL;
	int     k, rc;
	PJVAL   jvp, jvp2;
	PJSON   jsp;
	PJSNX   jsx;
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
		if (CheckMemory(g, initid, args, 1, !g->Xchk)) {
			PUSH_WARNING("CheckMemory error");
			*error = 1;
			goto err;
		} else
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

	jsx = new(g) JSNX(g, jsp, TYPE_STRING);
	path = jsx->Locate(g, jsp, jvp2, k);

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)path;

 err:
	g->jump_level--;

	if (!path) {
		*res_length = 0;
		*is_null = 1;
	}	else
		*res_length = strlen(path);

	return path;
} // end of jsonlocate

void jsonlocate_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsonlocate_deinit

/*********************************************************************************/
/*  Locate all occurences of a value in a Json tree.                             */
/*********************************************************************************/
my_bool json_locate_all_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
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
	} else if (args->arg_count > 3)
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more += (ulong)*(longlong*)args->args[2];

	CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) != 3)
		memlen += more;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_locate_all_init

char *json_locate_all(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path = NULL;
	int     rc, mx = 10;
	PJVAL   jvp, jvp2;
	PJSON   jsp;
	PJSNX   jsx;
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
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			*error = 1;
			goto err;
		} else
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

	jsx = new(g) JSNX(g, jsp, TYPE_STRING);
	path = jsx->LocateAll(g, jsp, jvp2, mx);

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)path;

 err:
	g->jump_level--;

	if (!path) {
		*res_length = 0;
		*is_null = 1;
	} else
		*res_length = strlen(path);

	return path;
} // end of json_locate_all

void json_locate_all_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_locate_all_deinit

/*********************************************************************************/
/*  Check whether the document contains a value or item.                         */
/*********************************************************************************/
my_bool jsoncontains_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsJson(args, 0);

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
	memlen += more;

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of jsoncontains_init

long long jsoncontains(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char         *p __attribute__((unused)), res[256];
	long long     n;
	unsigned long reslen;

	*is_null = 0;
	p = jsonlocate(initid, args, res, &reslen, is_null, error);
	n = (*is_null) ? 0LL : 1LL;
	return n;
} // end of jsoncontains

void jsoncontains_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsoncontains_deinit

/*********************************************************************************/
/*  Check whether the document contains a path.                                  */
/*********************************************************************************/
my_bool jsoncontains_path_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;
	int n = IsJson(args, 0);

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
	memlen += more;

	if (IsJson(args, 0) != 3)
		memlen += 1000;       // TODO: calculate this

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jsoncontains_path_init

long long jsoncontains_path(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path;
	long long n;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
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
			goto err;
		} else
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

	path = MakePSZ(g, args, 1);
	jsx = new(g)JSNX(g, jsp, TYPE_BIGINT);

	if (jsx->SetJpath(g, path)) {
		PUSH_WARNING(g->Message);
		goto err;
	} // endif SetJpath

	n = (jsx->CheckPath(g)) ? 1LL : 0LL;

	if (initid->const_item) {
		// Keep result of constant function
		long long *np = (long long*)PlugSubAlloc(g, NULL, sizeof(long long));
		*np = n;
		g->Activityp = (PACTIVITY)np;
	} // endif const_item

	return n;

 err:
	if (g->Mrr) *error = 1;
	*is_null = 1;
	return 0LL;
} // end of jsoncontains_path

void jsoncontains_path_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jsoncontains_path_deinit

/*********************************************************************************/
/*  Set Json items of a Json document according to path.                         */
/*********************************************************************************/
my_bool json_set_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;
	int n = IsJson(args, 0);

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
		memlen += fl * 3;
	} else if (n != 3)
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_set_item_init

char *json_set_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path, *str = NULL;
	int     w, rc;
	my_bool b = true;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PGLOBAL gb = GetMemPtr(g, args, 0);

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!strcmp(result, "$insert"))
		w = 1;
	else if (!strcmp(result, "$update"))
		w = 2;
	else
		w = 0;

	// Save stack and allocation environment and prepare error return
	if (g->jump_level == MAX_JUMP) {
		PUSH_WARNING(MSG(TOO_MANY_JUMPS));
		*error = 1;
		goto fin;
	} // endif jump_level

	if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
		PUSH_WARNING(g->Message);
		str = NULL;
		goto err;
	} // endif rc

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true, false, true)) {
			PUSH_WARNING("CheckMemory error");
			goto err;
		} else
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

	jsx = new(g)JSNX(g, jsp, TYPE_STRING, initid->max_length, 0, true);

	for (uint i = 1; i+1 < args->arg_count; i += 2) {
		jvp = MakeValue(gb, args, i);
		path = MakePSZ(g, args, i+1);

		if (jsx->SetJpath(g, path, false)) {
			PUSH_WARNING(g->Message);
			continue;
		}	// endif SetJpath

		if (w) {
			jsx->ReadValue(g);
			b = jsx->GetValue()->IsNull();
			b = (w == 1) ? b : !b;
		}	// endif w

		if (b && jsx->WriteValue(gb, jvp))
			PUSH_WARNING(g->Message);

	} // endfor i

	// In case of error or file, return unchanged argument
	if (!(str = MakeResult(g, args, jsp, INT_MAX32)))
		str = MakePSZ(g, args, 0);

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)str;

 err:
	g->jump_level--;

 fin:
	if (!str) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = strlen(str);

	return str;
} // end of json_set_item

void json_set_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_set_item_deinit

/*********************************************************************************/
/*  Insert Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool json_insert_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_set_item_init(initid, args, message);
} // end of json_insert_item_init

char *json_insert_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$insert");
	return json_set_item(initid, args, result, res_length, is_null, p);
} // end of json_insert_item

void json_insert_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_insert_item_deinit

/*********************************************************************************/
/*  Update Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool json_update_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_set_item_init(initid, args, message);
} // end of json_update_item_init

char *json_update_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$update");
	return json_set_item(initid, args, result, res_length, is_null, p);
} // end of json_update_item

void json_update_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_update_item_deinit

/*********************************************************************************/
/*  Returns a json file as a json string.                                        */
/*********************************************************************************/
my_bool json_file_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, fl, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 4) {
		strcpy(message, "This function only accepts 1 to 4 arguments");
		return true;
	} else if (!args->args[0] || args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a constant string (file name)");
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
	fl = GetFileLength(args->args[0]);
	reslen += fl;

	if (initid->const_item)
		more += fl;

	if (args->arg_count > 1) 
		more += fl * M;

  memlen += more;
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of json_file_init

char *json_file(UDF_INIT *initid, UDF_ARGS *args, char *result,
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

	if (args->arg_count > 1) {
		int    len, pretty, pty = 3;
		PJSON  jsp;
		PJVAL  jvp = NULL;

		pretty = (args->arg_type[1] == INT_RESULT) ? (int)*(longlong*)args->args[1]
					 : (args->arg_count > 2 && args->arg_type[2] == INT_RESULT)
					 ? (int)*(longlong*)args->args[2] : 3;

		/*******************************************************************************/
		/*  Parse the json file and allocate its tree structure.                       */
		/*******************************************************************************/
		if (!(jsp = ParseJsonFile(g, fn, &pty, len))) {
			PUSH_WARNING(g->Message);
			str = NULL;
			goto fin;
		} // endif jsp

		if (pty == 3)
			PUSH_WARNING("File pretty format cannot be determined");
		else if (pretty != 3 && pty != pretty)
			PUSH_WARNING("File pretty format doesn't match the specified pretty value");
		else if (pretty == 3)
			pretty = pty;

		// Check whether a path was specified
		if (CheckPath(g, args, jsp, jvp, 1)) {
			PUSH_WARNING(g->Message);
			str = NULL;
			goto fin;
		} else if (jvp)
			jsp = jvp->GetJson();

		if (!(str = Serialize(g, jsp, NULL, 0)))
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
} // end of json_file

void json_file_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_file_deinit

/*********************************************************************************/
/*  Make a json file from a json item.                                           */
/*********************************************************************************/
my_bool jfile_make_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 3) {
		strcpy(message, "Wrong number of arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "First argument must be a json item");
		return true;
	}	// endif

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jfile_make_init

char *jfile_make(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *)
{
	char   *p, *str = NULL, *fn = NULL;
	int     n, pretty = 2;
	PJSON   jsp;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if ((n = IsJson(args, 0)) == 3) {
		// Get default file name and pretty
		PBSON bsp = (PBSON)args->args[0];

		fn = bsp->Filename;
		pretty = bsp->Pretty;
	} else if (n == 2)
		fn = args->args[0];

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true)) {
			PUSH_WARNING("CheckMemory error");
			goto fin;
		}	else
			jvp = MakeValue(g, args, 0);

		if ((p = jvp->GetString())) {
			if (!strchr("[{ \t\r\n", *p)) {
				// Is this a file name?
				if (!(p = GetJsonFile(g, p))) {
					PUSH_WARNING(g->Message);
					goto fin;
				} else
					fn = jvp->GetString();

			} // endif p

			if (!(jsp = ParseJson(g, p, strlen(p)))) {
				PUSH_WARNING(g->Message);
				goto fin;
			} // endif jsp

			jvp->SetValue(jsp);
		} // endif p

		if (g->Mrr) {			 // First argument is a constant
			g->Xchk = jvp;
			JsonMemSave(g);
		} // endif Mrr

	} else
		jvp = (PJVAL)g->Xchk;

	for (uint i = 1; i < args->arg_count; i++)
		switch (args->arg_type[i]) {
			case STRING_RESULT:
				fn = MakePSZ(g, args, i);
				break;
			case INT_RESULT:
				pretty = (int)*(longlong*)args->args[i];
				break;
      default:
				PUSH_WARNING("Unexpected argument type in jfile_make");
			}	// endswitch arg_type

	if (fn) {
		if (!Serialize(g, jvp->GetJson(), fn, pretty))
			PUSH_WARNING(g->Message);
	} else
		PUSH_WARNING("Missing file name");

	str= fn;

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
} // end of jfile_make

void jfile_make_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jfile_make_deinit

/*********************************************************************************/
/*  Make and return a binary Json array containing all the parameters.           */
/*********************************************************************************/
my_bool jbin_array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, false, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jbin_array_init

char *jbin_array(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, false)) {
			PJAR arp =	 new(g) JARRAY;																	  

			bsp = JbinAlloc(g, args, initid->max_length, arp);
			strcat(bsp->Msg, " array");

			for (uint i = 0; i < args->arg_count; i++)
				arp->AddValue(g, MakeValue(g, args, i));

			arp->InitArray(g);
		} else
			if ((bsp = JbinAlloc(g, args, initid->max_length, NULL)))
				strncpy(bsp->Msg, g->Message, 139);

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
} // end of jbin_array

void jbin_array_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_array_deinit

/*********************************************************************************/
/*  Add one or several values to a Json array.                                   */
/*********************************************************************************/
my_bool jbin_array_add_values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_array_add_values_init(initid, args, message);
} // end of jbin_array_add_values_init

char *jbin_array_add_values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			char   *p;
			PJSON   top;
			PJAR    arp;
			PJVAL   jvp = MakeValue(g, args, 0, &top);
			PGLOBAL gb = GetMemPtr(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(top = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					return NULL;
				} // endif jsp

				jvp->SetValue(top);
			} // endif p

			if (jvp->GetValType() != TYPE_JAR) {
				arp = new(gb)JARRAY;
				arp->AddValue(gb, jvp);
			} else
				arp = jvp->GetArray();

			for (uint i = 1; i < args->arg_count; i++)
				arp->AddValue(gb, MakeValue(gb, args, i));

			arp->InitArray(gb);

			if ((bsp = JbinAlloc(g, args, initid->max_length, top))) {
				strcat(bsp->Msg, " array");
				bsp->Jsp = arp;
			}	// endif bsp

		} else
			if ((bsp = JbinAlloc(g, args, initid->max_length, NULL)))
				strncpy(bsp->Msg, g->Message, BMX);

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
} // end of jbin_array_add_values

void jbin_array_add_values_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_array_add_values_deinit

/*********************************************************************************/
/*  Add one value to a Json array.                                               */
/*********************************************************************************/
my_bool jbin_array_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_array_add_init(initid, args, message);
} // end of jbin_array_add_init

char *jbin_array_add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
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

	if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		int  *x = NULL;
		uint	n = 2;
//	PJSON jsp;
		PJVAL jvp;
		PJAR  arp;

		jvp = MakeValue(g, args, 0, &top);
//	jsp = jvp->GetJson();
		x = GetIntArgPtr(g, args, n);

		if (CheckPath(g, args, top, jvp, n))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			PGLOBAL gb = GetMemPtr(g, args, 0);

			arp = jvp->GetArray();
			arp->AddValue(gb, MakeValue(gb, args, 1), x);
			arp->InitArray(gb);
		} else {
			PUSH_WARNING("First argument is not an array");
//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length, n);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_array_add

void jbin_array_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_array_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json array.                                            */
/*********************************************************************************/
my_bool jbin_array_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_array_delete_init(initid, args, message);
} // end of jbin_array_delete_init

char *jbin_array_delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 1, false, false, true)) {
		int  *x;
		uint  n = 1;
		PJAR  arp;
		PJVAL jvp = MakeValue(g, args, 0, &top);

		if (CheckPath(g, args, top, jvp, 1))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JAR) {
			if ((x = GetIntArgPtr(g, args, n))) {
				arp = jvp->GetArray();
				arp->DeleteValue(*x);
				arp->InitArray(GetMemPtr(g, args, 0));
			} else
				PUSH_WARNING("Missing or null array index");

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

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_array_delete

void jbin_array_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_array_delete_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the parameters.                            */
/*********************************************************************************/
my_bool jbin_object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of jbin_object_init

char *jbin_object(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, true)) {
			PJOB objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				objp->SetValue(g, MakeValue(g, args, i), MakeKey(g, args, i));

			if ((bsp = JbinAlloc(g, args, initid->max_length, objp)))
				strcat(bsp->Msg, " object");

		} else
			if ((bsp = JbinAlloc(g, args, initid->max_length, NULL)))
				strncpy(bsp->Msg, g->Message, BMX);

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
} // end of jbin_object

void jbin_object_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_deinit

/*********************************************************************************/
/*  Make a Json Object containing all not null parameters.                       */
/*********************************************************************************/
my_bool jbin_object_nonull_init(UDF_INIT *initid, UDF_ARGS *args,	char *message)
{
	unsigned long reslen, memlen;

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jbin_object_nonull_init

char *jbin_object_nonull(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			PJVAL jvp;
			PJOB  objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i++)
				if (!(jvp = MakeValue(g, args, i))->IsNull())
					objp->SetValue(g, jvp, MakeKey(g, args, i));

			if ((bsp = JbinAlloc(g, args, initid->max_length, objp)))
				strcat(bsp->Msg, " object");

		} else
			if ((bsp = JbinAlloc(g, args, initid->max_length, NULL)))
				strncpy(bsp->Msg, g->Message, BMX);

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
} // end of jbin_object_nonull

void jbin_object_nonull_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_nonull_deinit

/*********************************************************************************/
/*  Make a Json Object containing all the key/value parameters.                  */
/*********************************************************************************/
my_bool jbin_object_key_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count % 2) {
		strcpy(message, "This function must have an even number of arguments");
		return true;
	} // endif arg_count

	CalcLen(args, true, reslen, memlen);
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jbin_object_key_init

char *jbin_object_key(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, args->arg_count, false, true)) {
			PJOB objp = new(g)JOBJECT;

			for (uint i = 0; i < args->arg_count; i += 2)
				objp->SetValue(g, MakeValue(g, args, i+1), MakePSZ(g, args, i));

			if ((bsp = JbinAlloc(g, args, initid->max_length, objp)))
				strcat(bsp->Msg, " object");

		} else
			if ((bsp = JbinAlloc(g, args, initid->max_length, NULL)))
				strncpy(bsp->Msg, g->Message, BMX);

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
} // end of jbin_object_key

void jbin_object_key_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_key_deinit

/*********************************************************************************/
/*  Add or replace a value in a Json Object.                                     */
/*********************************************************************************/
my_bool jbin_object_add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_object_add_init(initid, args, message);
} // end of jbin_object_add_init

char *jbin_object_add(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
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

	if (!CheckMemory(g, initid, args, 2, false, true, true)) {
		char *key;
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0, &top);
		PJSON jsp = jvp->GetJson();

		if (CheckPath(g, args, jsp, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			PGLOBAL gb = GetMemPtr(g, args, 0);

			jobp = jvp->GetObject();
			jvp = MakeValue(gb, args, 1);
			key = MakeKey(gb, args, 1);
			jobp->SetValue(gb, jvp, key);
		} else {
			PUSH_WARNING("First argument target is not an object");
//		if (g->Mrr) *error = 1;			 (only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_object_add

void jbin_object_add_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_add_deinit

/*********************************************************************************/
/*  Delete a value from a Json object.                                           */
/*********************************************************************************/
my_bool jbin_object_delete_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_object_delete_init(initid, args, message);
} // end of jbin_object_delete_init

char *jbin_object_delete(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
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

	if (!CheckMemory(g, initid, args, 1, false, true, true)) {
		char *key;
		PJOB  jobp;
		PJVAL jvp = MakeValue(g, args, 0, &top);
		PJSON jsp = jvp->GetJson();

		if (CheckPath(g, args, top, jvp, 2))
			PUSH_WARNING(g->Message);
		else if (jvp && jvp->GetValType() == TYPE_JOB) {
			key = MakeKey(g, args, 1);
			jobp = jvp->GetObject();
			jobp->DeleteKey(key);
		} else {
			PUSH_WARNING("First argument target is not an object");
//		if (g->Mrr) *error = 1;					(only if no path)
		} // endif jvp

	} // endif CheckMemory

	// In case of error unchanged argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_object_delete

void jbin_object_delete_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_delete_deinit

/*********************************************************************************/
/*  Returns an array of the Json object keys.                                    */
/*********************************************************************************/
my_bool jbin_object_list_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_object_list_init(initid, args, message);
} // end of jbin_object_list_init

char *jbin_object_list(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PJAR    jarp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (!bsp || bsp->Changed) {
		if (!CheckMemory(g, initid, args, 1, true, true)) {
			char *p;
			PJSON jsp;
			PJVAL jvp = MakeValue(g, args, 0);

			if ((p = jvp->GetString())) {
				if (!(jsp = ParseJson(g, p, strlen(p)))) {
					PUSH_WARNING(g->Message);
					return NULL;
				} // endif jsp

			} else
				jsp = jvp->GetJson();

			if (jsp->GetType() == TYPE_JOB) {
				jarp = ((PJOB)jsp)->GetKeyList(g);
			} else {
				PUSH_WARNING("First argument is not an object");
				if (g->Mrr) *error = 1;
			} // endif jsp type

		} // endif CheckMemory

		if ((bsp = JbinAlloc(g, args, initid->max_length, jarp)))
			strcat(bsp->Msg, " array");

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
} // end of jbin_object_list

void jbin_object_list_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_object_list_deinit

/*********************************************************************************/
/*  Get a Json item from a Json document.                                        */
/*********************************************************************************/
my_bool jbin_get_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_get_item_init(initid, args, message);
} // end of jbin_get_item_init

char *jbin_get_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp;
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		bsp = (PBSON)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true, true)) {
			PUSH_WARNING("CheckMemory error");
			goto fin;
		} else
			jvp = MakeValue(g, args, 0);

		if ((p = jvp->GetString())) {
			if (!(jsp = ParseJson(g, p, strlen(p)))) {
				PUSH_WARNING(g->Message);
				goto fin;
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
	jsx = new(g) JSNX(g, jsp, TYPE_STRING, initid->max_length);

	if (jsx->SetJpath(g, path, false)) {
		PUSH_WARNING(g->Message);
		goto fin;
	}	// endif SetJpath

	// Get the json tree
	if ((jvp = jsx->GetRowValue(g, jsp, 0, false))) {
		jsp = (jvp->GetJsp()) ? jvp->GetJsp() : new(g) JVALUE(g, jvp->GetValue());

		if ((bsp = JbinAlloc(g, args, initid->max_length, jsp)))
			strcat(bsp->Msg, " item");
		else
			*error = 1;

	} // endif jvp

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)bsp;

 fin:
	if (!bsp) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_get_item

void jbin_get_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_get_item_deinit

/*********************************************************************************/
/*  Merge two arrays or objects.                                                 */
/*********************************************************************************/
my_bool jbin_item_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_item_merge_init(initid, args, message);
} // end of jbin_item_merge_init

char *jbin_item_merge(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	PJSON   top = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed) {
		// This constant function was recalled
		*res_length = sizeof(BSON);
		return (char*)bsp;
	} // endif bsp

	if (!CheckMemory(g, initid, args, 2, false, false, true)) {
		PJVAL   jvp;
		PJSON   jsp[2] = {NULL, NULL};
		PGLOBAL gb = GetMemPtr(g, args, 0);

		for (int i = 0; i < 2; i++) {
			jvp = MakeValue(g, args, i);
			if (!i) top = jvp->GetJson();

			if (jvp->GetValType() != TYPE_JAR && jvp->GetValType() != TYPE_JOB) {
				sprintf(g->Message, "Argument %d is not an array or object", i);
				PUSH_WARNING(g->Message);
			} else
				jsp[i] = jvp->GetJsp();

		} // endfor i

		if (jsp[0] && jsp[0]->Merge(gb, jsp[1]))
			PUSH_WARNING(gb->Message);

	} // endif CheckMemory

	// In case of error unchanged first argument will be returned
	bsp = MakeBinResult(g, args, top, initid->max_length);

	if (initid->const_item)
		// Keep result of constant function
		g->Xchk = bsp;

	if (!bsp) {
		*is_null = 1;
		*error = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_item_merge

void jbin_item_merge_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_item_merge_deinit

/*********************************************************************************/
/*  Set Json items of a Json document according to path.                         */
/*********************************************************************************/
my_bool jbin_set_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_set_item_init(initid, args, message);
} // end of jbin_set_item_init

char *jbin_set_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *p, *path;
	int     w;
	my_bool b = true;
	PJSON   jsp;
	PJSNX   jsx;
	PJVAL   jvp= 0;
	PBSON   bsp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PGLOBAL gb = GetMemPtr(g, args, 0);

	if (g->N) {
		bsp = (PBSON)g->Activityp;
		goto fin;
	} else if (initid->const_item)
		g->N = 1;

	if (!strcmp(result, "$insert"))
		w = 1;
	else if (!strcmp(result, "$update"))
		w = 2;
	else
		w = 0;

	if (!g->Xchk) {
		if (CheckMemory(g, initid, args, 1, true, false, true)) {
			PUSH_WARNING("CheckMemory error");
                        goto fin;
		} else
			jvp = MakeValue(g, args, 0);

		if ((p = jvp->GetString())) {
			if (!(jsp = ParseJson(g, p, strlen(p)))) {
				PUSH_WARNING(g->Message);
				goto fin;
			} // endif jsp

		} else
			jsp = jvp->GetJson();

		if (g->Mrr) {			 // First argument is a constant
			g->Xchk = jsp;
			JsonMemSave(g);
		} // endif Mrr

	} else
		jsp = (PJSON)g->Xchk;

	jsx = new(g)JSNX(g, jsp, TYPE_STRING, initid->max_length, 0, true);

	for (uint i = 1; i+1 < args->arg_count; i += 2) {
		jvp = MakeValue(gb, args, i);
		path = MakePSZ(g, args, i+1);

		if (jsx->SetJpath(g, path, false)) {
			PUSH_WARNING(g->Message);
			continue;
		}	// endif SetJpath

		if (w) {
			jsx->ReadValue(g);
			b = jsx->GetValue()->IsNull();
			b = (w == 1) ? b : !b;
		}	// endif w

		if (b && jsx->WriteValue(gb, jvp))
			PUSH_WARNING(g->Message);

	} // endfor i

	if (!(bsp = MakeBinResult(g, args, jsp, initid->max_length, INT_MAX32)))
		*error = 1;

	if (initid->const_item)
		// Keep result of constant function
		g->Activityp = (PACTIVITY)bsp;

 fin:
	if (!bsp) {
		*is_null = 1;
		*res_length = 0;
	} else
		*res_length = sizeof(BSON);

	return (char*)bsp;
} // end of jbin_set_item

void jbin_set_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_set_item_deinit

/*********************************************************************************/
/*  Insert Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool jbin_insert_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_set_item_init(initid, args, message);
} // end of jbin_insert_item_init

char *jbin_insert_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$insert");
	return jbin_set_item(initid, args, result, res_length, is_null, p);
} // end of jbin_insert_item

void jbin_insert_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_insert_item_deinit

/*********************************************************************************/
/*  Update Json items of a Json document according to path.                      */
/*********************************************************************************/
my_bool jbin_update_item_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return json_set_item_init(initid, args, message);
} // end of jbin_update_item_init

char *jbin_update_item(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *p)
{
	strcpy(result, "$update");
	return jbin_set_item(initid, args, result, res_length, is_null, p);
} // end of jbin_update_item

void jbin_update_item_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_update_item_deinit

/*********************************************************************************/
/*  Returns a json file as a json item.                                          */
/*********************************************************************************/
my_bool jbin_file_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, fl, more = 1024;

	if (args->arg_count < 1 || args->arg_count > 4) {
		strcpy(message, "This function only accepts 1 to 4 arguments");
		return true;
	} else if (args->arg_type[0] != STRING_RESULT || !args->args[0]) {
		strcpy(message, "First argument must be a constant string (file name)");
		return true;
	} else if (args->arg_count > 1 &&	args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a string (path)");
		return true;
	} else if (args->arg_count > 2 &&	args->arg_type[2] != INT_RESULT) {
		strcpy(message, "Third argument is not an integer (pretty)");
		return true;
	} else if (args->arg_count > 3) {
		if (args->arg_type[3] != INT_RESULT) {
			strcpy(message, "Fourth argument is not an integer (memory)");
			return true;
		} else
			more += (ulong)*(longlong*)args->args[3];

	} // endifs

	initid->maybe_null = 1;
	CalcLen(args, false, reslen, memlen);
	fl = GetFileLength(args->args[0]);
	reslen += fl;
	more += fl * M;
	memlen += more;
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of jbin_file_init

char *jbin_file(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *is_null, char *error)
{
	char   *fn;
	int     pretty, len = 0, pty = 3;
	PJSON   jsp;
	PJVAL   jvp = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;
	PBSON   bsp = (PBSON)g->Xchk;

	if (bsp && !bsp->Changed)
		goto fin;

	PlugSubSet(g, g->Sarea, g->Sarea_Size);
	g->Xchk = NULL;
	fn = MakePSZ(g, args, 0);
	pretty = (args->arg_count > 2 && args->args[2]) ? (int)*(longlong*)args->args[2] : 3;

	/*********************************************************************************/
	/*  Parse the json file and allocate its tree structure.                         */
	/*********************************************************************************/
	if (!(jsp = ParseJsonFile(g, fn, &pty, len))) {
		PUSH_WARNING(g->Message);
		*error = 1;
		goto fin;
	} // endif jsp

	if (pty == 3)
		PUSH_WARNING("File pretty format cannot be determined");
	else if (pretty != 3 && pty != pretty)
		PUSH_WARNING("File pretty format doesn't match the specified pretty value");
	else if (pretty == 3)
		pretty = pty;

	if ((bsp = JbinAlloc(g, args, len, jsp))) {
		strcat(bsp->Msg, " file");
		bsp->Filename = fn;
		bsp->Pretty = pretty;
	} else {
		*error = 1;
		goto fin;
	}	// endif bsp

	// Check whether a path was specified
	if (CheckPath(g, args, jsp, jvp, 1)) {
		PUSH_WARNING(g->Message);
		bsp = NULL;
		goto fin;
	} else if (jvp)
		bsp->Jsp = jvp->GetJsp();

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
} // end of jbin_file

void jbin_file_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of jbin_file_deinit

/*********************************************************************************/
/*  Serialize a Json document.                .                                  */
/*********************************************************************************/
my_bool json_serialize_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 1) {
		strcpy(message, "This function must have 1 argument");
		return true;
	} else if (IsJson(args, 0) != 3) {
		strcpy(message, "Argument must be a Jbin tree");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, false, reslen, memlen);
} // end of json_serialize_init

char *json_serialize(UDF_INIT *initid, UDF_ARGS *args, char *result,
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
} // end of json_serialize

void json_serialize_deinit(UDF_INIT* initid)
{
	JsonFreeMem((PGLOBAL)initid->ptr);
} // end of json_serialize_deinit

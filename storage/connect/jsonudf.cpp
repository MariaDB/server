/************* jsonudf C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: jsonudf     Version 1.1                               */
/*  (C) Copyright to the author Olivier BERTRAND          2015         */
/*  This program are the JSON User Defined Functions     .             */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
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

/* ------------------------------ JSNX ------------------------------- */

/***********************************************************************/
/*  JSNX public constructor.                                           */
/***********************************************************************/
JSNX::JSNX(PGLOBAL g, PJSON row, int type, int len, int prec)
{
	Row = row;
	Value = AllocateValue(g, type, len, prec);
	MulVal = NULL;
	Nodes = NULL;
	Jp = NULL;
	Jpath = NULL;
	Buf_Type = type;
	Long = len;
	Prec = prec;
	Nod = 0;
	Xnod = -1;
	B = 0;
	Xpd = false;
	Parsed = false;
} // end of JSNX constructor

/***********************************************************************/
/*  SetJpath: set and parse the json path.                             */
/***********************************************************************/
my_bool JSNX::SetJpath(PGLOBAL g, char *path)
{
	// Check Value was allocated
	if (!Value)
		return true;

	Value->SetNullable(true);
	Jpath = path;

	// Parse the json path
	return ParseJpath(g);
} // end of SetJpath

/***********************************************************************/
/*  Check whether this object is expanded.                             */
/***********************************************************************/
my_bool JSNX::CheckExpand(PGLOBAL g, int i, PSZ nm, my_bool b)
{
#if 0
	if ((Tjp->Xcol && nm && !strcmp(nm, Tjp->Xcol) &&
		(Tjp->Xval < 0 || Tjp->Xval == i)) || Xpd) {
		Xpd = true;              // Expandable object
		Nodes[i].Op = OP_EXP;
	} else if (b) {
		strcpy(g->Message, "Cannot expand more than one branch");
		return true;
	} // endif Xcol
#endif // 0

	return false;
} // end of CheckExpand

/***********************************************************************/
/*  Analyse array processing options.                                  */
/***********************************************************************/
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
			sprintf(g->Message,
//			"Invalid array specification %s for %s", p, Name);
			  "Invalid array specification %s", p);
			return true;
		} // endif p

	} else
		b = true;

	// To check whether a numeric Rank was specified
	for (int k = 0; dg && p[k]; k++)
		dg = isdigit(p[k]) > 0;

	if (!n) {
		// Default specifications
		if (CheckExpand(g, i, nm, false))
			return true;
		else if (jnp->Op != OP_EXP) {
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
#if 0
			if (!Tjp->Xcol && nm) {
				Xpd = true;
				jnp->Op = OP_EXP;
				Tjp->Xval = i;
				Tjp->Xcol = nm;
			} else if (CheckExpand(g, i, nm, true))
				return true;

			break;
#endif // 0
		default:
			sprintf(g->Message,
//			"Invalid function specification %c for %s", *p, Name);
				"Invalid function specification %c", *p);
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
//	sprintf(g->Message, "Wrong array specification for %s", Name);
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

/***********************************************************************/
/*  Parse the eventual passed Jpath information.                       */
/*  This information can be specified in the Fieldfmt column option    */
/*  when creating the table. It permits to indicate the position of    */
/*  the node corresponding to that column.                             */
/***********************************************************************/
my_bool JSNX::ParseJpath(PGLOBAL g)
{
	char   *p, *p2 = NULL, *pbuf = NULL;
	int     i;
	my_bool mul = false;

	if (Parsed)
		return false;                       // Already done
	//else if (InitValue(g))
	//	return true;
	else if (!Jpath)
		//	Jpath = Name;
		return true;

#if 0
	if (To_Tdb->GetOrig()) {
		// This is an updated column, get nodes from origin
		for (PJCOL colp = (PJCOL)Tjp->GetColumns(); colp;
			colp = (PJCOL)colp->GetNext())
			if (!stricmp(Name, colp->GetName())) {
				Nod = colp->Nod;
				Nodes = colp->Nodes;
				goto fin;
			} // endif Name

		sprintf(g->Message, "Cannot parse updated column %s", Name);
		return true;
	} // endif To_Orig
#endif // 0

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

//fin:
	MulVal = AllocateValue(g, Value);
	Parsed = true;
	return false;
} // end of ParseJpath

/***********************************************************************/
/*  MakeJson: Serialize the json item and set value to it.             */
/***********************************************************************/
PVAL JSNX::MakeJson(PGLOBAL g, PJSON jsp)
{
	if (Value->IsTypeNum()) {
		strcpy(g->Message, "Cannot make Json for a numeric value");
		Value->Reset();
	} else
		Value->SetValue_psz(Serialize(g, jsp, NULL, 0));

	return Value;
} // end of MakeJson

/***********************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                      */
/***********************************************************************/
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

/***********************************************************************/
/*  GetJson:                                                           */
/***********************************************************************/
PJVAL JSNX::GetJson(PGLOBAL g)
{
	return GetValue(g, Row, 0);
} // end of GetJson

/***********************************************************************/
/*  ReadValue:                                                         */
/***********************************************************************/
void JSNX::ReadValue(PGLOBAL g)
{
//if (!Tjp->SameRow || Xnod >= Tjp->SameRow)
//	Value->SetValue_pval(GetColumnValue(g, Tjp->Row, 0));
		Value->SetValue_pval(GetColumnValue(g, Row, 0));

	// Set null when applicable
//if (Nullable)
//	Value->SetNull(Value->IsZero());

} // end of ReadValue

/***********************************************************************/
/*  GetColumnValue:                                                    */
/***********************************************************************/
PVAL JSNX::GetColumnValue(PGLOBAL g, PJSON row, int i)
{
	int   n = Nod - 1;
//my_bool expd = false;
//PJAR  arp;
	PJVAL val = NULL;

#if 0
	for (; i < Nod && row; i++) {
		if (Nodes[i].Op == OP_NUM) {
			Value->SetValue(row->GetType() == TYPE_JAR ? row->size() : 1);
			return(Value);
		} else if (Nodes[i].Op == OP_XX) {
			return MakeJson(g, row);
		} else switch (row->GetType()) {
		case TYPE_JOB:
			if (!Nodes[i].Key) {
				// Expected Array was not there
				if (i < Nod-1)
					continue;
				else
					val = new(g)JVALUE(row);

			} else
				val = ((PJOB)row)->GetValue(Nodes[i].Key);

			break;
		case TYPE_JAR:
			arp = (PJAR)row;

			if (!Nodes[i].Key) {
				if (Nodes[i].Op == OP_EQ)
					val = arp->GetValue(Nodes[i].Rank);
				else if (Nodes[i].Op == OP_EXP)
					return ExpandArray(g, arp, i);
				else
					return CalculateArray(g, arp, i);

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
#endif // 0

	val = GetValue(g, row, i);
	SetJsonValue(g, Value, val, n);
	return Value;
} // end of GetColumnValue

/***********************************************************************/
/*  GetValue:                                                          */
/***********************************************************************/
PJVAL JSNX::GetValue(PGLOBAL g, PJSON row, int i)
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
			return new(g) JVALUE(g, MakeJson(g, row));
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

/***********************************************************************/
/*  ExpandArray:                                                       */
/***********************************************************************/
PVAL JSNX::ExpandArray(PGLOBAL g, PJAR arp, int n)
{
#if 0
	int    ars;
	PJVAL  jvp;
	JVALUE jval;

	ars = MY_MIN(Tjp->Limit, arp->size());

	if (!(jvp = arp->GetValue((Nodes[n].Rx = Nodes[n].Nx)))) {
		strcpy(g->Message, "Logical error expanding array");
		longjmp(g->jumper[g->jump_level], 666);
	} // endif jvp

	if (n < Nod - 1 && jvp->GetJson()) {
		jval.SetValue(GetColumnValue(g, jvp->GetJson(), n + 1));
		jvp = &jval;
	} // endif n

	if (n >= Tjp->NextSame) {
		if (++Nodes[n].Nx == ars) {
			Nodes[n].Nx = 0;
			Xnod = 0;
		} else
			Xnod = n;

		Tjp->NextSame = Xnod;
	} // endif NextSame 

	SetJsonValue(g, Value, jvp, n);
	return Value;
#endif // 0
	strcpy(g->Message, "Expand cannot be done by this function");
	return NULL;
} // end of ExpandArray

/***********************************************************************/
/*  CalculateArray:                                                    */
/***********************************************************************/
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

#if 0
/***********************************************************************/
/*  GetRow: Get the object containing this column.                     */
/***********************************************************************/
PJSON JSNX::GetRow(PGLOBAL g)
{
	PJVAL val = NULL;
	PJAR  arp;
//PJSON nwr, row = Tjp->Row;
	PJSON nwr, row = Row;

	for (int i = 0; i < Nod-1 && row; i++) {
		if (Nodes[i+1].Op == OP_XX)
			break;
		else switch (row->GetType()) {
		case TYPE_JOB:
			if (!Nodes[i].Key)
				// Expected Array was not there
				continue;

			val = ((PJOB)row)->GetValue(Nodes[i].Key);
			break;
		case TYPE_JAR:
			if (!Nodes[i].Key) {
				arp = (PJAR)row;

				if (Nodes[i].Op == OP_EQ)
					val = arp->GetValue(Nodes[i].Rank);
				else
					val = arp->GetValue(Nodes[i].Rx);

			} else {
				strcpy(g->Message, "Unexpected array");
				val = NULL;          // Not an expected array
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
/*  WriteColumn:                                                       */
/***********************************************************************/
void JSNX::WriteColumn(PGLOBAL g)
{
	/*********************************************************************/
	/*  Check whether this node must be written.                         */
	/*********************************************************************/
	if (Value != To_Val)
		Value->SetValue_pval(To_Val, FALSE);    // Convert the updated value

	/*********************************************************************/
	/*  On INSERT Null values are represented by no node.                */
	/*********************************************************************/
	if (Value->IsNull() && Tjp->Mode == MODE_INSERT)
		return;

	char *s;
	PJOB  objp = NULL;
	PJAR  arp = NULL;
	PJVAL jvp = NULL;
	PJSON jsp, row = GetRow(g);
	JTYP  type = row->GetType();

	switch (row->GetType()) {
	case TYPE_JOB:  objp = (PJOB)row;  break;
	case TYPE_JAR:  arp  = (PJAR)row;  break;
	case TYPE_JVAL: jvp  = (PJVAL)row; break;
	default: row = NULL;     // ???????????????????????????
	} // endswitch Type

	if (row) switch (Buf_Type) {
	case TYPE_STRING:
		if (Nodes[Nod-1].Op == OP_XX) {
			s = Value->GetCharValue();

			if (!(jsp = ParseJson(g, s, (int)strlen(s)))) {
				strcpy(g->Message, s);
				longjmp(g->jumper[g->jump_level], 666);
			} // endif jsp

			if (arp) {
				if (Nod > 1 && Nodes[Nod-2].Op == OP_EQ)
					arp->SetValue(g, new(g)JVALUE(jsp), Nodes[Nod-2].Rank);
				else
					arp->AddValue(g, new(g)JVALUE(jsp));

				arp->InitArray(g);
			} else if (objp) {
				if (Nod > 1 && Nodes[Nod-2].Key)
					objp->SetValue(g, new(g)JVALUE(jsp), Nodes[Nod-2].Key);

			} else if (jvp)
				jvp->SetValue(jsp);

			break;
		} // endif Op

		// Passthru
	case TYPE_DATE:
	case TYPE_INT:
	case TYPE_DOUBLE:
		if (arp) {
			if (Nodes[Nod-1].Op == OP_EQ)
				arp->SetValue(g, new(g)JVALUE(g, Value), Nodes[Nod-1].Rank);
			else
				arp->AddValue(g, new(g)JVALUE(g, Value));

			arp->InitArray(g);
		} else if (objp) {
			if (Nodes[Nod-1].Key)
				objp->SetValue(g, new(g)JVALUE(g, Value), Nodes[Nod-1].Key);

		} else if (jvp)
			jvp->SetValue(Value);

		break;
	default:                  // ??????????
		sprintf(g->Message, "Invalid column type %d", Buf_Type);
	} // endswitch Type

} // end of WriteColumn
#endif // 0

/***********************************************************************/
/* Locate a value in a JSON tree:                                      */
/***********************************************************************/
PSZ JSNX::Locate(PGLOBAL g, PJSON jsp, char *what,
enum Item_result type, unsigned long len)
{
	my_bool b = false, err = true;

	g->Message[0] = 0;

	if (!jsp) {
		strcpy(g->Message, "Null json tree");
		return NULL;
	} else 		// Write to the path string
		Jp = new(g)JOUTPATH(g, what, type, len);

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

	} else if (Jp->Found) {
		Jp->WriteChr('\0');
		PlugSubAlloc(g, NULL, Jp->N);
		return Jp->Strp;
	} // endif's

	return NULL;
} // end of Locate

/***********************************************************************/
/* Locate in a JSON Array.                                             */
/***********************************************************************/
my_bool JSNX::LocateArray(PJAR jarp)
{
	char   s[16];
	size_t m = Jp->N;

	for (int i = 0; i < jarp->size() && !Jp->Found; i++) {
		Jp->N = m;
		sprintf(s, "[%d]", i + B);

		if (Jp->WriteStr(s))
			return true;

		if (LocateValue(jarp->GetValue(i)))
			return true;

		} // endfor i

	return false;
} // end of LocateArray

/***********************************************************************/
/* Locate in a JSON Object.                                            */
/***********************************************************************/
my_bool JSNX::LocateObject(PJOB jobp)
{
	size_t m = Jp->N;

	for (PJPR pair = jobp->First; pair && !Jp->Found; pair = pair->Next) {
		Jp->N = m;

		if (Jp->WriteStr(pair->Key))
			return true;

		if (LocateValue(pair->Val))
			return true;

		} // endfor i

	return false;
} // end of LocateObject

/***********************************************************************/
/* Locate a JSON Value.                                                */
/***********************************************************************/
my_bool JSNX::LocateValue(PJVAL jvp)
{
	char *p, buf[32];
	PJAR  jap;
	PJOB  jop;
	PVAL  valp;

	if ((jap = jvp->GetArray())) {
		if (Jp->WriteChr(':'))
			return true;

		return LocateArray(jap);
	} else if ((jop = jvp->GetObject())) {
		if (Jp->WriteChr(':'))
			return true;

		return LocateObject(jop);
	} else if (!(valp = jvp->Value) || valp->IsNull())
		return false;
	else switch (Jp->Type) {
		case STRING_RESULT:
			p = valp->GetCharString(buf);
			Jp->Found = (strlen(p) == Jp->Len &&
				!strncmp(Jp->What, valp->GetCharString(buf), Jp->Len));
			break;
		case INT_RESULT:
			Jp->Found = *(longlong*)Jp->What == valp->GetBigintValue();
			break;
		case DECIMAL_RESULT:
			Jp->Found = atof(Jp->What) == valp->GetFloatValue();
			break;
		case REAL_RESULT:
			Jp->Found = *(double*)Jp->What == valp->GetFloatValue();
			break;
		default:
			sprintf(Jp->g->Message, "Invalid type %d", Buf_Type);
			return true;
		} // endswitch Type

	return false;
} // end of LocateValue

/* ---------------------------- JSON UDF ----------------------------- */

/***********************************************************************/
/*  Program for SubSet re-initialization of the memory pool.           */
/***********************************************************************/
static my_bool JsonSubSet(PGLOBAL g)
{
	PPOOLHEADER pph = (PPOOLHEADER)g->Sarea;

	pph->To_Free = (OFFSET)((g->Createas) ? g->Createas : sizeof(POOLHEADER));
	pph->FreeBlk = g->Sarea_Size - pph->To_Free;
	return FALSE;
} /* end of JsonSubSet */

/***********************************************************************/
/*  Program for saving the status of the memory pools.                 */
/***********************************************************************/
inline void JsonMemSave(PGLOBAL g)
{
	g->Createas = (int)((PPOOLHEADER)g->Sarea)->To_Free;
} /* end of JsonMemSave */

/***********************************************************************/
/*  Allocate and initialise the memory area.                           */
/***********************************************************************/
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
  } else
    initid->ptr = (char*)g;

	g->Mrr = (args->arg_count && args->args[0]) ? 1 : 0;
	g->Alchecked = (initid->const_item) ? 1 : 0;
  initid->maybe_null = mbn;
  initid->max_length = reslen;
  return false;
} // end of JsonInit

/***********************************************************************/
/*  Check if a path was specified and set jvp according to it.         */
/***********************************************************************/
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

/***********************************************************************/
/*  Make the result according to the first argument type.              */
/***********************************************************************/
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
	} else if (!(str = Serialize(g, top, NULL, 0)))
		PUSH_WARNING(g->Message);

	return str;
} // end of MakeResult

/***********************************************************************/
/*  Returns not 0 if the argument is a JSON item or file name.         */
/***********************************************************************/
static int IsJson(UDF_ARGS *args, uint i)
{
	int n = 0;

	if (i >= args->arg_count)
		n = 0;
	else if (!strnicmp(args->attributes[i], "Json_", 5))
		n = 1;					 // arg is a json item
	else if (args->arg_type[i] == STRING_RESULT && 
		  !strnicmp(args->attributes[i], "Jfile_", 6))
		n = 2;					 //	arg is a json file name

	return n;
} // end of IsJson

/***********************************************************************/
/*  GetFileLength: returns file size in number of bytes.               */
/***********************************************************************/
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

/***********************************************************************/
/*  Calculate the reslen and memlen needed by a function.              */
/***********************************************************************/
static my_bool CalcLen(UDF_ARGS *args, my_bool obj,
                       unsigned long& reslen, unsigned long& memlen,
											 my_bool mod = false)
{
	char fn[_MAX_PATH];
  unsigned long i, k, n;
	long fl, j = -1;

  reslen = args->arg_count + 2;

  // Calculate the result max length
  for (i = 0; i < args->arg_count; i++) {
    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      reslen += (k + 3);     // For quotes and :
      } // endif obj

    switch (args->arg_type[i]) {
      case STRING_RESULT:
				if (IsJson(args, i) == 2 && args->args[i]) {
					if (!mod) {
						n = MY_MIN(args->lengths[i], sizeof(fn) - 1);
						memcpy(fn, args->args[i], n);
						fn[n] = 0;
						j = i;
						fl = GetFileLength(fn);
						reslen += fl;
					} else
						reslen += args->lengths[i];

				} else if (IsJson(args, i) == 1)
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
    memlen += (args->lengths[i] + sizeof(JVALUE));

    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      memlen += (k + sizeof(JOBJECT) + sizeof(JPAIR));
    } else
      memlen += sizeof(JARRAY);

    switch (args->arg_type[i]) {
      case STRING_RESULT:
				if (IsJson(args, i) == 2 && args->args[i]) {
					if ((signed)i != j) {
						n = MY_MIN(args->lengths[i], sizeof(fn) - 1);
						memcpy(fn, args->args[i], n);
						fn[n] = 0;
						j = -1;
						fl = GetFileLength(fn);
					}	// endif i

					memlen += fl * M;
				}	else if (IsJson(args, i) == 1)
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

/***********************************************************************/
/*  Check if the calculated memory is enough.                          */
/***********************************************************************/
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

/***********************************************************************/
/*  Make a zero terminated string from the passed argument.            */
/***********************************************************************/
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

/***********************************************************************/
/*  Make a valid key from the passed argument.                         */
/***********************************************************************/
static PSZ MakeKey(PGLOBAL g, UDF_ARGS *args, int i)
{
	if (args->arg_count > (unsigned)i) {
		int     n = args->attribute_lengths[i];
		my_bool b;  // true if attribute is zero terminated
		PSZ     p, s = args->attributes[i];

		if (s && *s && (n || *s == '\'')) {
			if ((b = (!n || !s[n])))
				n = strlen(s);

			if (n > 5 && IsJson(args, i)) {
				s += 5;
				n -= 5;
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

/***********************************************************************/
/*  Return a json file contains.                                       */
/***********************************************************************/
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

/***********************************************************************/
/*  Make a JSON value from the passed argument.                        */
/***********************************************************************/
static PJVAL MakeValue(PGLOBAL g, UDF_ARGS *args, uint i)
{
	char *sap = (args->arg_count > i) ? args->args[i] : NULL;
	int   n, len;
	long long bigint;
  PJSON jsp;
  PJVAL jvp = new(g) JVALUE;

  if (sap) switch (args->arg_type[i]) {
    case STRING_RESULT:
      if ((len = args->lengths[i])) {
				sap = MakePSZ(g, args, i);

        if ((n = IsJson(args, i))) {
					if (n == 2) {
						if  (!(sap = GetJsonFile(g, sap)))
							PUSH_WARNING(g->Message);

						len = (sap) ? strlen(sap) : 0;
					} // endif n

          if (!(jsp = ParseJson(g, sap, len, 3)))
            PUSH_WARNING(g->Message);

          if (jsp && jsp->GetType() == TYPE_JVAL)
            jvp = (PJVAL)jsp;
          else
            jvp->SetValue(jsp);

        } else
          jvp->SetString(g, sap);

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

/***********************************************************************/
/*  Make a Json value containing the parameter.                        */
/***********************************************************************/
my_bool Json_Value_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  if (args->arg_count > 1) {
    strcpy(message, "Json_Value cannot accept more than 1 argument");
    return true;
  } else
    CalcLen(args, false, reslen, memlen);

  return JsonInit(initid, args, message, false, reslen, memlen);
} // end of Json_Value_init

char *Json_Value(UDF_INIT *initid, UDF_ARGS *args, char *result, 
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
		g->Xchk = (g->Alchecked) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Value

void Json_Value_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Value_deinit

/***********************************************************************/
/*  Make a Json array containing all the parameters.                   */
/***********************************************************************/
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
		g->Xchk = (g->Alchecked) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Array

void Json_Array_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_deinit

/***********************************************************************/
/*  Add one or several values to a Json array.                         */
/***********************************************************************/
my_bool Json_Array_Add_Values_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Array_Add must have at least 2 arguments");
		return true;
	} else if (IsJson(args, 0) != 1) {
		strcpy(message, "Json_Array_Add first argument must be a json string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Array_Add_Values_init

char *Json_Array_Add_Values(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
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
		g->Xchk = (g->Alchecked) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Array_Add_Values

void Json_Array_Add_Values_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Add_Values_deinit

/***********************************************************************/
/*  Add one value to a Json array.                                     */
/***********************************************************************/
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
                     unsigned long *res_length, char *, char *)
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
		} else
			PUSH_WARNING("First argument is not an array");

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Array_Add

void Json_Array_Add_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Add_deinit

/***********************************************************************/
/*  Delete a value from a Json array.                                  */
/***********************************************************************/
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
                        unsigned long *res_length, char *, char *)
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
		} else
			PUSH_WARNING("First argument is not an array");

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Array_Delete

void Json_Array_Delete_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Delete_deinit

/***********************************************************************/
/*  Make a Json Oject containing all the parameters.                   */
/***********************************************************************/
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
		g->Xchk = (g->Alchecked) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Object

void Json_Object_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_deinit

/***********************************************************************/
/*  Make a Json Oject containing all not null parameters.              */
/***********************************************************************/
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
		g->Xchk = (g->Alchecked) ? str : NULL;
	} else
		str = (char*)g->Xchk;

	*res_length = strlen(str);
  return str;
} // end of Json_Object_Nonull

void Json_Object_Nonull_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_nonull_deinit

/***********************************************************************/
/*  Add or replace a value in a Json Object.                           */
/***********************************************************************/
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
	                    unsigned long *res_length, char *, char *)
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
		} else
			PUSH_WARNING("First argument is not an object");

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Object_Add

void Json_Object_Add_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_Add_deinit

/***********************************************************************/
/*  Delete a value from a Json object.                                 */
/***********************************************************************/
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
												 unsigned long *res_length, char *, char *)
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
		} else
			PUSH_WARNING("First argument is not an object");

	} // endif CheckMemory

	// In case of error or file, return unchanged argument
	if (!str)
		str = MakePSZ(g, args, 0);

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

	*res_length = strlen(str);
	return str;
} // end of Json_Object_Delete

void Json_Object_Delete_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_Delete_deinit

/***********************************************************************/
/*  Returns an array of the Json object keys.                          */
/***********************************************************************/
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
	unsigned long *res_length, char *, char *)
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

			} else
				PUSH_WARNING("First argument is not an object");


		} // endif CheckMemory

		if (g->Alchecked) {
			// Keep result of constant function
			g->Xchk = str;
			g->N = 1;			// str can be NULL
			} // endif Alchecked

	} else
		str = (char*)g->Xchk;

	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Object_List

void Json_Object_List_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_List_deinit

/***********************************************************************/
/*  Make a Json array from values coming from rows.                    */
/***********************************************************************/
my_bool Json_Array_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 1) {
    strcpy(message, "Json_Array_Grp can only accept 1 argument");
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
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Grp_deinit

/***********************************************************************/
/*  Make a Json object from values coming from rows.                   */
/***********************************************************************/
my_bool Json_Object_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 2) {
    strcpy(message, "Json_Array_Grp can only accept 2 arguments");
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
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_Grp_deinit

/***********************************************************************/
/*  Get a string value from a Json item.                               */
/***********************************************************************/
my_bool Json_Get_String_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Get_String must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Json_Get_String first argument must be a json item");
		return true;
  } else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	if (IsJson(args, 0) == 2) {
		char fn[_MAX_PATH];
		long fl;

		memcpy(fn, args->args[0], args->lengths[0]);
		fn[args->lengths[0]] = 0;
		fl = GetFileLength(fn);
		memlen += fl * 3;
	}	else
		memlen += args->lengths[0] * 3;

	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Get_String_init

char *Json_Get_String(UDF_INIT *initid, UDF_ARGS *args, char *result,
	             unsigned long *res_length, char *, char *)
{
	char   *str = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (g->Alchecked)
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
			return NULL;
		}	// endif SetJpath

		jsx->ReadValue(g);

		if (!jsx->GetValue()->IsNull())
			str = jsx->GetValue()->GetCharValue();

		if (g->Alchecked)
			// Keep result of constant function
			g->Xchk = str;

	} // endif CheckMemory

 fin:
	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Get_String

void Json_Get_String_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Get_String_deinit

/***********************************************************************/
/*  Get an integer value from a Json item.                             */
/***********************************************************************/
my_bool Json_Get_Int_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count != 2) {
		strcpy(message, "Json_Get_Int must have 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Json_Get_Int first argument must be a json item");
		return true;
	} else if (args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Second argument is not a (jpath) string");
		return true;
	} else
		CalcLen(args, false, reslen, memlen);

	memlen += 1000;       // TODO: calculate this
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Get_Int_init

long long Json_Get_Int(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                     unsigned long *res_length, char *, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N)
		return (g->Xchk) ? *(long long*)g->Xchk : NULL;
	else if (g->Alchecked)
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
		jsx = new(g)JSNX(g, jsp, TYPE_BIGINT);

		if (jsx->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			return NULL;
		} // endif SetJpath

		jsx->ReadValue(g);

		if (jsx->GetValue()->IsNull()) {
			PUSH_WARNING("Value not found");
			return NULL;
		}	// endif IsNull

		n = jsx->GetValue()->GetBigintValue();

		if (g->Alchecked) {
			// Keep result of constant function
			long long *np = (long long*)PlugSubAlloc(g, NULL, sizeof(long long));
			*np = n;
			g->Xchk = np;
		} // endif Alchecked

		return n;
	} else
		return NULL;

} // end of Json_Get_Int

void Json_Get_Int_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Get_Int_deinit

/***********************************************************************/
/*  Get a double value from a Json item.                               */
/***********************************************************************/
my_bool Json_Get_Real_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Get_Real must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[1] != STRING_RESULT) {
		strcpy(message, "Json_Get_Real first argument must be a json item");
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
	memlen += 1000;       // TODO: calculate this
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Get_Real_init

double Json_Get_Real(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                   unsigned long *res_length, char *, char *)
{
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N)
		return (g->Xchk) ? *(double*)g->Xchk : NULL;
	else if (g->Alchecked)
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
		jsx = new(g)JSNX(g, jsp, TYPE_DOUBLE);

		if (jsx->SetJpath(g, path)) {
			PUSH_WARNING(g->Message);
			return NULL;
		}	// endif SetJpath

		jsx->ReadValue(g);

		if (jsx->GetValue()->IsNull()) {
			PUSH_WARNING("Value not found");
			return NULL;
		}	// endif IsNull

		d = jsx->GetValue()->GetFloatValue();

		if (g->Alchecked) {
			// Keep result of constant function
			double *dp = (double*)PlugSubAlloc(g, NULL, sizeof(double));
			*dp = d;
			g->Xchk = dp;
		} // endif Alchecked

		return d;
	} else
		return NULL;

} // end of Json_Get_Real

void Json_Get_Real_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Get_Real_deinit

/***********************************************************************/
/*  Locate a value in a Json tree.                                     */
/***********************************************************************/
my_bool Json_Locate_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1000;

	if (args->arg_count < 2) {
		strcpy(message, "Json_Locate must have at least 2 arguments");
		return true;
	} else if (!IsJson(args, 0) && args->arg_type[0] != STRING_RESULT) {
		strcpy(message, "Json_Locate first argument must be a json item");
		return true;
	} else if (args->arg_count > 2)
		if (args->arg_type[2] != INT_RESULT) {
			strcpy(message, "Third argument is not an integer (memory)");
			return true;
		} else
			more = (ulong)*(longlong*)args->args[2];

	CalcLen(args, false, reslen, memlen);
	memlen += more;       // TODO: calculate this
	return JsonInit(initid, args, message, true, reslen, memlen);
} // end of Json_Locate_init

char *Json_Locate(UDF_INIT *initid, UDF_ARGS *args, char *result,
	                unsigned long *res_length, char *, char *)
{
	char   *path = NULL;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		path = (char*)g->Xchk;
		*res_length = (path) ? strlen(path) : 0;
		return path;
	} else if (g->Alchecked)
		g->N = 1;

	if (!CheckMemory(g, initid, args, 1, false)) {
		char *p;
		int   rc;
		PJVAL jvp;
		PJSON jsp;
		PJSNX jsx;

		// Save stack and allocation environment and prepare error return
		if (g->jump_level == MAX_JUMP) {
			PUSH_WARNING(MSG(TOO_MANY_JUMPS));
			return NULL;
		} // endif jump_level

		if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
			PUSH_WARNING(g->Message);
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

		jsx = new(g)JSNX(g, jsp, TYPE_STRING);
		path = jsx->Locate(g, jsp, args->args[1], args->arg_type[1], args->lengths[1]);

		if (g->Alchecked)
			// Keep result of constant function
			g->Xchk = path;

	err:
		g->jump_level--;
		*res_length = (path) ? strlen(path) : 0;
		return path;
	} else
		return NULL;

} // end of Json_Locate

void Json_Locate_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Locate_deinit

/***********************************************************************/
/*  Returns a json file as a string.                                   */
/***********************************************************************/
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
	unsigned long *res_length, char *, char *)
{
	char   *str, *fn;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (g->Alchecked)
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

		/*******************************************************************/
		/*  Create the mapping file object.                                */
		/*******************************************************************/
		hFile = CreateFileMap(g, fn, &mm, MODE_READ, false);

		if (hFile == INVALID_HANDLE_VALUE) {
			DWORD rc = GetLastError();

			if (!(*g->Message))
				sprintf(g->Message, MSG(OPEN_MODE_ERROR), "map", (int)rc, fn);

			PUSH_WARNING(g->Message);
			return NULL;
		} // endif hFile

		/*******************************************************************/
		/*  Get the file size (assuming file is smaller than 4 GB)         */
		/*******************************************************************/
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
		hFile = INVALID_HANDLE_VALUE;              // For Fblock

		/*********************************************************************/
		/*  Parse the json file and allocate its tree structure.             */
		/*********************************************************************/
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

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

 fin:
	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_File

void Json_File_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_File_deinit

/***********************************************************************/
/*  Make a json file from a json item.                                 */
/***********************************************************************/
my_bool Json_Make_File_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	unsigned long reslen, memlen, more = 1024;

	if (args->arg_count < 2 || args->arg_count > 3) {
		strcpy(message, "Json_Make_File only accepts 2 or 3 arguments");
		return true;
	} else if (IsJson(args, 0) != 1) {
		strcpy(message, "Json_Make_File first argument must be a json item");
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
} // end of Json_Make_File_init

char *Json_Make_File(UDF_INIT *initid, UDF_ARGS *args, char *result,
	unsigned long *res_length, char *, char *)
{
	char   *str, *fn, *msg;
	int     pretty;
	PJVAL   jvp;
	PGLOBAL g = (PGLOBAL)initid->ptr;

	if (g->N) {
		str = (char*)g->Xchk;
		goto fin;
	} else if (g->Alchecked)
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

	if (g->Alchecked)
		// Keep result of constant function
		g->Xchk = str;

 fin:
	*res_length = (str) ? strlen(str) : 0;
	return str;
} // end of Json_Make_File

void Json_Make_File_deinit(UDF_INIT* initid)
{
	PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Make_File_deinit

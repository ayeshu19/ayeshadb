/*-------------------------------------------------------------------------
 *
 * jsonpath_exec.c
 *	 Routines for SQL/JSON path execution.
 *
 * Jsonpath is executed in the global context stored in JsonPathExecContext,
 * which is passed to almost every function involved into execution.  Entry
 * point for jsonpath execution is executeJsonPath() function, which
 * initializes execution context including initial JsonPathItem and JsonbValue,
 * flags, stack for calculation of @ in filters.
 *
 * The result of jsonpath query execution is enum JsonPathExecResult and
 * if succeeded sequence of JsonbValue, written to JsonValueList *found, which
 * is passed through the jsonpath items.  When found == NULL, we're inside
 * exists-query and we're interested only in whether result is empty.  In this
 * case execution is stopped once first result item is found, and the only
 * execution result is JsonPathExecResult.  The values of JsonPathExecResult
 * are following:
 * - jperOk			-- result sequence is not empty
 * - jperNotFound	-- result sequence is empty
 * - jperError		-- error occurred during execution
 *
 * Jsonpath is executed recursively (see executeItem()) starting form the
 * first path item (which in turn might be, for instance, an arithmetic
 * expression evaluated separately).  On each step single JsonbValue obtained
 * from previous path item is processed.  The result of processing is a
 * sequence of JsonbValue (probably empty), which is passed to the next path
 * item one by one.  When there is no next path item, then JsonbValue is added
 * to the 'found' list.  When found == NULL, then execution functions just
 * return jperOk (see executeNextItem()).
 *
 * Many of jsonpath operations require automatic unwrapping of arrays in lax
 * mode.  So, if input value is array, then corresponding operation is
 * processed not on array itself, but on all of its members one by one.
 * executeItemOptUnwrapTarget() function have 'unwrap' argument, which indicates
 * whether unwrapping of array is needed.  When unwrap == true, each of array
 * members is passed to executeItemOptUnwrapTarget() again but with unwrap == false
 * in order to avoid subsequent array unwrapping.
 *
 * All boolean expressions (predicates) are evaluated by executeBoolItem()
 * function, which returns tri-state JsonPathBool.  When error is occurred
 * during predicate execution, it returns jpbUnknown.  According to standard
 * predicates can be only inside filters.  But we support their usage as
 * jsonpath expression.  This helps us to implement @@ operator.  In this case
 * resulting JsonPathBool is transformed into jsonb bool or null.
 *
 * Arithmetic and boolean expression are evaluated recursively from expression
 * tree top down to the leaves.  Therefore, for binary arithmetic expressions
 * we calculate operands first.  Then we check that results are numeric
 * singleton lists, calculate the result and pass it to the next path item.
 *
 * Copyright (c) 2019-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/jsonpath_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/formatting.h"
#include "utils/json.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*
 * Represents "base object" and it's "id" for .keyvalue() evaluation.
 */
typedef struct JsonBaseObjectInfo
{
	JsonbContainer *jbc;
	int			id;
} JsonBaseObjectInfo;

/* Callbacks for executeJsonPath() */
typedef JsonbValue *(*JsonPathGetVarCallback) (void *vars, char *varName, int varNameLen,
											   JsonbValue *baseObject, int *baseObjectId);
typedef int (*JsonPathCountVarsCallback) (void *vars);

/*
 * Context of jsonpath execution.
 */
typedef struct JsonPathExecContext
{
	void	   *vars;			/* variables to substitute into jsonpath */
	JsonPathGetVarCallback getVar;	/* callback to extract a given variable
									 * from 'vars' */
	JsonbValue *root;			/* for $ evaluation */
	JsonbValue *current;		/* for @ evaluation */
	JsonBaseObjectInfo baseObject;	/* "base object" for .keyvalue()
									 * evaluation */
	int			lastGeneratedObjectId;	/* "id" counter for .keyvalue()
										 * evaluation */
	int			innermostArraySize; /* for LAST array index evaluation */
	bool		laxMode;		/* true for "lax" mode, false for "strict"
								 * mode */
	bool		ignoreStructuralErrors; /* with "true" structural errors such
										 * as absence of required json item or
										 * unexpected json item type are
										 * ignored */
	bool		throwErrors;	/* with "false" all suppressible errors are
								 * suppressed */
	bool		useTz;
} JsonPathExecContext;

/* Context for LIKE_REGEX execution. */
typedef struct JsonLikeRegexContext
{
	text	   *regex;
	int			cflags;
} JsonLikeRegexContext;

/* Result of jsonpath predicate evaluation */
typedef enum JsonPathBool
{
	jpbFalse = 0,
	jpbTrue = 1,
	jpbUnknown = 2
} JsonPathBool;

/* Result of jsonpath expression evaluation */
typedef enum JsonPathExecResult
{
	jperOk = 0,
	jperNotFound = 1,
	jperError = 2
} JsonPathExecResult;

#define jperIsError(jper)			((jper) == jperError)

/*
 * List of jsonb values with shortcut for single-value list.
 */
typedef struct JsonValueList
{
	JsonbValue *singleton;
	List	   *list;
} JsonValueList;

typedef struct JsonValueListIterator
{
	JsonbValue *value;
	List	   *list;
	ListCell   *next;
} JsonValueListIterator;

/* Structures for JSON_TABLE execution  */

/*
 * Struct holding the result of jsonpath evaluation, to be used as source row
 * for JsonTableGetValue() which in turn computes the values of individual
 * JSON_TABLE columns.
 */
typedef struct JsonTablePlanRowSource
{
	Datum		value;
	bool		isnull;
} JsonTablePlanRowSource;

/*
 * State of evaluation of row pattern derived by applying jsonpath given in
 * a JsonTablePlan to an input document given in the parent TableFunc.
 */
typedef struct JsonTablePlanState
{
	/* Original plan */
	JsonTablePlan *plan;

	/* The following fields are only valid for JsonTablePathScan plans */

	/* jsonpath to evaluate against the input doc to get the row pattern */
	JsonPath   *path;

	/*
	 * Memory context to use when evaluating the row pattern from the jsonpath
	 */
	MemoryContext mcxt;

	/* PASSING arguments passed to jsonpath executor */
	List	   *args;

	/* List and iterator of jsonpath result values */
	JsonValueList found;
	JsonValueListIterator iter;

	/* Currently selected row for JsonTableGetValue() to use */
	JsonTablePlanRowSource current;

	/* Counter for ORDINAL columns */
	int			ordinal;

	/* Nested plan, if any */
	struct JsonTablePlanState *nested;

	/* Left sibling, if any */
	struct JsonTablePlanState *left;

	/* Right sibling, if any */
	struct JsonTablePlanState *right;

	/* Parent plan, if this is a nested plan */
	struct JsonTablePlanState *parent;
} JsonTablePlanState;

/* Random number to identify JsonTableExecContext for sanity checking */
#define JSON_TABLE_EXEC_CONTEXT_MAGIC		418352867

typedef struct JsonTableExecContext
{
	int			magic;

	/* State of the plan providing a row evaluated from "root" jsonpath */
	JsonTablePlanState *rootplanstate;

	/*
	 * Per-column JsonTablePlanStates for all columns including the nested
	 * ones.
	 */
	JsonTablePlanState **colplanstates;
} JsonTableExecContext;

/* strict/lax flags is decomposed into four [un]wrap/error flags */
#define jspStrictAbsenceOfErrors(cxt)	(!(cxt)->laxMode)
#define jspAutoUnwrap(cxt)				((cxt)->laxMode)
#define jspAutoWrap(cxt)				((cxt)->laxMode)
#define jspIgnoreStructuralErrors(cxt)	((cxt)->ignoreStructuralErrors)
#define jspThrowErrors(cxt)				((cxt)->throwErrors)

/* Convenience macro: return or throw error depending on context */
#define RETURN_ERROR(throw_error) \
do { \
	if (jspThrowErrors(cxt)) \
		throw_error; \
	else \
		return jperError; \
} while (0)

typedef JsonPathBool (*JsonPathPredicateCallback) (JsonPathItem *jsp,
												   JsonbValue *larg,
												   JsonbValue *rarg,
												   void *param);
typedef Numeric (*BinaryArithmFunc) (Numeric num1, Numeric num2, bool *error);

static JsonPathExecResult executeJsonPath(JsonPath *path, void *vars,
										  JsonPathGetVarCallback getVar,
										  JsonPathCountVarsCallback countVars,
										  Jsonb *json, bool throwErrors,
										  JsonValueList *result, bool useTz);
static JsonPathExecResult executeItem(JsonPathExecContext *cxt,
									  JsonPathItem *jsp, JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapTarget(JsonPathExecContext *cxt,
													 JsonPathItem *jsp, JsonbValue *jb,
													 JsonValueList *found, bool unwrap);
static JsonPathExecResult executeItemUnwrapTargetArray(JsonPathExecContext *cxt,
													   JsonPathItem *jsp, JsonbValue *jb,
													   JsonValueList *found, bool unwrapElements);
static JsonPathExecResult executeNextItem(JsonPathExecContext *cxt,
										  JsonPathItem *cur, JsonPathItem *next,
										  JsonbValue *v, JsonValueList *found, bool copy);
static JsonPathExecResult executeItemOptUnwrapResult(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbValue *jb,
													 bool unwrap, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapResultNoThrow(JsonPathExecContext *cxt, JsonPathItem *jsp,
															JsonbValue *jb, bool unwrap, JsonValueList *found);
static JsonPathBool executeBoolItem(JsonPathExecContext *cxt,
									JsonPathItem *jsp, JsonbValue *jb, bool canHaveNext);
static JsonPathBool executeNestedBoolItem(JsonPathExecContext *cxt,
										  JsonPathItem *jsp, JsonbValue *jb);
static JsonPathExecResult executeAnyItem(JsonPathExecContext *cxt,
										 JsonPathItem *jsp, JsonbContainer *jbc, JsonValueList *found,
										 uint32 level, uint32 first, uint32 last,
										 bool ignoreStructuralErrors, bool unwrapNext);
static JsonPathBool executePredicate(JsonPathExecContext *cxt,
									 JsonPathItem *pred, JsonPathItem *larg, JsonPathItem *rarg,
									 JsonbValue *jb, bool unwrapRightArg,
									 JsonPathPredicateCallback exec, void *param);
static JsonPathExecResult executeBinaryArithmExpr(JsonPathExecContext *cxt,
												  JsonPathItem *jsp, JsonbValue *jb,
												  BinaryArithmFunc func, JsonValueList *found);
static JsonPathExecResult executeUnaryArithmExpr(JsonPathExecContext *cxt,
												 JsonPathItem *jsp, JsonbValue *jb, PGFunction func,
												 JsonValueList *found);
static JsonPathBool executeStartsWith(JsonPathItem *jsp,
									  JsonbValue *whole, JsonbValue *initial, void *param);
static JsonPathBool executeLikeRegex(JsonPathItem *jsp, JsonbValue *str,
									 JsonbValue *rarg, void *param);
static JsonPathExecResult executeNumericItemMethod(JsonPathExecContext *cxt,
												   JsonPathItem *jsp, JsonbValue *jb, bool unwrap, PGFunction func,
												   JsonValueList *found);
static JsonPathExecResult executeDateTimeMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
												JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult executeKeyValueMethod(JsonPathExecContext *cxt,
												JsonPathItem *jsp, JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult appendBoolResult(JsonPathExecContext *cxt,
										   JsonPathItem *jsp, JsonValueList *found, JsonPathBool res);
static void getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
							JsonbValue *value);
static JsonbValue *GetJsonPathVar(void *cxt, char *varName, int varNameLen,
								  JsonbValue *baseObject, int *baseObjectId);
static int	CountJsonPathVars(void *cxt);
static void JsonItemFromDatum(Datum val, Oid typid, int32 typmod,
							  JsonbValue *res);
static void JsonbValueInitNumericDatum(JsonbValue *jbv, Datum num);
static void getJsonPathVariable(JsonPathExecContext *cxt,
								JsonPathItem *variable, JsonbValue *value);
static int	countVariablesFromJsonb(void *varsJsonb);
static JsonbValue *getJsonPathVariableFromJsonb(void *varsJsonb, char *varName,
												int varNameLength,
												JsonbValue *baseObject,
												int *baseObjectId);
static int	JsonbArraySize(JsonbValue *jb);
static JsonPathBool executeComparison(JsonPathItem *cmp, JsonbValue *lv,
									  JsonbValue *rv, void *p);
static JsonPathBool compareItems(int32 op, JsonbValue *jb1, JsonbValue *jb2,
								 bool useTz);
static int	compareNumeric(Numeric a, Numeric b);
static JsonbValue *copyJsonbValue(JsonbValue *src);
static JsonPathExecResult getArrayIndex(JsonPathExecContext *cxt,
										JsonPathItem *jsp, JsonbValue *jb, int32 *index);
static JsonBaseObjectInfo setBaseObject(JsonPathExecContext *cxt,
										JsonbValue *jbv, int32 id);
static void JsonValueListClear(JsonValueList *jvl);
static void JsonValueListAppend(JsonValueList *jvl, JsonbValue *jbv);
static int	JsonValueListLength(const JsonValueList *jvl);
static bool JsonValueListIsEmpty(JsonValueList *jvl);
static JsonbValue *JsonValueListHead(JsonValueList *jvl);
static List *JsonValueListGetList(JsonValueList *jvl);
static void JsonValueListInitIterator(const JsonValueList *jvl,
									  JsonValueListIterator *it);
static JsonbValue *JsonValueListNext(const JsonValueList *jvl,
									 JsonValueListIterator *it);
static JsonbValue *JsonbInitBinary(JsonbValue *jbv, Jsonb *jb);
static int	JsonbType(JsonbValue *jb);
static JsonbValue *getScalar(JsonbValue *scalar, enum jbvType type);
static JsonbValue *wrapItemsInArray(const JsonValueList *items);
static int	compareDatetime(Datum val1, Oid typid1, Datum val2, Oid typid2,
							bool useTz, bool *cast_error);
static void checkTimezoneIsUsedForCast(bool useTz, const char *type1,
									   const char *type2);

static void JsonTableInitOpaque(TableFuncScanState *state, int natts);
static JsonTablePlanState *JsonTableInitPlan(JsonTableExecContext *cxt,
											 JsonTablePlan *plan,
											 JsonTablePlanState *parentstate,
											 List *args,
											 MemoryContext mcxt);
static void JsonTableSetDocument(TableFuncScanState *state, Datum value);
static void JsonTableResetRowPattern(JsonTablePlanState *planstate, Datum item);
static bool JsonTableFetchRow(TableFuncScanState *state);
static Datum JsonTableGetValue(TableFuncScanState *state, int colnum,
							   Oid typid, int32 typmod, bool *isnull);
static void JsonTableDestroyOpaque(TableFuncScanState *state);
static bool JsonTablePlanScanNextRow(JsonTablePlanState *planstate);
static void JsonTableResetNestedPlan(JsonTablePlanState *planstate);
static bool JsonTablePlanJoinNextRow(JsonTablePlanState *planstate);
static bool JsonTablePlanNextRow(JsonTablePlanState *planstate);

const TableFuncRoutine JsonbTableRoutine =
{
	.InitOpaque = JsonTableInitOpaque,
	.SetDocument = JsonTableSetDocument,
	.SetNamespace = NULL,
	.SetRowFilter = NULL,
	.SetColumnFilter = NULL,
	.FetchRow = JsonTableFetchRow,
	.GetValue = JsonTableGetValue,
	.DestroyOpaque = JsonTableDestroyOpaque
};

/****************** User interface to JsonPath executor ********************/

/*
 * jsonb_path_exists
 *		Returns true if jsonpath returns at least one item for the specified
 *		jsonb value.  This function and jsonb_path_match() are used to
 *		implement @? and @@ operators, which in turn are intended to have an
 *		index support.  Thus, it's desirable to make it easier to achieve
 *		consistency between index scan results and sequential scan results.
 *		So, we throw as few errors as possible.  Regarding this function,
 *		such behavior also matches behavior of JSON_EXISTS() clause of
 *		SQL/JSON.  Regarding jsonb_path_match(), this function doesn't have
 *		an analogy in SQL/JSON, so we define its behavior on our own.
 */
static Datum
jsonb_path_exists_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonPathExecResult res;
	Jsonb	   *vars = NULL;
	bool		silent = true;

	if (PG_NARGS() == 4)
	{
		vars = PG_GETARG_JSONB_P(2);
		silent = PG_GETARG_BOOL(3);
	}

	res = executeJsonPath(jp, vars, getJsonPathVariableFromJsonb,
						  countVariablesFromJsonb,
						  jb, !silent, NULL, tz);

	PG_FREE_IF_COPY(jb, 0);
	PG_FREE_IF_COPY(jp, 1);

	if (jperIsError(res))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(res == jperOk);
}

Datum
jsonb_path_exists(PG_FUNCTION_ARGS)
{
	return jsonb_path_exists_internal(fcinfo, false);
}

Datum
jsonb_path_exists_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_exists_internal(fcinfo, true);
}

/*
 * jsonb_path_exists_opr
 *		Implementation of operator "jsonb @? jsonpath" (2-argument version of
 *		jsonb_path_exists()).
 */
Datum
jsonb_path_exists_opr(PG_FUNCTION_ARGS)
{
	/* just call the other one -- it can handle both cases */
	return jsonb_path_exists_internal(fcinfo, false);
}

/*
 * jsonb_path_match
 *		Returns jsonpath predicate result item for the specified jsonb value.
 *		See jsonb_path_exists() comment for details regarding error handling.
 */
static Datum
jsonb_path_match_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = NULL;
	bool		silent = true;

	if (PG_NARGS() == 4)
	{
		vars = PG_GETARG_JSONB_P(2);
		silent = PG_GETARG_BOOL(3);
	}

	(void) executeJsonPath(jp, vars, getJsonPathVariableFromJsonb,
						   countVariablesFromJsonb,
						   jb, !silent, &found, tz);

	PG_FREE_IF_COPY(jb, 0);
	PG_FREE_IF_COPY(jp, 1);

	if (JsonValueListLength(&found) == 1)
	{
		JsonbValue *jbv = JsonValueListHead(&found);

		if (jbv->type == jbvBool)
			PG_RETURN_BOOL(jbv->val.boolean);

		if (jbv->type == jbvNull)
			PG_RETURN_NULL();
	}

	if (!silent)
		ereport(ERROR,
				(errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
				 errmsg("single boolean result is expected")));

	PG_RETURN_NULL();
}

Datum
jsonb_path_match(PG_FUNCTION_ARGS)
{
	return jsonb_path_match_internal(fcinfo, false);
}

Datum
jsonb_path_match_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_match_internal(fcinfo, true);
}

/*
 * jsonb_path_match_opr
 *		Implementation of operator "jsonb @@ jsonpath" (2-argument version of
 *		jsonb_path_match()).
 */
Datum
jsonb_path_match_opr(PG_FUNCTION_ARGS)
{
	/* just call the other one -- it can handle both cases */
	return jsonb_path_match_internal(fcinfo, false);
}

/*
 * jsonb_path_query
 *		Executes jsonpath for given jsonb document and returns result as
 *		rowset.
 */
static Datum
jsonb_path_query_internal(FunctionCallInfo fcinfo, bool tz)
{
	FuncCallContext *funcctx;
	List	   *found;
	JsonbValue *v;
	ListCell   *c;

	if (SRF_IS_FIRSTCALL())
	{
		JsonPath   *jp;
		Jsonb	   *jb;
		MemoryContext oldcontext;
		Jsonb	   *vars;
		bool		silent;
		JsonValueList found = {0};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		jb = PG_GETARG_JSONB_P_COPY(0);
		jp = PG_GETARG_JSONPATH_P_COPY(1);
		vars = PG_GETARG_JSONB_P_COPY(2);
		silent = PG_GETARG_BOOL(3);

		(void) executeJsonPath(jp, vars, getJsonPathVariableFromJsonb,
							   countVariablesFromJsonb,
							   jb, !silent, &found, tz);

		funcctx->user_fctx = JsonValueListGetList(&found);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	found = funcctx->user_fctx;

	c = list_head(found);

	if (c == NULL)
		SRF_RETURN_DONE(funcctx);

	v = lfirst(c);
	funcctx->user_fctx = list_delete_first(found);

	SRF_RETURN_NEXT(funcctx, JsonbPGetDatum(JsonbValueToJsonb(v)));
}

Datum
jsonb_path_query(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_internal(fcinfo, false);
}

Datum
jsonb_path_query_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_internal(fcinfo, true);
}

/*
 * jsonb_path_query_array
 *		Executes jsonpath for given jsonb document and returns result as
 *		jsonb array.
 */
static Datum
jsonb_path_query_array_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = PG_GETARG_JSONB_P(2);
	bool		silent = PG_GETARG_BOOL(3);

	(void) executeJsonPath(jp, vars, getJsonPathVariableFromJsonb,
						   countVariablesFromJsonb,
						   jb, !silent, &found, tz);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(wrapItemsInArray(&found)));
}

Datum
jsonb_path_query_array(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_array_internal(fcinfo, false);
}

Datum
jsonb_path_query_array_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_array_internal(fcinfo, true);
}

/*
 * jsonb_path_query_first
 *		Executes jsonpath for given jsonb document and returns first result
 *		item.  If there are no items, NULL returned.
 */
static Datum
jsonb_path_query_first_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = PG_GETARG_JSONB_P(2);
	bool		silent = PG_GETARG_BOOL(3);

	(void) executeJsonPath(jp, vars, getJsonPathVariableFromJsonb,
						   countVariablesFromJsonb,
						   jb, !silent, &found, tz);

	if (JsonValueListLength(&found) >= 1)
		PG_RETURN_JSONB_P(JsonbValueToJsonb(JsonValueListHead(&found)));
	else
		PG_RETURN_NULL();
}

Datum
jsonb_path_query_first(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_first_internal(fcinfo, false);
}

Datum
jsonb_path_query_first_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_first_internal(fcinfo, true);
}

/********************Execute functions for JsonPath**************************/

/*
 * Interface to jsonpath executor
 *
 * 'path' - jsonpath to be executed
 * 'vars' - variables to be substituted to jsonpath
 * 'getVar' - callback used by getJsonPathVariable() to extract variables from
 *		'vars'
 * 'countVars' - callback to count the number of jsonpath variables in 'vars'
 * 'json' - target document for jsonpath evaluation
 * 'throwErrors' - whether we should throw suppressible errors
 * 'result' - list to store result items into
 *
 * Returns an error if a recoverable error happens during processing, or NULL
 * on no error.
 *
 * Note, jsonb and jsonpath values should be available and untoasted during
 * work because JsonPathItem, JsonbValue and result item could have pointers
 * into input values.  If caller needs to just check if document matches
 * jsonpath, then it doesn't provide a result arg.  In this case executor
 * works till first positive result and does not check the rest if possible.
 * In other case it tries to find all the satisfied result items.
 */
static JsonPathExecResult
executeJsonPath(JsonPath *path, void *vars, JsonPathGetVarCallback getVar,
				JsonPathCountVarsCallback countVars,
				Jsonb *json, bool throwErrors, JsonValueList *result,
				bool useTz)
{
	JsonPathExecContext cxt;
	JsonPathExecResult res;
	JsonPathItem jsp;
	JsonbValue	jbv;

	jspInit(&jsp, path);

	if (!JsonbExtractScalar(&json->root, &jbv))
		JsonbInitBinary(&jbv, json);

	cxt.vars = vars;
	cxt.getVar = getVar;
	cxt.laxMode = (path->header & JSONPATH_LAX) != 0;
	cxt.ignoreStructuralErrors = cxt.laxMode;
	cxt.root = &jbv;
	cxt.current = &jbv;
	cxt.baseObject.jbc = NULL;
	cxt.baseObject.id = 0;
	/* 1 + number of base objects in vars */
	cxt.lastGeneratedObjectId = 1 + countVars(vars);
	cxt.innermostArraySize = -1;
	cxt.throwErrors = throwErrors;
	cxt.useTz = useTz;

	if (jspStrictAbsenceOfErrors(&cxt) && !result)
	{
		/*
		 * In strict mode we must get a complete list of values to check that
		 * there are no errors at all.
		 */
		JsonValueList vals = {0};

		res = executeItem(&cxt, &jsp, &jbv, &vals);

		if (jperIsError(res))
			return res;

		return JsonValueListIsEmpty(&vals) ? jperNotFound : jperOk;
	}

	res = executeItem(&cxt, &jsp, &jbv, result);

	Assert(!throwErrors || !jperIsError(res));

	return res;
}

/*
 * Execute jsonpath with automatic unwrapping of current item in lax mode.
 */
static JsonPathExecResult
executeItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
			JsonbValue *jb, JsonValueList *found)
{
	return executeItemOptUnwrapTarget(cxt, jsp, jb, found, jspAutoUnwrap(cxt));
}

/*
 * Main jsonpath executor function: walks on jsonpath structure, finds
 * relevant parts of jsonb and evaluates expressions over them.
 * When 'unwrap' is true current SQL/JSON item is unwrapped if it is an array.
 */
static JsonPathExecResult
executeItemOptUnwrapTarget(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonbValue *jb, JsonValueList *found, bool unwrap)
{
	JsonPathItem elem;
	JsonPathExecResult res = jperNotFound;
	JsonBaseObjectInfo baseObject;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (jsp->type)
	{
		case jpiNull:
		case jpiBool:
		case jpiNumeric:
		case jpiString:
		case jpiVariable:
			{
				JsonbValue	vbuf;
				JsonbValue *v;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (!hasNext && !found && jsp->type != jpiVariable)
				{
					/*
					 * Skip evaluation, but not for variables.  We must
					 * trigger an error for the missing variable.
					 */
					res = jperOk;
					break;
				}

				v = hasNext ? &vbuf : palloc(sizeof(*v));

				baseObject = cxt->baseObject;
				getJsonPathItem(cxt, jsp, v);

				res = executeNextItem(cxt, jsp, &elem,
									  v, found, hasNext);
				cxt->baseObject = baseObject;
			}
			break;

			/* all boolean item types: */
		case jpiAnd:
		case jpiOr:
		case jpiNot:
		case jpiIsUnknown:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiExists:
		case jpiStartsWith:
		case jpiLikeRegex:
			{
				JsonPathBool st = executeBoolItem(cxt, jsp, jb, true);

				res = appendBoolResult(cxt, jsp, found, st);
				break;
			}

		case jpiAdd:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_add_opt_error, found);

		case jpiSub:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_sub_opt_error, found);

		case jpiMul:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mul_opt_error, found);

		case jpiDiv:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_div_opt_error, found);

		case jpiMod:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mod_opt_error, found);

		case jpiPlus:
			return executeUnaryArithmExpr(cxt, jsp, jb, NULL, found);

		case jpiMinus:
			return executeUnaryArithmExpr(cxt, jsp, jb, numeric_uminus,
										  found);

		case jpiAnyArray:
			if (JsonbType(jb) == jbvArray)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				res = executeItemUnwrapTargetArray(cxt, hasNext ? &elem : NULL,
												   jb, found, jspAutoUnwrap(cxt));
			}
			else if (jspAutoWrap(cxt))
				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			else if (!jspIgnoreStructuralErrors(cxt))
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
									  errmsg("jsonpath wildcard array accessor can only be applied to an array"))));
			break;

		case jpiAnyKey:
			if (JsonbType(jb) == jbvObject)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				if (jb->type != jbvBinary)
					elog(ERROR, "invalid jsonb object type: %d", jb->type);

				return executeAnyItem
					(cxt, hasNext ? &elem : NULL,
					 jb->val.binary.data, found, 1, 1, 1,
					 false, jspAutoUnwrap(cxt));
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_OBJECT_NOT_FOUND),
									  errmsg("jsonpath wildcard member accessor can only be applied to an object"))));
			}
			break;

		case jpiIndexArray:
			if (JsonbType(jb) == jbvArray || jspAutoWrap(cxt))
			{
				int			innermostArraySize = cxt->innermostArraySize;
				int			i;
				int			size = JsonbArraySize(jb);
				bool		singleton = size < 0;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (singleton)
					size = 1;

				cxt->innermostArraySize = size; /* for LAST evaluation */

				for (i = 0; i < jsp->content.array.nelems; i++)
				{
					JsonPathItem from;
					JsonPathItem to;
					int32		index;
					int32		index_from;
					int32		index_to;
					bool		range = jspGetArraySubscript(jsp, &from,
															 &to, i);

					res = getArrayIndex(cxt, &from, jb, &index_from);

					if (jperIsError(res))
						break;

					if (range)
					{
						res = getArrayIndex(cxt, &to, jb, &index_to);

						if (jperIsError(res))
							break;
					}
					else
						index_to = index_from;

					if (!jspIgnoreStructuralErrors(cxt) &&
						(index_from < 0 ||
						 index_from > index_to ||
						 index_to >= size))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
											  errmsg("jsonpath array subscript is out of bounds"))));

					if (index_from < 0)
						index_from = 0;

					if (index_to >= size)
						index_to = size - 1;

					res = jperNotFound;

					for (index = index_from; index <= index_to; index++)
					{
						JsonbValue *v;
						bool		copy;

						if (singleton)
						{
							v = jb;
							copy = true;
						}
						else
						{
							v = getIthJsonbValueFromContainer(jb->val.binary.data,
															  (uint32) index);

							if (v == NULL)
								continue;

							copy = false;
						}

						if (!hasNext && !found)
							return jperOk;

						res = executeNextItem(cxt, jsp, &elem, v, found,
											  copy);

						if (jperIsError(res))
							break;

						if (res == jperOk && !found)
							break;
					}

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}

				cxt->innermostArraySize = innermostArraySize;
			}
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
									  errmsg("jsonpath array accessor can only be applied to an array"))));
			}
			break;

		case jpiAny:
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				/* first try without any intermediate steps */
				if (jsp->content.anybounds.first == 0)
				{
					bool		savedIgnoreStructuralErrors;

					savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
					cxt->ignoreStructuralErrors = true;
					res = executeNextItem(cxt, jsp, &elem,
										  jb, found, true);
					cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;

					if (res == jperOk && !found)
						break;
				}

				if (jb->type == jbvBinary)
					res = executeAnyItem
						(cxt, hasNext ? &elem : NULL,
						 jb->val.binary.data, found,
						 1,
						 jsp->content.anybounds.first,
						 jsp->content.anybounds.last,
						 true, jspAutoUnwrap(cxt));
				break;
			}

		case jpiKey:
			if (JsonbType(jb) == jbvObject)
			{
				JsonbValue *v;
				JsonbValue	key;

				key.type = jbvString;
				key.val.string.val = jspGetString(jsp, &key.val.string.len);

				v = findJsonbValueFromContainer(jb->val.binary.data,
												JB_FOBJECT, &key);

				if (v != NULL)
				{
					res = executeNextItem(cxt, jsp, NULL,
										  v, found, false);

					/* free value if it was not added to found list */
					if (jspHasNext(jsp) || !found)
						pfree(v);
				}
				else if (!jspIgnoreStructuralErrors(cxt))
				{
					Assert(found);

					if (!jspThrowErrors(cxt))
						return jperError;

					ereport(ERROR,
							(errcode(ERRCODE_SQL_JSON_MEMBER_NOT_FOUND), \
							 errmsg("JSON object does not contain key \"%s\"",
									pnstrdup(key.val.string.val,
											 key.val.string.len))));
				}
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_MEMBER_NOT_FOUND),
									  errmsg("jsonpath member accessor can only be applied to an object"))));
			}
			break;

		case jpiCurrent:
			res = executeNextItem(cxt, jsp, NULL, cxt->current,
								  found, true);
			break;

		case jpiRoot:
			jb = cxt->root;
			baseObject = setBaseObject(cxt, jb, 0);
			res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			cxt->baseObject = baseObject;
			break;

		case jpiFilter:
			{
				JsonPathBool st;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				jspGetArg(jsp, &elem);
				st = executeNestedBoolItem(cxt, &elem, jb);
				if (st != jpbTrue)
					res = jperNotFound;
				else
					res = executeNextItem(cxt, jsp, NULL,
										  jb, found, true);
				break;
			}

		case jpiType:
			{
				JsonbValue *jbv = palloc(sizeof(*jbv));

				jbv->type = jbvString;
				jbv->val.string.val = pstrdup(JsonbTypeName(jb));
				jbv->val.string.len = strlen(jbv->val.string.val);

				res = executeNextItem(cxt, jsp, NULL, jbv,
									  found, false);
			}
			break;

		case jpiSize:
			{
				int			size = JsonbArraySize(jb);

				if (size < 0)
				{
					if (!jspAutoWrap(cxt))
					{
						if (!jspIgnoreStructuralErrors(cxt))
							RETURN_ERROR(ereport(ERROR,
												 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
												  errmsg("jsonpath item method .%s() can only be applied to an array",
														 jspOperationName(jsp->type)))));
						break;
					}

					size = 1;
				}

				jb = palloc(sizeof(*jb));

				jb->type = jbvNumeric;
				jb->val.numeric = int64_to_numeric(size);

				res = executeNextItem(cxt, jsp, NULL, jb, found, false);
			}
			break;

		case jpiAbs:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_abs,
											found);

		case jpiFloor:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_floor,
											found);

		case jpiCeiling:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_ceil,
											found);

		case jpiDouble:
			{
				JsonbValue	jbv;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					char	   *tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
																		  NumericGetDatum(jb->val.numeric)));
					double		val;
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					val = float8in_internal(tmp,
											NULL,
											"double precision",
											tmp,
											(Node *) &escontext);

					if (escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type double precision",
													 tmp, jspOperationName(jsp->type)))));
					if (isinf(val) || isnan(val))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("NaN or Infinity is not allowed for jsonpath item method .%s()",
													 jspOperationName(jsp->type)))));
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as double */
					double		val;
					char	   *tmp = pnstrdup(jb->val.string.val,
											   jb->val.string.len);
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					val = float8in_internal(tmp,
											NULL,
											"double precision",
											tmp,
											(Node *) &escontext);

					if (escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type double precision",
													 tmp, jspOperationName(jsp->type)))));
					if (isinf(val) || isnan(val))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("NaN or Infinity is not allowed for jsonpath item method .%s()",
													 jspOperationName(jsp->type)))));

					jb = &jbv;
					jb->type = jbvNumeric;
					jb->val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric,
																		  Float8GetDatum(val)));
					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a string or numeric value",
												 jspOperationName(jsp->type)))));

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiDatetime:
		case jpiDate:
		case jpiTime:
		case jpiTimeTz:
		case jpiTimestamp:
		case jpiTimestampTz:
			if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

			return executeDateTimeMethod(cxt, jsp, jb, found);

		case jpiKeyValue:
			if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

			return executeKeyValueMethod(cxt, jsp, jb, found);

		case jpiLast:
			{
				JsonbValue	tmpjbv;
				JsonbValue *lastjbv;
				int			last;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (cxt->innermostArraySize < 0)
					elog(ERROR, "evaluating jsonpath LAST outside of array subscript");

				if (!hasNext && !found)
				{
					res = jperOk;
					break;
				}

				last = cxt->innermostArraySize - 1;

				lastjbv = hasNext ? &tmpjbv : palloc(sizeof(*lastjbv));

				lastjbv->type = jbvNumeric;
				lastjbv->val.numeric = int64_to_numeric(last);

				res = executeNextItem(cxt, jsp, &elem,
									  lastjbv, found, hasNext);
			}
			break;

		case jpiBigint:
			{
				JsonbValue	jbv;
				Datum		datum;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					bool		have_error;
					int64		val;

					val = numeric_int8_opt_error(jb->val.numeric, &have_error);
					if (have_error)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type bigint",
													 DatumGetCString(DirectFunctionCall1(numeric_out,
																						 NumericGetDatum(jb->val.numeric))),
													 jspOperationName(jsp->type)))));

					datum = Int64GetDatum(val);
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as bigint */
					char	   *tmp = pnstrdup(jb->val.string.val,
											   jb->val.string.len);
					ErrorSaveContext escontext = {T_ErrorSaveContext};
					bool		noerr;

					noerr = DirectInputFunctionCallSafe(int8in, tmp,
														InvalidOid, -1,
														(Node *) &escontext,
														&datum);

					if (!noerr || escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type bigint",
													 tmp, jspOperationName(jsp->type)))));
					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a string or numeric value",
												 jspOperationName(jsp->type)))));

				jb = &jbv;
				jb->type = jbvNumeric;
				jb->val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
																	  datum));

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiBoolean:
			{
				JsonbValue	jbv;
				bool		bval;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvBool)
				{
					bval = jb->val.boolean;

					res = jperOk;
				}
				else if (jb->type == jbvNumeric)
				{
					int			ival;
					Datum		datum;
					bool		noerr;
					char	   *tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
																		  NumericGetDatum(jb->val.numeric)));
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					noerr = DirectInputFunctionCallSafe(int4in, tmp,
														InvalidOid, -1,
														(Node *) &escontext,
														&datum);

					if (!noerr || escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type boolean",
													 tmp, jspOperationName(jsp->type)))));

					ival = DatumGetInt32(datum);
					if (ival == 0)
						bval = false;
					else
						bval = true;

					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as boolean */
					char	   *tmp = pnstrdup(jb->val.string.val,
											   jb->val.string.len);

					if (!parse_bool(tmp, &bval))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type boolean",
													 tmp, jspOperationName(jsp->type)))));

					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a boolean, string, or numeric value",
												 jspOperationName(jsp->type)))));

				jb = &jbv;
				jb->type = jbvBool;
				jb->val.boolean = bval;

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiDecimal:
		case jpiNumber:
			{
				JsonbValue	jbv;
				Numeric		num;
				char	   *numstr = NULL;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					num = jb->val.numeric;
					if (numeric_is_nan(num) || numeric_is_inf(num))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("NaN or Infinity is not allowed for jsonpath item method .%s()",
													 jspOperationName(jsp->type)))));

					if (jsp->type == jpiDecimal)
						numstr = DatumGetCString(DirectFunctionCall1(numeric_out,
																	 NumericGetDatum(num)));
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as number */
					Datum		datum;
					bool		noerr;
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					numstr = pnstrdup(jb->val.string.val, jb->val.string.len);

					noerr = DirectInputFunctionCallSafe(numeric_in, numstr,
														InvalidOid, -1,
														(Node *) &escontext,
														&datum);

					if (!noerr || escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type numeric",
													 numstr, jspOperationName(jsp->type)))));

					num = DatumGetNumeric(datum);
					if (numeric_is_nan(num) || numeric_is_inf(num))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("NaN or Infinity is not allowed for jsonpath item method .%s()",
													 jspOperationName(jsp->type)))));

					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a string or numeric value",
												 jspOperationName(jsp->type)))));

				/*
				 * If we have arguments, then they must be the precision and
				 * optional scale used in .decimal().  Convert them to the
				 * typmod equivalent and then truncate the numeric value per
				 * this typmod details.
				 */
				if (jsp->type == jpiDecimal && jsp->content.args.left)
				{
					Datum		numdatum;
					Datum		dtypmod;
					int32		precision;
					int32		scale = 0;
					bool		have_error;
					bool		noerr;
					ArrayType  *arrtypmod;
					Datum		datums[2];
					char		pstr[12];	/* sign, 10 digits and '\0' */
					char		sstr[12];	/* sign, 10 digits and '\0' */
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					jspGetLeftArg(jsp, &elem);
					if (elem.type != jpiNumeric)
						elog(ERROR, "invalid jsonpath item type for .decimal() precision");

					precision = numeric_int4_opt_error(jspGetNumeric(&elem),
													   &have_error);
					if (have_error)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("precision of jsonpath item method .%s() is out of range for type integer",
													 jspOperationName(jsp->type)))));

					if (jsp->content.args.right)
					{
						jspGetRightArg(jsp, &elem);
						if (elem.type != jpiNumeric)
							elog(ERROR, "invalid jsonpath item type for .decimal() scale");

						scale = numeric_int4_opt_error(jspGetNumeric(&elem),
													   &have_error);
						if (have_error)
							RETURN_ERROR(ereport(ERROR,
												 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
												  errmsg("scale of jsonpath item method .%s() is out of range for type integer",
														 jspOperationName(jsp->type)))));
					}

					/*
					 * numerictypmodin() takes the precision and scale in the
					 * form of CString arrays.
					 */
					pg_ltoa(precision, pstr);
					datums[0] = CStringGetDatum(pstr);
					pg_ltoa(scale, sstr);
					datums[1] = CStringGetDatum(sstr);
					arrtypmod = construct_array_builtin(datums, 2, CSTRINGOID);

					dtypmod = DirectFunctionCall1(numerictypmodin,
												  PointerGetDatum(arrtypmod));

					/* Convert numstr to Numeric with typmod */
					Assert(numstr != NULL);
					noerr = DirectInputFunctionCallSafe(numeric_in, numstr,
														InvalidOid, dtypmod,
														(Node *) &escontext,
														&numdatum);

					if (!noerr || escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type numeric",
													 numstr, jspOperationName(jsp->type)))));

					num = DatumGetNumeric(numdatum);
					pfree(arrtypmod);
				}

				jb = &jbv;
				jb->type = jbvNumeric;
				jb->val.numeric = num;

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiInteger:
			{
				JsonbValue	jbv;
				Datum		datum;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					bool		have_error;
					int32		val;

					val = numeric_int4_opt_error(jb->val.numeric, &have_error);
					if (have_error)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type integer",
													 DatumGetCString(DirectFunctionCall1(numeric_out,
																						 NumericGetDatum(jb->val.numeric))),
													 jspOperationName(jsp->type)))));

					datum = Int32GetDatum(val);
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as integer */
					char	   *tmp = pnstrdup(jb->val.string.val,
											   jb->val.string.len);
					ErrorSaveContext escontext = {T_ErrorSaveContext};
					bool		noerr;

					noerr = DirectInputFunctionCallSafe(int4in, tmp,
														InvalidOid, -1,
														(Node *) &escontext,
														&datum);

					if (!noerr || escontext.error_occurred)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("argument \"%s\" of jsonpath item method .%s() is invalid for type integer",
													 tmp, jspOperationName(jsp->type)))));
					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a string or numeric value",
												 jspOperationName(jsp->type)))));

				jb = &jbv;
				jb->type = jbvNumeric;
				jb->val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
																	  datum));

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiStringFunc:
			{
				JsonbValue	jbv;
				char	   *tmp = NULL;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

				switch (JsonbType(jb))
				{
					case jbvString:

						/*
						 * Value is not necessarily null-terminated, so we do
						 * pnstrdup() here.
						 */
						tmp = pnstrdup(jb->val.string.val,
									   jb->val.string.len);
						break;
					case jbvNumeric:
						tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
																  NumericGetDatum(jb->val.numeric)));
						break;
					case jbvBool:
						tmp = (jb->val.boolean) ? "true" : "false";
						break;
					case jbvDatetime:
						{
							char		buf[MAXDATELEN + 1];

							JsonEncodeDateTime(buf,
											   jb->val.datetime.value,
											   jb->val.datetime.typid,
											   &jb->val.datetime.tz);
							tmp = pstrdup(buf);
						}
						break;
					case jbvNull:
					case jbvArray:
					case jbvObject:
					case jbvBinary:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("jsonpath item method .%s() can only be applied to a boolean, string, numeric, or datetime value",
													 jspOperationName(jsp->type)))));
						break;
				}

				jb = &jbv;
				Assert(tmp != NULL);	/* We must have set tmp above */
				jb->val.string.val = tmp;
				jb->val.string.len = strlen(jb->val.string.val);
				jb->type = jbvString;

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", jsp->type);
	}

	return res;
}

/*
 * Unwrap current array item and execute jsonpath for each of its elements.
 */
static JsonPathExecResult
executeItemUnwrapTargetArray(JsonPathExecContext *cxt, JsonPathItem *jsp,
							 JsonbValue *jb, JsonValueList *found,
							 bool unwrapElements)
{
	if (jb->type != jbvBinary)
	{
		Assert(jb->type != jbvArray);
		elog(ERROR, "invalid jsonb array value type: %d", jb->type);
	}

	return executeAnyItem
		(cxt, jsp, jb->val.binary.data, found, 1, 1, 1,
		 false, unwrapElements);
}

/*
 * Execute next jsonpath item if exists.  Otherwise put "v" to the "found"
 * list if provided.
 */
static JsonPathExecResult
executeNextItem(JsonPathExecContext *cxt,
				JsonPathItem *cur, JsonPathItem *next,
				JsonbValue *v, JsonValueList *found, bool copy)
{
	JsonPathItem elem;
	bool		hasNext;

	if (!cur)
		hasNext = next != NULL;
	else if (next)
		hasNext = jspHasNext(cur);
	else
	{
		next = &elem;
		hasNext = jspGetNext(cur, next);
	}

	if (hasNext)
		return executeItem(cxt, next, v, found);

	if (found)
		JsonValueListAppend(found, copy ? copyJsonbValue(v) : v);

	return jperOk;
}

/*
 * Same as executeItem(), but when "unwrap == true" automatically unwraps
 * each array item from the resulting sequence in lax mode.
 */
static JsonPathExecResult
executeItemOptUnwrapResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonbValue *jb, bool unwrap,
						   JsonValueList *found)
{
	if (unwrap && jspAutoUnwrap(cxt))
	{
		JsonValueList seq = {0};
		JsonValueListIterator it;
		JsonPathExecResult res = executeItem(cxt, jsp, jb, &seq);
		JsonbValue *item;

		if (jperIsError(res))
			return res;

		JsonValueListInitIterator(&seq, &it);
		while ((item = JsonValueListNext(&seq, &it)))
		{
			Assert(item->type != jbvArray);

			if (JsonbType(item) == jbvArray)
				executeItemUnwrapTargetArray(cxt, NULL, item, found, false);
			else
				JsonValueListAppend(found, item);
		}

		return jperOk;
	}

	return executeItem(cxt, jsp, jb, found);
}

/*
 * Same as executeItemOptUnwrapResult(), but with error suppression.
 */
static JsonPathExecResult
executeItemOptUnwrapResultNoThrow(JsonPathExecContext *cxt,
								  JsonPathItem *jsp,
								  JsonbValue *jb, bool unwrap,
								  JsonValueList *found)
{
	JsonPathExecResult res;
	bool		throwErrors = cxt->throwErrors;

	cxt->throwErrors = false;
	res = executeItemOptUnwrapResult(cxt, jsp, jb, unwrap, found);
	cxt->throwErrors = throwErrors;

	return res;
}

/* Execute boolean-valued jsonpath expression. */
static JsonPathBool
executeBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
				JsonbValue *jb, bool canHaveNext)
{
	JsonPathItem larg;
	JsonPathItem rarg;
	JsonPathBool res;
	JsonPathBool res2;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (!canHaveNext && jspHasNext(jsp))
		elog(ERROR, "boolean jsonpath item cannot have next item");

	switch (jsp->type)
	{
		case jpiAnd:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbFalse)
				return jpbFalse;

			/*
			 * SQL/JSON says that we should check second arg in case of
			 * jperError
			 */

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbTrue ? res : res2;

		case jpiOr:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbTrue)
				return jpbTrue;

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbFalse ? res : res2;

		case jpiNot:
			jspGetArg(jsp, &larg);

			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbUnknown)
				return jpbUnknown;

			return res == jpbTrue ? jpbFalse : jpbTrue;

		case jpiIsUnknown:
			jspGetArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);
			return res == jpbUnknown ? jpbTrue : jpbFalse;

		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
			jspGetLeftArg(jsp, &larg);
			jspGetRightArg(jsp, &rarg);
			return executePredicate(cxt, jsp, &larg, &rarg, jb, true,
									executeComparison, cxt);

		case jpiStartsWith:		/* 'whole STARTS WITH initial' */
			jspGetLeftArg(jsp, &larg);	/* 'whole' */
			jspGetRightArg(jsp, &rarg); /* 'initial' */
			return executePredicate(cxt, jsp, &larg, &rarg, jb, false,
									executeStartsWith, NULL);

		case jpiLikeRegex:		/* 'expr LIKE_REGEX pattern FLAGS flags' */
			{
				/*
				 * 'expr' is a sequence-returning expression.  'pattern' is a
				 * regex string literal.  SQL/JSON standard requires XQuery
				 * regexes, but we use Postgres regexes here.  'flags' is a
				 * string literal converted to integer flags at compile-time.
				 */
				JsonLikeRegexContext lrcxt = {0};

				jspInitByBuffer(&larg, jsp->base,
								jsp->content.like_regex.expr);

				return executePredicate(cxt, jsp, &larg, NULL, jb, false,
										executeLikeRegex, &lrcxt);
			}

		case jpiExists:
			jspGetArg(jsp, &larg);

			if (jspStrictAbsenceOfErrors(cxt))
			{
				/*
				 * In strict mode we must get a complete list of values to
				 * check that there are no errors at all.
				 */
				JsonValueList vals = {0};
				JsonPathExecResult res =
					executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
													  false, &vals);

				if (jperIsError(res))
					return jpbUnknown;

				return JsonValueListIsEmpty(&vals) ? jpbFalse : jpbTrue;
			}
			else
			{
				JsonPathExecResult res =
					executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
													  false, NULL);

				if (jperIsError(res))
					return jpbUnknown;

				return res == jperOk ? jpbTrue : jpbFalse;
			}

		default:
			elog(ERROR, "invalid boolean jsonpath item type: %d", jsp->type);
			return jpbUnknown;
	}
}

/*
 * Execute nested (filters etc.) boolean expression pushing current SQL/JSON
 * item onto the stack.
 */
static JsonPathBool
executeNestedBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb)
{
	JsonbValue *prev;
	JsonPathBool res;

	prev = cxt->current;
	cxt->current = jb;
	res = executeBoolItem(cxt, jsp, jb, false);
	cxt->current = prev;

	return res;
}

/*
 * Implementation of several jsonpath nodes:
 *  - jpiAny (.** accessor),
 *  - jpiAnyKey (.* accessor),
 *  - jpiAnyArray ([*] accessor)
 */
static JsonPathExecResult
executeAnyItem(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbContainer *jbc,
			   JsonValueList *found, uint32 level, uint32 first, uint32 last,
			   bool ignoreStructuralErrors, bool unwrapNext)
{
	JsonPathExecResult res = jperNotFound;
	JsonbIterator *it;
	int32		r;
	JsonbValue	v;

	check_stack_depth();

	if (level > last)
		return res;

	it = JsonbIteratorInit(jbc);

	/*
	 * Recursively iterate over jsonb objects/arrays
	 */
	while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (r == WJB_KEY)
		{
			r = JsonbIteratorNext(&it, &v, true);
			Assert(r == WJB_VALUE);
		}

		if (r == WJB_VALUE || r == WJB_ELEM)
		{

			if (level >= first ||
				(first == PG_UINT32_MAX && last == PG_UINT32_MAX &&
				 v.type != jbvBinary))	/* leaves only requested */
			{
				/* check expression */
				if (jsp)
				{
					if (ignoreStructuralErrors)
					{
						bool		savedIgnoreStructuralErrors;

						savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
						cxt->ignoreStructuralErrors = true;
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);
						cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;
					}
					else
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}
				else if (found)
					JsonValueListAppend(found, copyJsonbValue(&v));
				else
					return jperOk;
			}

			if (level < last && v.type == jbvBinary)
			{
				res = executeAnyItem
					(cxt, jsp, v.val.binary.data, found,
					 level + 1, first, last,
					 ignoreStructuralErrors, unwrapNext);

				if (jperIsError(res))
					break;

				if (res == jperOk && found == NULL)
					break;
			}
		}
	}

	return res;
}

/*
 * Execute unary or binary predicate.
 *
 * Predicates have existence semantics, because their operands are item
 * sequences.  Pairs of items from the left and right operand's sequences are
 * checked.  TRUE returned only if any pair satisfying the condition is found.
 * In strict mode, even if the desired pair has already been found, all pairs
 * still need to be examined to check the absence of errors.  If any error
 * occurs, UNKNOWN (analogous to SQL NULL) is returned.
 */
static JsonPathBool
executePredicate(JsonPathExecContext *cxt, JsonPathItem *pred,
				 JsonPathItem *larg, JsonPathItem *rarg, JsonbValue *jb,
				 bool unwrapRightArg, JsonPathPredicateCallback exec,
				 void *param)
{
	JsonPathExecResult res;
	JsonValueListIterator lseqit;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonbValue *lval;
	bool		error = false;
	bool		found = false;

	/* Left argument is always auto-unwrapped. */
	res = executeItemOptUnwrapResultNoThrow(cxt, larg, jb, true, &lseq);
	if (jperIsError(res))
		return jpbUnknown;

	if (rarg)
	{
		/* Right argument is conditionally auto-unwrapped. */
		res = executeItemOptUnwrapResultNoThrow(cxt, rarg, jb,
												unwrapRightArg, &rseq);
		if (jperIsError(res))
			return jpbUnknown;
	}

	JsonValueListInitIterator(&lseq, &lseqit);
	while ((lval = JsonValueListNext(&lseq, &lseqit)))
	{
		JsonValueListIterator rseqit;
		JsonbValue *rval;
		bool		first = true;

		JsonValueListInitIterator(&rseq, &rseqit);
		if (rarg)
			rval = JsonValueListNext(&rseq, &rseqit);
		else
			rval = NULL;

		/* Loop over right arg sequence or do single pass otherwise */
		while (rarg ? (rval != NULL) : first)
		{
			JsonPathBool res = exec(pred, lval, rval, param);

			if (res == jpbUnknown)
			{
				if (jspStrictAbsenceOfErrors(cxt))
					return jpbUnknown;

				error = true;
			}
			else if (res == jpbTrue)
			{
				if (!jspStrictAbsenceOfErrors(cxt))
					return jpbTrue;

				found = true;
			}

			first = false;
			if (rarg)
				rval = JsonValueListNext(&rseq, &rseqit);
		}
	}

	if (found)					/* possible only in strict mode */
		return jpbTrue;

	if (error)					/* possible only in lax mode */
		return jpbUnknown;

	return jpbFalse;
}

/*
 * Execute binary arithmetic expression on singleton numeric operands.
 * Array operands are automatically unwrapped in lax mode.
 */
static JsonPathExecResult
executeBinaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
						JsonbValue *jb, BinaryArithmFunc func,
						JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathItem elem;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonbValue *lval;
	JsonbValue *rval;
	Numeric		res;

	jspGetLeftArg(jsp, &elem);

	/*
	 * XXX: By standard only operands of multiplicative expressions are
	 * unwrapped.  We extend it to other binary arithmetic expressions too.
	 */
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &lseq);
	if (jperIsError(jper))
		return jper;

	jspGetRightArg(jsp, &elem);

	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &rseq);
	if (jperIsError(jper))
		return jper;

	if (JsonValueListLength(&lseq) != 1 ||
		!(lval = getScalar(JsonValueListHead(&lseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
							  errmsg("left operand of jsonpath operator %s is not a single numeric value",
									 jspOperationName(jsp->type)))));

	if (JsonValueListLength(&rseq) != 1 ||
		!(rval = getScalar(JsonValueListHead(&rseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
							  errmsg("right operand of jsonpath operator %s is not a single numeric value",
									 jspOperationName(jsp->type)))));

	if (jspThrowErrors(cxt))
	{
		res = func(lval->val.numeric, rval->val.numeric, NULL);
	}
	else
	{
		bool		error = false;

		res = func(lval->val.numeric, rval->val.numeric, &error);

		if (error)
			return jperError;
	}

	if (!jspGetNext(jsp, &elem) && !found)
		return jperOk;

	lval = palloc(sizeof(*lval));
	lval->type = jbvNumeric;
	lval->val.numeric = res;

	return executeNextItem(cxt, jsp, &elem, lval, found, false);
}

/*
 * Execute unary arithmetic expression for each numeric item in its operand's
 * sequence.  Array operand is automatically unwrapped in lax mode.
 */
static JsonPathExecResult
executeUnaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
					   JsonbValue *jb, PGFunction func, JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathExecResult jper2;
	JsonPathItem elem;
	JsonValueList seq = {0};
	JsonValueListIterator it;
	JsonbValue *val;
	bool		hasNext;

	jspGetArg(jsp, &elem);
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &seq);

	if (jperIsError(jper))
		return jper;

	jper = jperNotFound;

	hasNext = jspGetNext(jsp, &elem);

	JsonValueListInitIterator(&seq, &it);
	while ((val = JsonValueListNext(&seq, &it)))
	{
		if ((val = getScalar(val, jbvNumeric)))
		{
			if (!found && !hasNext)
				return jperOk;
		}
		else
		{
			if (!found && !hasNext)
				continue;		/* skip non-numerics processing */

			RETURN_ERROR(ereport(ERROR,
								 (errcode(ERRCODE_SQL_JSON_NUMBER_NOT_FOUND),
								  errmsg("operand of unary jsonpath operator %s is not a numeric value",
										 jspOperationName(jsp->type)))));
		}

		if (func)
			val->val.numeric =
				DatumGetNumeric(DirectFunctionCall1(func,
													NumericGetDatum(val->val.numeric)));

		jper2 = executeNextItem(cxt, jsp, &elem, val, found, false);

		if (jperIsError(jper2))
			return jper2;

		if (jper2 == jperOk)
		{
			if (!found)
				return jperOk;
			jper = jperOk;
		}
	}

	return jper;
}

/*
 * STARTS_WITH predicate callback.
 *
 * Check if the 'whole' string starts from 'initial' string.
 */
static JsonPathBool
executeStartsWith(JsonPathItem *jsp, JsonbValue *whole, JsonbValue *initial,
				  void *param)
{
	if (!(whole = getScalar(whole, jbvString)))
		return jpbUnknown;		/* error */

	if (!(initial = getScalar(initial, jbvString)))
		return jpbUnknown;		/* error */

	if (whole->val.string.len >= initial->val.string.len &&
		!memcmp(whole->val.string.val,
				initial->val.string.val,
				initial->val.string.len))
		return jpbTrue;

	return jpbFalse;
}

/*
 * LIKE_REGEX predicate callback.
 *
 * Check if the string matches regex pattern.
 */
static JsonPathBool
executeLikeRegex(JsonPathItem *jsp, JsonbValue *str, JsonbValue *rarg,
				 void *param)
{
	JsonLikeRegexContext *cxt = param;

	if (!(str = getScalar(str, jbvString)))
		return jpbUnknown;

	/* Cache regex text and converted flags. */
	if (!cxt->regex)
	{
		cxt->regex =
			cstring_to_text_with_len(jsp->content.like_regex.pattern,
									 jsp->content.like_regex.patternlen);
		(void) jspConvertRegexFlags(jsp->content.like_regex.flags,
									&(cxt->cflags), NULL);
	}

	if (RE_compile_and_execute(cxt->regex, str->val.string.val,
							   str->val.string.len,
							   cxt->cflags, DEFAULT_COLLATION_OID, 0, NULL))
		return jpbTrue;

	return jpbFalse;
}

/*
 * Execute numeric item methods (.abs(), .floor(), .ceil()) using the specified
 * user function 'func'.
 */
static JsonPathExecResult
executeNumericItemMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
						 JsonbValue *jb, bool unwrap, PGFunction func,
						 JsonValueList *found)
{
	JsonPathItem next;
	Datum		datum;

	if (unwrap && JsonbType(jb) == jbvArray)
		return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

	if (!(jb = getScalar(jb, jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
							  errmsg("jsonpath item method .%s() can only be applied to a numeric value",
									 jspOperationName(jsp->type)))));

	datum = DirectFunctionCall1(func, NumericGetDatum(jb->val.numeric));

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;

	jb = palloc(sizeof(*jb));
	jb->type = jbvNumeric;
	jb->val.numeric = DatumGetNumeric(datum);

	return executeNextItem(cxt, jsp, &next, jb, found, false);
}

/*
 * Implementation of the .datetime() and related methods.
 *
 * Converts a string into a date/time value. The actual type is determined at
 * run time.
 * If an argument is provided, this argument is used as a template string.
 * Otherwise, the first fitting ISO format is selected.
 *
 * .date(), .time(), .time_tz(), .timestamp(), .timestamp_tz() methods don't
 * have a format, so ISO format is used.  However, except for .date(), they all
 * take an optional time precision.
 */
static JsonPathExecResult
executeDateTimeMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb, JsonValueList *found)
{
	JsonbValue	jbvbuf;
	Datum		value;
	text	   *datetime;
	Oid			collid;
	Oid			typid;
	int32		typmod = -1;
	int			tz = 0;
	bool		hasNext;
	JsonPathExecResult res = jperNotFound;
	JsonPathItem elem;
	int32		time_precision = -1;

	if (!(jb = getScalar(jb, jbvString)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
							  errmsg("jsonpath item method .%s() can only be applied to a string",
									 jspOperationName(jsp->type)))));

	datetime = cstring_to_text_with_len(jb->val.string.val,
										jb->val.string.len);

	/*
	 * At some point we might wish to have callers supply the collation to
	 * use, but right now it's unclear that they'd be able to do better than
	 * DEFAULT_COLLATION_OID anyway.
	 */
	collid = DEFAULT_COLLATION_OID;

	/*
	 * .datetime(template) has an argument, the rest of the methods don't have
	 * an argument.  So we handle that separately.
	 */
	if (jsp->type == jpiDatetime && jsp->content.arg)
	{
		text	   *template;
		char	   *template_str;
		int			template_len;
		ErrorSaveContext escontext = {T_ErrorSaveContext};

		jspGetArg(jsp, &elem);

		if (elem.type != jpiString)
			elog(ERROR, "invalid jsonpath item type for .datetime() argument");

		template_str = jspGetString(&elem, &template_len);

		template = cstring_to_text_with_len(template_str,
											template_len);

		value = parse_datetime(datetime, template, collid, true,
							   &typid, &typmod, &tz,
							   jspThrowErrors(cxt) ? NULL : (Node *) &escontext);

		if (escontext.error_occurred)
			res = jperError;
		else
			res = jperOk;
	}
	else
	{
		/*
		 * According to SQL/JSON standard enumerate ISO formats for: date,
		 * timetz, time, timestamptz, timestamp.
		 *
		 * We also support ISO 8601 format (with "T") for timestamps, because
		 * to_json[b]() functions use this format.
		 */
		static const char *fmt_str[] =
		{
			"yyyy-mm-dd",		/* date */
			"HH24:MI:SS.USTZ",	/* timetz */
			"HH24:MI:SSTZ",
			"HH24:MI:SS.US",	/* time without tz */
			"HH24:MI:SS",
			"yyyy-mm-dd HH24:MI:SS.USTZ",	/* timestamptz */
			"yyyy-mm-dd HH24:MI:SSTZ",
			"yyyy-mm-dd\"T\"HH24:MI:SS.USTZ",
			"yyyy-mm-dd\"T\"HH24:MI:SSTZ",
			"yyyy-mm-dd HH24:MI:SS.US", /* timestamp without tz */
			"yyyy-mm-dd HH24:MI:SS",
			"yyyy-mm-dd\"T\"HH24:MI:SS.US",
			"yyyy-mm-dd\"T\"HH24:MI:SS"
		};

		/* cache for format texts */
		static text *fmt_txt[lengthof(fmt_str)] = {0};
		int			i;

		/*
		 * Check for optional precision for methods other than .datetime() and
		 * .date()
		 */
		if (jsp->type != jpiDatetime && jsp->type != jpiDate &&
			jsp->content.arg)
		{
			bool		have_error;

			jspGetArg(jsp, &elem);

			if (elem.type != jpiNumeric)
				elog(ERROR, "invalid jsonpath item type for %s argument",
					 jspOperationName(jsp->type));

			time_precision = numeric_int4_opt_error(jspGetNumeric(&elem),
													&have_error);
			if (have_error)
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
									  errmsg("time precision of jsonpath item method .%s() is out of range for type integer",
											 jspOperationName(jsp->type)))));
		}

		/* loop until datetime format fits */
		for (i = 0; i < lengthof(fmt_str); i++)
		{
			ErrorSaveContext escontext = {T_ErrorSaveContext};

			if (!fmt_txt[i])
			{
				MemoryContext oldcxt =
					MemoryContextSwitchTo(TopMemoryContext);

				fmt_txt[i] = cstring_to_text(fmt_str[i]);
				MemoryContextSwitchTo(oldcxt);
			}

			value = parse_datetime(datetime, fmt_txt[i], collid, true,
								   &typid, &typmod, &tz,
								   (Node *) &escontext);

			if (!escontext.error_occurred)
			{
				res = jperOk;
				break;
			}
		}

		if (res == jperNotFound)
		{
			if (jsp->type == jpiDatetime)
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
									  errmsg("%s format is not recognized: \"%s\"",
											 "datetime", text_to_cstring(datetime)),
									  errhint("Use a datetime template argument to specify the input data format."))));
			else
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
									  errmsg("%s format is not recognized: \"%s\"",
											 jspOperationName(jsp->type), text_to_cstring(datetime)))));

		}
	}

	/*
	 * parse_datetime() processes the entire input string per the template or
	 * ISO format and returns the Datum in best fitted datetime type.  So, if
	 * this call is for a specific datatype, then we do the conversion here.
	 * Throw an error for incompatible types.
	 */
	switch (jsp->type)
	{
		case jpiDatetime:		/* Nothing to do for DATETIME */
			break;
		case jpiDate:
			{
				/* Convert result type to date */
				switch (typid)
				{
					case DATEOID:	/* Nothing to do for DATE */
						break;
					case TIMEOID:
					case TIMETZOID:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("%s format is not recognized: \"%s\"",
													 "date", text_to_cstring(datetime)))));
						break;
					case TIMESTAMPOID:
						value = DirectFunctionCall1(timestamp_date,
													value);
						break;
					case TIMESTAMPTZOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "timestamptz", "date");
						value = DirectFunctionCall1(timestamptz_date,
													value);
						break;
					default:
						elog(ERROR, "type with oid %u not supported", typid);
				}

				typid = DATEOID;
			}
			break;
		case jpiTime:
			{
				/* Convert result type to time without time zone */
				switch (typid)
				{
					case DATEOID:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("%s format is not recognized: \"%s\"",
													 "time", text_to_cstring(datetime)))));
						break;
					case TIMEOID:	/* Nothing to do for TIME */
						break;
					case TIMETZOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "timetz", "time");
						value = DirectFunctionCall1(timetz_time,
													value);
						break;
					case TIMESTAMPOID:
						value = DirectFunctionCall1(timestamp_time,
													value);
						break;
					case TIMESTAMPTZOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "timestamptz", "time");
						value = DirectFunctionCall1(timestamptz_time,
													value);
						break;
					default:
						elog(ERROR, "type with oid %u not supported", typid);
				}

				/* Force the user-given time precision, if any */
				if (time_precision != -1)
				{
					TimeADT		result;

					/* Get a warning when precision is reduced */
					time_precision = anytime_typmod_check(false,
														  time_precision);
					result = DatumGetTimeADT(value);
					AdjustTimeForTypmod(&result, time_precision);
					value = TimeADTGetDatum(result);

					/* Update the typmod value with the user-given precision */
					typmod = time_precision;
				}

				typid = TIMEOID;
			}
			break;
		case jpiTimeTz:
			{
				/* Convert result type to time with time zone */
				switch (typid)
				{
					case DATEOID:
					case TIMESTAMPOID:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("%s format is not recognized: \"%s\"",
													 "time_tz", text_to_cstring(datetime)))));
						break;
					case TIMEOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "time", "timetz");
						value = DirectFunctionCall1(time_timetz,
													value);
						break;
					case TIMETZOID: /* Nothing to do for TIMETZ */
						break;
					case TIMESTAMPTZOID:
						value = DirectFunctionCall1(timestamptz_timetz,
													value);
						break;
					default:
						elog(ERROR, "type with oid %u not supported", typid);
				}

				/* Force the user-given time precision, if any */
				if (time_precision != -1)
				{
					TimeTzADT  *result;

					/* Get a warning when precision is reduced */
					time_precision = anytime_typmod_check(true,
														  time_precision);
					result = DatumGetTimeTzADTP(value);
					AdjustTimeForTypmod(&result->time, time_precision);
					value = TimeTzADTPGetDatum(result);

					/* Update the typmod value with the user-given precision */
					typmod = time_precision;
				}

				typid = TIMETZOID;
			}
			break;
		case jpiTimestamp:
			{
				/* Convert result type to timestamp without time zone */
				switch (typid)
				{
					case DATEOID:
						value = DirectFunctionCall1(date_timestamp,
													value);
						break;
					case TIMEOID:
					case TIMETZOID:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("%s format is not recognized: \"%s\"",
													 "timestamp", text_to_cstring(datetime)))));
						break;
					case TIMESTAMPOID:	/* Nothing to do for TIMESTAMP */
						break;
					case TIMESTAMPTZOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "timestamptz", "timestamp");
						value = DirectFunctionCall1(timestamptz_timestamp,
													value);
						break;
					default:
						elog(ERROR, "type with oid %u not supported", typid);
				}

				/* Force the user-given time precision, if any */
				if (time_precision != -1)
				{
					Timestamp	result;
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					/* Get a warning when precision is reduced */
					time_precision = anytimestamp_typmod_check(false,
															   time_precision);
					result = DatumGetTimestamp(value);
					AdjustTimestampForTypmod(&result, time_precision,
											 (Node *) &escontext);
					if (escontext.error_occurred)	/* should not happen */
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("time precision of jsonpath item method .%s() is invalid",
													 jspOperationName(jsp->type)))));
					value = TimestampGetDatum(result);

					/* Update the typmod value with the user-given precision */
					typmod = time_precision;
				}

				typid = TIMESTAMPOID;
			}
			break;
		case jpiTimestampTz:
			{
				struct pg_tm tm;
				fsec_t		fsec;

				/* Convert result type to timestamp with time zone */
				switch (typid)
				{
					case DATEOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "date", "timestamptz");

						/*
						 * Get the timezone value explicitly since JsonbValue
						 * keeps that separate.
						 */
						j2date(DatumGetDateADT(value) + POSTGRES_EPOCH_JDATE,
							   &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
						tm.tm_hour = 0;
						tm.tm_min = 0;
						tm.tm_sec = 0;
						tz = DetermineTimeZoneOffset(&tm, session_timezone);

						value = DirectFunctionCall1(date_timestamptz,
													value);
						break;
					case TIMEOID:
					case TIMETZOID:
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("%s format is not recognized: \"%s\"",
													 "timestamp_tz", text_to_cstring(datetime)))));
						break;
					case TIMESTAMPOID:
						checkTimezoneIsUsedForCast(cxt->useTz,
												   "timestamp", "timestamptz");

						/*
						 * Get the timezone value explicitly since JsonbValue
						 * keeps that separate.
						 */
						if (timestamp2tm(DatumGetTimestamp(value), NULL, &tm,
										 &fsec, NULL, NULL) == 0)
							tz = DetermineTimeZoneOffset(&tm,
														 session_timezone);

						value = DirectFunctionCall1(timestamp_timestamptz,
													value);
						break;
					case TIMESTAMPTZOID:	/* Nothing to do for TIMESTAMPTZ */
						break;
					default:
						elog(ERROR, "type with oid %u not supported", typid);
				}

				/* Force the user-given time precision, if any */
				if (time_precision != -1)
				{
					Timestamp	result;
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					/* Get a warning when precision is reduced */
					time_precision = anytimestamp_typmod_check(true,
															   time_precision);
					result = DatumGetTimestampTz(value);
					AdjustTimestampForTypmod(&result, time_precision,
											 (Node *) &escontext);
					if (escontext.error_occurred)	/* should not happen */
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
											  errmsg("time precision of jsonpath item method .%s() is invalid",
													 jspOperationName(jsp->type)))));
					value = TimestampTzGetDatum(result);

					/* Update the typmod value with the user-given precision */
					typmod = time_precision;
				}

				typid = TIMESTAMPTZOID;
			}
			break;
		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", jsp->type);
	}

	pfree(datetime);

	if (jperIsError(res))
		return res;

	hasNext = jspGetNext(jsp, &elem);

	if (!hasNext && !found)
		return res;

	jb = hasNext ? &jbvbuf : palloc(sizeof(*jb));

	jb->type = jbvDatetime;
	jb->val.datetime.value = value;
	jb->val.datetime.typid = typid;
	jb->val.datetime.typmod = typmod;
	jb->val.datetime.tz = tz;

	return executeNextItem(cxt, jsp, &elem, jb, found, hasNext);
}

/*
 * Implementation of .keyvalue() method.
 *
 * .keyvalue() method returns a sequence of object's key-value pairs in the
 * following format: '{ "key": key, "value": value, "id": id }'.
 *
 * "id" field is an object identifier which is constructed from the two parts:
 * base object id and its binary offset in base object's jsonb:
 * id = 10000000000 * base_object_id + obj_offset_in_base_object
 *
 * 10000000000 (10^10) -- is a first round decimal number greater than 2^32
 * (maximal offset in jsonb).  Decimal multiplier is used here to improve the
 * readability of identifiers.
 *
 * Base object is usually a root object of the path: context item '$' or path
 * variable '$var', literals can't produce objects for now.  But if the path
 * contains generated objects (.keyvalue() itself, for example), then they
 * become base object for the subsequent .keyvalue().
 *
 * Id of '$' is 0. Id of '$var' is its ordinal (positive) number in the list
 * of variables (see getJsonPathVariable()).  Ids for generated objects
 * are assigned using global counter JsonPathExecContext.lastGeneratedObjectId.
 */
static JsonPathExecResult
executeKeyValueMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb, JsonValueList *found)
{
	JsonPathExecResult res = jperNotFound;
	JsonPathItem next;
	JsonbContainer *jbc;
	JsonbValue	key;
	JsonbValue	val;
	JsonbValue	idval;
	JsonbValue	keystr;
	JsonbValue	valstr;
	JsonbValue	idstr;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	int64		id;
	bool		hasNext;

	if (JsonbType(jb) != jbvObject || jb->type != jbvBinary)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SQL_JSON_OBJECT_NOT_FOUND),
							  errmsg("jsonpath item method .%s() can only be applied to an object",
									 jspOperationName(jsp->type)))));

	jbc = jb->val.binary.data;

	if (!JsonContainerSize(jbc))
		return jperNotFound;	/* no key-value pairs */

	hasNext = jspGetNext(jsp, &next);

	keystr.type = jbvString;
	keystr.val.string.val = "key";
	keystr.val.string.len = 3;

	valstr.type = jbvString;
	valstr.val.string.val = "value";
	valstr.val.string.len = 5;

	idstr.type = jbvString;
	idstr.val.string.val = "id";
	idstr.val.string.len = 2;

	/* construct object id from its base object and offset inside that */
	id = jb->type != jbvBinary ? 0 :
		(int64) ((char *) jbc - (char *) cxt->baseObject.jbc);
	id += (int64) cxt->baseObject.id * INT64CONST(10000000000);

	idval.type = jbvNumeric;
	idval.val.numeric = int64_to_numeric(id);

	it = JsonbIteratorInit(jbc);

	while ((tok = JsonbIteratorNext(&it, &key, true)) != WJB_DONE)
	{
		JsonBaseObjectInfo baseObject;
		JsonbValue	obj;
		JsonbParseState *ps;
		JsonbValue *keyval;
		Jsonb	   *jsonb;

		if (tok != WJB_KEY)
			continue;

		res = jperOk;

		if (!hasNext && !found)
			break;

		tok = JsonbIteratorNext(&it, &val, true);
		Assert(tok == WJB_VALUE);

		ps = NULL;
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);

		pushJsonbValue(&ps, WJB_KEY, &keystr);
		pushJsonbValue(&ps, WJB_VALUE, &key);

		pushJsonbValue(&ps, WJB_KEY, &valstr);
		pushJsonbValue(&ps, WJB_VALUE, &val);

		pushJsonbValue(&ps, WJB_KEY, &idstr);
		pushJsonbValue(&ps, WJB_VALUE, &idval);

		keyval = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);

		jsonb = JsonbValueToJsonb(keyval);

		JsonbInitBinary(&obj, jsonb);

		baseObject = setBaseObject(cxt, &obj, cxt->lastGeneratedObjectId++);

		res = executeNextItem(cxt, jsp, &next, &obj, found, true);

		cxt->baseObject = baseObject;

		if (jperIsError(res))
			return res;

		if (res == jperOk && !found)
			break;
	}

	return res;
}

/*
 * Convert boolean execution status 'res' to a boolean JSON item and execute
 * next jsonpath.
 */
static JsonPathExecResult
appendBoolResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
				 JsonValueList *found, JsonPathBool res)
{
	JsonPathItem next;
	JsonbValue	jbv;

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;			/* found singleton boolean value */

	if (res == jpbUnknown)
	{
		jbv.type = jbvNull;
	}
	else
	{
		jbv.type = jbvBool;
		jbv.val.boolean = res == jpbTrue;
	}

	return executeNextItem(cxt, jsp, &next, &jbv, found, true);
}

/*
 * Convert jsonpath's scalar or variable node to actual jsonb value.
 *
 * If node is a variable then its id returned, otherwise 0 returned.
 */
static void
getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
				JsonbValue *value)
{
	switch (item->type)
	{
		case jpiNull:
			value->type = jbvNull;
			break;
		case jpiBool:
			value->type = jbvBool;
			value->val.boolean = jspGetBool(item);
			break;
		case jpiNumeric:
			value->type = jbvNumeric;
			value->val.numeric = jspGetNumeric(item);
			break;
		case jpiString:
			value->type = jbvString;
			value->val.string.val = jspGetString(item,
												 &value->val.string.len);
			break;
		case jpiVariable:
			getJsonPathVariable(cxt, item, value);
			return;
		default:
			elog(ERROR, "unexpected jsonpath item type");
	}
}

/*
 * Returns the computed value of a JSON path variable with given name.
 */
static JsonbValue *
GetJsonPathVar(void *cxt, char *varName, int varNameLen,
			   JsonbValue *baseObject, int *baseObjectId)
{
	JsonPathVariable *var = NULL;
	List	   *vars = cxt;
	ListCell   *lc;
	JsonbValue *result;
	int			id = 1;

	foreach(lc, vars)
	{
		JsonPathVariable *curvar = lfirst(lc);

		if (curvar->namelen == varNameLen &&
			strncmp(curvar->name, varName, varNameLen) == 0)
		{
			var = curvar;
			break;
		}

		id++;
	}

	if (var == NULL)
	{
		*baseObjectId = -1;
		return NULL;
	}

	result = palloc(sizeof(JsonbValue));
	if (var->isnull)
	{
		*baseObjectId = 0;
		result->type = jbvNull;
	}
	else
		JsonItemFromDatum(var->value, var->typid, var->typmod, result);

	*baseObject = *result;
	*baseObjectId = id;

	return result;
}

static int
CountJsonPathVars(void *cxt)
{
	List	   *vars = (List *) cxt;

	return list_length(vars);
}


/*
 * Initialize JsonbValue to pass to jsonpath executor from given
 * datum value of the specified type.
 */
static void
JsonItemFromDatum(Datum val, Oid typid, int32 typmod, JsonbValue *res)
{
	switch (typid)
	{
		case BOOLOID:
			res->type = jbvBool;
			res->val.boolean = DatumGetBool(val);
			break;
		case NUMERICOID:
			JsonbValueInitNumericDatum(res, val);
			break;
		case INT2OID:
			JsonbValueInitNumericDatum(res, DirectFunctionCall1(int2_numeric, val));
			break;
		case INT4OID:
			JsonbValueInitNumericDatum(res, DirectFunctionCall1(int4_numeric, val));
			break;
		case INT8OID:
			JsonbValueInitNumericDatum(res, DirectFunctionCall1(int8_numeric, val));
			break;
		case FLOAT4OID:
			JsonbValueInitNumericDatum(res, DirectFunctionCall1(float4_numeric, val));
			break;
		case FLOAT8OID:
			JsonbValueInitNumericDatum(res, DirectFunctionCall1(float8_numeric, val));
			break;
		case TEXTOID:
		case VARCHAROID:
			res->type = jbvString;
			res->val.string.val = VARDATA_ANY(val);
			res->val.string.len = VARSIZE_ANY_EXHDR(val);
			break;
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			res->type = jbvDatetime;
			res->val.datetime.value = val;
			res->val.datetime.typid = typid;
			res->val.datetime.typmod = typmod;
			res->val.datetime.tz = 0;
			break;
		case JSONBOID:
			{
				JsonbValue *jbv = res;
				Jsonb	   *jb = DatumGetJsonbP(val);

				if (JsonContainerIsScalar(&jb->root))
				{
					bool		result PG_USED_FOR_ASSERTS_ONLY;

					result = JsonbExtractScalar(&jb->root, jbv);
					Assert(result);
				}
				else
					JsonbInitBinary(jbv, jb);
				break;
			}
		case JSONOID:
			{
				text	   *txt = DatumGetTextP(val);
				char	   *str = text_to_cstring(txt);
				Jsonb	   *jb;

				jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
														CStringGetDatum(str)));
				pfree(str);

				JsonItemFromDatum(JsonbPGetDatum(jb), JSONBOID, -1, res);
				break;
			}
		default:
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not convert value of type %s to jsonpath",
						   format_type_be(typid)));
	}
}

/* Initialize numeric value from the given datum */
static void
JsonbValueInitNumericDatum(JsonbValue *jbv, Datum num)
{
	jbv->type = jbvNumeric;
	jbv->val.numeric = DatumGetNumeric(num);
}

/*
 * Get the value of variable passed to jsonpath executor
 */
static void
getJsonPathVariable(JsonPathExecContext *cxt, JsonPathItem *variable,
					JsonbValue *value)
{
	char	   *varName;
	int			varNameLength;
	JsonbValue	baseObject;
	int			baseObjectId;
	JsonbValue *v;

	Assert(variable->type == jpiVariable);
	varName = jspGetString(variable, &varNameLength);

	if (cxt->vars == NULL ||
		(v = cxt->getVar(cxt->vars, varName, varNameLength,
						 &baseObject, &baseObjectId)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find jsonpath variable \"%s\"",
						pnstrdup(varName, varNameLength))));

	if (baseObjectId > 0)
	{
		*value = *v;
		setBaseObject(cxt, &baseObject, baseObjectId);
	}
}

/*
 * Definition of JsonPathGetVarCallback for when JsonPathExecContext.vars
 * is specified as a jsonb value.
 */
static JsonbValue *
getJsonPathVariableFromJsonb(void *varsJsonb, char *varName, int varNameLength,
							 JsonbValue *baseObject, int *baseObjectId)
{
	Jsonb	   *vars = varsJsonb;
	JsonbValue	tmp;
	JsonbValue *result;

	tmp.type = jbvString;
	tmp.val.string.val = varName;
	tmp.val.string.len = varNameLength;

	result = findJsonbValueFromContainer(&vars->root, JB_FOBJECT, &tmp);

	if (result == NULL)
	{
		*baseObjectId = -1;
		return NULL;
	}

	*baseObjectId = 1;
	JsonbInitBinary(baseObject, vars);

	return result;
}

/*
 * Definition of JsonPathCountVarsCallback for when JsonPathExecContext.vars
 * is specified as a jsonb value.
 */
static int
countVariablesFromJsonb(void *varsJsonb)
{
	Jsonb	   *vars = varsJsonb;

	if (vars && !JsonContainerIsObject(&vars->root))
	{
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"vars\" argument is not an object"),
				errdetail("Jsonpath parameters should be encoded as key-value pairs of \"vars\" object."));
	}

	/* count of base objects */
	return vars != NULL ? 1 : 0;
}

/**************** Support functions for JsonPath execution *****************/

/*
 * Returns the size of an array item, or -1 if item is not an array.
 */
static int
JsonbArraySize(JsonbValue *jb)
{
	Assert(jb->type != jbvArray);

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = jb->val.binary.data;

		if (JsonContainerIsArray(jbc) && !JsonContainerIsScalar(jbc))
			return JsonContainerSize(jbc);
	}

	return -1;
}

/* Comparison predicate callback. */
static JsonPathBool
executeComparison(JsonPathItem *cmp, JsonbValue *lv, JsonbValue *rv, void *p)
{
	JsonPathExecContext *cxt = (JsonPathExecContext *) p;

	return compareItems(cmp->type, lv, rv, cxt->useTz);
}

/*
 * Perform per-byte comparison of two strings.
 */
static int
binaryCompareStrings(const char *s1, int len1,
					 const char *s2, int len2)
{
	int			cmp;

	cmp = memcmp(s1, s2, Min(len1, len2));

	if (cmp != 0)
		return cmp;

	if (len1 == len2)
		return 0;

	return len1 < len2 ? -1 : 1;
}

/*
 * Compare two strings in the current server encoding using Unicode codepoint
 * collation.
 */
static int
compareStrings(const char *mbstr1, int mblen1,
			   const char *mbstr2, int mblen2)
{
	if (GetDatabaseEncoding() == PG_SQL_ASCII ||
		GetDatabaseEncoding() == PG_UTF8)
	{
		/*
		 * It's known property of UTF-8 strings that their per-byte comparison
		 * result matches codepoints comparison result.  ASCII can be
		 * considered as special case of UTF-8.
		 */
		return binaryCompareStrings(mbstr1, mblen1, mbstr2, mblen2);
	}
	else
	{
		char	   *utf8str1,
				   *utf8str2;
		int			cmp,
					utf8len1,
					utf8len2;

		/*
		 * We have to convert other encodings to UTF-8 first, then compare.
		 * Input strings may be not null-terminated and pg_server_to_any() may
		 * return them "as is".  So, use strlen() only if there is real
		 * conversion.
		 */
		utf8str1 = pg_server_to_any(mbstr1, mblen1, PG_UTF8);
		utf8str2 = pg_server_to_any(mbstr2, mblen2, PG_UTF8);
		utf8len1 = (mbstr1 == utf8str1) ? mblen1 : strlen(utf8str1);
		utf8len2 = (mbstr2 == utf8str2) ? mblen2 : strlen(utf8str2);

		cmp = binaryCompareStrings(utf8str1, utf8len1, utf8str2, utf8len2);

		/*
		 * If pg_server_to_any() did no real conversion, then we actually
		 * compared original strings.  So, we already done.
		 */
		if (mbstr1 == utf8str1 && mbstr2 == utf8str2)
			return cmp;

		/* Free memory if needed */
		if (mbstr1 != utf8str1)
			pfree(utf8str1);
		if (mbstr2 != utf8str2)
			pfree(utf8str2);

		/*
		 * When all Unicode codepoints are equal, return result of binary
		 * comparison.  In some edge cases, same characters may have different
		 * representations in encoding.  Then our behavior could diverge from
		 * standard.  However, that allow us to do simple binary comparison
		 * for "==" operator, which is performance critical in typical cases.
		 * In future to implement strict standard conformance, we can do
		 * normalization of input JSON strings.
		 */
		if (cmp == 0)
			return binaryCompareStrings(mbstr1, mblen1, mbstr2, mblen2);
		else
			return cmp;
	}
}

/*
 * Compare two SQL/JSON items using comparison operation 'op'.
 */
static JsonPathBool
compareItems(int32 op, JsonbValue *jb1, JsonbValue *jb2, bool useTz)
{
	int			cmp;
	bool		res;

	if (jb1->type != jb2->type)
	{
		if (jb1->type == jbvNull || jb2->type == jbvNull)

			/*
			 * Equality and order comparison of nulls to non-nulls returns
			 * always false, but inequality comparison returns true.
			 */
			return op == jpiNotEqual ? jpbTrue : jpbFalse;

		/* Non-null items of different types are not comparable. */
		return jpbUnknown;
	}

	switch (jb1->type)
	{
		case jbvNull:
			cmp = 0;
			break;
		case jbvBool:
			cmp = jb1->val.boolean == jb2->val.boolean ? 0 :
				jb1->val.boolean ? 1 : -1;
			break;
		case jbvNumeric:
			cmp = compareNumeric(jb1->val.numeric, jb2->val.numeric);
			break;
		case jbvString:
			if (op == jpiEqual)
				return jb1->val.string.len != jb2->val.string.len ||
					memcmp(jb1->val.string.val,
						   jb2->val.string.val,
						   jb1->val.string.len) ? jpbFalse : jpbTrue;

			cmp = compareStrings(jb1->val.string.val, jb1->val.string.len,
								 jb2->val.string.val, jb2->val.string.len);
			break;
		case jbvDatetime:
			{
				bool		cast_error;

				cmp = compareDatetime(jb1->val.datetime.value,
									  jb1->val.datetime.typid,
									  jb2->val.datetime.value,
									  jb2->val.datetime.typid,
									  useTz,
									  &cast_error);

				if (cast_error)
					return jpbUnknown;
			}
			break;

		case jbvBinary:
		case jbvArray:
		case jbvObject:
			return jpbUnknown;	/* non-scalars are not comparable */

		default:
			elog(ERROR, "invalid jsonb value type %d", jb1->type);
	}

	switch (op)
	{
		case jpiEqual:
			res = (cmp == 0);
			break;
		case jpiNotEqual:
			res = (cmp != 0);
			break;
		case jpiLess:
			res = (cmp < 0);
			break;
		case jpiGreater:
			res = (cmp > 0);
			break;
		case jpiLessOrEqual:
			res = (cmp <= 0);
			break;
		case jpiGreaterOrEqual:
			res = (cmp >= 0);
			break;
		default:
			elog(ERROR, "unrecognized jsonpath operation: %d", op);
			return jpbUnknown;
	}

	return res ? jpbTrue : jpbFalse;
}

/* Compare two numerics */
static int
compareNumeric(Numeric a, Numeric b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
											 NumericGetDatum(a),
											 NumericGetDatum(b)));
}

static JsonbValue *
copyJsonbValue(JsonbValue *src)
{
	JsonbValue *dst = palloc(sizeof(*dst));

	*dst = *src;

	return dst;
}

/*
 * Execute array subscript expression and convert resulting numeric item to
 * the integer type with truncation.
 */
static JsonPathExecResult
getArrayIndex(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbValue *jb,
			  int32 *index)
{
	JsonbValue *jbv;
	JsonValueList found = {0};
	JsonPathExecResult res = executeItem(cxt, jsp, jb, &found);
	Datum		numeric_index;
	bool		have_error = false;

	if (jperIsError(res))
		return res;

	if (JsonValueListLength(&found) != 1 ||
		!(jbv = getScalar(JsonValueListHead(&found), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
							  errmsg("jsonpath array subscript is not a single numeric value"))));

	numeric_index = DirectFunctionCall2(numeric_trunc,
										NumericGetDatum(jbv->val.numeric),
										Int32GetDatum(0));

	*index = numeric_int4_opt_error(DatumGetNumeric(numeric_index),
									&have_error);

	if (have_error)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
							  errmsg("jsonpath array subscript is out of integer range"))));

	return jperOk;
}

/* Save base object and its id needed for the execution of .keyvalue(). */
static JsonBaseObjectInfo
setBaseObject(JsonPathExecContext *cxt, JsonbValue *jbv, int32 id)
{
	JsonBaseObjectInfo baseObject = cxt->baseObject;

	cxt->baseObject.jbc = jbv->type != jbvBinary ? NULL :
		(JsonbContainer *) jbv->val.binary.data;
	cxt->baseObject.id = id;

	return baseObject;
}

static void
JsonValueListClear(JsonValueList *jvl)
{
	jvl->singleton = NULL;
	jvl->list = NIL;
}

static void
JsonValueListAppend(JsonValueList *jvl, JsonbValue *jbv)
{
	if (jvl->singleton)
	{
		jvl->list = list_make2(jvl->singleton, jbv);
		jvl->singleton = NULL;
	}
	else if (!jvl->list)
		jvl->singleton = jbv;
	else
		jvl->list = lappend(jvl->list, jbv);
}

static int
JsonValueListLength(const JsonValueList *jvl)
{
	return jvl->singleton ? 1 : list_length(jvl->list);
}

static bool
JsonValueListIsEmpty(JsonValueList *jvl)
{
	return !jvl->singleton && (jvl->list == NIL);
}

static JsonbValue *
JsonValueListHead(JsonValueList *jvl)
{
	return jvl->singleton ? jvl->singleton : linitial(jvl->list);
}

static List *
JsonValueListGetList(JsonValueList *jvl)
{
	if (jvl->singleton)
		return list_make1(jvl->singleton);

	return jvl->list;
}

static void
JsonValueListInitIterator(const JsonValueList *jvl, JsonValueListIterator *it)
{
	if (jvl->singleton)
	{
		it->value = jvl->singleton;
		it->list = NIL;
		it->next = NULL;
	}
	else if (jvl->list != NIL)
	{
		it->value = (JsonbValue *) linitial(jvl->list);
		it->list = jvl->list;
		it->next = list_second_cell(jvl->list);
	}
	else
	{
		it->value = NULL;
		it->list = NIL;
		it->next = NULL;
	}
}

/*
 * Get the next item from the sequence advancing iterator.
 */
static JsonbValue *
JsonValueListNext(const JsonValueList *jvl, JsonValueListIterator *it)
{
	JsonbValue *result = it->value;

	if (it->next)
	{
		it->value = lfirst(it->next);
		it->next = lnext(it->list, it->next);
	}
	else
	{
		it->value = NULL;
	}

	return result;
}

/*
 * Initialize a binary JsonbValue with the given jsonb container.
 */
static JsonbValue *
JsonbInitBinary(JsonbValue *jbv, Jsonb *jb)
{
	jbv->type = jbvBinary;
	jbv->val.binary.data = &jb->root;
	jbv->val.binary.len = VARSIZE_ANY_EXHDR(jb);

	return jbv;
}

/*
 * Returns jbv* type of JsonbValue. Note, it never returns jbvBinary as is.
 */
static int
JsonbType(JsonbValue *jb)
{
	int			type = jb->type;

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = (void *) jb->val.binary.data;

		/* Scalars should be always extracted during jsonpath execution. */
		Assert(!JsonContainerIsScalar(jbc));

		if (JsonContainerIsObject(jbc))
			type = jbvObject;
		else if (JsonContainerIsArray(jbc))
			type = jbvArray;
		else
			elog(ERROR, "invalid jsonb container type: 0x%08x", jbc->header);
	}

	return type;
}

/* Get scalar of given type or NULL on type mismatch */
static JsonbValue *
getScalar(JsonbValue *scalar, enum jbvType type)
{
	/* Scalars should be always extracted during jsonpath execution. */
	Assert(scalar->type != jbvBinary ||
		   !JsonContainerIsScalar(scalar->val.binary.data));

	return scalar->type == type ? scalar : NULL;
}

/* Construct a JSON array from the item list */
static JsonbValue *
wrapItemsInArray(const JsonValueList *items)
{
	JsonbParseState *ps = NULL;
	JsonValueListIterator it;
	JsonbValue *jbv;

	pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);

	JsonValueListInitIterator(items, &it);
	while ((jbv = JsonValueListNext(items, &it)))
		pushJsonbValue(&ps, WJB_ELEM, jbv);

	return pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
}

/* Check if the timezone required for casting from type1 to type2 is used */
static void
checkTimezoneIsUsedForCast(bool useTz, const char *type1, const char *type2)
{
	if (!useTz)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot convert value from %s to %s without time zone usage",
						type1, type2),
				 errhint("Use *_tz() function for time zone support.")));
}

/* Convert time datum to timetz datum */
static Datum
castTimeToTimeTz(Datum time, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "time", "timetz");

	return DirectFunctionCall1(time_timetz, time);
}

/*
 * Compare date to timestamp.
 * Note that this doesn't involve any timezone considerations.
 */
static int
cmpDateToTimestamp(DateADT date1, Timestamp ts2, bool useTz)
{
	return date_cmp_timestamp_internal(date1, ts2);
}

/*
 * Compare date to timestamptz.
 */
static int
cmpDateToTimestampTz(DateADT date1, TimestampTz tstz2, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "date", "timestamptz");

	return date_cmp_timestamptz_internal(date1, tstz2);
}

/*
 * Compare timestamp to timestamptz.
 */
static int
cmpTimestampToTimestampTz(Timestamp ts1, TimestampTz tstz2, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "timestamp", "timestamptz");

	return timestamp_cmp_timestamptz_internal(ts1, tstz2);
}

/*
 * Cross-type comparison of two datetime SQL/JSON items.  If items are
 * uncomparable *cast_error flag is set, otherwise *cast_error is unset.
 * If the cast requires timezone and it is not used, then explicit error is thrown.
 */
static int
compareDatetime(Datum val1, Oid typid1, Datum val2, Oid typid2,
				bool useTz, bool *cast_error)
{
	PGFunction	cmpfunc;

	*cast_error = false;

	switch (typid1)
	{
		case DATEOID:
			switch (typid2)
			{
				case DATEOID:
					cmpfunc = date_cmp;

					break;

				case TIMESTAMPOID:
					return cmpDateToTimestamp(DatumGetDateADT(val1),
											  DatumGetTimestamp(val2),
											  useTz);

				case TIMESTAMPTZOID:
					return cmpDateToTimestampTz(DatumGetDateADT(val1),
												DatumGetTimestampTz(val2),
												useTz);

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* uncomparable types */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMEOID:
			switch (typid2)
			{
				case TIMEOID:
					cmpfunc = time_cmp;

					break;

				case TIMETZOID:
					val1 = castTimeToTimeTz(val1, useTz);
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*cast_error = true; /* uncomparable types */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMETZOID:
			switch (typid2)
			{
				case TIMEOID:
					val2 = castTimeToTimeTz(val2, useTz);
					cmpfunc = timetz_cmp;

					break;

				case TIMETZOID:
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*cast_error = true; /* uncomparable types */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMESTAMPOID:
			switch (typid2)
			{
				case DATEOID:
					return -cmpDateToTimestamp(DatumGetDateADT(val2),
											   DatumGetTimestamp(val1),
											   useTz);

				case TIMESTAMPOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPTZOID:
					return cmpTimestampToTimestampTz(DatumGetTimestamp(val1),
													 DatumGetTimestampTz(val2),
													 useTz);

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* uncomparable types */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMESTAMPTZOID:
			switch (typid2)
			{
				case DATEOID:
					return -cmpDateToTimestampTz(DatumGetDateADT(val2),
												 DatumGetTimestampTz(val1),
												 useTz);

				case TIMESTAMPOID:
					return -cmpTimestampToTimestampTz(DatumGetTimestamp(val2),
													  DatumGetTimestampTz(val1),
													  useTz);

				case TIMESTAMPTZOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* uncomparable types */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		default:
			elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u", typid1);
	}

	if (*cast_error)
		return 0;				/* cast error */

	return DatumGetInt32(DirectFunctionCall2(cmpfunc, val1, val2));
}

/*
 * Executor-callable JSON_EXISTS implementation
 *
 * Returns NULL instead of throwing errors if 'error' is not NULL, setting
 * *error to true.
 */
bool
JsonPathExists(Datum jb, JsonPath *jp, bool *error, List *vars)
{
	JsonPathExecResult res;

	res = executeJsonPath(jp, vars,
						  GetJsonPathVar, CountJsonPathVars,
						  DatumGetJsonbP(jb), !error, NULL, true);

	Assert(error || !jperIsError(res));

	if (error && jperIsError(res))
		*error = true;

	return res == jperOk;
}

/*
 * Executor-callable JSON_QUERY implementation
 *
 * Returns NULL instead of throwing errors if 'error' is not NULL, setting
 * *error to true.  *empty is set to true if no match is found.
 */
Datum
JsonPathQuery(Datum jb, JsonPath *jp, JsonWrapper wrapper, bool *empty,
			  bool *error, List *vars,
			  const char *column_name)
{
	JsonbValue *singleton;
	bool		wrap;
	JsonValueList found = {0};
	JsonPathExecResult res;
	int			count;

	res = executeJsonPath(jp, vars,
						  GetJsonPathVar, CountJsonPathVars,
						  DatumGetJsonbP(jb), !error, &found, true);
	Assert(error || !jperIsError(res));
	if (error && jperIsError(res))
	{
		*error = true;
		*empty = false;
		return (Datum) 0;
	}

	/*
	 * Determine whether to wrap the result in a JSON array or not.
	 *
	 * First, count the number of SQL/JSON items in the returned
	 * JsonValueList. If the list is empty (singleton == NULL), no wrapping is
	 * necessary.
	 *
	 * If the wrapper mode is JSW_NONE or JSW_UNSPEC, wrapping is explicitly
	 * disabled. This enforces a WITHOUT WRAPPER clause, which is also the
	 * default when no WRAPPER clause is specified.
	 *
	 * If the mode is JSW_UNCONDITIONAL, wrapping is enforced regardless of
	 * the number of SQL/JSON items, enforcing a WITH WRAPPER or WITH
	 * UNCONDITIONAL WRAPPER clause.
	 *
	 * For JSW_CONDITIONAL, wrapping occurs only if there is more than one
	 * SQL/JSON item in the list, enforcing a WITH CONDITIONAL WRAPPER clause.
	 */
	count = JsonValueListLength(&found);
	singleton = count > 0 ? JsonValueListHead(&found) : NULL;
	if (singleton == NULL)
		wrap = false;
	else if (wrapper == JSW_NONE || wrapper == JSW_UNSPEC)
		wrap = false;
	else if (wrapper == JSW_UNCONDITIONAL)
		wrap = true;
	else if (wrapper == JSW_CONDITIONAL)
		wrap = count > 1;
	else
	{
		elog(ERROR, "unrecognized json wrapper %d", (int) wrapper);
		wrap = false;
	}

	if (wrap)
		return JsonbPGetDatum(JsonbValueToJsonb(wrapItemsInArray(&found)));

	/* No wrapping means only one item is expected. */
	if (count > 1)
	{
		if (error)
		{
			*error = true;
			return (Datum) 0;
		}

		if (column_name)
			ereport(ERROR,
					(errcode(ERRCODE_MORE_THAN_ONE_SQL_JSON_ITEM),
					 errmsg("JSON path expression for column \"%s\" must return single item when no wrapper is requested",
							column_name),
					 errhint("Use the WITH WRAPPER clause to wrap SQL/JSON items into an array.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_MORE_THAN_ONE_SQL_JSON_ITEM),
					 errmsg("JSON path expression in JSON_QUERY must return single item when no wrapper is requested"),
					 errhint("Use the WITH WRAPPER clause to wrap SQL/JSON items into an array.")));
	}

	if (singleton)
		return JsonbPGetDatum(JsonbValueToJsonb(singleton));

	*empty = true;
	return PointerGetDatum(NULL);
}

/*
 * Executor-callable JSON_VALUE implementation
 *
 * Returns NULL instead of throwing errors if 'error' is not NULL, setting
 * *error to true.  *empty is set to true if no match is found.
 */
JsonbValue *
JsonPathValue(Datum jb, JsonPath *jp, bool *empty, bool *error, List *vars,
			  const char *column_name)
{
	JsonbValue *res;
	JsonValueList found = {0};
	JsonPathExecResult jper PG_USED_FOR_ASSERTS_ONLY;
	int			count;

	jper = executeJsonPath(jp, vars, GetJsonPathVar, CountJsonPathVars,
						   DatumGetJsonbP(jb),
						   !error, &found, true);

	Assert(error || !jperIsError(jper));

	if (error && jperIsError(jper))
	{
		*error = true;
		*empty = false;
		return NULL;
	}

	count = JsonValueListLength(&found);

	*empty = (count == 0);

	if (*empty)
		return NULL;

	/* JSON_VALUE expects to get only singletons. */
	if (count > 1)
	{
		if (error)
		{
			*error = true;
			return NULL;
		}

		if (column_name)
			ereport(ERROR,
					(errcode(ERRCODE_MORE_THAN_ONE_SQL_JSON_ITEM),
					 errmsg("JSON path expression for column \"%s\" must return single scalar item",
							column_name)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_MORE_THAN_ONE_SQL_JSON_ITEM),
					 errmsg("JSON path expression in JSON_VALUE must return single scalar item")));
	}

	res = JsonValueListHead(&found);
	if (res->type == jbvBinary && JsonContainerIsScalar(res->val.binary.data))
		JsonbExtractScalar(res->val.binary.data, res);

	/* JSON_VALUE expects to get only scalars. */
	if (!IsAJsonbScalar(res))
	{
		if (error)
		{
			*error = true;
			return NULL;
		}

		if (column_name)
			ereport(ERROR,
					(errcode(ERRCODE_SQL_JSON_SCALAR_REQUIRED),
					 errmsg("JSON path expression for column \"%s\" must return single scalar item",
							column_name)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_SQL_JSON_SCALAR_REQUIRED),
					 errmsg("JSON path expression in JSON_VALUE must return single scalar item")));
	}

	if (res->type == jbvNull)
		return NULL;

	return res;
}

/************************ JSON_TABLE functions ***************************/

/*
 * Sanity-checks and returns the opaque JsonTableExecContext from the
 * given executor state struct.
 */
static inline JsonTableExecContext *
GetJsonTableExecContext(TableFuncScanState *state, const char *fname)
{
	JsonTableExecContext *result;

	if (!IsA(state, TableFuncScanState))
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);
	result = (JsonTableExecContext *) state->opaque;
	if (result->magic != JSON_TABLE_EXEC_CONTEXT_MAGIC)
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);

	return result;
}

/*
 * JsonTableInitOpaque
 *		Fill in TableFuncScanState->opaque for processing JSON_TABLE
 *
 * This initializes the PASSING arguments and the JsonTablePlanState for
 * JsonTablePlan given in TableFunc.
 */
static void
JsonTableInitOpaque(TableFuncScanState *state, int natts)
{
	JsonTableExecContext *cxt;
	PlanState  *ps = &state->ss.ps;
	TableFuncScan *tfs = castNode(TableFuncScan, ps->plan);
	TableFunc  *tf = tfs->tablefunc;
	JsonTablePlan *rootplan = (JsonTablePlan *) tf->plan;
	JsonExpr   *je = castNode(JsonExpr, tf->docexpr);
	List	   *args = NIL;

	cxt = palloc0(sizeof(JsonTableExecContext));
	cxt->magic = JSON_TABLE_EXEC_CONTEXT_MAGIC;

	/*
	 * Evaluate JSON_TABLE() PASSING arguments to be passed to the jsonpath
	 * executor via JsonPathVariables.
	 */
	if (state->passingvalexprs)
	{
		ListCell   *exprlc;
		ListCell   *namelc;

		Assert(list_length(state->passingvalexprs) ==
			   list_length(je->passing_names));
		forboth(exprlc, state->passingvalexprs,
				namelc, je->passing_names)
		{
			ExprState  *state = lfirst_node(ExprState, exprlc);
			String	   *name = lfirst_node(String, namelc);
			JsonPathVariable *var = palloc(sizeof(*var));

			var->name = pstrdup(name->sval);
			var->namelen = strlen(var->name);
			var->typid = exprType((Node *) state->expr);
			var->typmod = exprTypmod((Node *) state->expr);

			/*
			 * Evaluate the expression and save the value to be returned by
			 * GetJsonPathVar().
			 */
			var->value = ExecEvalExpr(state, ps->ps_ExprContext,
									  &var->isnull);

			args = lappend(args, var);
		}
	}

	cxt->colplanstates = palloc(sizeof(JsonTablePlanState *) *
								list_length(tf->colvalexprs));

	/*
	 * Initialize plan for the root path and, recursively, also any child
	 * plans that compute the NESTED paths.
	 */
	cxt->rootplanstate = JsonTableInitPlan(cxt, rootplan, NULL, args,
										   CurrentMemoryContext);

	state->opaque = cxt;
}

/*
 * JsonTableDestroyOpaque
 *		Resets state->opaque
 */
static void
JsonTableDestroyOpaque(TableFuncScanState *state)
{
	JsonTableExecContext *cxt =
		GetJsonTableExecContext(state, "JsonTableDestroyOpaque");

	/* not valid anymore */
	cxt->magic = 0;

	state->opaque = NULL;
}

/*
 * JsonTableInitPlan
 *		Initialize information for evaluating jsonpath in the given
 *		JsonTablePlan and, recursively, in any child plans
 */
static JsonTablePlanState *
JsonTableInitPlan(JsonTableExecContext *cxt, JsonTablePlan *plan,
				  JsonTablePlanState *parentstate,
				  List *args, MemoryContext mcxt)
{
	JsonTablePlanState *planstate = palloc0(sizeof(*planstate));

	planstate->plan = plan;
	planstate->parent = parentstate;

	if (IsA(plan, JsonTablePathScan))
	{
		JsonTablePathScan *scan = (JsonTablePathScan *) plan;
		int			i;

		planstate->path = DatumGetJsonPathP(scan->path->value->constvalue);
		planstate->args = args;
		planstate->mcxt = AllocSetContextCreate(mcxt, "JsonTableExecContext",
												ALLOCSET_DEFAULT_SIZES);

		/* No row pattern evaluated yet. */
		planstate->current.value = PointerGetDatum(NULL);
		planstate->current.isnull = true;

		for (i = scan->colMin; i >= 0 && i <= scan->colMax; i++)
			cxt->colplanstates[i] = planstate;

		planstate->nested = scan->child ?
			JsonTableInitPlan(cxt, scan->child, planstate, args, mcxt) : NULL;
	}
	else if (IsA(plan, JsonTableSiblingJoin))
	{
		JsonTableSiblingJoin *join = (JsonTableSiblingJoin *) plan;

		planstate->left = JsonTableInitPlan(cxt, join->lplan, parentstate,
											args, mcxt);
		planstate->right = JsonTableInitPlan(cxt, join->rplan, parentstate,
											 args, mcxt);
	}

	return planstate;
}

/*
 * JsonTableSetDocument
 *		Install the input document and evaluate the row pattern
 */
static void
JsonTableSetDocument(TableFuncScanState *state, Datum value)
{
	JsonTableExecContext *cxt =
		GetJsonTableExecContext(state, "JsonTableSetDocument");

	JsonTableResetRowPattern(cxt->rootplanstate, value);
}

/*
 * Evaluate a JsonTablePlan's jsonpath to get a new row pattern from
 * the given context item
 */
static void
JsonTableResetRowPattern(JsonTablePlanState *planstate, Datum item)
{
	JsonTablePathScan *scan = castNode(JsonTablePathScan, planstate->plan);
	MemoryContext oldcxt;
	JsonPathExecResult res;
	Jsonb	   *js = (Jsonb *) DatumGetJsonbP(item);

	JsonValueListClear(&planstate->found);

	MemoryContextResetOnly(planstate->mcxt);

	oldcxt = MemoryContextSwitchTo(planstate->mcxt);

	res = executeJsonPath(planstate->path, planstate->args,
						  GetJsonPathVar, CountJsonPathVars,
						  js, scan->errorOnError,
						  &planstate->found,
						  true);

	MemoryContextSwitchTo(oldcxt);

	if (jperIsError(res))
	{
		Assert(!scan->errorOnError);
		JsonValueListClear(&planstate->found);
	}

	/* Reset plan iterator to the beginning of the item list */
	JsonValueListInitIterator(&planstate->found, &planstate->iter);
	planstate->current.value = PointerGetDatum(NULL);
	planstate->current.isnull = true;
	planstate->ordinal = 0;
}

/*
 * Fetch next row from a JsonTablePlan.
 *
 * Returns false if the plan has run out of rows, true otherwise.
 */
static bool
JsonTablePlanNextRow(JsonTablePlanState *planstate)
{
	if (IsA(planstate->plan, JsonTablePathScan))
		return JsonTablePlanScanNextRow(planstate);
	else if (IsA(planstate->plan, JsonTableSiblingJoin))
		return JsonTablePlanJoinNextRow(planstate);
	else
		elog(ERROR, "invalid JsonTablePlan %d", (int) planstate->plan->type);

	Assert(false);
	/* Appease compiler */
	return false;
}

/*
 * Fetch next row from a JsonTablePlan's path evaluation result and from
 * any child nested path(s).
 *
 * Returns true if any of the paths (this or the nested) has more rows to
 * return.
 *
 * By fetching the nested path(s)'s rows based on the parent row at each
 * level, this essentially joins the rows of different levels.  If a nested
 * path at a given level has no matching rows, the columns of that level will
 * compute to NULL, making it an OUTER join.
 */
static bool
JsonTablePlanScanNextRow(JsonTablePlanState *planstate)
{
	JsonbValue *jbv;
	MemoryContext oldcxt;

	/*
	 * If planstate already has an active row and there is a nested plan,
	 * check if it has an active row to join with the former.
	 */
	if (!planstate->current.isnull)
	{
		if (planstate->nested && JsonTablePlanNextRow(planstate->nested))
			return true;
	}

	/* Fetch new row from the list of found values to set as active. */
	jbv = JsonValueListNext(&planstate->found, &planstate->iter);

	/* End of list? */
	if (jbv == NULL)
	{
		planstate->current.value = PointerGetDatum(NULL);
		planstate->current.isnull = true;
		return false;
	}

	/*
	 * Set current row item for subsequent JsonTableGetValue() calls for
	 * evaluating individual columns.
	 */
	oldcxt = MemoryContextSwitchTo(planstate->mcxt);
	planstate->current.value = JsonbPGetDatum(JsonbValueToJsonb(jbv));
	planstate->current.isnull = false;
	MemoryContextSwitchTo(oldcxt);

	/* Next row! */
	planstate->ordinal++;

	/* Process nested plan(s), if any. */
	if (planstate->nested)
	{
		/* Re-evaluate the nested path using the above parent row. */
		JsonTableResetNestedPlan(planstate->nested);

		/*
		 * Now fetch the nested plan's current row to be joined against the
		 * parent row.  Any further nested plans' paths will be re-evaluated
		 * recursively, level at a time, after setting each nested plan's
		 * current row.
		 */
		(void) JsonTablePlanNextRow(planstate->nested);
	}

	/* There are more rows. */
	return true;
}

/*
 * Re-evaluate the row pattern of a nested plan using the new parent row
 * pattern.
 */
static void
JsonTableResetNestedPlan(JsonTablePlanState *planstate)
{
	/* This better be a child plan. */
	Assert(planstate->parent != NULL);
	if (IsA(planstate->plan, JsonTablePathScan))
	{
		JsonTablePlanState *parent = planstate->parent;

		if (!parent->current.isnull)
			JsonTableResetRowPattern(planstate, parent->current.value);

		/*
		 * If this plan itself has a child nested plan, it will be reset when
		 * the caller calls JsonTablePlanNextRow() on this plan.
		 */
	}
	else if (IsA(planstate->plan, JsonTableSiblingJoin))
	{
		JsonTableResetNestedPlan(planstate->left);
		JsonTableResetNestedPlan(planstate->right);
	}
}

/*
 * Fetch the next row from a JsonTableSiblingJoin.
 *
 * This is essentially a UNION between the rows from left and right siblings.
 */
static bool
JsonTablePlanJoinNextRow(JsonTablePlanState *planstate)
{

	/* Fetch row from left sibling. */
	if (!JsonTablePlanNextRow(planstate->left))
	{
		/*
		 * Left sibling ran out of rows, so start fetching from the right
		 * sibling.
		 */
		if (!JsonTablePlanNextRow(planstate->right))
		{
			/* Right sibling ran out of row, so there are more rows. */
			return false;
		}
	}

	return true;
}

/*
 * JsonTableFetchRow
 *		Prepare the next "current" row for upcoming GetValue calls.
 *
 * Returns false if no more rows can be returned.
 */
static bool
JsonTableFetchRow(TableFuncScanState *state)
{
	JsonTableExecContext *cxt =
		GetJsonTableExecContext(state, "JsonTableFetchRow");

	return JsonTablePlanNextRow(cxt->rootplanstate);
}

/*
 * JsonTableGetValue
 *		Return the value for column number 'colnum' for the current row.
 *
 * This leaks memory, so be sure to reset often the context in which it's
 * called.
 */
static Datum
JsonTableGetValue(TableFuncScanState *state, int colnum,
				  Oid typid, int32 typmod, bool *isnull)
{
	JsonTableExecContext *cxt =
		GetJsonTableExecContext(state, "JsonTableGetValue");
	ExprContext *econtext = state->ss.ps.ps_ExprContext;
	ExprState  *estate = list_nth(state->colvalexprs, colnum);
	JsonTablePlanState *planstate = cxt->colplanstates[colnum];
	JsonTablePlanRowSource *current = &planstate->current;
	Datum		result;

	/* Row pattern value is NULL */
	if (current->isnull)
	{
		result = (Datum) 0;
		*isnull = true;
	}
	/* Evaluate JsonExpr. */
	else if (estate)
	{
		Datum		saved_caseValue = econtext->caseValue_datum;
		bool		saved_caseIsNull = econtext->caseValue_isNull;

		/* Pass the row pattern value via CaseTestExpr. */
		econtext->caseValue_datum = current->value;
		econtext->caseValue_isNull = false;

		result = ExecEvalExpr(estate, econtext, isnull);

		econtext->caseValue_datum = saved_caseValue;
		econtext->caseValue_isNull = saved_caseIsNull;
	}
	/* ORDINAL column */
	else
	{
		result = Int32GetDatum(planstate->ordinal);
		*isnull = false;
	}

	return result;
}

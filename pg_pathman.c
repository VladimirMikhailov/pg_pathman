/* ------------------------------------------------------------------------
 *
 * pg_pathman.c
 *		This module sets planner hooks, handles SELECT queries and produces
 *		paths for partitioned tables
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */
#include "pathman.h"
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/cost.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "utils/hsearch.h"
#include "utils/tqual.h"
#include "utils/rel.h"
#include "utils/elog.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "storage/ipc.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"

PG_MODULE_MAGIC;

typedef struct
{
	Oid old_varno;
	Oid new_varno;
} change_varno_context;

typedef struct
{
	const Node	   *orig;
	List		   *args;
	List		   *rangeset;
} WrapperNode;

/* Original hooks */
static set_rel_pathlist_hook_type set_rel_pathlist_hook_original = NULL;
static shmem_startup_hook_type shmem_startup_hook_original = NULL;
static post_parse_analyze_hook_type post_parse_analyze_hook_original = NULL;
static planner_hook_type planner_hook_original = NULL;

/* pg module functions */
void _PG_init(void);
void _PG_fini(void);

/* Hook functions */
static void pathman_shmem_startup(void);
static void pathman_set_rel_pathlist_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
void pathman_post_parse_analysis_hook(ParseState *pstate, Query *query);
static PlannedStmt * pathman_planner_hook(Query *parse, int cursorOptions, ParamListInfo boundParams);

/* Utility functions */
static void handle_modification_query(Query *parse);
static void append_child_relation(PlannerInfo *root, RelOptInfo *rel, Index rti,
				RangeTblEntry *rte, int index, Oid childOID, List *wrappers);
static Node *wrapper_make_expression(WrapperNode *wrap, int index, bool *alwaysTrue);
static void disable_inheritance(Query *parse);
bool inheritance_disabled;

/* Expression tree handlers */
static WrapperNode *walk_expr_tree(Expr *expr, const PartRelationInfo *prel);
static int make_hash(const PartRelationInfo *prel, int value);
static void handle_binary_opexpr(const PartRelationInfo *prel, WrapperNode *result, const Var *v, const Const *c);
static WrapperNode *handle_opexpr(const OpExpr *expr, const PartRelationInfo *prel);
static WrapperNode *handle_boolexpr(const BoolExpr *expr, const PartRelationInfo *prel);
static WrapperNode *handle_arrexpr(const ScalarArrayOpExpr *expr, const PartRelationInfo *prel);
static void change_varnos_in_restrinct_info(RestrictInfo *rinfo, change_varno_context *context);
static void change_varnos(Node *node, Oid old_varno, Oid new_varno);
static bool change_varno_walker(Node *node, change_varno_context *context);

/* copied from allpaths.h */
static void set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
static void set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
static List *accumulate_append_subpath(List *subpaths, Path *path);
static void set_pathkeys(PlannerInfo *root, RelOptInfo *childrel, Path *path);


/*
 * Compare two Datums with the given comarison function
 *
 * flinfo is a pointer to an instance of FmgrInfo
 * arg1, arg2 are Datum instances
 */
#define check_lt(flinfo, arg1, arg2) \
	((int) FunctionCall2(cmp_func, arg1, arg2) < 0)
#define check_le(flinfo, arg1, arg2) \
	((int) FunctionCall2(cmp_func, arg1, arg2) <= 0)
#define check_eq(flinfo, arg1, arg2) \
	((int) FunctionCall2(cmp_func, arg1, arg2) == 0)
#define check_ge(flinfo, arg1, arg2) \
	((int) FunctionCall2(cmp_func, arg1, arg2) >= 0)
#define check_gt(flinfo, arg1, arg2) \
	((int) FunctionCall2(cmp_func, arg1, arg2) > 0)


/*
 * Entry point
 */
void
_PG_init(void)
{
#ifndef WIN32
	if (IsUnderPostmaster)
	{
		elog(ERROR, "Pathman module must be initialized in postmaster. "
					"Put the following line to configuration file: "
					"shared_preload_libraries='pg_pathman'");
	        initialization_needed = false;
	}
#endif

	/* Request additional shared resources */
	RequestAddinShmemSpace(pathman_memsize());
	RequestAddinLWLocks(3);

	set_rel_pathlist_hook_original = set_rel_pathlist_hook;
	set_rel_pathlist_hook = pathman_set_rel_pathlist_hook;
	shmem_startup_hook_original = shmem_startup_hook;
	shmem_startup_hook = pathman_shmem_startup;
	post_parse_analyze_hook_original = post_parse_analyze_hook;
	post_parse_analyze_hook = pathman_post_parse_analysis_hook;
	planner_hook_original = planner_hook;
	planner_hook = pathman_planner_hook;
}

void
_PG_fini(void)
{
	set_rel_pathlist_hook = set_rel_pathlist_hook_original;
	shmem_startup_hook = shmem_startup_hook_original;
	post_parse_analyze_hook = post_parse_analyze_hook_original;
	planner_hook = planner_hook_original;
}

PartRelationInfo *
get_pathman_relation_info(Oid relid, bool *found)
{
	RelationKey key;

	key.dbid = MyDatabaseId;
	key.relid = relid;
	return hash_search(relations, (const void *) &key, HASH_FIND, found);
}

RangeRelation *
get_pathman_range_relation(Oid relid, bool *found)
{
	RelationKey key;

	key.dbid = MyDatabaseId;
	key.relid = relid;
	return hash_search(range_restrictions, (const void *) &key, HASH_FIND, found);
}

FmgrInfo *
get_cmp_func(Oid type1, Oid type2)
{
	FmgrInfo   *cmp_func;
	Oid			cmp_proc_oid;
	TypeCacheEntry	*tce;

	cmp_func = palloc(sizeof(FmgrInfo));
	tce = lookup_type_cache(type1,
							TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
							TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO);
	cmp_proc_oid = get_opfamily_proc(tce->btree_opf,
									 type1,
									 type2,
									 BTORDER_PROC);
	fmgr_info(cmp_proc_oid, cmp_func);
	return cmp_func;
}

/*
 * Post parse analysis hook. It makes sure the config is loaded before executing
 * any statement, including utility commands
 */
void
pathman_post_parse_analysis_hook(ParseState *pstate, Query *query)
{
	if (initialization_needed)
		load_config();

	if (post_parse_analyze_hook_original)
		post_parse_analyze_hook_original(pstate, query);
}

/*
 * Planner hook. It disables inheritance for tables that have been partitioned
 * by pathman to prevent standart PostgreSQL partitioning mechanism from
 * handling that tables.
 */
PlannedStmt *
pathman_planner_hook(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt	  *result;
	ListCell	  *lc;

	inheritance_disabled = false;
	switch(parse->commandType)
	{
		case CMD_SELECT:
			disable_inheritance(parse);
			break;
		case CMD_UPDATE:
		case CMD_DELETE:
			handle_modification_query(parse);
			break;
		default:
			break;
	}

	/* If query contains CTE (WITH statement) then handle subqueries too */
	foreach(lc, parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr*) lfirst(lc);

		if (IsA(cte->ctequery, Query))
			disable_inheritance((Query *)cte->ctequery);
	}

	/* Invoke original hook */
	if (planner_hook_original)
		result = planner_hook_original(parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

	return result;
}

/*
 * Disables inheritance for partitioned by pathman relations. It must be done to
 * prevent PostgresSQL from full search.
 */
static void
disable_inheritance(Query *parse)
{
	RangeTblEntry *rte;
	ListCell	  *lc;
	PartRelationInfo *prel;
	bool found;

	foreach(lc, parse->rtable)
	{
		rte = (RangeTblEntry*) lfirst(lc);
		switch(rte->rtekind)
		{
			case RTE_RELATION:
				if (rte->inh)
				{
					/* Look up this relation in pathman relations */
					prel = get_pathman_relation_info(rte->relid, &found);
					if (prel != NULL && found)
					{
						rte->inh = false;
						/*
						 * Sometimes user uses the ONLY statement and in this case
						 * rte->inh is also false. We should differ the case
						 * when user uses ONLY statement from case when we
						 * make rte->inh false intentionally.
						 */
						inheritance_disabled = true;
					}
				}
				break;
			case RTE_SUBQUERY:
				/* Recursively disable inheritance for subqueries */
				disable_inheritance(rte->subquery);
				break;
			default:
				break;
		}
	}
}

/*
 * Checks if query is affects only one partition. If true then substitute
 */
static void
handle_modification_query(Query *parse)
{
	PartRelationInfo *prel;
	List	   *ranges,
			   *wrappers = NIL;
	RangeTblEntry *rte;
	WrapperNode *wrap;
	bool found;

	Assert(parse->commandType == CMD_UPDATE ||
		   parse->commandType == CMD_DELETE);
	Assert(parse->resultRelation > 0);

	rte = rt_fetch(parse->resultRelation, parse->rtable);
	prel = get_pathman_relation_info(rte->relid, &found);

	if (!found)
		return;

	/* Parse syntax tree and extract partition ranges */
	ranges = list_make1_int(make_irange(0, prel->children_count - 1, false));
	wrap = walk_expr_tree((Expr *) eval_const_expressions(NULL, parse->jointree->quals), prel);
	wrappers = lappend(wrappers, wrap);
	ranges = irange_list_intersect(ranges, wrap->rangeset);

	/* If only one partition is affected then substitute parent table with partition */
	if (irange_list_length(ranges) == 1)
	{
		IndexRange irange = (IndexRange) linitial_oid(ranges);
		if (irange_lower(irange) == irange_upper(irange))
		{
			Oid *children = (Oid *) dsm_array_get_pointer(&prel->children);
			rte->relid = children[irange_lower(irange)];
			rte->inh = false;
		}
	}

	return;
}

/*
 * Shared memory startup hook
 */
static void
pathman_shmem_startup(void)
{
	/* Allocate shared memory objects */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	init_dsm_config();
	init_shmem_config();
	LWLockRelease(AddinShmemInitLock);

	/* Invoke original hook if needed */
	if (shmem_startup_hook_original != NULL)
		shmem_startup_hook_original();
}

/*
 * Main hook. All the magic goes here
 */
void
pathman_set_rel_pathlist_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	PartRelationInfo *prel = NULL;
	RelOptInfo **new_rel_array;
	RangeTblEntry **new_rte_array;
	int len;
	bool found;

	/* This works only for SELECT queries */
	if (root->parse->commandType != CMD_SELECT || !inheritance_disabled)
		return;

	/* Lookup partitioning information for parent relation */
	prel = get_pathman_relation_info(rte->relid, &found);

	if (prel != NULL && found)
	{
		ListCell   *lc;
		int			i;
		Oid		   *dsm_arr;
		List	   *ranges,
				   *wrappers;

		rte->inh = true;
		dsm_arr = (Oid *) dsm_array_get_pointer(&prel->children);
		ranges = list_make1_int(make_irange(0, prel->children_count - 1, false));

		/* Make wrappers over restrictions and collect final rangeset */
		wrappers = NIL;
		foreach(lc, rel->baserestrictinfo)
		{
			WrapperNode *wrap;

			RestrictInfo *rinfo = (RestrictInfo*) lfirst(lc);

			wrap = walk_expr_tree(rinfo->clause, prel);
			wrappers = lappend(wrappers, wrap);
			ranges = irange_list_intersect(ranges, wrap->rangeset);
		}

		/*
		 * Expand simple_rte_array and simple_rel_array
		 */

		 if (ranges)
		 {
			len = irange_list_length(ranges);

			/* Expand simple_rel_array and simple_rte_array */
			new_rel_array = (RelOptInfo **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RelOptInfo *));

			/* simple_rte_array is an array equivalent of the rtable list */
			new_rte_array = (RangeTblEntry **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RangeTblEntry *));

			/* Copy relations to the new arrays */
	        for (i = 0; i < root->simple_rel_array_size; i++)
	        {
	                new_rel_array[i] = root->simple_rel_array[i];
	                new_rte_array[i] = root->simple_rte_array[i];
	        }

			/* Free old arrays */
			pfree(root->simple_rel_array);
			pfree(root->simple_rte_array);

			root->simple_rel_array_size += len;
			root->simple_rel_array = new_rel_array;
			root->simple_rte_array = new_rte_array;
		}

		/*
		 * Iterate all indexes in rangeset and append corresponding child
		 * relations.
		 */
		foreach(lc, ranges)
		{
			IndexRange	irange = lfirst_irange(lc);
			Oid			childOid;

			for (i = irange_lower(irange); i <= irange_upper(irange); i++)
			{
				childOid = dsm_arr[i];
				append_child_relation(root, rel, rti, rte, i, childOid, wrappers);
			}
		}

		/* Clear old path list */
		list_free(rel->pathlist);
		rel->pathlist = NIL;
		set_append_rel_pathlist(root, rel, rti, rte);
	}

	/* Invoke original hook if needed */
	if (set_rel_pathlist_hook_original != NULL)
	{
		set_rel_pathlist_hook_original(root, rel, rti, rte);
	}
}

static void
append_child_relation(PlannerInfo *root, RelOptInfo *rel, Index rti,
	RangeTblEntry *rte, int index, Oid childOid, List *wrappers)
{
	RangeTblEntry *childrte;
	RelOptInfo    *childrel;
	Index		childRTindex;
	AppendRelInfo *appinfo;
	Node *node;
	ListCell *lc, *lc2;
	Relation	newrelation;

	newrelation = heap_open(childOid, NoLock);

	/*
	 * Create RangeTblEntry for child relation.
	 * This code partially based on expand_inherited_rtentry() function.
	 */
	childrte = copyObject(rte);
	childrte->relid = childOid;
	childrte->relkind = newrelation->rd_rel->relkind;
	childrte->inh = false;
	childrte->requiredPerms = 0;
	root->parse->rtable = lappend(root->parse->rtable, childrte);
	childRTindex = list_length(root->parse->rtable);
	root->simple_rte_array[childRTindex] = childrte;

	/* Create RelOptInfo */
	childrel = build_simple_rel(root, childRTindex, RELOPT_OTHER_MEMBER_REL);

	/* Copy targetlist */
	childrel->reltargetlist = NIL;
	foreach(lc, rel->reltargetlist)
	{
		Node *new_target;

		node = (Node *) lfirst(lc);
		new_target = copyObject(node);
		change_varnos(new_target, rel->relid, childrel->relid);
		childrel->reltargetlist = lappend(childrel->reltargetlist, new_target);
	}

	/* Copy restrictions */
	childrel->baserestrictinfo = NIL;
	forboth(lc, wrappers, lc2, rel->baserestrictinfo)
	{
		bool alwaysTrue;
		WrapperNode *wrap = (WrapperNode *) lfirst(lc);
		Node *new_clause = wrapper_make_expression(wrap, index, &alwaysTrue);
		RestrictInfo *old_rinfo = (RestrictInfo *) lfirst(lc2),
					 *new_rinfo;

		if (alwaysTrue)
		{
			continue;
		}
		Assert(new_clause);

		new_rinfo = make_restrictinfo((Expr *) new_clause,
									  old_rinfo->is_pushed_down,
									  old_rinfo->outerjoin_delayed,
									  old_rinfo->pseudoconstant,
									  old_rinfo->required_relids,
									  old_rinfo->outer_relids,
									  old_rinfo->nullable_relids);

		/* Replace old relids with new ones */
		change_varnos((Node *)new_rinfo, rel->relid, childrel->relid);

		childrel->baserestrictinfo = lappend(childrel->baserestrictinfo,
											 (void *) new_rinfo);
	}

	/* Build an AppendRelInfo for this parent and child */
	appinfo = makeNode(AppendRelInfo);
	appinfo->parent_relid = rti;
	appinfo->child_relid = childRTindex;
	appinfo->parent_reloid = rte->relid;
	root->append_rel_list = lappend(root->append_rel_list, appinfo);
	root->total_table_pages += (double) childrel->pages;

	heap_close(newrelation, NoLock);
}

/* Convert wrapper into expression for given index */
static Node *
wrapper_make_expression(WrapperNode *wrap, int index, bool *alwaysTrue)
{
	bool	lossy, found;

	*alwaysTrue = false;
	/*
	 * TODO: use faster algorithm using knowledge that we enumerate indexes
	 * sequntially.
	 */
	found = irange_list_find(wrap->rangeset, index, &lossy);
	/* Return NULL for always true and always false. */
	if (!found)
		return NULL;
	if (!lossy)
	{
		*alwaysTrue = true;
		return NULL;
	}

	if (IsA(wrap->orig, BoolExpr))
	{
		const BoolExpr *expr = (const BoolExpr *) wrap->orig;
		BoolExpr *result;

		if (expr->boolop == OR_EXPR || expr->boolop == AND_EXPR)
		{
			ListCell *lc;
			List *args = NIL;

			foreach (lc, wrap->args)
			{
				Node   *arg;
				bool	childAlwaysTrue;

				arg = wrapper_make_expression((WrapperNode *)lfirst(lc), index, &childAlwaysTrue);
#ifdef USE_ASSERT_CHECKING
				/*
				 * We shouldn't get there for always true clause under OR and
				 * always false clause under AND.
				 */
				if (expr->boolop == OR_EXPR)
					Assert(!childAlwaysTrue);
				if (expr->boolop == AND_EXPR)
					Assert(arg || childAlwaysTrue);
#endif
				if (arg)
					args = lappend(args, arg);
			}

			Assert(list_length(args) >= 1);

			/* Remove redundant OR/AND when child is single. */
			if (list_length(args) == 1)
				return (Node *) linitial(args);

			result = makeNode(BoolExpr);
			result->xpr.type = T_BoolExpr;
			result->args = args;
			result->boolop = expr->boolop;
			result->location = expr->location;
			return (Node *)result;
		}
		else
		{
			return copyObject(wrap->orig);
		}
	}
	else
	{
		return copyObject(wrap->orig);
	}
}

/*
 * Changes varno attribute in RestrictInfo objects
 */
static void
change_varnos(Node *node, Oid old_varno, Oid new_varno)
{
	change_varno_context context;
	context.old_varno = old_varno;
	context.new_varno = new_varno;

	change_varno_walker(node, &context);
}

static bool
change_varno_walker(Node *node, change_varno_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->old_varno)
			var->varno = context->new_varno;
		return false;
	}
	if (IsA(node, RestrictInfo))
	{
		change_varnos_in_restrinct_info((RestrictInfo *) node, context);
		return false;
	}
	if (IsA(node, List))
	{
		ListCell *lc;

		foreach(lc, (List *) node)
			change_varno_walker((Node *) lfirst(lc), context);
		return false;
	}

	/* Should not find an unplanned subquery */
	Assert(!IsA(node, Query));

	return expression_tree_walker(node, change_varno_walker, (void *) context);
}

static void
change_varnos_in_restrinct_info(RestrictInfo *rinfo, change_varno_context *context)
{
	ListCell *lc;

	change_varno_walker((Node *) rinfo->clause, context);
	if (rinfo->left_em)
	{
		change_varno_walker((Node *) rinfo->left_em->em_expr, context);
	}
	if (rinfo->right_em)
	{
		change_varno_walker((Node *) rinfo->right_em->em_expr, context);
	}
	if (rinfo->orclause)
		foreach(lc, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node *node = (Node *) lfirst(lc);
			change_varno_walker(node, context);
		}

	/* TODO: find some elegant way to do this */
	if (bms_is_member(context->old_varno, rinfo->clause_relids))
	{
		rinfo->clause_relids = bms_del_member(rinfo->clause_relids, context->old_varno);
		rinfo->clause_relids = bms_add_member(rinfo->clause_relids, context->new_varno);
	}
	if (bms_is_member(context->old_varno, rinfo->left_relids))
	{
		rinfo->left_relids = bms_del_member(rinfo->left_relids, context->old_varno);
		rinfo->left_relids = bms_add_member(rinfo->left_relids, context->new_varno);
	}
	if (bms_is_member(context->old_varno, rinfo->right_relids))
	{
		rinfo->right_relids = bms_del_member(rinfo->right_relids, context->old_varno);
		rinfo->right_relids = bms_add_member(rinfo->right_relids, context->new_varno);
	}
}

/*
 * Recursive function to walk through conditions tree
 */
static WrapperNode *
walk_expr_tree(Expr *expr, const PartRelationInfo *prel)
{
	BoolExpr		   *boolexpr;
	OpExpr			   *opexpr;
	ScalarArrayOpExpr  *arrexpr;
	WrapperNode		   *result;

	switch (expr->type)
	{
		/* AND, OR, NOT expressions */
		case T_BoolExpr:
			boolexpr = (BoolExpr *) expr;
			return handle_boolexpr(boolexpr, prel);
		/* =, !=, <, > etc. */
		case T_OpExpr:
			opexpr = (OpExpr *) expr;
			return handle_opexpr(opexpr, prel);
		/* IN expression */
		case T_ScalarArrayOpExpr:
			arrexpr = (ScalarArrayOpExpr *) expr;
			return handle_arrexpr(arrexpr, prel);
		default:
			result = (WrapperNode *)palloc(sizeof(WrapperNode));
			result->orig = (const Node *)expr;
			result->args = NIL;
			result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, true));
			return result;
	}
}

/*
 *	This function determines which partitions should appear in query plan
 */
static void
handle_binary_opexpr(const PartRelationInfo *prel, WrapperNode *result,
					 const Var *v, const Const *c)
{
	HashRelationKey		key;
	RangeRelation	   *rangerel;
	Datum				value;
	int					i,
						int_value,
						strategy;
	bool				is_less,
						is_greater;
	FmgrInfo		    cmp_func;
	Oid					cmp_proc_oid;
	const OpExpr	   *expr = (const OpExpr *)result->orig;
	TypeCacheEntry	   *tce;

	/* Determine operator type */
	tce = lookup_type_cache(v->vartype,
		TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR | TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO);
	strategy = get_op_opfamily_strategy(expr->opno, tce->btree_opf);
	cmp_proc_oid = get_opfamily_proc(tce->btree_opf,
									 c->consttype,
									 prel->atttype,
									 BTORDER_PROC);
	fmgr_info(cmp_proc_oid, &cmp_func);

	switch (prel->parttype)
	{
		case PT_HASH:
			if (strategy == BTEqualStrategyNumber)
			{
				int_value = DatumGetInt32(c->constvalue);
				key.hash = make_hash(prel, int_value);
				result->rangeset = list_make1_irange(make_irange(key.hash, key.hash, true));
				return;
			}
		case PT_RANGE:
			value = c->constvalue;
			rangerel = get_pathman_range_relation(prel->key.relid, NULL);
			if (rangerel != NULL)
			{
				RangeEntry *re;
				bool		lossy = false;
#ifdef USE_ASSERT_CHECKING
				bool		found = false;
				int			counter = 0;
#endif
				int			startidx = 0,
							cmp_min,
							cmp_max,
							endidx = rangerel->ranges.length - 1;
				RangeEntry *ranges = dsm_array_get_pointer(&rangerel->ranges);
				bool byVal = rangerel->by_val;

				/* Check boundaries */
				if (rangerel->ranges.length == 0)
				{
					result->rangeset = NIL;
					return;
				}
				else
				{
					/* Corner cases */
					cmp_min = FunctionCall2(&cmp_func, value,
											PATHMAN_GET_DATUM(ranges[0].min, byVal)),
					cmp_max = FunctionCall2(&cmp_func, value,
											PATHMAN_GET_DATUM(ranges[rangerel->ranges.length - 1].max, byVal));

					if ((cmp_min < 0 &&
						 (strategy == BTLessEqualStrategyNumber ||
						  strategy == BTEqualStrategyNumber)) || 
						(cmp_min <= 0 && strategy == BTLessStrategyNumber))
					{
						result->rangeset = NIL;
						return;
					}

					if (cmp_max >= 0 && (strategy == BTGreaterEqualStrategyNumber || 
						strategy == BTGreaterStrategyNumber ||
						strategy == BTEqualStrategyNumber))
					{
						result->rangeset = NIL;
						return;
					}

					if ((cmp_min < 0 && strategy == BTGreaterStrategyNumber) || 
						(cmp_min <= 0 && strategy == BTGreaterEqualStrategyNumber))
					{
						result->rangeset = list_make1_irange(make_irange(startidx, endidx, false));
						return;
					}

					if (cmp_max >= 0 && (strategy == BTLessEqualStrategyNumber || 
						strategy == BTLessStrategyNumber))
					{
						result->rangeset = list_make1_irange(make_irange(startidx, endidx, false));
						return;
					}
				}

				/* Binary search */
				while (true)
				{
					i = startidx + (endidx - startidx) / 2;
					Assert(i >= 0 && i < rangerel->ranges.length);
					re = &ranges[i];
					cmp_min = FunctionCall2(&cmp_func, value, PATHMAN_GET_DATUM(re->min, byVal));
					cmp_max = FunctionCall2(&cmp_func, value, PATHMAN_GET_DATUM(re->max, byVal));

					is_less = (cmp_min < 0 || (cmp_min == 0 && strategy == BTLessStrategyNumber));
					is_greater = (cmp_max > 0 || (cmp_max >= 0 && strategy != BTLessStrategyNumber));

					if (!is_less && !is_greater)
					{
						if (strategy == BTGreaterEqualStrategyNumber && cmp_min == 0)
							lossy = false;
						else if (strategy == BTLessStrategyNumber && cmp_max == 0)
							lossy = false;
						else
							lossy = true;
#ifdef USE_ASSERT_CHECKING
						found = true;
#endif
						break;
					}

					/* If we still didn't find partition then it doesn't exist */
					if (startidx >= endidx)
					{
						result->rangeset = NIL;
						return;
					}

					if (is_less)
						endidx = i - 1;
					else if (is_greater)
						startidx = i + 1;

					/* For debug's sake */
					Assert(++counter < 100);
				}

				Assert(found);

				/* Filter partitions */
				switch(strategy)
				{
					case BTLessStrategyNumber:
					case BTLessEqualStrategyNumber:
						if (lossy)
						{
							result->rangeset = list_make1_irange(make_irange(i, i, true));
							if (i > 0)
								result->rangeset = lcons_irange(
									make_irange(0, i - 1, false), result->rangeset);
						}
						else
						{
							result->rangeset = list_make1_irange(
								make_irange(0, i, false));
						}
						return;
					case BTEqualStrategyNumber:
						result->rangeset = list_make1_irange(make_irange(i, i, true));
						return;
					case BTGreaterEqualStrategyNumber:
					case BTGreaterStrategyNumber:
						if (lossy)
						{
							result->rangeset = list_make1_irange(make_irange(i, i, true));
							if (i < prel->children_count - 1)
								result->rangeset = lappend_irange(result->rangeset,
									make_irange(i + 1, prel->children_count - 1, false));
						}
						else
						{
							result->rangeset = list_make1_irange(
								make_irange(i, prel->children_count - 1, false));
						}
						return;
				}
				result->rangeset = list_make1_irange(make_irange(startidx, endidx, true));
				return;
			}
	}

	result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, true));
}

/*
 * Calculates hash value
 */
static int
make_hash(const PartRelationInfo *prel, int value)
{
	return value % prel->children_count;
}

/*
 * Search for range section. Returns position of the item in array.
 * If item wasn't found then function returns closest position and sets
 * foundPtr to false. If value is outside the range covered by partitions
 * then returns -1.
 */
int
range_binary_search(const RangeRelation *rangerel, FmgrInfo *cmp_func, Datum value, bool *foundPtr)
{
	RangeEntry *ranges = dsm_array_get_pointer(&rangerel->ranges);
	RangeEntry *re;
	bool		byVal = rangerel->by_val;
	int			cmp_min,
				cmp_max,
				i = 0,
				startidx = 0,
				endidx = rangerel->ranges.length-1;
#ifdef USE_ASSERT_CHECKING
	int			counter = 0;
#endif

	*foundPtr = false;

	/* Check boundaries */
	cmp_min = FunctionCall2(cmp_func, value, PATHMAN_GET_DATUM(ranges[0].min, byVal)),
	cmp_max = FunctionCall2(cmp_func, value, PATHMAN_GET_DATUM(ranges[rangerel->ranges.length - 1].max, byVal));

	if (cmp_min < 0 || cmp_max >= 0)
	{
		return -1;
	}

	while (true)
	{
		i = startidx + (endidx - startidx) / 2;
		Assert(i >= 0 && i < rangerel->ranges.length);
		re = &ranges[i];
		cmp_min = FunctionCall2(cmp_func, value, PATHMAN_GET_DATUM(re->min, byVal));
		cmp_max = FunctionCall2(cmp_func, value, PATHMAN_GET_DATUM(re->max, byVal));

		if (cmp_min >= 0 && cmp_max < 0)
		{
			*foundPtr = true;
			break;
		}

		if (startidx >= endidx)
			return i;

		if (cmp_min < 0)
			endidx = i - 1;
		else if (cmp_max >= 0)
			startidx = i + 1;

		/* For debug's sake */
		Assert(++counter < 100);
	}

	return i;
}

/*
 * Operator expression handler
 */
static WrapperNode *
handle_opexpr(const OpExpr *expr, const PartRelationInfo *prel)
{
	WrapperNode	*result = (WrapperNode *)palloc(sizeof(WrapperNode));
	Node		*firstarg = NULL,
				*secondarg = NULL;

	result->orig = (const Node *)expr;
	result->args = NIL;

	if (list_length(expr->args) == 2)
	{
		firstarg = (Node *) linitial(expr->args);
		secondarg = (Node *) lsecond(expr->args);

		if (IsA(firstarg, Var) && IsA(secondarg, Const) &&
			((Var *)firstarg)->varattno == prel->attnum)
		{
			handle_binary_opexpr(prel, result, (Var *)firstarg, (Const *)secondarg);
			return result;
		}
		else if (IsA(secondarg, Var) && IsA(firstarg, Const) &&
				 ((Var *)secondarg)->varattno == prel->attnum)
		{
			handle_binary_opexpr(prel, result, (Var *)secondarg, (Const *)firstarg);
			return result;
		}
	}

	result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, true));
	return result;
}

/*
 * Boolean expression handler
 */
static WrapperNode *
handle_boolexpr(const BoolExpr *expr, const PartRelationInfo *prel)
{
	WrapperNode	*result = (WrapperNode *)palloc(sizeof(WrapperNode));
	ListCell	*lc;

	result->orig = (const Node *)expr;
	result->args = NIL;

	if (expr->boolop == AND_EXPR)
		result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, false));
	else
		result->rangeset = NIL;

	foreach (lc, expr->args)
	{
		WrapperNode *arg;

		arg = walk_expr_tree((Expr *)lfirst(lc), prel);
		result->args = lappend(result->args, arg);
		switch(expr->boolop)
		{
			case OR_EXPR:
				result->rangeset = irange_list_union(result->rangeset, arg->rangeset);
				break;
			case AND_EXPR:
				result->rangeset = irange_list_intersect(result->rangeset, arg->rangeset);
				break;
			default:
				result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, false));
				break;
		}
	}

	return result;
}

/*
 * Scalar array expression
 */
static WrapperNode *
handle_arrexpr(const ScalarArrayOpExpr *expr, const PartRelationInfo *prel)
{
	WrapperNode *result = (WrapperNode *)palloc(sizeof(WrapperNode));
	Node		*varnode = (Node *) linitial(expr->args);
	Node		*arraynode = (Node *) lsecond(expr->args);
	int			 hash;

	result->orig = (const Node *)expr;
	result->args = NIL;

	if (varnode == NULL || !IsA(varnode, Var))
	{
		result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, true));
		return result;
	}

	if (arraynode && IsA(arraynode, Const) &&
		!((Const *) arraynode)->constisnull)
	{
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			i;

		/* Extract values from array */
		arrayval = DatumGetArrayTypeP(((Const *) arraynode)->constvalue);
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		result->rangeset = NIL;

		/* Construct OIDs list */
		for (i = 0; i < num_elems; i++)
		{
			hash = make_hash(prel, elem_values[i]);
			result->rangeset = irange_list_union(result->rangeset,
						list_make1_irange(make_irange(hash, hash, true)));
		}

		/* Free resources */
		pfree(elem_values);
		pfree(elem_nulls);

		return result;
	}

	result->rangeset = list_make1_irange(make_irange(0, prel->children_count - 1, true));
	return result;
}

/*
 * Theres are functions below copied from allpaths.c with (or without) some
 * modifications. Couldn't use original because of 'static' modifier.
 */

/*
 * set_plain_rel_pathlist
 *	  Build access paths for a plain relation (no subquery, no inheritance)
 */
static void
set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;
	Path *path;

	/*
	 * We don't support pushing join clauses into the quals of a seqscan, but
	 * it could still have required parameterization due to LATERAL refs in
	 * its tlist.
	 */
	required_outer = rel->lateral_relids;

	/* Consider sequential scan */
#if PG_VERSION_NUM >= 90600
	path = create_seqscan_path(root, rel, required_outer, 0);
#else
	path = create_seqscan_path(root, rel, required_outer);
#endif
	add_path(rel, path);
	set_pathkeys(root, rel, path);

	/* Consider index scans */
	create_index_paths(root, rel);

	/* Consider TID scans */
	create_tidscan_paths(root, rel);
}

/*
 * set_foreign_size
 *		Set size estimates for a foreign table RTE
 */
static void
set_foreign_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Mark rel with estimated output rows, width, etc */
	set_foreign_size_estimates(root, rel);

	/* Let FDW adjust the size estimates, if it can */
	rel->fdwroutine->GetForeignRelSize(root, rel, rte->relid);

	/* ... but do not let it set the rows estimate to zero */
	rel->rows = clamp_row_est(rel->rows);
}

/*
 * set_foreign_pathlist
 *		Build access paths for a foreign table RTE
 */
static void
set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Call the FDW's GetForeignPaths function to generate path(s) */
	rel->fdwroutine->GetForeignPaths(root, rel, rte->relid);
}

/*
 * set_append_rel_pathlist
 *	  Build access paths for an "append relation"
 */
static void
set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	int			parentRTindex = rti;
	List	   *live_childrels = NIL;
	List	   *subpaths = NIL;
	bool		subpaths_valid = true;
	ListCell   *l;

	/*
	 * Generate access paths for each member relation, and remember the
	 * cheapest path for each one.  Also, identify all pathkeys (orderings)
	 * and parameterizations (required_outer sets) available for the member
	 * relations.
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/* Re-locate the child RTE and RelOptInfo */
		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];
		childrel = root->simple_rel_array[childRTindex];

		/*
		 * Compute the child's access paths.
		 */
		if (childRTE->relkind == RELKIND_FOREIGN_TABLE)
		{
			set_foreign_size(root, childrel, childRTE);
			set_foreign_pathlist(root, childrel, childRTE);
		}
		else
		{
			set_plain_rel_pathlist(root, childrel, childRTE);
		}
		set_cheapest(childrel);

		/*
		 * If child is dummy, ignore it.
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/*
		 * Child is live, so add it to the live_childrels list for use below.
		 */
		live_childrels = lappend(live_childrels, childrel);

		/*
		 * If child has an unparameterized cheapest-total path, add that to
		 * the unparameterized Append path we are constructing for the parent.
		 * If not, there's no workable unparameterized path.
		 */
		if (childrel->cheapest_total_path->param_info == NULL)
			subpaths = accumulate_append_subpath(subpaths,
											  childrel->cheapest_total_path);
		else
			subpaths_valid = false;
	}

	/*
	 * If we found unparameterized paths for all children, build an unordered,
	 * unparameterized Append path for the rel.  (Note: this is correct even
	 * if we have zero or one live subpath due to constraint exclusion.)
	 */
	if (subpaths_valid)
		add_path(rel, (Path *) create_append_path(rel, subpaths, NULL));

}

void
set_pathkeys(PlannerInfo *root, RelOptInfo *childrel, Path *path)
{
	ListCell *lc;
	PathKey *pathkey;

	foreach (lc, root->sort_pathkeys)
	{
		pathkey = (PathKey *) lfirst(lc);
		path->pathkeys = lappend(path->pathkeys, pathkey);
	}
}

static List *
accumulate_append_subpath(List *subpaths, Path *path)
{
	return lappend(subpaths, path);
}

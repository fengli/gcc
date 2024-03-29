/* Global, SSA-based optimizations using mathematical identities.
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010
   Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

GCC is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* Currently, the only mini-pass in this file tries to CSE reciprocal
   operations.  These are common in sequences such as this one:

	modulus = sqrt(x*x + y*y + z*z);
	x = x / modulus;
	y = y / modulus;
	z = z / modulus;

   that can be optimized to

	modulus = sqrt(x*x + y*y + z*z);
        rmodulus = 1.0 / modulus;
	x = x * rmodulus;
	y = y * rmodulus;
	z = z * rmodulus;

   We do this for loop invariant divisors, and with this pass whenever
   we notice that a division has the same divisor multiple times.

   Of course, like in PRE, we don't insert a division if a dominator
   already has one.  However, this cannot be done as an extension of
   PRE for several reasons.

   First of all, with some experiments it was found out that the
   transformation is not always useful if there are only two divisions
   hy the same divisor.  This is probably because modern processors
   can pipeline the divisions; on older, in-order processors it should
   still be effective to optimize two divisions by the same number.
   We make this a param, and it shall be called N in the remainder of
   this comment.

   Second, if trapping math is active, we have less freedom on where
   to insert divisions: we can only do so in basic blocks that already
   contain one.  (If divisions don't trap, instead, we can insert
   divisions elsewhere, which will be in blocks that are common dominators
   of those that have the division).

   We really don't want to compute the reciprocal unless a division will
   be found.  To do this, we won't insert the division in a basic block
   that has less than N divisions *post-dominating* it.

   The algorithm constructs a subset of the dominator tree, holding the
   blocks containing the divisions and the common dominators to them,
   and walk it twice.  The first walk is in post-order, and it annotates
   each block with the number of divisions that post-dominate it: this
   gives information on where divisions can be inserted profitably.
   The second walk is in pre-order, and it inserts divisions as explained
   above, and replaces divisions by multiplications.

   In the best case, the cost of the pass is O(n_statements).  In the
   worst-case, the cost is due to creating the dominator tree subset,
   with a cost of O(n_basic_blocks ^ 2); however this can only happen
   for n_statements / n_basic_blocks statements.  So, the amortized cost
   of creating the dominator tree subset is O(n_basic_blocks) and the
   worst-case cost of the pass is O(n_statements * n_basic_blocks).

   More practically, the cost will be small because there are few
   divisions, and they tend to be in the same basic block, so insert_bb
   is called very few times.

   If we did this using domwalk.c, an efficient implementation would have
   to work on all the variables in a single pass, because we could not
   work on just a subset of the dominator tree, as we do now, and the
   cost would also be something like O(n_statements * n_basic_blocks).
   The data structures would be more complex in order to work on all the
   variables in a single pass.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "flags.h"
#include "tree.h"
#include "tree-flow.h"
#include "timevar.h"
#include "tree-pass.h"
#include "alloc-pool.h"
#include "basic-block.h"
#include "target.h"
#include "gimple-pretty-print.h"

/* FIXME: RTL headers have to be included here for optabs.  */
#include "rtl.h"		/* Because optabs.h wants enum rtx_code.  */
#include "expr.h"		/* Because optabs.h wants sepops.  */
#include "optabs.h"

/* This structure represents one basic block that either computes a
   division, or is a common dominator for basic block that compute a
   division.  */
struct occurrence {
  /* The basic block represented by this structure.  */
  basic_block bb;

  /* If non-NULL, the SSA_NAME holding the definition for a reciprocal
     inserted in BB.  */
  tree recip_def;

  /* If non-NULL, the GIMPLE_ASSIGN for a reciprocal computation that
     was inserted in BB.  */
  gimple recip_def_stmt;

  /* Pointer to a list of "struct occurrence"s for blocks dominated
     by BB.  */
  struct occurrence *children;

  /* Pointer to the next "struct occurrence"s in the list of blocks
     sharing a common dominator.  */
  struct occurrence *next;

  /* The number of divisions that are in BB before compute_merit.  The
     number of divisions that are in BB or post-dominate it after
     compute_merit.  */
  int num_divisions;

  /* True if the basic block has a division, false if it is a common
     dominator for basic blocks that do.  If it is false and trapping
     math is active, BB is not a candidate for inserting a reciprocal.  */
  bool bb_has_division;
};

static struct
{
  /* Number of 1.0/X ops inserted.  */
  int rdivs_inserted;

  /* Number of 1.0/FUNC ops inserted.  */
  int rfuncs_inserted;
} reciprocal_stats;

static struct
{
  /* Number of cexpi calls inserted.  */
  int inserted;
} sincos_stats;

static struct
{
  /* Number of hand-written 32-bit bswaps found.  */
  int found_32bit;

  /* Number of hand-written 64-bit bswaps found.  */
  int found_64bit;
} bswap_stats;

static struct
{
  /* Number of widening multiplication ops inserted.  */
  int widen_mults_inserted;

  /* Number of integer multiply-and-accumulate ops inserted.  */
  int maccs_inserted;

  /* Number of fp fused multiply-add ops inserted.  */
  int fmas_inserted;
} widen_mul_stats;

/* The instance of "struct occurrence" representing the highest
   interesting block in the dominator tree.  */
static struct occurrence *occ_head;

/* Allocation pool for getting instances of "struct occurrence".  */
static alloc_pool occ_pool;



/* Allocate and return a new struct occurrence for basic block BB, and
   whose children list is headed by CHILDREN.  */
static struct occurrence *
occ_new (basic_block bb, struct occurrence *children)
{
  struct occurrence *occ;

  bb->aux = occ = (struct occurrence *) pool_alloc (occ_pool);
  memset (occ, 0, sizeof (struct occurrence));

  occ->bb = bb;
  occ->children = children;
  return occ;
}


/* Insert NEW_OCC into our subset of the dominator tree.  P_HEAD points to a
   list of "struct occurrence"s, one per basic block, having IDOM as
   their common dominator.

   We try to insert NEW_OCC as deep as possible in the tree, and we also
   insert any other block that is a common dominator for BB and one
   block already in the tree.  */

static void
insert_bb (struct occurrence *new_occ, basic_block idom,
	   struct occurrence **p_head)
{
  struct occurrence *occ, **p_occ;

  for (p_occ = p_head; (occ = *p_occ) != NULL; )
    {
      basic_block bb = new_occ->bb, occ_bb = occ->bb;
      basic_block dom = nearest_common_dominator (CDI_DOMINATORS, occ_bb, bb);
      if (dom == bb)
	{
	  /* BB dominates OCC_BB.  OCC becomes NEW_OCC's child: remove OCC
	     from its list.  */
	  *p_occ = occ->next;
	  occ->next = new_occ->children;
	  new_occ->children = occ;

	  /* Try the next block (it may as well be dominated by BB).  */
	}

      else if (dom == occ_bb)
	{
	  /* OCC_BB dominates BB.  Tail recurse to look deeper.  */
	  insert_bb (new_occ, dom, &occ->children);
	  return;
	}

      else if (dom != idom)
	{
	  gcc_assert (!dom->aux);

	  /* There is a dominator between IDOM and BB, add it and make
	     two children out of NEW_OCC and OCC.  First, remove OCC from
	     its list.	*/
	  *p_occ = occ->next;
	  new_occ->next = occ;
	  occ->next = NULL;

	  /* None of the previous blocks has DOM as a dominator: if we tail
	     recursed, we would reexamine them uselessly. Just switch BB with
	     DOM, and go on looking for blocks dominated by DOM.  */
          new_occ = occ_new (dom, new_occ);
	}

      else
	{
	  /* Nothing special, go on with the next element.  */
	  p_occ = &occ->next;
	}
    }

  /* No place was found as a child of IDOM.  Make BB a sibling of IDOM.  */
  new_occ->next = *p_head;
  *p_head = new_occ;
}

/* Register that we found a division in BB.  */

static inline void
register_division_in (basic_block bb)
{
  struct occurrence *occ;

  occ = (struct occurrence *) bb->aux;
  if (!occ)
    {
      occ = occ_new (bb, NULL);
      insert_bb (occ, ENTRY_BLOCK_PTR, &occ_head);
    }

  occ->bb_has_division = true;
  occ->num_divisions++;
}


/* Compute the number of divisions that postdominate each block in OCC and
   its children.  */

static void
compute_merit (struct occurrence *occ)
{
  struct occurrence *occ_child;
  basic_block dom = occ->bb;

  for (occ_child = occ->children; occ_child; occ_child = occ_child->next)
    {
      basic_block bb;
      if (occ_child->children)
        compute_merit (occ_child);

      if (flag_exceptions)
	bb = single_noncomplex_succ (dom);
      else
	bb = dom;

      if (dominated_by_p (CDI_POST_DOMINATORS, bb, occ_child->bb))
        occ->num_divisions += occ_child->num_divisions;
    }
}


/* Return whether USE_STMT is a floating-point division by DEF.  */
static inline bool
is_division_by (gimple use_stmt, tree def)
{
  return is_gimple_assign (use_stmt)
	 && gimple_assign_rhs_code (use_stmt) == RDIV_EXPR
	 && gimple_assign_rhs2 (use_stmt) == def
	 /* Do not recognize x / x as valid division, as we are getting
	    confused later by replacing all immediate uses x in such
	    a stmt.  */
	 && gimple_assign_rhs1 (use_stmt) != def;
}

/* Walk the subset of the dominator tree rooted at OCC, setting the
   RECIP_DEF field to a definition of 1.0 / DEF that can be used in
   the given basic block.  The field may be left NULL, of course,
   if it is not possible or profitable to do the optimization.

   DEF_BSI is an iterator pointing at the statement defining DEF.
   If RECIP_DEF is set, a dominator already has a computation that can
   be used.  */

static void
insert_reciprocals (gimple_stmt_iterator *def_gsi, struct occurrence *occ,
		    tree def, tree recip_def, int threshold)
{
  tree type;
  gimple new_stmt;
  gimple_stmt_iterator gsi;
  struct occurrence *occ_child;

  if (!recip_def
      && (occ->bb_has_division || !flag_trapping_math)
      && occ->num_divisions >= threshold)
    {
      /* Make a variable with the replacement and substitute it.  */
      type = TREE_TYPE (def);
      recip_def = make_rename_temp (type, "reciptmp");
      new_stmt = gimple_build_assign_with_ops (RDIV_EXPR, recip_def,
					       build_one_cst (type), def);

      if (occ->bb_has_division)
        {
          /* Case 1: insert before an existing division.  */
          gsi = gsi_after_labels (occ->bb);
          while (!gsi_end_p (gsi) && !is_division_by (gsi_stmt (gsi), def))
	    gsi_next (&gsi);

          gsi_insert_before (&gsi, new_stmt, GSI_SAME_STMT);
        }
      else if (def_gsi && occ->bb == def_gsi->bb)
        {
          /* Case 2: insert right after the definition.  Note that this will
	     never happen if the definition statement can throw, because in
	     that case the sole successor of the statement's basic block will
	     dominate all the uses as well.  */
          gsi_insert_after (def_gsi, new_stmt, GSI_NEW_STMT);
        }
      else
        {
          /* Case 3: insert in a basic block not containing defs/uses.  */
          gsi = gsi_after_labels (occ->bb);
          gsi_insert_before (&gsi, new_stmt, GSI_SAME_STMT);
        }

      reciprocal_stats.rdivs_inserted++;

      occ->recip_def_stmt = new_stmt;
    }

  occ->recip_def = recip_def;
  for (occ_child = occ->children; occ_child; occ_child = occ_child->next)
    insert_reciprocals (def_gsi, occ_child, def, recip_def, threshold);
}


/* Replace the division at USE_P with a multiplication by the reciprocal, if
   possible.  */

static inline void
replace_reciprocal (use_operand_p use_p)
{
  gimple use_stmt = USE_STMT (use_p);
  basic_block bb = gimple_bb (use_stmt);
  struct occurrence *occ = (struct occurrence *) bb->aux;

  if (optimize_bb_for_speed_p (bb)
      && occ->recip_def && use_stmt != occ->recip_def_stmt)
    {
      gimple_assign_set_rhs_code (use_stmt, MULT_EXPR);
      SET_USE (use_p, occ->recip_def);
      fold_stmt_inplace (use_stmt);
      update_stmt (use_stmt);
    }
}


/* Free OCC and return one more "struct occurrence" to be freed.  */

static struct occurrence *
free_bb (struct occurrence *occ)
{
  struct occurrence *child, *next;

  /* First get the two pointers hanging off OCC.  */
  next = occ->next;
  child = occ->children;
  occ->bb->aux = NULL;
  pool_free (occ_pool, occ);

  /* Now ensure that we don't recurse unless it is necessary.  */
  if (!child)
    return next;
  else
    {
      while (next)
	next = free_bb (next);

      return child;
    }
}


/* Look for floating-point divisions among DEF's uses, and try to
   replace them by multiplications with the reciprocal.  Add
   as many statements computing the reciprocal as needed.

   DEF must be a GIMPLE register of a floating-point type.  */

static void
execute_cse_reciprocals_1 (gimple_stmt_iterator *def_gsi, tree def)
{
  use_operand_p use_p;
  imm_use_iterator use_iter;
  struct occurrence *occ;
  int count = 0, threshold;

  gcc_assert (FLOAT_TYPE_P (TREE_TYPE (def)) && is_gimple_reg (def));

  FOR_EACH_IMM_USE_FAST (use_p, use_iter, def)
    {
      gimple use_stmt = USE_STMT (use_p);
      if (is_division_by (use_stmt, def))
	{
	  register_division_in (gimple_bb (use_stmt));
	  count++;
	}
    }

  /* Do the expensive part only if we can hope to optimize something.  */
  threshold = targetm.min_divisions_for_recip_mul (TYPE_MODE (TREE_TYPE (def)));
  if (count >= threshold)
    {
      gimple use_stmt;
      for (occ = occ_head; occ; occ = occ->next)
	{
	  compute_merit (occ);
	  insert_reciprocals (def_gsi, occ, def, NULL, threshold);
	}

      FOR_EACH_IMM_USE_STMT (use_stmt, use_iter, def)
	{
	  if (is_division_by (use_stmt, def))
	    {
	      FOR_EACH_IMM_USE_ON_STMT (use_p, use_iter)
		replace_reciprocal (use_p);
	    }
	}
    }

  for (occ = occ_head; occ; )
    occ = free_bb (occ);

  occ_head = NULL;
}

static bool
gate_cse_reciprocals (void)
{
  return optimize && flag_reciprocal_math;
}

/* Go through all the floating-point SSA_NAMEs, and call
   execute_cse_reciprocals_1 on each of them.  */
static unsigned int
execute_cse_reciprocals (void)
{
  basic_block bb;
  tree arg;

  occ_pool = create_alloc_pool ("dominators for recip",
				sizeof (struct occurrence),
				n_basic_blocks / 3 + 1);

  memset (&reciprocal_stats, 0, sizeof (reciprocal_stats));
  calculate_dominance_info (CDI_DOMINATORS);
  calculate_dominance_info (CDI_POST_DOMINATORS);

#ifdef ENABLE_CHECKING
  FOR_EACH_BB (bb)
    gcc_assert (!bb->aux);
#endif

  for (arg = DECL_ARGUMENTS (cfun->decl); arg; arg = DECL_CHAIN (arg))
    if (gimple_default_def (cfun, arg)
	&& FLOAT_TYPE_P (TREE_TYPE (arg))
	&& is_gimple_reg (arg))
      execute_cse_reciprocals_1 (NULL, gimple_default_def (cfun, arg));

  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;
      gimple phi;
      tree def;

      for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  phi = gsi_stmt (gsi);
	  def = PHI_RESULT (phi);
	  if (FLOAT_TYPE_P (TREE_TYPE (def))
	      && is_gimple_reg (def))
	    execute_cse_reciprocals_1 (NULL, def);
	}

      for (gsi = gsi_after_labels (bb); !gsi_end_p (gsi); gsi_next (&gsi))
        {
	  gimple stmt = gsi_stmt (gsi);

	  if (gimple_has_lhs (stmt)
	      && (def = SINGLE_SSA_TREE_OPERAND (stmt, SSA_OP_DEF)) != NULL
	      && FLOAT_TYPE_P (TREE_TYPE (def))
	      && TREE_CODE (def) == SSA_NAME)
	    execute_cse_reciprocals_1 (&gsi, def);
	}

      if (optimize_bb_for_size_p (bb))
        continue;

      /* Scan for a/func(b) and convert it to reciprocal a*rfunc(b).  */
      for (gsi = gsi_after_labels (bb); !gsi_end_p (gsi); gsi_next (&gsi))
        {
	  gimple stmt = gsi_stmt (gsi);
	  tree fndecl;

	  if (is_gimple_assign (stmt)
	      && gimple_assign_rhs_code (stmt) == RDIV_EXPR)
	    {
	      tree arg1 = gimple_assign_rhs2 (stmt);
	      gimple stmt1;

	      if (TREE_CODE (arg1) != SSA_NAME)
		continue;

	      stmt1 = SSA_NAME_DEF_STMT (arg1);

	      if (is_gimple_call (stmt1)
		  && gimple_call_lhs (stmt1)
		  && (fndecl = gimple_call_fndecl (stmt1))
		  && (DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_NORMAL
		      || DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_MD))
		{
		  enum built_in_function code;
		  bool md_code, fail;
		  imm_use_iterator ui;
		  use_operand_p use_p;

		  code = DECL_FUNCTION_CODE (fndecl);
		  md_code = DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_MD;

		  fndecl = targetm.builtin_reciprocal (code, md_code, false);
		  if (!fndecl)
		    continue;

		  /* Check that all uses of the SSA name are divisions,
		     otherwise replacing the defining statement will do
		     the wrong thing.  */
		  fail = false;
		  FOR_EACH_IMM_USE_FAST (use_p, ui, arg1)
		    {
		      gimple stmt2 = USE_STMT (use_p);
		      if (is_gimple_debug (stmt2))
			continue;
		      if (!is_gimple_assign (stmt2)
			  || gimple_assign_rhs_code (stmt2) != RDIV_EXPR
			  || gimple_assign_rhs1 (stmt2) == arg1
			  || gimple_assign_rhs2 (stmt2) != arg1)
			{
			  fail = true;
			  break;
			}
		    }
		  if (fail)
		    continue;

		  gimple_replace_lhs (stmt1, arg1);
		  gimple_call_set_fndecl (stmt1, fndecl);
		  update_stmt (stmt1);
		  reciprocal_stats.rfuncs_inserted++;

		  FOR_EACH_IMM_USE_STMT (stmt, ui, arg1)
		    {
		      gimple_assign_set_rhs_code (stmt, MULT_EXPR);
		      fold_stmt_inplace (stmt);
		      update_stmt (stmt);
		    }
		}
	    }
	}
    }

  statistics_counter_event (cfun, "reciprocal divs inserted",
			    reciprocal_stats.rdivs_inserted);
  statistics_counter_event (cfun, "reciprocal functions inserted",
			    reciprocal_stats.rfuncs_inserted);

  free_dominance_info (CDI_DOMINATORS);
  free_dominance_info (CDI_POST_DOMINATORS);
  free_alloc_pool (occ_pool);
  return 0;
}

struct gimple_opt_pass pass_cse_reciprocals =
{
 {
  GIMPLE_PASS,
  "recip",				/* name */
  gate_cse_reciprocals,			/* gate */
  execute_cse_reciprocals,		/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  PROP_ssa,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_update_ssa | TODO_verify_ssa
    | TODO_verify_stmts                /* todo_flags_finish */
 }
};

/* Records an occurrence at statement USE_STMT in the vector of trees
   STMTS if it is dominated by *TOP_BB or dominates it or this basic block
   is not yet initialized.  Returns true if the occurrence was pushed on
   the vector.  Adjusts *TOP_BB to be the basic block dominating all
   statements in the vector.  */

static bool
maybe_record_sincos (VEC(gimple, heap) **stmts,
		     basic_block *top_bb, gimple use_stmt)
{
  basic_block use_bb = gimple_bb (use_stmt);
  if (*top_bb
      && (*top_bb == use_bb
	  || dominated_by_p (CDI_DOMINATORS, use_bb, *top_bb)))
    VEC_safe_push (gimple, heap, *stmts, use_stmt);
  else if (!*top_bb
	   || dominated_by_p (CDI_DOMINATORS, *top_bb, use_bb))
    {
      VEC_safe_push (gimple, heap, *stmts, use_stmt);
      *top_bb = use_bb;
    }
  else
    return false;

  return true;
}

/* Look for sin, cos and cexpi calls with the same argument NAME and
   create a single call to cexpi CSEing the result in this case.
   We first walk over all immediate uses of the argument collecting
   statements that we can CSE in a vector and in a second pass replace
   the statement rhs with a REALPART or IMAGPART expression on the
   result of the cexpi call we insert before the use statement that
   dominates all other candidates.  */

static bool
execute_cse_sincos_1 (tree name)
{
  gimple_stmt_iterator gsi;
  imm_use_iterator use_iter;
  tree fndecl, res, type;
  gimple def_stmt, use_stmt, stmt;
  int seen_cos = 0, seen_sin = 0, seen_cexpi = 0;
  VEC(gimple, heap) *stmts = NULL;
  basic_block top_bb = NULL;
  int i;
  bool cfg_changed = false;

  type = TREE_TYPE (name);
  FOR_EACH_IMM_USE_STMT (use_stmt, use_iter, name)
    {
      if (gimple_code (use_stmt) != GIMPLE_CALL
	  || !gimple_call_lhs (use_stmt)
	  || !(fndecl = gimple_call_fndecl (use_stmt))
	  || DECL_BUILT_IN_CLASS (fndecl) != BUILT_IN_NORMAL)
	continue;

      switch (DECL_FUNCTION_CODE (fndecl))
	{
	CASE_FLT_FN (BUILT_IN_COS):
	  seen_cos |= maybe_record_sincos (&stmts, &top_bb, use_stmt) ? 1 : 0;
	  break;

	CASE_FLT_FN (BUILT_IN_SIN):
	  seen_sin |= maybe_record_sincos (&stmts, &top_bb, use_stmt) ? 1 : 0;
	  break;

	CASE_FLT_FN (BUILT_IN_CEXPI):
	  seen_cexpi |= maybe_record_sincos (&stmts, &top_bb, use_stmt) ? 1 : 0;
	  break;

	default:;
	}
    }

  if (seen_cos + seen_sin + seen_cexpi <= 1)
    {
      VEC_free(gimple, heap, stmts);
      return false;
    }

  /* Simply insert cexpi at the beginning of top_bb but not earlier than
     the name def statement.  */
  fndecl = mathfn_built_in (type, BUILT_IN_CEXPI);
  if (!fndecl)
    return false;
  res = create_tmp_reg (TREE_TYPE (TREE_TYPE (fndecl)), "sincostmp");
  stmt = gimple_build_call (fndecl, 1, name);
  res = make_ssa_name (res, stmt);
  gimple_call_set_lhs (stmt, res);

  def_stmt = SSA_NAME_DEF_STMT (name);
  if (!SSA_NAME_IS_DEFAULT_DEF (name)
      && gimple_code (def_stmt) != GIMPLE_PHI
      && gimple_bb (def_stmt) == top_bb)
    {
      gsi = gsi_for_stmt (def_stmt);
      gsi_insert_after (&gsi, stmt, GSI_SAME_STMT);
    }
  else
    {
      gsi = gsi_after_labels (top_bb);
      gsi_insert_before (&gsi, stmt, GSI_SAME_STMT);
    }
  update_stmt (stmt);
  sincos_stats.inserted++;

  /* And adjust the recorded old call sites.  */
  for (i = 0; VEC_iterate(gimple, stmts, i, use_stmt); ++i)
    {
      tree rhs = NULL;
      fndecl = gimple_call_fndecl (use_stmt);

      switch (DECL_FUNCTION_CODE (fndecl))
	{
	CASE_FLT_FN (BUILT_IN_COS):
	  rhs = fold_build1 (REALPART_EXPR, type, res);
	  break;

	CASE_FLT_FN (BUILT_IN_SIN):
	  rhs = fold_build1 (IMAGPART_EXPR, type, res);
	  break;

	CASE_FLT_FN (BUILT_IN_CEXPI):
	  rhs = res;
	  break;

	default:;
	  gcc_unreachable ();
	}

	/* Replace call with a copy.  */
	stmt = gimple_build_assign (gimple_call_lhs (use_stmt), rhs);

	gsi = gsi_for_stmt (use_stmt);
	gsi_replace (&gsi, stmt, true);
	if (gimple_purge_dead_eh_edges (gimple_bb (stmt)))
	  cfg_changed = true;
    }

  VEC_free(gimple, heap, stmts);

  return cfg_changed;
}

/* Go through all calls to sin, cos and cexpi and call execute_cse_sincos_1
   on the SSA_NAME argument of each of them.  */

static unsigned int
execute_cse_sincos (void)
{
  basic_block bb;
  bool cfg_changed = false;

  calculate_dominance_info (CDI_DOMINATORS);
  memset (&sincos_stats, 0, sizeof (sincos_stats));

  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_after_labels (bb); !gsi_end_p (gsi); gsi_next (&gsi))
        {
	  gimple stmt = gsi_stmt (gsi);
	  tree fndecl;

	  if (is_gimple_call (stmt)
	      && gimple_call_lhs (stmt)
	      && (fndecl = gimple_call_fndecl (stmt))
	      && DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_NORMAL)
	    {
	      tree arg;

	      switch (DECL_FUNCTION_CODE (fndecl))
		{
		CASE_FLT_FN (BUILT_IN_COS):
		CASE_FLT_FN (BUILT_IN_SIN):
		CASE_FLT_FN (BUILT_IN_CEXPI):
		  arg = gimple_call_arg (stmt, 0);
		  if (TREE_CODE (arg) == SSA_NAME)
		    cfg_changed |= execute_cse_sincos_1 (arg);
		  break;

		default:;
		}
	    }
	}
    }

  statistics_counter_event (cfun, "sincos statements inserted",
			    sincos_stats.inserted);

  free_dominance_info (CDI_DOMINATORS);
  return cfg_changed ? TODO_cleanup_cfg : 0;
}

static bool
gate_cse_sincos (void)
{
  /* Make sure we have either sincos or cexp.  */
  return (TARGET_HAS_SINCOS
	  || TARGET_C99_FUNCTIONS)
	 && optimize;
}

struct gimple_opt_pass pass_cse_sincos =
{
 {
  GIMPLE_PASS,
  "sincos",				/* name */
  gate_cse_sincos,			/* gate */
  execute_cse_sincos,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  PROP_ssa,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_update_ssa | TODO_verify_ssa
    | TODO_verify_stmts                 /* todo_flags_finish */
 }
};

/* A symbolic number is used to detect byte permutation and selection
   patterns.  Therefore the field N contains an artificial number
   consisting of byte size markers:

   0    - byte has the value 0
   1..size - byte contains the content of the byte
   number indexed with that value minus one  */

struct symbolic_number {
  unsigned HOST_WIDEST_INT n;
  int size;
};

/* Perform a SHIFT or ROTATE operation by COUNT bits on symbolic
   number N.  Return false if the requested operation is not permitted
   on a symbolic number.  */

static inline bool
do_shift_rotate (enum tree_code code,
		 struct symbolic_number *n,
		 int count)
{
  if (count % 8 != 0)
    return false;

  /* Zero out the extra bits of N in order to avoid them being shifted
     into the significant bits.  */
  if (n->size < (int)sizeof (HOST_WIDEST_INT))
    n->n &= ((unsigned HOST_WIDEST_INT)1 << (n->size * BITS_PER_UNIT)) - 1;

  switch (code)
    {
    case LSHIFT_EXPR:
      n->n <<= count;
      break;
    case RSHIFT_EXPR:
      n->n >>= count;
      break;
    case LROTATE_EXPR:
      n->n = (n->n << count) | (n->n >> ((n->size * BITS_PER_UNIT) - count));
      break;
    case RROTATE_EXPR:
      n->n = (n->n >> count) | (n->n << ((n->size * BITS_PER_UNIT) - count));
      break;
    default:
      return false;
    }
  return true;
}

/* Perform sanity checking for the symbolic number N and the gimple
   statement STMT.  */

static inline bool
verify_symbolic_number_p (struct symbolic_number *n, gimple stmt)
{
  tree lhs_type;

  lhs_type = gimple_expr_type (stmt);

  if (TREE_CODE (lhs_type) != INTEGER_TYPE)
    return false;

  if (TYPE_PRECISION (lhs_type) != n->size * BITS_PER_UNIT)
    return false;

  return true;
}

/* find_bswap_1 invokes itself recursively with N and tries to perform
   the operation given by the rhs of STMT on the result.  If the
   operation could successfully be executed the function returns the
   tree expression of the source operand and NULL otherwise.  */

static tree
find_bswap_1 (gimple stmt, struct symbolic_number *n, int limit)
{
  enum tree_code code;
  tree rhs1, rhs2 = NULL;
  gimple rhs1_stmt, rhs2_stmt;
  tree source_expr1;
  enum gimple_rhs_class rhs_class;

  if (!limit || !is_gimple_assign (stmt))
    return NULL_TREE;

  rhs1 = gimple_assign_rhs1 (stmt);

  if (TREE_CODE (rhs1) != SSA_NAME)
    return NULL_TREE;

  code = gimple_assign_rhs_code (stmt);
  rhs_class = gimple_assign_rhs_class (stmt);
  rhs1_stmt = SSA_NAME_DEF_STMT (rhs1);

  if (rhs_class == GIMPLE_BINARY_RHS)
    rhs2 = gimple_assign_rhs2 (stmt);

  /* Handle unary rhs and binary rhs with integer constants as second
     operand.  */

  if (rhs_class == GIMPLE_UNARY_RHS
      || (rhs_class == GIMPLE_BINARY_RHS
	  && TREE_CODE (rhs2) == INTEGER_CST))
    {
      if (code != BIT_AND_EXPR
	  && code != LSHIFT_EXPR
	  && code != RSHIFT_EXPR
	  && code != LROTATE_EXPR
	  && code != RROTATE_EXPR
	  && code != NOP_EXPR
	  && code != CONVERT_EXPR)
	return NULL_TREE;

      source_expr1 = find_bswap_1 (rhs1_stmt, n, limit - 1);

      /* If find_bswap_1 returned NULL STMT is a leaf node and we have
	 to initialize the symbolic number.  */
      if (!source_expr1)
	{
	  /* Set up the symbolic number N by setting each byte to a
	     value between 1 and the byte size of rhs1.  The highest
	     order byte is set to n->size and the lowest order
	     byte to 1.  */
	  n->size = TYPE_PRECISION (TREE_TYPE (rhs1));
	  if (n->size % BITS_PER_UNIT != 0)
	    return NULL_TREE;
	  n->size /= BITS_PER_UNIT;
	  n->n = (sizeof (HOST_WIDEST_INT) < 8 ? 0 :
		  (unsigned HOST_WIDEST_INT)0x08070605 << 32 | 0x04030201);

	  if (n->size < (int)sizeof (HOST_WIDEST_INT))
	    n->n &= ((unsigned HOST_WIDEST_INT)1 <<
		     (n->size * BITS_PER_UNIT)) - 1;

	  source_expr1 = rhs1;
	}

      switch (code)
	{
	case BIT_AND_EXPR:
	  {
	    int i;
	    unsigned HOST_WIDEST_INT val = widest_int_cst_value (rhs2);
	    unsigned HOST_WIDEST_INT tmp = val;

	    /* Only constants masking full bytes are allowed.  */
	    for (i = 0; i < n->size; i++, tmp >>= BITS_PER_UNIT)
	      if ((tmp & 0xff) != 0 && (tmp & 0xff) != 0xff)
		return NULL_TREE;

	    n->n &= val;
	  }
	  break;
	case LSHIFT_EXPR:
	case RSHIFT_EXPR:
	case LROTATE_EXPR:
	case RROTATE_EXPR:
	  if (!do_shift_rotate (code, n, (int)TREE_INT_CST_LOW (rhs2)))
	    return NULL_TREE;
	  break;
	CASE_CONVERT:
	  {
	    int type_size;

	    type_size = TYPE_PRECISION (gimple_expr_type (stmt));
	    if (type_size % BITS_PER_UNIT != 0)
	      return NULL_TREE;

	    if (type_size / BITS_PER_UNIT < (int)(sizeof (HOST_WIDEST_INT)))
	      {
		/* If STMT casts to a smaller type mask out the bits not
		   belonging to the target type.  */
		n->n &= ((unsigned HOST_WIDEST_INT)1 << type_size) - 1;
	      }
	    n->size = type_size / BITS_PER_UNIT;
	  }
	  break;
	default:
	  return NULL_TREE;
	};
      return verify_symbolic_number_p (n, stmt) ? source_expr1 : NULL;
    }

  /* Handle binary rhs.  */

  if (rhs_class == GIMPLE_BINARY_RHS)
    {
      struct symbolic_number n1, n2;
      tree source_expr2;

      if (code != BIT_IOR_EXPR)
	return NULL_TREE;

      if (TREE_CODE (rhs2) != SSA_NAME)
	return NULL_TREE;

      rhs2_stmt = SSA_NAME_DEF_STMT (rhs2);

      switch (code)
	{
	case BIT_IOR_EXPR:
	  source_expr1 = find_bswap_1 (rhs1_stmt, &n1, limit - 1);

	  if (!source_expr1)
	    return NULL_TREE;

	  source_expr2 = find_bswap_1 (rhs2_stmt, &n2, limit - 1);

	  if (source_expr1 != source_expr2
	      || n1.size != n2.size)
	    return NULL_TREE;

	  n->size = n1.size;
	  n->n = n1.n | n2.n;

	  if (!verify_symbolic_number_p (n, stmt))
	    return NULL_TREE;

	  break;
	default:
	  return NULL_TREE;
	}
      return source_expr1;
    }
  return NULL_TREE;
}

/* Check if STMT completes a bswap implementation consisting of ORs,
   SHIFTs and ANDs.  Return the source tree expression on which the
   byte swap is performed and NULL if no bswap was found.  */

static tree
find_bswap (gimple stmt)
{
/* The number which the find_bswap result should match in order to
   have a full byte swap.  The number is shifted to the left according
   to the size of the symbolic number before using it.  */
  unsigned HOST_WIDEST_INT cmp =
    sizeof (HOST_WIDEST_INT) < 8 ? 0 :
    (unsigned HOST_WIDEST_INT)0x01020304 << 32 | 0x05060708;

  struct symbolic_number n;
  tree source_expr;

  /* The last parameter determines the depth search limit.  It usually
     correlates directly to the number of bytes to be touched.  We
     increase that number by one here in order to also cover signed ->
     unsigned conversions of the src operand as can be seen in
     libgcc.  */
  source_expr =  find_bswap_1 (stmt, &n,
			       TREE_INT_CST_LOW (
				 TYPE_SIZE_UNIT (gimple_expr_type (stmt))) + 1);

  if (!source_expr)
    return NULL_TREE;

  /* Zero out the extra bits of N and CMP.  */
  if (n.size < (int)sizeof (HOST_WIDEST_INT))
    {
      unsigned HOST_WIDEST_INT mask =
	((unsigned HOST_WIDEST_INT)1 << (n.size * BITS_PER_UNIT)) - 1;

      n.n &= mask;
      cmp >>= (sizeof (HOST_WIDEST_INT) - n.size) * BITS_PER_UNIT;
    }

  /* A complete byte swap should make the symbolic number to start
     with the largest digit in the highest order byte.  */
  if (cmp != n.n)
    return NULL_TREE;

  return source_expr;
}

/* Find manual byte swap implementations and turn them into a bswap
   builtin invokation.  */

static unsigned int
execute_optimize_bswap (void)
{
  basic_block bb;
  bool bswap32_p, bswap64_p;
  bool changed = false;
  tree bswap32_type = NULL_TREE, bswap64_type = NULL_TREE;

  if (BITS_PER_UNIT != 8)
    return 0;

  if (sizeof (HOST_WIDEST_INT) < 8)
    return 0;

  bswap32_p = (built_in_decls[BUILT_IN_BSWAP32]
	       && optab_handler (bswap_optab, SImode) != CODE_FOR_nothing);
  bswap64_p = (built_in_decls[BUILT_IN_BSWAP64]
	       && (optab_handler (bswap_optab, DImode) != CODE_FOR_nothing
		   || (bswap32_p && word_mode == SImode)));

  if (!bswap32_p && !bswap64_p)
    return 0;

  /* Determine the argument type of the builtins.  The code later on
     assumes that the return and argument type are the same.  */
  if (bswap32_p)
    {
      tree fndecl = built_in_decls[BUILT_IN_BSWAP32];
      bswap32_type = TREE_VALUE (TYPE_ARG_TYPES (TREE_TYPE (fndecl)));
    }

  if (bswap64_p)
    {
      tree fndecl = built_in_decls[BUILT_IN_BSWAP64];
      bswap64_type = TREE_VALUE (TYPE_ARG_TYPES (TREE_TYPE (fndecl)));
    }

  memset (&bswap_stats, 0, sizeof (bswap_stats));

  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_after_labels (bb); !gsi_end_p (gsi); gsi_next (&gsi))
        {
	  gimple stmt = gsi_stmt (gsi);
	  tree bswap_src, bswap_type;
	  tree bswap_tmp;
	  tree fndecl = NULL_TREE;
	  int type_size;
	  gimple call;

	  if (!is_gimple_assign (stmt)
	      || gimple_assign_rhs_code (stmt) != BIT_IOR_EXPR)
	    continue;

	  type_size = TYPE_PRECISION (gimple_expr_type (stmt));

	  switch (type_size)
	    {
	    case 32:
	      if (bswap32_p)
		{
		  fndecl = built_in_decls[BUILT_IN_BSWAP32];
		  bswap_type = bswap32_type;
		}
	      break;
	    case 64:
	      if (bswap64_p)
		{
		  fndecl = built_in_decls[BUILT_IN_BSWAP64];
		  bswap_type = bswap64_type;
		}
	      break;
	    default:
	      continue;
	    }

	  if (!fndecl)
	    continue;

	  bswap_src = find_bswap (stmt);

	  if (!bswap_src)
	    continue;

	  changed = true;
	  if (type_size == 32)
	    bswap_stats.found_32bit++;
	  else
	    bswap_stats.found_64bit++;

	  bswap_tmp = bswap_src;

	  /* Convert the src expression if necessary.  */
	  if (!useless_type_conversion_p (TREE_TYPE (bswap_tmp), bswap_type))
	    {
	      gimple convert_stmt;

	      bswap_tmp = create_tmp_var (bswap_type, "bswapsrc");
	      add_referenced_var (bswap_tmp);
	      bswap_tmp = make_ssa_name (bswap_tmp, NULL);

	      convert_stmt = gimple_build_assign_with_ops (
			       CONVERT_EXPR, bswap_tmp, bswap_src, NULL);
	      gsi_insert_before (&gsi, convert_stmt, GSI_SAME_STMT);
	    }

	  call = gimple_build_call (fndecl, 1, bswap_tmp);

	  bswap_tmp = gimple_assign_lhs (stmt);

	  /* Convert the result if necessary.  */
	  if (!useless_type_conversion_p (TREE_TYPE (bswap_tmp), bswap_type))
	    {
	      gimple convert_stmt;

	      bswap_tmp = create_tmp_var (bswap_type, "bswapdst");
	      add_referenced_var (bswap_tmp);
	      bswap_tmp = make_ssa_name (bswap_tmp, NULL);
	      convert_stmt = gimple_build_assign_with_ops (
		               CONVERT_EXPR, gimple_assign_lhs (stmt), bswap_tmp, NULL);
	      gsi_insert_after (&gsi, convert_stmt, GSI_SAME_STMT);
	    }

	  gimple_call_set_lhs (call, bswap_tmp);

	  if (dump_file)
	    {
	      fprintf (dump_file, "%d bit bswap implementation found at: ",
		       (int)type_size);
	      print_gimple_stmt (dump_file, stmt, 0, 0);
	    }

	  gsi_insert_after (&gsi, call, GSI_SAME_STMT);
	  gsi_remove (&gsi, true);
	}
    }

  statistics_counter_event (cfun, "32-bit bswap implementations found",
			    bswap_stats.found_32bit);
  statistics_counter_event (cfun, "64-bit bswap implementations found",
			    bswap_stats.found_64bit);

  return (changed ? TODO_dump_func | TODO_update_ssa | TODO_verify_ssa
	  | TODO_verify_stmts : 0);
}

static bool
gate_optimize_bswap (void)
{
  return flag_expensive_optimizations && optimize;
}

struct gimple_opt_pass pass_optimize_bswap =
{
 {
  GIMPLE_PASS,
  "bswap",				/* name */
  gate_optimize_bswap,                  /* gate */
  execute_optimize_bswap,		/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  PROP_ssa,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0                                     /* todo_flags_finish */
 }
};

/* Return true if RHS is a suitable operand for a widening multiplication.
   There are two cases:

     - RHS makes some value twice as wide.  Store that value in *NEW_RHS_OUT
       if so, and store its type in *TYPE_OUT.

     - RHS is an integer constant.  Store that value in *NEW_RHS_OUT if so,
       but leave *TYPE_OUT untouched.  */

static bool
is_widening_mult_rhs_p (tree rhs, tree *type_out, tree *new_rhs_out)
{
  gimple stmt;
  tree type, type1, rhs1;
  enum tree_code rhs_code;

  if (TREE_CODE (rhs) == SSA_NAME)
    {
      type = TREE_TYPE (rhs);
      stmt = SSA_NAME_DEF_STMT (rhs);
      if (!is_gimple_assign (stmt))
	return false;

      rhs_code = gimple_assign_rhs_code (stmt);
      if (TREE_CODE (type) == INTEGER_TYPE
	  ? !CONVERT_EXPR_CODE_P (rhs_code)
	  : rhs_code != FIXED_CONVERT_EXPR)
	return false;

      rhs1 = gimple_assign_rhs1 (stmt);
      type1 = TREE_TYPE (rhs1);
      if (TREE_CODE (type1) != TREE_CODE (type)
	  || TYPE_PRECISION (type1) * 2 != TYPE_PRECISION (type))
	return false;

      *new_rhs_out = rhs1;
      *type_out = type1;
      return true;
    }

  if (TREE_CODE (rhs) == INTEGER_CST)
    {
      *new_rhs_out = rhs;
      *type_out = NULL;
      return true;
    }

  return false;
}

/* Return true if STMT performs a widening multiplication.  If so,
   store the unwidened types of the operands in *TYPE1_OUT and *TYPE2_OUT
   respectively.  Also fill *RHS1_OUT and *RHS2_OUT such that converting
   those operands to types *TYPE1_OUT and *TYPE2_OUT would give the
   operands of the multiplication.  */

static bool
is_widening_mult_p (gimple stmt,
		    tree *type1_out, tree *rhs1_out,
		    tree *type2_out, tree *rhs2_out)
{
  tree type;

  type = TREE_TYPE (gimple_assign_lhs (stmt));
  if (TREE_CODE (type) != INTEGER_TYPE
      && TREE_CODE (type) != FIXED_POINT_TYPE)
    return false;

  if (!is_widening_mult_rhs_p (gimple_assign_rhs1 (stmt), type1_out, rhs1_out))
    return false;

  if (!is_widening_mult_rhs_p (gimple_assign_rhs2 (stmt), type2_out, rhs2_out))
    return false;

  if (*type1_out == NULL)
    {
      if (*type2_out == NULL || !int_fits_type_p (*rhs1_out, *type2_out))
	return false;
      *type1_out = *type2_out;
    }

  if (*type2_out == NULL)
    {
      if (!int_fits_type_p (*rhs2_out, *type1_out))
	return false;
      *type2_out = *type1_out;
    }

  return true;
}

/* Process a single gimple statement STMT, which has a MULT_EXPR as
   its rhs, and try to convert it into a WIDEN_MULT_EXPR.  The return
   value is true iff we converted the statement.  */

static bool
convert_mult_to_widen (gimple stmt)
{
  tree lhs, rhs1, rhs2, type, type1, type2;
  enum insn_code handler;

  lhs = gimple_assign_lhs (stmt);
  type = TREE_TYPE (lhs);
  if (TREE_CODE (type) != INTEGER_TYPE)
    return false;

  if (!is_widening_mult_p (stmt, &type1, &rhs1, &type2, &rhs2))
    return false;

  if (TYPE_UNSIGNED (type1) && TYPE_UNSIGNED (type2))
    handler = optab_handler (umul_widen_optab, TYPE_MODE (type));
  else if (!TYPE_UNSIGNED (type1) && !TYPE_UNSIGNED (type2))
    handler = optab_handler (smul_widen_optab, TYPE_MODE (type));
  else
    handler = optab_handler (usmul_widen_optab, TYPE_MODE (type));

  if (handler == CODE_FOR_nothing)
    return false;

  gimple_assign_set_rhs1 (stmt, fold_convert (type1, rhs1));
  gimple_assign_set_rhs2 (stmt, fold_convert (type2, rhs2));
  gimple_assign_set_rhs_code (stmt, WIDEN_MULT_EXPR);
  update_stmt (stmt);
  widen_mul_stats.widen_mults_inserted++;
  return true;
}

/* Process a single gimple statement STMT, which is found at the
   iterator GSI and has a either a PLUS_EXPR or a MINUS_EXPR as its
   rhs (given by CODE), and try to convert it into a
   WIDEN_MULT_PLUS_EXPR or a WIDEN_MULT_MINUS_EXPR.  The return value
   is true iff we converted the statement.  */

static bool
convert_plusminus_to_widen (gimple_stmt_iterator *gsi, gimple stmt,
			    enum tree_code code)
{
  gimple rhs1_stmt = NULL, rhs2_stmt = NULL;
  tree type, type1, type2;
  tree lhs, rhs1, rhs2, mult_rhs1, mult_rhs2, add_rhs;
  enum tree_code rhs1_code = ERROR_MARK, rhs2_code = ERROR_MARK;
  optab this_optab;
  enum tree_code wmult_code;

  lhs = gimple_assign_lhs (stmt);
  type = TREE_TYPE (lhs);
  if (TREE_CODE (type) != INTEGER_TYPE
      && TREE_CODE (type) != FIXED_POINT_TYPE)
    return false;

  if (code == MINUS_EXPR)
    wmult_code = WIDEN_MULT_MINUS_EXPR;
  else
    wmult_code = WIDEN_MULT_PLUS_EXPR;

  rhs1 = gimple_assign_rhs1 (stmt);
  rhs2 = gimple_assign_rhs2 (stmt);

  if (TREE_CODE (rhs1) == SSA_NAME)
    {
      rhs1_stmt = SSA_NAME_DEF_STMT (rhs1);
      if (is_gimple_assign (rhs1_stmt))
	rhs1_code = gimple_assign_rhs_code (rhs1_stmt);
    }
  else
    return false;

  if (TREE_CODE (rhs2) == SSA_NAME)
    {
      rhs2_stmt = SSA_NAME_DEF_STMT (rhs2);
      if (is_gimple_assign (rhs2_stmt))
	rhs2_code = gimple_assign_rhs_code (rhs2_stmt);
    }
  else
    return false;

  if (code == PLUS_EXPR && rhs1_code == MULT_EXPR)
    {
      if (!is_widening_mult_p (rhs1_stmt, &type1, &mult_rhs1,
			       &type2, &mult_rhs2))
	return false;
      add_rhs = rhs2;
    }
  else if (rhs2_code == MULT_EXPR)
    {
      if (!is_widening_mult_p (rhs2_stmt, &type1, &mult_rhs1,
			       &type2, &mult_rhs2))
	return false;
      add_rhs = rhs1;
    }
  else if (code == PLUS_EXPR && rhs1_code == WIDEN_MULT_EXPR)
    {
      mult_rhs1 = gimple_assign_rhs1 (rhs1_stmt);
      mult_rhs2 = gimple_assign_rhs2 (rhs1_stmt);
      type1 = TREE_TYPE (mult_rhs1);
      type2 = TREE_TYPE (mult_rhs2);
      add_rhs = rhs2;
    }
  else if (rhs2_code == WIDEN_MULT_EXPR)
    {
      mult_rhs1 = gimple_assign_rhs1 (rhs2_stmt);
      mult_rhs2 = gimple_assign_rhs2 (rhs2_stmt);
      type1 = TREE_TYPE (mult_rhs1);
      type2 = TREE_TYPE (mult_rhs2);
      add_rhs = rhs1;
    }
  else
    return false;

  if (TYPE_UNSIGNED (type1) != TYPE_UNSIGNED (type2))
    return false;

  /* Verify that the machine can perform a widening multiply
     accumulate in this mode/signedness combination, otherwise
     this transformation is likely to pessimize code.  */
  this_optab = optab_for_tree_code (wmult_code, type1, optab_default);
  if (optab_handler (this_optab, TYPE_MODE (type)) == CODE_FOR_nothing)
    return false;

  /* ??? May need some type verification here?  */

  gimple_assign_set_rhs_with_ops_1 (gsi, wmult_code,
				    fold_convert (type1, mult_rhs1),
				    fold_convert (type2, mult_rhs2),
				    add_rhs);
  update_stmt (gsi_stmt (*gsi));
  widen_mul_stats.maccs_inserted++;
  return true;
}

/* Combine the multiplication at MUL_STMT with operands MULOP1 and MULOP2
   with uses in additions and subtractions to form fused multiply-add
   operations.  Returns true if successful and MUL_STMT should be removed.  */

static bool
convert_mult_to_fma (gimple mul_stmt, tree op1, tree op2)
{
  tree mul_result = gimple_get_lhs (mul_stmt);
  tree type = TREE_TYPE (mul_result);
  gimple use_stmt, neguse_stmt, fma_stmt;
  use_operand_p use_p;
  imm_use_iterator imm_iter;

  if (FLOAT_TYPE_P (type)
      && flag_fp_contract_mode == FP_CONTRACT_OFF)
    return false;

  /* We don't want to do bitfield reduction ops.  */
  if (INTEGRAL_TYPE_P (type)
      && (TYPE_PRECISION (type)
	  != GET_MODE_PRECISION (TYPE_MODE (type))))
    return false;

  /* If the target doesn't support it, don't generate it.  We assume that
     if fma isn't available then fms, fnma or fnms are not either.  */
  if (optab_handler (fma_optab, TYPE_MODE (type)) == CODE_FOR_nothing)
    return false;

  /* Make sure that the multiplication statement becomes dead after
     the transformation, thus that all uses are transformed to FMAs.
     This means we assume that an FMA operation has the same cost
     as an addition.  */
  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, mul_result)
    {
      enum tree_code use_code;
      tree result = mul_result;
      bool negate_p = false;

      use_stmt = USE_STMT (use_p);

      if (is_gimple_debug (use_stmt))
	continue;

      /* For now restrict this operations to single basic blocks.  In theory
	 we would want to support sinking the multiplication in
	 m = a*b;
	 if ()
	   ma = m + c;
	 else
	   d = m;
	 to form a fma in the then block and sink the multiplication to the
	 else block.  */
      if (gimple_bb (use_stmt) != gimple_bb (mul_stmt))
	return false;

      if (!is_gimple_assign (use_stmt))
	return false;

      use_code = gimple_assign_rhs_code (use_stmt);

      /* A negate on the multiplication leads to FNMA.  */
      if (use_code == NEGATE_EXPR)
	{
	  ssa_op_iter iter;
	  tree use;

	  result = gimple_assign_lhs (use_stmt);

	  /* Make sure the negate statement becomes dead with this
	     single transformation.  */
	  if (!single_imm_use (gimple_assign_lhs (use_stmt),
			       &use_p, &neguse_stmt))
	    return false;

	  /* Make sure the multiplication isn't also used on that stmt.  */
	  FOR_EACH_SSA_TREE_OPERAND (use, neguse_stmt, iter, SSA_OP_USE)
	    if (use == mul_result)
	      return false;

	  /* Re-validate.  */
	  use_stmt = neguse_stmt;
	  if (gimple_bb (use_stmt) != gimple_bb (mul_stmt))
	    return false;
	  if (!is_gimple_assign (use_stmt))
	    return false;

	  use_code = gimple_assign_rhs_code (use_stmt);
	  negate_p = true;
	}

      switch (use_code)
	{
	case MINUS_EXPR:
	  if (gimple_assign_rhs2 (use_stmt) == result)
	    negate_p = !negate_p;
	  break;
	case PLUS_EXPR:
	  break;
	default:
	  /* FMA can only be formed from PLUS and MINUS.  */
	  return false;
	}

      /* We can't handle a * b + a * b.  */
      if (gimple_assign_rhs1 (use_stmt) == gimple_assign_rhs2 (use_stmt))
	return false;

      /* While it is possible to validate whether or not the exact form
	 that we've recognized is available in the backend, the assumption
	 is that the transformation is never a loss.  For instance, suppose
	 the target only has the plain FMA pattern available.  Consider
	 a*b-c -> fma(a,b,-c): we've exchanged MUL+SUB for FMA+NEG, which
	 is still two operations.  Consider -(a*b)-c -> fma(-a,b,-c): we
	 still have 3 operations, but in the FMA form the two NEGs are
	 independant and could be run in parallel.  */
    }

  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, mul_result)
    {
      gimple_stmt_iterator gsi = gsi_for_stmt (use_stmt);
      enum tree_code use_code;
      tree addop, mulop1 = op1, result = mul_result;
      bool negate_p = false;

      if (is_gimple_debug (use_stmt))
	continue;

      use_code = gimple_assign_rhs_code (use_stmt);
      if (use_code == NEGATE_EXPR)
	{
	  result = gimple_assign_lhs (use_stmt);
	  single_imm_use (gimple_assign_lhs (use_stmt), &use_p, &neguse_stmt);
	  gsi_remove (&gsi, true);
	  release_defs (use_stmt);

	  use_stmt = neguse_stmt;
	  gsi = gsi_for_stmt (use_stmt);
	  use_code = gimple_assign_rhs_code (use_stmt);
	  negate_p = true;
	}

      if (gimple_assign_rhs1 (use_stmt) == result)
	{
	  addop = gimple_assign_rhs2 (use_stmt);
	  /* a * b - c -> a * b + (-c)  */
	  if (gimple_assign_rhs_code (use_stmt) == MINUS_EXPR)
	    addop = force_gimple_operand_gsi (&gsi,
					      build1 (NEGATE_EXPR,
						      type, addop),
					      true, NULL_TREE, true,
					      GSI_SAME_STMT);
	}
      else
	{
	  addop = gimple_assign_rhs1 (use_stmt);
	  /* a - b * c -> (-b) * c + a */
	  if (gimple_assign_rhs_code (use_stmt) == MINUS_EXPR)
	    negate_p = !negate_p;
	}

      if (negate_p)
	mulop1 = force_gimple_operand_gsi (&gsi,
					   build1 (NEGATE_EXPR,
						   type, mulop1),
					   true, NULL_TREE, true,
					   GSI_SAME_STMT);

      fma_stmt = gimple_build_assign_with_ops3 (FMA_EXPR,
						gimple_assign_lhs (use_stmt),
						mulop1, op2,
						addop);
      gsi_replace (&gsi, fma_stmt, true);
      widen_mul_stats.fmas_inserted++;
    }

  return true;
}

/* Find integer multiplications where the operands are extended from
   smaller types, and replace the MULT_EXPR with a WIDEN_MULT_EXPR
   where appropriate.  */

static unsigned int
execute_optimize_widening_mul (void)
{
  basic_block bb;
  bool cfg_changed = false;

  memset (&widen_mul_stats, 0, sizeof (widen_mul_stats));

  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_after_labels (bb); !gsi_end_p (gsi);)
        {
	  gimple stmt = gsi_stmt (gsi);
	  enum tree_code code;

	  if (is_gimple_assign (stmt))
	    {
	      code = gimple_assign_rhs_code (stmt);
	      switch (code)
		{
		case MULT_EXPR:
		  if (!convert_mult_to_widen (stmt)
		      && convert_mult_to_fma (stmt,
					      gimple_assign_rhs1 (stmt),
					      gimple_assign_rhs2 (stmt)))
		    {
		      gsi_remove (&gsi, true);
		      release_defs (stmt);
		      continue;
		    }
		  break;

		case PLUS_EXPR:
		case MINUS_EXPR:
		  convert_plusminus_to_widen (&gsi, stmt, code);
		  break;

		default:;
		}
	    }
	  else if (is_gimple_call (stmt)
		   && gimple_call_lhs (stmt))
	    {
	      tree fndecl = gimple_call_fndecl (stmt);
	      if (fndecl
		  && DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_NORMAL)
		{
		  switch (DECL_FUNCTION_CODE (fndecl))
		    {
		      case BUILT_IN_POWF:
		      case BUILT_IN_POW:
		      case BUILT_IN_POWL:
			if (TREE_CODE (gimple_call_arg (stmt, 1)) == REAL_CST
			    && REAL_VALUES_EQUAL
			         (TREE_REAL_CST (gimple_call_arg (stmt, 1)),
				  dconst2)
			    && convert_mult_to_fma (stmt,
						    gimple_call_arg (stmt, 0),
						    gimple_call_arg (stmt, 0)))
			  {
			    unlink_stmt_vdef (stmt);
			    gsi_remove (&gsi, true);
			    release_defs (stmt);
			    if (gimple_purge_dead_eh_edges (bb))
			      cfg_changed = true;
			    continue;
			  }
			  break;

		      default:;
		    }
		}
	    }
	  gsi_next (&gsi);
	}
    }

  statistics_counter_event (cfun, "widening multiplications inserted",
			    widen_mul_stats.widen_mults_inserted);
  statistics_counter_event (cfun, "widening maccs inserted",
			    widen_mul_stats.maccs_inserted);
  statistics_counter_event (cfun, "fused multiply-adds inserted",
			    widen_mul_stats.fmas_inserted);

  return cfg_changed ? TODO_cleanup_cfg : 0;
}

static bool
gate_optimize_widening_mul (void)
{
  return flag_expensive_optimizations && optimize;
}

struct gimple_opt_pass pass_optimize_widening_mul =
{
 {
  GIMPLE_PASS,
  "widening_mul",			/* name */
  gate_optimize_widening_mul,		/* gate */
  execute_optimize_widening_mul,	/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  PROP_ssa,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_verify_ssa
  | TODO_verify_stmts
  | TODO_dump_func
  | TODO_update_ssa                     /* todo_flags_finish */
 }
};

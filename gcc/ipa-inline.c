/* Inlining decision heuristics.
   Copyright (C) 2003, 2004, 2007, 2008, 2009, 2010, 2011
   Free Software Foundation, Inc.
   Contributed by Jan Hubicka

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/*  Inlining decision heuristics

    The implementation of inliner is organized as follows:

    Transformation of callgraph to represent inlining decisions.

      The inline decisions are stored in callgraph in "inline plan" and
      all applied later.

      To mark given call inline, use cgraph_mark_inline function.
      The function marks the edge inlinable and, if necessary, produces
      virtual clone in the callgraph representing the new copy of callee's
      function body.

      The inline plan is applied on given function body by inline_transform. 

    inlining heuristics limits

      can_inline_edge_p allow to check that particular inlining is allowed
      by the limits specified by user (allowed function growth, growth and so
      on).

      Functions are inlined when it is obvious the result is profitable (such
      as functions called once or when inlining reduce code size).
      In addition to that we perform inlining of small functions and recursive
      inlining.

    inlining heuristics

       The inliner itself is split into two passes:

       pass_early_inlining

	 Simple local inlining pass inlining callees into current function.
	 This pass makes no use of whole unit analysis and thus it can do only
	 very simple decisions based on local properties.

	 The strength of the pass is that it is run in topological order
	 (reverse postorder) on the callgraph. Functions are converted into SSA
	 form just before this pass and optimized subsequently. As a result, the
	 callees of the function seen by the early inliner was already optimized
	 and results of early inlining adds a lot of optimization opportunities
	 for the local optimization.

	 The pass handle the obvious inlining decisions within the compilation
	 unit - inlining auto inline functions, inlining for size and
	 flattening.

	 main strength of the pass is the ability to eliminate abstraction
	 penalty in C++ code (via combination of inlining and early
	 optimization) and thus improve quality of analysis done by real IPA
	 optimizers.

	 Because of lack of whole unit knowledge, the pass can not really make
	 good code size/performance tradeoffs.  It however does very simple
	 speculative inlining allowing code size to grow by
	 EARLY_INLINING_INSNS when callee is leaf function.  In this case the
	 optimizations performed later are very likely to eliminate the cost.

       pass_ipa_inline

	 This is the real inliner able to handle inlining with whole program
	 knowledge. It performs following steps:

	 1) inlining of small functions.  This is implemented by greedy
	 algorithm ordering all inlinable cgraph edges by their badness and
	 inlining them in this order as long as inline limits allows doing so.

	 This heuristics is not very good on inlining recursive calls. Recursive
	 calls can be inlined with results similar to loop unrolling. To do so,
	 special purpose recursive inliner is executed on function when
	 recursive edge is met as viable candidate.

	 2) Unreachable functions are removed from callgraph.  Inlining leads
	 to devirtualization and other modification of callgraph so functions
	 may become unreachable during the process. Also functions declared as
	 extern inline or virtual functions are removed, since after inlining
	 we no longer need the offline bodies.

	 3) Functions called once and not exported from the unit are inlined.
	 This should almost always lead to reduction of code size by eliminating
	 the need for offline copy of the function.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "tree-inline.h"
#include "langhooks.h"
#include "flags.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "gimple-pretty-print.h"
#include "timevar.h"
#include "params.h"
#include "fibheap.h"
#include "intl.h"
#include "tree-pass.h"
#include "coverage.h"
#include "ggc.h"
#include "rtl.h"
#include "tree-flow.h"
#include "ipa-prop.h"
#include "except.h"
#include "target.h"
#include "ipa-inline.h"

/* Statistics we collect about inlining algorithm.  */
static int ncalls_inlined;
static int nfunctions_inlined;
static int overall_size;
static gcov_type max_count, max_benefit;

/* Scale frequency of NODE edges by FREQ_SCALE and increase loop nest
   by NEST.  */

static void
update_noncloned_frequencies (struct cgraph_node *node,
			      int freq_scale, int nest)
{
  struct cgraph_edge *e;

  /* We do not want to ignore high loop nest after freq drops to 0.  */
  if (!freq_scale)
    freq_scale = 1;
  for (e = node->callees; e; e = e->next_callee)
    {
      e->loop_nest += nest;
      e->frequency = e->frequency * (gcov_type) freq_scale / CGRAPH_FREQ_BASE;
      if (e->frequency > CGRAPH_FREQ_MAX)
        e->frequency = CGRAPH_FREQ_MAX;
      if (!e->inline_failed)
        update_noncloned_frequencies (e->callee, freq_scale, nest);
    }
}

/* E is expected to be an edge being inlined.  Clone destination node of
   the edge and redirect it to the new clone.
   DUPLICATE is used for bookkeeping on whether we are actually creating new
   clones or re-using node originally representing out-of-line function call.
   */
void
cgraph_clone_inlined_nodes (struct cgraph_edge *e, bool duplicate,
			    bool update_original)
{
  HOST_WIDE_INT peak;
  struct inline_summary *caller_info, *callee_info;

  if (duplicate)
    {
      /* We may eliminate the need for out-of-line copy to be output.
	 In that case just go ahead and re-use it.  */
      if (!e->callee->callers->next_caller
	  /* Recursive inlining never wants the master clone to
	     be overwritten.  */
	  && update_original
	  /* FIXME: When address is taken of DECL_EXTERNAL function we still
	     can remove its offline copy, but we would need to keep unanalyzed
	     node in the callgraph so references can point to it.  */
	  && !e->callee->address_taken
	  && cgraph_can_remove_if_no_direct_calls_p (e->callee)
	  /* Inlining might enable more devirtualizing, so we want to remove
	     those only after all devirtualizable virtual calls are processed.
	     Lacking may edges in callgraph we just preserve them post
	     inlining.  */
	  && (!DECL_VIRTUAL_P (e->callee->decl)
	      || (!DECL_COMDAT (e->callee->decl)
		  && !DECL_EXTERNAL (e->callee->decl)))
	  /* Don't reuse if more than one function shares a comdat group.
	     If the other function(s) are needed, we need to emit even
	     this function out of line.  */
	  && !e->callee->same_comdat_group
	  && !cgraph_new_nodes)
	{
	  gcc_assert (!e->callee->global.inlined_to);
	  if (e->callee->analyzed && !DECL_EXTERNAL (e->callee->decl))
	    {
	      overall_size -= inline_summary (e->callee)->size;
	      nfunctions_inlined++;
	    }
	  duplicate = false;
	  e->callee->local.externally_visible = false;
          update_noncloned_frequencies (e->callee, e->frequency, e->loop_nest);
	}
      else
	{
	  struct cgraph_node *n;
	  n = cgraph_clone_node (e->callee, e->callee->decl,
				 e->count, e->frequency, e->loop_nest,
				 update_original, NULL);
	  cgraph_redirect_edge_callee (e, n);
	}
    }

  callee_info = inline_summary (e->callee);
  caller_info = inline_summary (e->caller);

  if (e->caller->global.inlined_to)
    e->callee->global.inlined_to = e->caller->global.inlined_to;
  else
    e->callee->global.inlined_to = e->caller;
  callee_info->stack_frame_offset
    = caller_info->stack_frame_offset
      + caller_info->estimated_self_stack_size;
  peak = callee_info->stack_frame_offset
      + callee_info->estimated_self_stack_size;
  if (inline_summary (e->callee->global.inlined_to)->estimated_stack_size
      < peak)
    inline_summary (e->callee->global.inlined_to)->estimated_stack_size = peak;
  cgraph_propagate_frequency (e->callee);

  /* Recursively clone all bodies.  */
  for (e = e->callee->callees; e; e = e->next_callee)
    if (!e->inline_failed)
      cgraph_clone_inlined_nodes (e, duplicate, update_original);
}

/* Mark edge E as inlined and update callgraph accordingly.  UPDATE_ORIGINAL
   specify whether profile of original function should be updated.  If any new
   indirect edges are discovered in the process, add them to NEW_EDGES, unless
   it is NULL.  Return true iff any new callgraph edges were discovered as a
   result of inlining.  */

static bool
cgraph_mark_inline_edge (struct cgraph_edge *e, bool update_original,
			 VEC (cgraph_edge_p, heap) **new_edges)
{
  int old_size = 0, new_size = 0;
  struct cgraph_node *to = NULL;
  struct cgraph_edge *curr = e;
  struct inline_summary *info;

  /* Don't inline inlined edges.  */
  gcc_assert (e->inline_failed);
  /* Don't even think of inlining inline clone.  */
  gcc_assert (!e->callee->global.inlined_to);

  e->inline_failed = CIF_OK;
  DECL_POSSIBLY_INLINED (e->callee->decl) = true;

  cgraph_clone_inlined_nodes (e, true, update_original);

  /* Now update size of caller and all functions caller is inlined into.  */
  for (;e && !e->inline_failed; e = e->caller->callers)
    {
      to = e->caller;
      info = inline_summary (to);
      old_size = info->size;
      new_size = estimate_size_after_inlining (to, curr);
      info->size = new_size;
      info->time = estimate_time_after_inlining (to, curr);
    }
  gcc_assert (curr->callee->global.inlined_to == to);
  if (new_size > old_size)
    overall_size += new_size - old_size;
  ncalls_inlined++;

  /* FIXME: We should remove the optimize check after we ensure we never run
     IPA passes when not optimizing.  */
  if (flag_indirect_inlining && optimize)
    return ipa_propagate_indirect_call_infos (curr, new_edges);
  else
    return false;
}

/* Return false when inlining edge E would lead to violating
   limits on function unit growth or stack usage growth.  

   The relative function body growth limit is present generally
   to avoid problems with non-linear behavior of the compiler.
   To allow inlining huge functions into tiny wrapper, the limit
   is always based on the bigger of the two functions considered.

   For stack growth limits we always base the growth in stack usage
   of the callers.  We want to prevent applications from segfaulting
   on stack overflow when functions with huge stack frames gets
   inlined. */

static bool
caller_growth_limits (struct cgraph_edge *e)
{
  struct cgraph_node *to = e->caller;
  struct cgraph_node *what = e->callee;
  int newsize;
  int limit = 0;
  HOST_WIDE_INT stack_size_limit = 0, inlined_stack;
  struct inline_summary *info, *what_info, *outer_info = inline_summary (to);

  /* Look for function e->caller is inlined to.  While doing
     so work out the largest function body on the way.  As
     described above, we want to base our function growth
     limits based on that.  Not on the self size of the
     outer function, not on the self size of inline code
     we immediately inline to.  This is the most relaxed
     interpretation of the rule "do not grow large functions
     too much in order to prevent compiler from exploding".  */
  do
    {
      info = inline_summary (to);
      if (limit < info->self_size)
	limit = info->self_size;
      if (stack_size_limit < info->estimated_self_stack_size)
	stack_size_limit = info->estimated_self_stack_size;
      if (to->global.inlined_to)
        to = to->callers->caller;
    }
  while (to->global.inlined_to);

  what_info = inline_summary (what);

  if (limit < what_info->self_size)
    limit = what_info->self_size;

  limit += limit * PARAM_VALUE (PARAM_LARGE_FUNCTION_GROWTH) / 100;

  /* Check the size after inlining against the function limits.  But allow
     the function to shrink if it went over the limits by forced inlining.  */
  newsize = estimate_size_after_inlining (to, e);
  if (newsize >= info->size
      && newsize > PARAM_VALUE (PARAM_LARGE_FUNCTION_INSNS)
      && newsize > limit)
    {
      e->inline_failed = CIF_LARGE_FUNCTION_GROWTH_LIMIT;
      return false;
    }

  /* FIXME: Stack size limit often prevents inlining in Fortran programs
     due to large i/o datastructures used by the Fortran front-end.
     We ought to ignore this limit when we know that the edge is executed
     on every invocation of the caller (i.e. its call statement dominates
     exit block).  We do not track this information, yet.  */
  stack_size_limit += (stack_size_limit
		       * PARAM_VALUE (PARAM_STACK_FRAME_GROWTH) / 100);

  inlined_stack = (outer_info->stack_frame_offset
		   + outer_info->estimated_self_stack_size
		   + what_info->estimated_stack_size);
  /* Check new stack consumption with stack consumption at the place
     stack is used.  */
  if (inlined_stack > stack_size_limit
      /* If function already has large stack usage from sibling
	 inline call, we can inline, too.
	 This bit overoptimistically assume that we are good at stack
	 packing.  */
      && inlined_stack > info->estimated_stack_size
      && inlined_stack > PARAM_VALUE (PARAM_LARGE_STACK_FRAME))
    {
      e->inline_failed = CIF_LARGE_STACK_FRAME_GROWTH_LIMIT;
      return false;
    }
  return true;
}

/* Dump info about why inlining has failed.  */

static void
report_inline_failed_reason (struct cgraph_edge *e)
{
  if (dump_file)
    {
      fprintf (dump_file, "  not inlinable: %s/%i -> %s/%i, %s\n",
	       cgraph_node_name (e->caller), e->caller->uid,
	       cgraph_node_name (e->callee), e->callee->uid,
	       cgraph_inline_failed_string (e->inline_failed));
    }
}

/* Decide if we can inline the edge and possibly update
   inline_failed reason.  
   We check whether inlining is possible at all and whether
   caller growth limits allow doing so.  

   if REPORT is true, output reason to the dump file.  */

static bool
can_inline_edge_p (struct cgraph_edge *e, bool report)
{
  bool inlinable = true;
  tree caller_tree = DECL_FUNCTION_SPECIFIC_OPTIMIZATION (e->caller->decl);
  tree callee_tree = DECL_FUNCTION_SPECIFIC_OPTIMIZATION (e->callee->decl);

  gcc_assert (e->inline_failed);

  if (!e->callee->analyzed)
    {
      e->inline_failed = CIF_BODY_NOT_AVAILABLE;
      inlinable = false;
    }
  else if (!inline_summary (e->callee)->inlinable)
    {
      e->inline_failed = CIF_FUNCTION_NOT_INLINABLE;
      inlinable = false;
    }
  else if (cgraph_function_body_availability (e->callee) <= AVAIL_OVERWRITABLE)
    {
      e->inline_failed = CIF_OVERWRITABLE;
      return false;
    }
  else if (e->call_stmt_cannot_inline_p)
    {
      e->inline_failed = CIF_MISMATCHED_ARGUMENTS;
      inlinable = false;
    }
  /* Don't inline if the functions have different EH personalities.  */
  else if (DECL_FUNCTION_PERSONALITY (e->caller->decl)
	   && DECL_FUNCTION_PERSONALITY (e->callee->decl)
	   && (DECL_FUNCTION_PERSONALITY (e->caller->decl)
	       != DECL_FUNCTION_PERSONALITY (e->callee->decl)))
    {
      e->inline_failed = CIF_EH_PERSONALITY;
      inlinable = false;
    }
  /* Don't inline if the callee can throw non-call exceptions but the
     caller cannot.
     FIXME: this is obviously wrong for LTO where STRUCT_FUNCTION is missing.
     Move the flag into cgraph node or mirror it in the inline summary.  */
  else if (DECL_STRUCT_FUNCTION (e->callee->decl)
	   && DECL_STRUCT_FUNCTION (e->callee->decl)->can_throw_non_call_exceptions
	   && !(DECL_STRUCT_FUNCTION (e->caller->decl)
	        && DECL_STRUCT_FUNCTION (e->caller->decl)->can_throw_non_call_exceptions))
    {
      e->inline_failed = CIF_NON_CALL_EXCEPTIONS;
      inlinable = false;
    }
  /* Check compatibility of target optimization options.  */
  else if (!targetm.target_option.can_inline_p (e->caller->decl,
						e->callee->decl))
    {
      e->inline_failed = CIF_TARGET_OPTION_MISMATCH;
      inlinable = false;
    }
  /* Check if caller growth allows the inlining.  */
  else if (!DECL_DISREGARD_INLINE_LIMITS (e->callee->decl)
           && !caller_growth_limits (e))
    inlinable = false;
  /* Don't inline a function with a higher optimization level than the
     caller.  FIXME: this is really just tip of iceberg of handling
     optimization attribute.  */
  else if (caller_tree != callee_tree)
    {
      struct cl_optimization *caller_opt
	= TREE_OPTIMIZATION ((caller_tree)
			     ? caller_tree
			     : optimization_default_node);

      struct cl_optimization *callee_opt
	= TREE_OPTIMIZATION ((callee_tree)
			     ? callee_tree
			     : optimization_default_node);

      if ((caller_opt->x_optimize > callee_opt->x_optimize)
	  || (caller_opt->x_optimize_size != callee_opt->x_optimize_size))
	{
          e->inline_failed = CIF_TARGET_OPTIMIZATION_MISMATCH;
	  inlinable = false;
	}
    }

  /* Be sure that the cannot_inline_p flag is up to date.  */
  gcc_checking_assert (!e->call_stmt
		       || (gimple_call_cannot_inline_p (e->call_stmt)
		           == e->call_stmt_cannot_inline_p)
		       /* In -flto-partition=none mode we really keep things out of
			  sync because call_stmt_cannot_inline_p is set at cgraph
			  merging when function bodies are not there yet.  */
		       || (in_lto_p && !gimple_call_cannot_inline_p (e->call_stmt)));
  if (!inlinable && report)
    report_inline_failed_reason (e);
  return inlinable;
}


/* Return true if the edge E is inlinable during early inlining.  */

static bool
can_early_inline_edge_p (struct cgraph_edge *e)
{
  /* Early inliner might get called at WPA stage when IPA pass adds new
     function.  In this case we can not really do any of early inlining
     because function bodies are missing.  */
  if (!gimple_has_body_p (e->callee->decl))
    {
      e->inline_failed = CIF_BODY_NOT_AVAILABLE;
      return false;
    }
  /* In early inliner some of callees may not be in SSA form yet
     (i.e. the callgraph is cyclic and we did not process
     the callee by early inliner, yet).  We don't have CIF code for this
     case; later we will re-do the decision in the real inliner.  */
  if (!gimple_in_ssa_p (DECL_STRUCT_FUNCTION (e->caller->decl))
      || !gimple_in_ssa_p (DECL_STRUCT_FUNCTION (e->callee->decl)))
    {
      if (dump_file)
	fprintf (dump_file, "  edge not inlinable: not in SSA form\n");
      return false;
    }
  if (!can_inline_edge_p (e, true))
    return false;
  return true;
}


/* Return true when N is leaf function.  Accept cheap builtins
   in leaf functions.  */

static bool
leaf_node_p (struct cgraph_node *n)
{
  struct cgraph_edge *e;
  for (e = n->callees; e; e = e->next_callee)
    if (!is_inexpensive_builtin (e->callee->decl))
      return false;
  return true;
}


/* Return true if we are interested in inlining small function.  */

static bool
want_early_inline_function_p (struct cgraph_edge *e)
{
  bool want_inline = true;

  if (DECL_DISREGARD_INLINE_LIMITS (e->callee->decl))
    ;
  else if (!DECL_DECLARED_INLINE_P (e->callee->decl)
	   && !flag_inline_small_functions)
    {
      e->inline_failed = CIF_FUNCTION_NOT_INLINE_CANDIDATE;
      report_inline_failed_reason (e);
      want_inline = false;
    }
  else
    {
      int growth = estimate_edge_growth (e);
      if (growth <= 0)
	;
      else if (!cgraph_maybe_hot_edge_p (e)
	       && growth > 0)
	{
	  if (dump_file)
	    fprintf (dump_file, "  will not early inline: %s/%i->%s/%i, "
		     "call is cold and code would grow by %i\n",
		     cgraph_node_name (e->caller), e->caller->uid,
		     cgraph_node_name (e->callee), e->callee->uid,
		     growth);
	  want_inline = false;
	}
      else if (!leaf_node_p (e->callee)
	       && growth > 0)
	{
	  if (dump_file)
	    fprintf (dump_file, "  will not early inline: %s/%i->%s/%i, "
		     "callee is not leaf and code would grow by %i\n",
		     cgraph_node_name (e->caller), e->caller->uid,
		     cgraph_node_name (e->callee), e->callee->uid,
		     growth);
	  want_inline = false;
	}
      else if (growth > PARAM_VALUE (PARAM_EARLY_INLINING_INSNS))
	{
	  if (dump_file)
	    fprintf (dump_file, "  will not early inline: %s/%i->%s/%i, "
		     "growth %i exceeds --param early-inlining-insns\n",
		     cgraph_node_name (e->caller), e->caller->uid,
		     cgraph_node_name (e->callee), e->callee->uid,
		     growth);
	  want_inline = false;
	}
    }
  return want_inline;
}

/* Return true if we are interested in inlining small function.
   When REPORT is true, report reason to dump file.  */

static bool
want_inline_small_function_p (struct cgraph_edge *e, bool report)
{
  bool want_inline = true;

  if (DECL_DISREGARD_INLINE_LIMITS (e->callee->decl))
    ;
  else if (!DECL_DECLARED_INLINE_P (e->callee->decl)
	   && !flag_inline_small_functions)
    {
      e->inline_failed = CIF_FUNCTION_NOT_INLINE_CANDIDATE;
      want_inline = false;
    }
  else
    {
      int growth = estimate_edge_growth (e);

      if (growth <= 0)
	;
      else if (DECL_DECLARED_INLINE_P (e->callee->decl)
	       && growth >= MAX_INLINE_INSNS_SINGLE)
	{
          e->inline_failed = CIF_MAX_INLINE_INSNS_SINGLE_LIMIT;
	  want_inline = false;
	}
      else if (!DECL_DECLARED_INLINE_P (e->callee->decl)
	       && !flag_inline_functions)
	{
          e->inline_failed = CIF_NOT_DECLARED_INLINED;
	  want_inline = false;
	}
      else if (!DECL_DECLARED_INLINE_P (e->callee->decl)
	       && growth >= MAX_INLINE_INSNS_AUTO)
	{
          e->inline_failed = CIF_MAX_INLINE_INSNS_AUTO_LIMIT;
	  want_inline = false;
	}
      else if (!cgraph_maybe_hot_edge_p (e)
	       && estimate_growth (e->callee) > 0)
	{
          e->inline_failed = CIF_UNLIKELY_CALL;
	  want_inline = false;
	}
    }
  if (!want_inline && report)
    report_inline_failed_reason (e);
  return want_inline;
}

/* EDGE is self recursive edge.
   We hand two cases - when function A is inlining into itself
   or when function A is being inlined into another inliner copy of function
   A within function B.  

   In first case OUTER_NODE points to the toplevel copy of A, while
   in the second case OUTER_NODE points to the outermost copy of A in B.

   In both cases we want to be extra selective since
   inlining the call will just introduce new recursive calls to appear.  */

static bool
want_inline_self_recursive_call_p (struct cgraph_edge *edge,
				   struct cgraph_node *outer_node,
				   bool peeling,
				   int depth)
{
  char const *reason = NULL;
  bool want_inline = true;
  int caller_freq = CGRAPH_FREQ_BASE;
  int max_depth = PARAM_VALUE (PARAM_MAX_INLINE_RECURSIVE_DEPTH_AUTO);

  if (DECL_DECLARED_INLINE_P (edge->callee->decl))
    max_depth = PARAM_VALUE (PARAM_MAX_INLINE_RECURSIVE_DEPTH);

  if (!cgraph_maybe_hot_edge_p (edge))
    {
      reason = "recursive call is cold";
      want_inline = false;
    }
  else if (max_count && !outer_node->count)
    {
      reason = "not executed in profile";
      want_inline = false;
    }
  else if (depth > max_depth)
    {
      reason = "--param max-inline-recursive-depth exceeded.";
      want_inline = false;
    }

  if (outer_node->global.inlined_to)
    caller_freq = outer_node->callers->frequency;

  if (!want_inline)
    ;
  /* Inlining of self recursive function into copy of itself within other function
     is transformation similar to loop peeling.

     Peeling is profitable if we can inline enough copies to make probability
     of actual call to the self recursive function very small.  Be sure that
     the probability of recursion is small.

     We ensure that the frequency of recursing is at most 1 - (1/max_depth).
     This way the expected number of recision is at most max_depth.  */
  else if (peeling)
    {
      int max_prob = CGRAPH_FREQ_BASE - ((CGRAPH_FREQ_BASE + max_depth - 1)
					 / max_depth);
      int i;
      for (i = 1; i < depth; i++)
	max_prob = max_prob * max_prob / CGRAPH_FREQ_BASE;
      if (max_count
	  && (edge->count * CGRAPH_FREQ_BASE / outer_node->count
	      >= max_prob))
	{
	  reason = "profile of recursive call is too large";
	  want_inline = false;
	}
      if (!max_count
	  && (edge->frequency * CGRAPH_FREQ_BASE / caller_freq
	      >= max_prob))
	{
	  reason = "frequency of recursive call is too large";
	  want_inline = false;
	}
    }
  /* Recursive inlining, i.e. equivalent of unrolling, is profitable if recursion
     depth is large.  We reduce function call overhead and increase chances that
     things fit in hardware return predictor.

     Recursive inlining might however increase cost of stack frame setup
     actually slowing down functions whose recursion tree is wide rather than
     deep.

     Deciding reliably on when to do recursive inlining without profile feedback
     is tricky.  For now we disable recursive inlining when probability of self
     recursion is low. 

     Recursive inlining of self recursive call within loop also results in large loop
     depths that generally optimize badly.  We may want to throttle down inlining
     in those cases.  In particular this seems to happen in one of libstdc++ rb tree
     methods.  */
  else
    {
      if (max_count
	  && (edge->count * 100 / outer_node->count
	      <= PARAM_VALUE (PARAM_MIN_INLINE_RECURSIVE_PROBABILITY)))
	{
	  reason = "profile of recursive call is too small";
	  want_inline = false;
	}
      else if (!max_count
	       && (edge->frequency * 100 / caller_freq
	           <= PARAM_VALUE (PARAM_MIN_INLINE_RECURSIVE_PROBABILITY)))
	{
	  reason = "frequency of recursive call is too small";
	  want_inline = false;
	}
    }
  if (!want_inline && dump_file)
    fprintf (dump_file, "   not inlining recursively: %s\n", reason);
  return want_inline;
}


/* Decide if NODE is called once inlining it would eliminate need
   for the offline copy of function.  */

static bool
want_inline_function_called_once_p (struct cgraph_node *node)
{
   /* Already inlined?  */
   if (node->global.inlined_to)
     return false;
   /* Zero or more then one callers?  */
   if (!node->callers
       || node->callers->next_caller)
     return false;
   /* Recursive call makes no sense to inline.  */
   if (node->callers->caller == node)
     return false;
   /* External functions are not really in the unit, so inlining
      them when called once would just increase the program size.  */
   if (DECL_EXTERNAL (node->decl))
     return false;
   /* Offline body must be optimized out.  */
   if (!cgraph_will_be_removed_from_program_if_no_direct_calls (node))
     return false;
   if (!can_inline_edge_p (node->callers, true))
     return false;
   return true;
}

/* A cost model driving the inlining heuristics in a way so the edges with
   smallest badness are inlined first.  After each inlining is performed
   the costs of all caller edges of nodes affected are recomputed so the
   metrics may accurately depend on values such as number of inlinable callers
   of the function or function body size.  */

static int
edge_badness (struct cgraph_edge *edge, bool dump)
{
  gcov_type badness;
  int growth;
  struct inline_summary *callee_info = inline_summary (edge->callee);

  if (DECL_DISREGARD_INLINE_LIMITS (edge->callee->decl))
    return INT_MIN;

  growth = estimate_edge_growth (edge);

  if (dump)
    {
      fprintf (dump_file, "    Badness calculation for %s -> %s\n",
	       cgraph_node_name (edge->caller),
	       cgraph_node_name (edge->callee));
      fprintf (dump_file, "      growth %i, time %i-%i, size %i-%i\n",
	       growth,
	       callee_info->time,
	       callee_info->time_inlining_benefit
	       + edge->call_stmt_time,
	       callee_info->size,
	       callee_info->size_inlining_benefit
	       + edge->call_stmt_size);
    }

  /* Always prefer inlining saving code size.  */
  if (growth <= 0)
    {
      badness = INT_MIN - growth;
      if (dump)
	fprintf (dump_file, "      %i: Growth %i < 0\n", (int) badness,
		 growth);
    }

  /* When profiling is available, base priorities -(#calls / growth).
     So we optimize for overall number of "executed" inlined calls.  */
  else if (max_count)
    {
      badness =
	((int)
	 ((double) edge->count * INT_MIN / max_count / (max_benefit + 1)) *
	 (callee_info->time_inlining_benefit
	  + edge->call_stmt_time + 1)) / growth;
      
      /* Be sure that insanity of the profile won't lead to increasing counts
	 in the scalling and thus to overflow in the computation above.  */
      gcc_assert (max_count >= edge->count);
      if (dump)
	{
	  fprintf (dump_file,
		   "      %i (relative %f): profile info. Relative count %f"
		   " * Relative benefit %f\n",
		   (int) badness, (double) badness / INT_MIN,
		   (double) edge->count / max_count,
		   (double) (inline_summary (edge->callee)->
			     time_inlining_benefit
			     + edge->call_stmt_time + 1) / (max_benefit + 1));
	}
    }

  /* When function local profile is available, base priorities on
     growth / frequency, so we optimize for overall frequency of inlined
     calls.  This is not too accurate since while the call might be frequent
     within function, the function itself is infrequent.

     Other objective to optimize for is number of different calls inlined.
     We add the estimated growth after inlining all functions to bias the
     priorities slightly in this direction (so fewer times called functions
     of the same size gets priority).  */
  else if (flag_guess_branch_prob)
    {
      int div = edge->frequency * 100 / CGRAPH_FREQ_BASE + 1;
      int benefitperc;
      int growth_for_all;
      badness = growth * 10000;
      benefitperc =
	100 * (callee_info->time_inlining_benefit
	       + edge->call_stmt_time)
	    / (callee_info->time + 1) + 1;
      benefitperc = MIN (benefitperc, 100);
      div *= benefitperc;

      /* Decrease badness if call is nested.  */
      /* Compress the range so we don't overflow.  */
      if (div > 10000)
	div = 10000 + ceil_log2 (div) - 8;
      if (div < 1)
	div = 1;
      if (badness > 0)
	badness /= div;
      growth_for_all = estimate_growth (edge->callee);
      badness += growth_for_all;
      if (badness > INT_MAX)
	badness = INT_MAX;
      if (dump)
	{
	  fprintf (dump_file,
		   "      %i: guessed profile. frequency %i, overall growth %i,"
		   " benefit %i%%, divisor %i\n",
		   (int) badness, edge->frequency, growth_for_all,
		   benefitperc, div);
	}
    }
  /* When function local profile is not available or it does not give
     useful information (ie frequency is zero), base the cost on
     loop nest and overall size growth, so we optimize for overall number
     of functions fully inlined in program.  */
  else
    {
      int nest = MIN (edge->loop_nest, 8);
      badness = estimate_growth (edge->callee) * 256;

      /* Decrease badness if call is nested.  */
      if (badness > 0)
	badness >>= nest;
      else
	{
	  badness <<= nest;
	}
      if (dump)
	fprintf (dump_file, "      %i: no profile. nest %i\n", (int) badness,
		 nest);
    }

  /* Ensure that we did not overflow in all the fixed point math above.  */
  gcc_assert (badness >= INT_MIN);
  gcc_assert (badness <= INT_MAX - 1);
  /* Make recursive inlining happen always after other inlining is done.  */
  if (cgraph_edge_recursive_p (edge))
    return badness + 1;
  else
    return badness;
}

/* Recompute badness of EDGE and update its key in HEAP if needed.  */
static inline void
update_edge_key (fibheap_t heap, struct cgraph_edge *edge)
{
  int badness = edge_badness (edge, false);
  if (edge->aux)
    {
      fibnode_t n = (fibnode_t) edge->aux;
      gcc_checking_assert (n->data == edge);

      /* fibheap_replace_key only decrease the keys.
	 When we increase the key we do not update heap
	 and instead re-insert the element once it becomes
	 a minimum of heap.  */
      if (badness < n->key)
	{
	  fibheap_replace_key (heap, n, badness);
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file,
		       "  decreasing badness %s/%i -> %s/%i, %i to %i\n",
		       cgraph_node_name (edge->caller), edge->caller->uid,
		       cgraph_node_name (edge->callee), edge->callee->uid,
		       (int)n->key,
		       badness);
	    }
	  gcc_checking_assert (n->key == badness);
	}
    }
  else
    {
       if (dump_file && (dump_flags & TDF_DETAILS))
	 {
	   fprintf (dump_file,
		    "  enqueuing call %s/%i -> %s/%i, badness %i\n",
		    cgraph_node_name (edge->caller), edge->caller->uid,
		    cgraph_node_name (edge->callee), edge->callee->uid,
		    badness);
	 }
      edge->aux = fibheap_insert (heap, badness, edge);
    }
}

/* Recompute heap nodes for each of caller edge.  */

static void
update_caller_keys (fibheap_t heap, struct cgraph_node *node,
		    bitmap updated_nodes)
{
  struct cgraph_edge *edge;

  if (!inline_summary (node)->inlinable
      || cgraph_function_body_availability (node) <= AVAIL_OVERWRITABLE
      || node->global.inlined_to)
    return;
  if (!bitmap_set_bit (updated_nodes, node->uid))
    return;
  inline_summary (node)->estimated_growth = INT_MIN;

  /* See if there is something to do.  */
  for (edge = node->callers; edge; edge = edge->next_caller)
    if (edge->inline_failed)
      break;
  if (!edge)
    return;

  for (; edge; edge = edge->next_caller)
    if (edge->inline_failed)
      {
	if (can_inline_edge_p (edge, false)
	    && want_inline_small_function_p (edge, false))
          update_edge_key (heap, edge);
	else if (edge->aux)
	  {
	    report_inline_failed_reason (edge);
	    fibheap_delete_node (heap, (fibnode_t) edge->aux);
	    edge->aux = NULL;
	  }
      }
}

/* Recompute heap nodes for each uninlined call.
   This is used when we know that edge badnesses are going only to increase
   (we introduced new call site) and thus all we need is to insert newly
   created edges into heap.  */

static void
update_callee_keys (fibheap_t heap, struct cgraph_node *node,
		    bitmap updated_nodes)
{
  struct cgraph_edge *e = node->callees;

  inline_summary (node)->estimated_growth = INT_MIN;

  if (!e)
    return;
  while (true)
    if (!e->inline_failed && e->callee->callees)
      e = e->callee->callees;
    else
      {
	if (e->inline_failed
	    && inline_summary (e->callee)->inlinable
	    && cgraph_function_body_availability (e->callee) >= AVAIL_AVAILABLE
	    && !bitmap_bit_p (updated_nodes, e->callee->uid))
	  {
	    inline_summary (node)->estimated_growth = INT_MIN;
	    update_edge_key (heap, e);
	  }
	if (e->next_callee)
	  e = e->next_callee;
	else
	  {
	    do
	      {
		if (e->caller == node)
		  return;
		e = e->caller->callers;
	      }
	    while (!e->next_callee);
	    e = e->next_callee;
	  }
      }
}

/* Recompute heap nodes for each of caller edges of each of callees.
   Walk recursively into all inline clones.  */

static void
update_all_callee_keys (fibheap_t heap, struct cgraph_node *node,
			bitmap updated_nodes)
{
  struct cgraph_edge *e = node->callees;

  inline_summary (node)->estimated_growth = INT_MIN;

  if (!e)
    return;
  while (true)
    if (!e->inline_failed && e->callee->callees)
      e = e->callee->callees;
    else
      {
	if (e->inline_failed)
	  update_caller_keys (heap, e->callee, updated_nodes);
	if (e->next_callee)
	  e = e->next_callee;
	else
	  {
	    do
	      {
		if (e->caller == node)
		  return;
		e = e->caller->callers;
	      }
	    while (!e->next_callee);
	    e = e->next_callee;
	  }
      }
}

/* Enqueue all recursive calls from NODE into priority queue depending on
   how likely we want to recursively inline the call.  */

static void
lookup_recursive_calls (struct cgraph_node *node, struct cgraph_node *where,
			fibheap_t heap)
{
  struct cgraph_edge *e;
  for (e = where->callees; e; e = e->next_callee)
    if (e->callee == node)
      {
	/* When profile feedback is available, prioritize by expected number
	   of calls.  */
        fibheap_insert (heap,
			!max_count ? -e->frequency
		        : -(e->count / ((max_count + (1<<24) - 1) / (1<<24))),
		        e);
      }
  for (e = where->callees; e; e = e->next_callee)
    if (!e->inline_failed)
      lookup_recursive_calls (node, e->callee, heap);
}

/* Decide on recursive inlining: in the case function has recursive calls,
   inline until body size reaches given argument.  If any new indirect edges
   are discovered in the process, add them to *NEW_EDGES, unless NEW_EDGES
   is NULL.  */

static bool
recursive_inlining (struct cgraph_edge *edge,
		    VEC (cgraph_edge_p, heap) **new_edges)
{
  int limit = PARAM_VALUE (PARAM_MAX_INLINE_INSNS_RECURSIVE_AUTO);
  fibheap_t heap;
  struct cgraph_node *node;
  struct cgraph_edge *e;
  struct cgraph_node *master_clone = NULL, *next;
  int depth = 0;
  int n = 0;

  node = edge->caller;
  if (node->global.inlined_to)
    node = node->global.inlined_to;

  if (DECL_DECLARED_INLINE_P (node->decl))
    limit = PARAM_VALUE (PARAM_MAX_INLINE_INSNS_RECURSIVE);

  /* Make sure that function is small enough to be considered for inlining.  */
  if (estimate_size_after_inlining (node, edge)  >= limit)
    return false;
  heap = fibheap_new ();
  lookup_recursive_calls (node, node, heap);
  if (fibheap_empty (heap))
    {
      fibheap_delete (heap);
      return false;
    }

  if (dump_file)
    fprintf (dump_file,
	     "  Performing recursive inlining on %s\n",
	     cgraph_node_name (node));

  /* Do the inlining and update list of recursive call during process.  */
  while (!fibheap_empty (heap))
    {
      struct cgraph_edge *curr
	= (struct cgraph_edge *) fibheap_extract_min (heap);
      struct cgraph_node *cnode;

      if (estimate_size_after_inlining (node, curr) > limit)
	break;

      if (!can_inline_edge_p (curr, true))
	continue;

      depth = 1;
      for (cnode = curr->caller;
	   cnode->global.inlined_to; cnode = cnode->callers->caller)
	if (node->decl == curr->callee->decl)
	  depth++;

      if (!want_inline_self_recursive_call_p (curr, node, false, depth))
	continue;

      if (dump_file)
	{
	  fprintf (dump_file,
		   "   Inlining call of depth %i", depth);
	  if (node->count)
	    {
	      fprintf (dump_file, " called approx. %.2f times per call",
		       (double)curr->count / node->count);
	    }
	  fprintf (dump_file, "\n");
	}
      if (!master_clone)
	{
	  /* We need original clone to copy around.  */
	  master_clone = cgraph_clone_node (node, node->decl,
					    node->count, CGRAPH_FREQ_BASE, 1,
					    false, NULL);
	  for (e = master_clone->callees; e; e = e->next_callee)
	    if (!e->inline_failed)
	      cgraph_clone_inlined_nodes (e, true, false);
	}

      cgraph_redirect_edge_callee (curr, master_clone);
      cgraph_mark_inline_edge (curr, false, new_edges);
      lookup_recursive_calls (node, curr->callee, heap);
      n++;
    }

  if (!fibheap_empty (heap) && dump_file)
    fprintf (dump_file, "    Recursive inlining growth limit met.\n");
  fibheap_delete (heap);

  if (!master_clone)
    return false;

  if (dump_file)
    fprintf (dump_file,
	     "\n   Inlined %i times, "
	     "body grown from size %i to %i, time %i to %i\n", n,
	     inline_summary (master_clone)->size, inline_summary (node)->size,
	     inline_summary (master_clone)->time, inline_summary (node)->time);

  /* Remove master clone we used for inlining.  We rely that clones inlined
     into master clone gets queued just before master clone so we don't
     need recursion.  */
  for (node = cgraph_nodes; node != master_clone;
       node = next)
    {
      next = node->next;
      if (node->global.inlined_to == master_clone)
	cgraph_remove_node (node);
    }
  cgraph_remove_node (master_clone);
  return true;
}


/* Given whole compilation unit estimate of INSNS, compute how large we can
   allow the unit to grow.  */

static int
compute_max_insns (int insns)
{
  int max_insns = insns;
  if (max_insns < PARAM_VALUE (PARAM_LARGE_UNIT_INSNS))
    max_insns = PARAM_VALUE (PARAM_LARGE_UNIT_INSNS);

  return ((HOST_WIDEST_INT) max_insns
	  * (100 + PARAM_VALUE (PARAM_INLINE_UNIT_GROWTH)) / 100);
}


/* Compute badness of all edges in NEW_EDGES and add them to the HEAP.  */

static void
add_new_edges_to_heap (fibheap_t heap, VEC (cgraph_edge_p, heap) *new_edges)
{
  while (VEC_length (cgraph_edge_p, new_edges) > 0)
    {
      struct cgraph_edge *edge = VEC_pop (cgraph_edge_p, new_edges);

      gcc_assert (!edge->aux);
      if (inline_summary (edge->callee)->inlinable
	  && edge->inline_failed
	  && can_inline_edge_p (edge, true)
	  && want_inline_small_function_p (edge, true))
        edge->aux = fibheap_insert (heap, edge_badness (edge, false), edge);
    }
}


/* We use greedy algorithm for inlining of small functions:
   All inline candidates are put into prioritized heap ordered in
   increasing badness.

   The inlining of small functions is bounded by unit growth parameters.  */

static void
inline_small_functions (void)
{
  struct cgraph_node *node;
  struct cgraph_edge *edge;
  fibheap_t heap = fibheap_new ();
  bitmap updated_nodes = BITMAP_ALLOC (NULL);
  int min_size, max_size;
  VEC (cgraph_edge_p, heap) *new_indirect_edges = NULL;
  int initial_size = 0;

  if (flag_indirect_inlining)
    new_indirect_edges = VEC_alloc (cgraph_edge_p, heap, 8);

  if (dump_file)
    fprintf (dump_file,
	     "\nDeciding on inlining of small functions.  Starting with size %i.\n",
	     initial_size);

  /* Populate the heeap with all edges we might inline.
     While doing so compute overall unit size and other global
     parameters used by badness metrics.  */

  max_count = 0;
  max_benefit = 0;
  for (node = cgraph_nodes; node; node = node->next)
    if (node->analyzed
	&& !node->global.inlined_to)
      {
	struct inline_summary *info = inline_summary (node);

	if (dump_file)
	  fprintf (dump_file, "Enqueueing calls of %s/%i.\n",
		   cgraph_node_name (node), node->uid);

	info->estimated_growth = INT_MIN;

	if (!DECL_EXTERNAL (node->decl))
	  initial_size += info->size;

	for (edge = node->callers; edge; edge = edge->next_caller)
	  {
	    int benefit = (info->time_inlining_benefit
			   + edge->call_stmt_time);
	    if (max_count < edge->count)
	      max_count = edge->count;
	    if (max_benefit < benefit)
	      max_benefit = benefit;
	    if (edge->inline_failed
		&& can_inline_edge_p (edge, true)
		&& want_inline_small_function_p (edge, true)
		&& edge->inline_failed)
	      {
		gcc_assert (!edge->aux);
		update_edge_key (heap, edge);
	      }
	   }
      }

  overall_size = initial_size;
  max_size = compute_max_insns (overall_size);
  min_size = overall_size;
  gcc_assert (in_lto_p
	      || !max_count
	      || (profile_info && flag_branch_probabilities));

  while (!fibheap_empty (heap))
    {
      int old_size = overall_size;
      struct cgraph_node *where, *callee;
      int badness = fibheap_min_key (heap);
      int current_badness;
      int growth;

      edge = (struct cgraph_edge *) fibheap_extract_min (heap);
      gcc_assert (edge->aux);
      edge->aux = NULL;
      if (!edge->inline_failed)
	continue;

      /* When updating the edge costs, we only decrease badness in the keys.
	 Increases of badness are handled lazilly; when we see key with out
	 of date value on it, we re-insert it now.  */
      current_badness = edge_badness (edge, false);
      gcc_assert (current_badness >= badness);
      if (current_badness != badness)
	{
	  edge->aux = fibheap_insert (heap, current_badness, edge);
	  continue;
	}

      if (!can_inline_edge_p (edge, true))
	continue;
      
      callee = edge->callee;
      growth = estimate_edge_growth (edge);
      if (dump_file)
	{
	  fprintf (dump_file,
		   "\nConsidering %s with %i size\n",
		   cgraph_node_name (edge->callee),
		   inline_summary (edge->callee)->size);
	  fprintf (dump_file,
		   " to be inlined into %s in %s:%i\n"
		   " Estimated growth after inlined into all is %+i insns.\n"
		   " Estimated badness is %i, frequency %.2f.\n",
		   cgraph_node_name (edge->caller),
		   flag_wpa ? "unknown"
		   : gimple_filename ((const_gimple) edge->call_stmt),
		   flag_wpa ? -1
		   : gimple_lineno ((const_gimple) edge->call_stmt),
		   estimate_growth (edge->callee),
		   badness,
		   edge->frequency / (double)CGRAPH_FREQ_BASE);
	  if (edge->count)
	    fprintf (dump_file," Called "HOST_WIDEST_INT_PRINT_DEC"x\n",
		     edge->count);
	  if (dump_flags & TDF_DETAILS)
	    edge_badness (edge, true);
	}

      if (overall_size + growth > max_size
	  && !DECL_DISREGARD_INLINE_LIMITS (edge->callee->decl))
	{
	  edge->inline_failed = CIF_INLINE_UNIT_GROWTH_LIMIT;
	  report_inline_failed_reason (edge);
	  continue;
	}

      if (!want_inline_small_function_p (edge, true))
	continue;

      /* Heuristics for inlining small functions works poorly for
	 recursive calls where we do efect similar to loop unrolling.
	 When inliing such edge seems profitable, leave decision on
	 specific inliner.  */
      if (cgraph_edge_recursive_p (edge))
	{
	  where = edge->caller;
	  if (where->global.inlined_to)
	    where = where->global.inlined_to;
	  if (!recursive_inlining (edge,
				   flag_indirect_inlining
				   ? &new_indirect_edges : NULL))
	    {
	      edge->inline_failed = CIF_RECURSIVE_INLINING;
	      continue;
	    }
	  /* Recursive inliner inlines all recursive calls of the function
	     at once. Consequently we need to update all callee keys.  */
	  if (flag_indirect_inlining)
	    add_new_edges_to_heap (heap, new_indirect_edges);
          update_all_callee_keys (heap, where, updated_nodes);
	}
      else
	{
	  struct cgraph_node *callee;
	  struct cgraph_node *outer_node = NULL;
	  int depth = 0;

	  /* Consider the case where self recursive function A is inlined into B.
	     This is desired optimization in some cases, since it leads to effect
	     similar of loop peeling and we might completely optimize out the
	     recursive call.  However we must be extra selective.  */

	  where = edge->caller;
	  while (where->global.inlined_to)
	    {
	      if (where->decl == edge->callee->decl)
		outer_node = where, depth++;
	      where = where->callers->caller;
	    }
	  if (outer_node
	      && !want_inline_self_recursive_call_p (edge, outer_node,
						     true, depth))
	    {
	      edge->inline_failed
		= (DECL_DISREGARD_INLINE_LIMITS (edge->callee->decl)
		   ? CIF_RECURSIVE_INLINING : CIF_UNSPECIFIED);
	      continue;
	    }
	  else if (depth && dump_file)
	    fprintf (dump_file, " Peeling recursion with depth %i\n", depth);

	  callee = edge->callee;
	  gcc_checking_assert (!callee->global.inlined_to);
	  cgraph_mark_inline_edge (edge, true, &new_indirect_edges);
	  if (flag_indirect_inlining)
	    add_new_edges_to_heap (heap, new_indirect_edges);

	  /* We inlined last offline copy to the body.  This might lead
	     to callees of function having fewer call sites and thus they
	     may need updating.  */
	  if (callee->global.inlined_to)
	    update_all_callee_keys (heap, callee, updated_nodes);
	  else
	    update_callee_keys (heap, edge->callee, updated_nodes);
	}
      where = edge->caller;
      if (where->global.inlined_to)
	where = where->global.inlined_to;

      /* Our profitability metric can depend on local properties
	 such as number of inlinable calls and size of the function body.
	 After inlining these properties might change for the function we
	 inlined into (since it's body size changed) and for the functions
	 called by function we inlined (since number of it inlinable callers
	 might change).  */
      update_caller_keys (heap, where, updated_nodes);

      /* We removed one call of the function we just inlined.  If offline
	 copy is still needed, be sure to update the keys.  */
      if (callee != where && !callee->global.inlined_to)
        update_caller_keys (heap, callee, updated_nodes);
      bitmap_clear (updated_nodes);

      if (dump_file)
	{
	  fprintf (dump_file,
		   " Inlined into %s which now has time %i and size %i,"
		   "net change of %+i.\n",
		   cgraph_node_name (edge->caller),
		   inline_summary (edge->caller)->time,
		   inline_summary (edge->caller)->size,
		   overall_size - old_size);
	}
      if (min_size > overall_size)
	{
	  min_size = overall_size;
	  max_size = compute_max_insns (min_size);

	  if (dump_file)
	    fprintf (dump_file, "New minimal size reached: %i\n", min_size);
	}
    }

  if (new_indirect_edges)
    VEC_free (cgraph_edge_p, heap, new_indirect_edges);
  fibheap_delete (heap);
  if (dump_file)
    fprintf (dump_file,
	     "Unit growth for small function inlining: %i->%i (%i%%)\n",
	     overall_size, initial_size,
	     overall_size * 100 / (initial_size + 1) - 100);
  BITMAP_FREE (updated_nodes);
}

/* Flatten NODE.  Performed both during early inlining and
   at IPA inlining time.  */

static void
flatten_function (struct cgraph_node *node)
{
  struct cgraph_edge *e;

  /* We shouldn't be called recursively when we are being processed.  */
  gcc_assert (node->aux == NULL);

  node->aux = (void *) node;

  for (e = node->callees; e; e = e->next_callee)
    {
      struct cgraph_node *orig_callee;

      /* We've hit cycle?  It is time to give up.  */
      if (e->callee->aux)
	{
	  if (dump_file)
	    fprintf (dump_file,
		     "Not inlining %s into %s to avoid cycle.\n",
		     cgraph_node_name (e->callee),
		     cgraph_node_name (e->caller));
	  e->inline_failed = CIF_RECURSIVE_INLINING;
	  continue;
	}

      /* When the edge is already inlined, we just need to recurse into
	 it in order to fully flatten the leaves.  */
      if (!e->inline_failed)
	{
	  flatten_function (e->callee);
	  continue;
	}

      /* Flatten attribute needs to be processed during late inlining. For
	 extra code quality we however do flattening during early optimization,
	 too.  */
      if (cgraph_state != CGRAPH_STATE_IPA_SSA
	  ? !can_inline_edge_p (e, true)
	  : !can_early_inline_edge_p (e))
	continue;

      if (cgraph_edge_recursive_p (e))
	{
	  if (dump_file)
	    fprintf (dump_file, "Not inlining: recursive call.\n");
	  continue;
	}

      if (gimple_in_ssa_p (DECL_STRUCT_FUNCTION (node->decl))
	  != gimple_in_ssa_p (DECL_STRUCT_FUNCTION (e->callee->decl)))
	{
	  if (dump_file)
	    fprintf (dump_file, "Not inlining: SSA form does not match.\n");
	  continue;
	}

      /* Inline the edge and flatten the inline clone.  Avoid
         recursing through the original node if the node was cloned.  */
      if (dump_file)
	fprintf (dump_file, " Inlining %s into %s.\n",
		 cgraph_node_name (e->callee),
		 cgraph_node_name (e->caller));
      orig_callee = e->callee;
      cgraph_mark_inline_edge (e, true, NULL);
      if (e->callee != orig_callee)
	orig_callee->aux = (void *) node;
      flatten_function (e->callee);
      if (e->callee != orig_callee)
	orig_callee->aux = NULL;
    }

  node->aux = NULL;
}

/* Decide on the inlining.  We do so in the topological order to avoid
   expenses on updating data structures.  */

static unsigned int
ipa_inline (void)
{
  struct cgraph_node *node;
  int nnodes;
  struct cgraph_node **order =
    XCNEWVEC (struct cgraph_node *, cgraph_n_nodes);
  int i;

  if (in_lto_p && flag_indirect_inlining)
    ipa_update_after_lto_read ();
  if (flag_indirect_inlining)
    ipa_create_all_structures_for_iinln ();

  if (dump_file)
    dump_inline_summaries (dump_file);

  nnodes = cgraph_postorder (order);

  for (node = cgraph_nodes; node; node = node->next)
    node->aux = 0;

  if (dump_file)
    fprintf (dump_file, "\nFlattening functions:\n");

  /* In the first pass handle functions to be flattened.  Do this with
     a priority so none of our later choices will make this impossible.  */
  for (i = nnodes - 1; i >= 0; i--)
    {
      node = order[i];

      /* Handle nodes to be flattened.
	 Ideally when processing callees we stop inlining at the
	 entry of cycles, possibly cloning that entry point and
	 try to flatten itself turning it into a self-recursive
	 function.  */
      if (lookup_attribute ("flatten",
			    DECL_ATTRIBUTES (node->decl)) != NULL)
	{
	  if (dump_file)
	    fprintf (dump_file,
		     "Flattening %s\n", cgraph_node_name (node));
	  flatten_function (node);
	}
    }

  inline_small_functions ();
  cgraph_remove_unreachable_nodes (true, dump_file);
  free (order);

  /* We already perform some inlining of functions called once during
     inlining small functions above.  After unreachable nodes are removed,
     we still might do a quick check that nothing new is found.  */
  if (flag_inline_functions_called_once)
    {
      int cold;
      if (dump_file)
	fprintf (dump_file, "\nDeciding on functions called once:\n");

      /* Inlining one function called once has good chance of preventing
	 inlining other function into the same callee.  Ideally we should
	 work in priority order, but probably inlining hot functions first
	 is good cut without the extra pain of maintaining the queue.

	 ??? this is not really fitting the bill perfectly: inlining function
	 into callee often leads to better optimization of callee due to
	 increased context for optimization.
	 For example if main() function calls a function that outputs help
	 and then function that does the main optmization, we should inline
	 the second with priority even if both calls are cold by themselves.

	 We probably want to implement new predicate replacing our use of
	 maybe_hot_edge interpreted as maybe_hot_edge || callee is known
	 to be hot.  */
      for (cold = 0; cold <= 1; cold ++)
	{
	  for (node = cgraph_nodes; node; node = node->next)
	    {
	      if (want_inline_function_called_once_p (node)
		  && (cold
		      || cgraph_maybe_hot_edge_p (node->callers)))
		{
		  struct cgraph_node *caller = node->callers->caller;

		  if (dump_file)
		    {
		      fprintf (dump_file,
			       "\nInlining %s size %i.\n",
			       cgraph_node_name (node), inline_summary (node)->size);
		      fprintf (dump_file,
			       " Called once from %s %i insns.\n",
			       cgraph_node_name (node->callers->caller),
			       inline_summary (node->callers->caller)->size);
		    }

		  cgraph_mark_inline_edge (node->callers, true, NULL);
		  if (dump_file)
		    fprintf (dump_file,
			     " Inlined into %s which now has %i size\n",
			     cgraph_node_name (caller),
			     inline_summary (caller)->size);
		}
	    }
	}
    }

  /* Free ipa-prop structures if they are no longer needed.  */
  if (flag_indirect_inlining)
    ipa_free_all_structures_after_iinln ();

  if (dump_file)
    fprintf (dump_file,
	     "\nInlined %i calls, eliminated %i functions\n\n",
	     ncalls_inlined, nfunctions_inlined);

  /* In WPA we use inline summaries for partitioning process.  */
  if (!flag_wpa)
    inline_free_summary ();
  return 0;
}

/* Inline always-inline function calls in NODE.  */

static bool
inline_always_inline_functions (struct cgraph_node *node)
{
  struct cgraph_edge *e;
  bool inlined = false;

  for (e = node->callees; e; e = e->next_callee)
    {
      if (!DECL_DISREGARD_INLINE_LIMITS (e->callee->decl))
	continue;

      if (cgraph_edge_recursive_p (e))
	{
	  if (dump_file)
	    fprintf (dump_file, "  Not inlining recursive call to %s.\n",
		     cgraph_node_name (e->callee));
	  e->inline_failed = CIF_RECURSIVE_INLINING;
	  continue;
	}

      if (!can_early_inline_edge_p (e))
	continue;

      if (dump_file)
	fprintf (dump_file, "  Inlining %s into %s (always_inline).\n",
		 cgraph_node_name (e->callee),
		 cgraph_node_name (e->caller));
      cgraph_mark_inline_edge (e, true, NULL);
      inlined = true;
    }

  return inlined;
}

/* Decide on the inlining.  We do so in the topological order to avoid
   expenses on updating data structures.  */

static bool
early_inline_small_functions (struct cgraph_node *node)
{
  struct cgraph_edge *e;
  bool inlined = false;

  for (e = node->callees; e; e = e->next_callee)
    {
      if (!inline_summary (e->callee)->inlinable
	  || !e->inline_failed)
	continue;

      /* Do not consider functions not declared inline.  */
      if (!DECL_DECLARED_INLINE_P (e->callee->decl)
	  && !flag_inline_small_functions
	  && !flag_inline_functions)
	continue;

      if (dump_file)
	fprintf (dump_file, "Considering inline candidate %s.\n",
		 cgraph_node_name (e->callee));

      if (!can_early_inline_edge_p (e))
	continue;

      if (cgraph_edge_recursive_p (e))
	{
	  if (dump_file)
	    fprintf (dump_file, "  Not inlining: recursive call.\n");
	  continue;
	}

      if (!want_early_inline_function_p (e))
	continue;

      if (dump_file)
	fprintf (dump_file, " Inlining %s into %s.\n",
		 cgraph_node_name (e->callee),
		 cgraph_node_name (e->caller));
      cgraph_mark_inline_edge (e, true, NULL);
      inlined = true;
    }

  return inlined;
}

/* Do inlining of small functions.  Doing so early helps profiling and other
   passes to be somewhat more effective and avoids some code duplication in
   later real inlining pass for testcases with very many function calls.  */
static unsigned int
early_inliner (void)
{
  struct cgraph_node *node = cgraph_get_node (current_function_decl);
  struct cgraph_edge *edge;
  unsigned int todo = 0;
  int iterations = 0;
  bool inlined = false;

  if (seen_error ())
    return 0;

#ifdef ENABLE_CHECKING
  verify_cgraph_node (node);
#endif

  /* Even when not optimizing or not inlining inline always-inline
     functions.  */
  inlined = inline_always_inline_functions (node);

  if (!optimize
      || flag_no_inline
      || !flag_early_inlining
      /* Never inline regular functions into always-inline functions
	 during incremental inlining.  This sucks as functions calling
	 always inline functions will get less optimized, but at the
	 same time inlining of functions calling always inline
	 function into an always inline function might introduce
	 cycles of edges to be always inlined in the callgraph.

	 We might want to be smarter and just avoid this type of inlining.  */
      || DECL_DISREGARD_INLINE_LIMITS (node->decl))
    ;
  else if (lookup_attribute ("flatten",
			     DECL_ATTRIBUTES (node->decl)) != NULL)
    {
      /* When the function is marked to be flattened, recursively inline
	 all calls in it.  */
      if (dump_file)
	fprintf (dump_file,
		 "Flattening %s\n", cgraph_node_name (node));
      flatten_function (node);
      inlined = true;
    }
  else
    {
      /* We iterate incremental inlining to get trivial cases of indirect
	 inlining.  */
      while (iterations < PARAM_VALUE (PARAM_EARLY_INLINER_MAX_ITERATIONS)
	     && early_inline_small_functions (node))
	{
	  timevar_push (TV_INTEGRATION);
	  todo |= optimize_inline_calls (current_function_decl);

	  /* Technically we ought to recompute inline parameters so the new
 	     iteration of early inliner works as expected.  We however have
	     values approximately right and thus we only need to update edge
	     info that might be cleared out for newly discovered edges.  */
	  for (edge = node->callees; edge; edge = edge->next_callee)
	    {
	      edge->call_stmt_size
		= estimate_num_insns (edge->call_stmt, &eni_size_weights);
	      edge->call_stmt_time
		= estimate_num_insns (edge->call_stmt, &eni_time_weights);
	    }
	  timevar_pop (TV_INTEGRATION);
	  iterations++;
	  inlined = false;
	}
      if (dump_file)
	fprintf (dump_file, "Iterations: %i\n", iterations);
    }

  if (inlined)
    {
      timevar_push (TV_INTEGRATION);
      todo |= optimize_inline_calls (current_function_decl);
      timevar_pop (TV_INTEGRATION);
    }

  cfun->always_inline_functions_inlined = true;

  return todo;
}

struct gimple_opt_pass pass_early_inline =
{
 {
  GIMPLE_PASS,
  "einline",	 			/* name */
  NULL,					/* gate */
  early_inliner,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_INLINE_HEURISTICS,			/* tv_id */
  PROP_ssa,                             /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func    			/* todo_flags_finish */
 }
};


/* Apply inline plan to function.  */
static unsigned int
inline_transform (struct cgraph_node *node)
{
  unsigned int todo = 0;
  struct cgraph_edge *e;
  bool inline_p = false;

  /* FIXME: Currently the pass manager is adding inline transform more than
     once to some clones.  This needs revisiting after WPA cleanups.  */
  if (cfun->after_inlining)
    return 0;

  /* We might need the body of this function so that we can expand
     it inline somewhere else.  */
  if (cgraph_preserve_function_body_p (node->decl))
    save_inline_function_body (node);

  for (e = node->callees; e; e = e->next_callee)
    {
      cgraph_redirect_edge_call_stmt_to_callee (e);
      if (!e->inline_failed || warn_inline)
        inline_p = true;
    }

  if (inline_p)
    {
      timevar_push (TV_INTEGRATION);
      todo = optimize_inline_calls (current_function_decl);
      timevar_pop (TV_INTEGRATION);
    }
  cfun->always_inline_functions_inlined = true;
  cfun->after_inlining = true;
  return todo | execute_fixup_cfg ();
}

/* When to run IPA inlining.  Inlining of always-inline functions
   happens during early inlining.  */

static bool
gate_ipa_inline (void)
{
  /* ???  We'd like to skip this if not optimizing or not inlining as
     all always-inline functions have been processed by early
     inlining already.  But this at least breaks EH with C++ as
     we need to unconditionally run fixup_cfg even at -O0.
     So leave it on unconditionally for now.  */
  return 1;
}

struct ipa_opt_pass_d pass_ipa_inline =
{
 {
  IPA_PASS,
  "inline",				/* name */
  gate_ipa_inline,			/* gate */
  ipa_inline,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_INLINE_HEURISTICS,			/* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  TODO_remove_functions,		/* todo_flags_finish */
  TODO_dump_cgraph | TODO_dump_func
  | TODO_remove_functions | TODO_ggc_collect	/* todo_flags_finish */
 },
 inline_generate_summary,		/* generate_summary */
 inline_write_summary,			/* write_summary */
 inline_read_summary,			/* read_summary */
 NULL,					/* write_optimization_summary */
 NULL,					/* read_optimization_summary */
 NULL,					/* stmt_fixup */
 0,					/* TODOs */
 inline_transform,			/* function_transform */
 NULL,					/* variable_transform */
};

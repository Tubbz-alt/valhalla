/*
 * Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "memory/allocation.inline.hpp"
#include "opto/mulnode.hpp"
#include "opto/addnode.hpp"
#include "opto/connode.hpp"
#include "opto/convertnode.hpp"
#include "opto/loopnode.hpp"
#include "opto/opaquenode.hpp"
#include "opto/rootnode.hpp"

//================= Loop Unswitching =====================
//
// orig:                       transformed:
//                               if (invariant-test) then
//  predicate                      predicate
//  loop                           loop
//    stmt1                          stmt1
//    if (invariant-test) then       stmt2
//      stmt2                        stmt4
//    else                         endloop
//      stmt3                    else
//    endif                        predicate [clone]
//    stmt4                        loop [clone]
//  endloop                          stmt1 [clone]
//                                   stmt3
//                                   stmt4 [clone]
//                                 endloop
//                               endif
//
// Note: the "else" clause may be empty

static bool is_flattened_array_check(Node* iff, PhaseTransform* phase) {
  if (iff->Opcode() != Op_If) {
    return false;
  }
  Node* bol = iff->in(1);
  if (!bol->is_Bool() || bol->as_Bool()->_test._test != BoolTest::ne) {
    return false;
  }
  Node* cmp = bol->in(1);
  if (cmp->Opcode() != Op_CmpI) {
    return false;
  }
  Node* cmp_in1 = cmp->in(1);
  Node* cmp_in2 = cmp->in(2);
  if ((unsigned int)cmp_in2->find_int_con(0) != Klass::_lh_array_tag_vt_value) {
    return false;
  }
  if (cmp_in1->Opcode() != Op_RShiftI) {
    return false;
  }
  Node* shift_in1 = cmp_in1->in(1);
  Node* shift_in2 = cmp_in1->in(2);
  if ((unsigned int)shift_in2->find_int_con(0) != Klass::_lh_array_tag_shift) {
    return false;
  }
  if (shift_in1->Opcode() != Op_LoadI) {
    return false;
  }
  intptr_t offset;
  Node* addr = AddPNode::Ideal_base_and_offset(shift_in1->in(MemNode::Address), phase, offset);
  if (addr == NULL || offset != in_bytes(Klass::layout_helper_offset())) {
    return false;
  }
  if (!phase->type(addr)->isa_klassptr()) {
    return false;
  }

  return true;
}

//------------------------------policy_unswitching-----------------------------
// Return TRUE or FALSE if the loop should be unswitched
// (ie. clone loop with an invariant test that does not exit the loop)
bool IdealLoopTree::policy_unswitching( PhaseIdealLoop *phase ) const {
  if (!LoopUnswitching) {
    return false;
  }
  if (!_head->is_Loop()) {
    return false;
  }

  // If nodes are depleted, some transform has miscalculated its needs.
  assert(!phase->exceeding_node_budget(), "sanity");

  // check for vectorized loops, any unswitching was already applied
  if (_head->is_CountedLoop() && _head->as_CountedLoop()->is_unroll_only()) {
    return false;
  }

  LoopNode* head = _head->as_Loop();
  if (head->unswitch_count() + 1 > head->unswitch_max()) {
    return false;
  }

  if (head->is_flattened_arrays()) {
    return false;
  }

  Node_List flattened_checks;
  if (phase->find_unswitching_candidate(this, flattened_checks) == NULL && flattened_checks.size() == 0) {
    return false;
  }

  // Too speculative if running low on nodes.
  return phase->may_require_nodes(est_loop_clone_sz(2));
}

//------------------------------find_unswitching_candidate-----------------------------
// Find candidate "if" for unswitching
IfNode* PhaseIdealLoop::find_unswitching_candidate(const IdealLoopTree *loop, Node_List& flattened_checks) const {

  // Find first invariant test that doesn't exit the loop
  LoopNode *head = loop->_head->as_Loop();
  IfNode* unswitch_iff = NULL;
  Node* n = head->in(LoopNode::LoopBackControl);
  while (n != head) {
    Node* n_dom = idom(n);
    if (n->is_Region()) {
      if (n_dom->is_If()) {
        IfNode* iff = n_dom->as_If();
        if (iff->in(1)->is_Bool()) {
          BoolNode* bol = iff->in(1)->as_Bool();
          if (bol->in(1)->is_Cmp()) {
            // If condition is invariant and not a loop exit,
            // then found reason to unswitch.
            if (loop->is_invariant(bol) && !loop->is_loop_exit(iff)) {
              unswitch_iff = iff;
            }
          }
        }
      }
    }
    n = n_dom;
  }

  if (unswitch_iff == NULL || is_flattened_array_check(unswitch_iff, &_igvn)) {
    // collect all flattened array checks
    for (uint i = 0; i < loop->_body.size(); i++) {
      Node* n = loop->_body.at(i);
      if (is_flattened_array_check(n, &_igvn) &&
          loop->is_invariant(n->in(1)) &&
          !loop->is_loop_exit(n)) {
        flattened_checks.push(n);
      }
    }
    unswitch_iff = NULL;
  }

  return unswitch_iff;
}

//------------------------------do_unswitching-----------------------------
// Clone loop with an invariant test (that does not exit) and
// insert a clone of the test that selects which version to
// execute.
void PhaseIdealLoop::do_unswitching(IdealLoopTree *loop, Node_List &old_new) {

  // Find first invariant test that doesn't exit the loop
  LoopNode *head = loop->_head->as_Loop();

  Node_List flattened_checks;
  IfNode* unswitch_iff = find_unswitching_candidate((const IdealLoopTree *)loop, flattened_checks);
  assert(unswitch_iff != NULL || flattened_checks.size() > 0, "should be at least one");
  if (unswitch_iff == NULL) {
    unswitch_iff = flattened_checks.at(0)->as_If();
  }

#ifndef PRODUCT
  if (TraceLoopOpts) {
    tty->print("Unswitch   %d ", head->unswitch_count()+1);
    loop->dump_head();
  }
#endif

  // Need to revert back to normal loop
  if (head->is_CountedLoop() && !head->as_CountedLoop()->is_normal_loop()) {
    head->as_CountedLoop()->set_normal_loop();
  }

  ProjNode* proj_true = create_slow_version_of_loop(loop, old_new, unswitch_iff->Opcode(), CloneIncludesStripMined);

#ifdef ASSERT
  Node* uniqc = proj_true->unique_ctrl_out();
  Node* entry = head->skip_strip_mined()->in(LoopNode::EntryControl);
  Node* predicate = find_predicate(entry);
  if (predicate != NULL) {
    entry = skip_loop_predicates(entry);
  }
  if (predicate != NULL && UseLoopPredicate) {
    // We may have two predicates, find first.
    Node* n = find_predicate(entry);
    if (n != NULL) {
      predicate = n;
      entry = skip_loop_predicates(entry);
    }
  }
  if (predicate != NULL && UseProfiledLoopPredicate) {
    entry = find_predicate(entry);
    if (entry != NULL) predicate = entry;
  }
  if (predicate != NULL) predicate = predicate->in(0);
  assert(proj_true->is_IfTrue() &&
         (predicate == NULL && uniqc == head && !head->is_strip_mined() ||
          predicate == NULL && uniqc == head->in(LoopNode::EntryControl) && head->is_strip_mined() ||
          predicate != NULL && uniqc == predicate), "by construction");
#endif
  // Increment unswitch count
  LoopNode* head_clone = old_new[head->_idx]->as_Loop();
  int nct = head->unswitch_count() + 1;
  head->set_unswitch_count(nct);
  head_clone->set_unswitch_count(nct);
  head_clone->mark_flattened_arrays();

  // Add test to new "if" outside of loop
  IfNode* invar_iff   = proj_true->in(0)->as_If();
  Node* invar_iff_c   = invar_iff->in(0);
  invar_iff->_prob    = unswitch_iff->_prob;
  if (flattened_checks.size() > 0) {
    // Flattened array checks are used in
    // Parse::array_store()/Parse::array_load() to switch between a
    // legacy object array access and a flattened value array
    // access. We want the performance impact on legacy accesses to be
    // as small as possible so we make 2 copies of the loops: a fast
    // one where all accesses are known to be legacy, a slow one where
    // some accesses are to flattened arrays. Flattened array checks
    // can be removed from the first one but not from the second one
    // as it can have a mix of flattened/legacy accesses.
    BoolNode* bol       = unswitch_iff->in(1)->clone()->as_Bool();
    register_new_node(bol, invar_iff->in(0));
    Node* cmp = bol->in(1)->clone();
    register_new_node(cmp, invar_iff->in(0));
    bol->set_req(1, cmp);
    Node* in1 = NULL;
    for (uint i = 0; i < flattened_checks.size(); i++) {
      Node* v = flattened_checks.at(i)->in(1)->in(1)->in(1);
      v = new AndINode(v, _igvn.intcon(Klass::_lh_array_tag_vt_value));
      register_new_node(v, invar_iff->in(0));
      if (in1 == NULL) {
        in1 = v;
      } else {
        in1 = new OrINode(in1, v);
        register_new_node(in1, invar_iff->in(0));
      }
    }
    cmp->set_req(1, in1);
    invar_iff->set_req(1, bol);
  } else {
    BoolNode* bol       = unswitch_iff->in(1)->as_Bool();
    invar_iff->set_req(1, bol);
  }

  ProjNode* proj_false = invar_iff->proj_out(0)->as_Proj();

  // Hoist invariant casts out of each loop to the appropriate
  // control projection.

  Node_List worklist;

  for (DUIterator_Fast imax, i = unswitch_iff->fast_outs(imax); i < imax; i++) {
    ProjNode* proj= unswitch_iff->fast_out(i)->as_Proj();
    // Copy to a worklist for easier manipulation
    for (DUIterator_Fast jmax, j = proj->fast_outs(jmax); j < jmax; j++) {
      Node* use = proj->fast_out(j);
      if (use->Opcode() == Op_CheckCastPP && loop->is_invariant(use->in(1))) {
        worklist.push(use);
      }
    }
    ProjNode* invar_proj = invar_iff->proj_out(proj->_con)->as_Proj();
    while (worklist.size() > 0) {
      Node* use = worklist.pop();
      Node* nuse = use->clone();
      nuse->set_req(0, invar_proj);
      _igvn.replace_input_of(use, 1, nuse);
      register_new_node(nuse, invar_proj);
      // Same for the clone
      Node* use_clone = old_new[use->_idx];
      _igvn.replace_input_of(use_clone, 1, nuse);
    }
  }

  IfNode* unswitch_iff_clone = old_new[unswitch_iff->_idx]->as_If();
  if (flattened_checks.size() > 0) {
    for (uint i = 0; i < flattened_checks.size(); i++) {
      IfNode* iff = flattened_checks.at(i)->as_If();
      _igvn.rehash_node_delayed(iff);
      short_circuit_if(iff, proj_true);
    }
  } else {
    // Hardwire the control paths in the loops into if(true) and if(false)
    _igvn.rehash_node_delayed(unswitch_iff);
    short_circuit_if(unswitch_iff, proj_true);

    _igvn.rehash_node_delayed(unswitch_iff_clone);
    short_circuit_if(unswitch_iff_clone, proj_false);
  }

  // Reoptimize loops
  loop->record_for_igvn();
  for(int i = loop->_body.size() - 1; i >= 0 ; i--) {
    Node *n = loop->_body[i];
    Node *n_clone = old_new[n->_idx];
    _igvn._worklist.push(n_clone);
  }

#ifndef PRODUCT
  if (TraceLoopUnswitching) {
    tty->print_cr("Loop unswitching orig: %d @ %d  new: %d @ %d",
                  head->_idx,                unswitch_iff->_idx,
                  old_new[head->_idx]->_idx, unswitch_iff_clone->_idx);
  }
#endif

  C->set_major_progress();
}

//-------------------------create_slow_version_of_loop------------------------
// Create a slow version of the loop by cloning the loop
// and inserting an if to select fast-slow versions.
// Return control projection of the entry to the fast version.
ProjNode* PhaseIdealLoop::create_slow_version_of_loop(IdealLoopTree *loop,
                                                      Node_List &old_new,
                                                      int opcode,
                                                      CloneLoopMode mode) {
  LoopNode* head  = loop->_head->as_Loop();
  bool counted_loop = head->is_CountedLoop();
  Node*     entry = head->skip_strip_mined()->in(LoopNode::EntryControl);
  _igvn.rehash_node_delayed(entry);
  IdealLoopTree* outer_loop = loop->_parent;

  head->verify_strip_mined(1);

  Node *cont      = _igvn.intcon(1);
  set_ctrl(cont, C->root());
  Node* opq       = new Opaque1Node(C, cont);
  register_node(opq, outer_loop, entry, dom_depth(entry));
  Node *bol       = new Conv2BNode(opq);
  register_node(bol, outer_loop, entry, dom_depth(entry));
  IfNode* iff = (opcode == Op_RangeCheck) ? new RangeCheckNode(entry, bol, PROB_MAX, COUNT_UNKNOWN) :
    new IfNode(entry, bol, PROB_MAX, COUNT_UNKNOWN);
  register_node(iff, outer_loop, entry, dom_depth(entry));
  ProjNode* iffast = new IfTrueNode(iff);
  register_node(iffast, outer_loop, iff, dom_depth(iff));
  ProjNode* ifslow = new IfFalseNode(iff);
  register_node(ifslow, outer_loop, iff, dom_depth(iff));

  // Clone the loop body.  The clone becomes the fast loop.  The
  // original pre-header will (illegally) have 3 control users
  // (old & new loops & new if).
  clone_loop(loop, old_new, dom_depth(head->skip_strip_mined()), mode, iff);
  assert(old_new[head->_idx]->is_Loop(), "" );

  // Fast (true) control
  Node* iffast_pred = clone_loop_predicates(entry, iffast, !counted_loop);

  // Slow (false) control
  Node* ifslow_pred = clone_loop_predicates(entry, ifslow, !counted_loop);

  Node* l = head->skip_strip_mined();
  _igvn.replace_input_of(l, LoopNode::EntryControl, iffast_pred);
  set_idom(l, iffast_pred, dom_depth(l));
  LoopNode* slow_l = old_new[head->_idx]->as_Loop()->skip_strip_mined();
  _igvn.replace_input_of(slow_l, LoopNode::EntryControl, ifslow_pred);
  set_idom(slow_l, ifslow_pred, dom_depth(l));

  recompute_dom_depth();

  return iffast;
}

LoopNode* PhaseIdealLoop::create_reserve_version_of_loop(IdealLoopTree *loop, CountedLoopReserveKit* lk) {
  Node_List old_new;
  LoopNode* head  = loop->_head->as_Loop();
  bool counted_loop = head->is_CountedLoop();
  Node*     entry = head->skip_strip_mined()->in(LoopNode::EntryControl);
  _igvn.rehash_node_delayed(entry);
  IdealLoopTree* outer_loop = head->is_strip_mined() ? loop->_parent->_parent : loop->_parent;

  ConINode* const_1 = _igvn.intcon(1);
  set_ctrl(const_1, C->root());
  IfNode* iff = new IfNode(entry, const_1, PROB_MAX, COUNT_UNKNOWN);
  register_node(iff, outer_loop, entry, dom_depth(entry));
  ProjNode* iffast = new IfTrueNode(iff);
  register_node(iffast, outer_loop, iff, dom_depth(iff));
  ProjNode* ifslow = new IfFalseNode(iff);
  register_node(ifslow, outer_loop, iff, dom_depth(iff));

  // Clone the loop body.  The clone becomes the fast loop.  The
  // original pre-header will (illegally) have 3 control users
  // (old & new loops & new if).
  clone_loop(loop, old_new, dom_depth(head), CloneIncludesStripMined, iff);
  assert(old_new[head->_idx]->is_Loop(), "" );

  LoopNode* slow_head = old_new[head->_idx]->as_Loop();

#ifndef PRODUCT
  if (TraceLoopOpts) {
    tty->print_cr("PhaseIdealLoop::create_reserve_version_of_loop:");
    tty->print("\t iff = %d, ", iff->_idx); iff->dump();
    tty->print("\t iffast = %d, ", iffast->_idx); iffast->dump();
    tty->print("\t ifslow = %d, ", ifslow->_idx); ifslow->dump();
    tty->print("\t before replace_input_of: head = %d, ", head->_idx); head->dump();
    tty->print("\t before replace_input_of: slow_head = %d, ", slow_head->_idx); slow_head->dump();
  }
#endif

  // Fast (true) control
  _igvn.replace_input_of(head->skip_strip_mined(), LoopNode::EntryControl, iffast);
  // Slow (false) control
  _igvn.replace_input_of(slow_head->skip_strip_mined(), LoopNode::EntryControl, ifslow);

  recompute_dom_depth();

  lk->set_iff(iff);

#ifndef PRODUCT
  if (TraceLoopOpts ) {
    tty->print("\t after  replace_input_of: head = %d, ", head->_idx); head->dump();
    tty->print("\t after  replace_input_of: slow_head = %d, ", slow_head->_idx); slow_head->dump();
  }
#endif

  return slow_head->as_Loop();
}

CountedLoopReserveKit::CountedLoopReserveKit(PhaseIdealLoop* phase, IdealLoopTree *loop, bool active = true) :
  _phase(phase),
  _lpt(loop),
  _lp(NULL),
  _iff(NULL),
  _lp_reserved(NULL),
  _has_reserved(false),
  _use_new(false),
  _active(active)
  {
    create_reserve();
  };

CountedLoopReserveKit::~CountedLoopReserveKit() {
  if (!_active) {
    return;
  }

  if (_has_reserved && !_use_new) {
    // intcon(0)->iff-node reverts CF to the reserved copy
    ConINode* const_0 = _phase->_igvn.intcon(0);
    _phase->set_ctrl(const_0, _phase->C->root());
    _iff->set_req(1, const_0);

    #ifndef PRODUCT
      if (TraceLoopOpts) {
        tty->print_cr("CountedLoopReserveKit::~CountedLoopReserveKit()");
        tty->print("\t discard loop %d and revert to the reserved loop clone %d: ", _lp->_idx, _lp_reserved->_idx);
        _lp_reserved->dump();
      }
    #endif
  }
}

bool CountedLoopReserveKit::create_reserve() {
  if (!_active) {
    return false;
  }

  if(!_lpt->_head->is_CountedLoop()) {
    if (TraceLoopOpts) {
      tty->print_cr("CountedLoopReserveKit::create_reserve: %d not counted loop", _lpt->_head->_idx);
    }
    return false;
  }
  CountedLoopNode *cl = _lpt->_head->as_CountedLoop();
  if (!cl->is_valid_counted_loop()) {
    if (TraceLoopOpts) {
      tty->print_cr("CountedLoopReserveKit::create_reserve: %d not valid counted loop", cl->_idx);
    }
    return false; // skip malformed counted loop
  }
  if (!cl->is_main_loop()) {
    bool loop_not_canonical = true;
    if (cl->is_post_loop() && (cl->slp_max_unroll() > 0)) {
      loop_not_canonical = false;
    }
    // only reject some loop forms
    if (loop_not_canonical) {
      if (TraceLoopOpts) {
        tty->print_cr("CountedLoopReserveKit::create_reserve: %d not canonical loop", cl->_idx);
      }
      return false; // skip normal, pre, and post (conditionally) loops
    }
  }

  _lp = _lpt->_head->as_Loop();
  _lp_reserved = _phase->create_reserve_version_of_loop(_lpt, this);

  if (!_lp_reserved->is_CountedLoop()) {
    return false;
  }

  Node* ifslow_pred = _lp_reserved->skip_strip_mined()->in(LoopNode::EntryControl);

  if (!ifslow_pred->is_IfFalse()) {
    return false;
  }

  Node* iff = ifslow_pred->in(0);
  if (!iff->is_If() || iff != _iff) {
    return false;
  }

  if (iff->in(1)->Opcode() != Op_ConI) {
    return false;
  }

  return _has_reserved = true;
}

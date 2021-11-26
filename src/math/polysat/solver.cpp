/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat

Abstract:

    Polynomial solver for modular arithmetic.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

--*/

#include "math/polysat/solver.h"
#include "math/polysat/explain.h"
#include "math/polysat/log.h"
#include "math/polysat/variable_elimination.h"
#include "util/luby.h"

// For development; to be removed once the linear solver works well enough
#define ENABLE_LINEAR_SOLVER 0

namespace polysat {



    solver::solver(reslimit& lim): 
        m_lim(lim),
        m_viable(*this),
        m_linear_solver(*this),
        m_conflict(*this),
        m_forbidden_intervals(*this),
        m_bvars(),
        m_free_pvars(m_activity),
        m_constraints(m_bvars) {
    }

    solver::~solver() {
        // Need to remove any lingering clause/constraint references before the constraint manager is destructed
        m_conflict.reset();
    }

    void solver::updt_params(params_ref const& p) {
        m_params.append(p);
        m_branch_bool = m_params.get_bool("branch_bool", false);
        m_max_conflicts = m_params.get_uint("max_conflicts", UINT_MAX);
        m_max_decisions = m_params.get_uint("max_decisions", UINT_MAX);
    }

    bool solver::should_search() {
        return 
            m_lim.inc() && 
            (m_stats.m_num_conflicts < m_max_conflicts) &&
            (m_stats.m_num_decisions < m_max_decisions);
    }
   
    lbool solver::check_sat() {         
        LOG("Starting");
        while (should_search()) {
            m_stats.m_num_iterations++;
            LOG_H1("Next solving loop iteration (#" << m_stats.m_num_iterations << ")");
            LOG("Free variables: " << m_free_pvars);
            LOG("Assignment:     " << assignments_pp(*this));
            if (is_conflict()) LOG("Conflict:       " << m_conflict);
            IF_LOGGING(m_viable.log());
            if (is_conflict() && at_base_level()) { LOG_H2("UNSAT"); return l_false; }
            else if (is_conflict()) resolve_conflict();
            else if (can_propagate()) propagate();
            else if (!can_decide()) { LOG_H2("SAT"); SASSERT(verify_sat()); return l_true; }
            else if (m_constraints.should_gc()) m_constraints.gc(*this);
            else if (should_simplify()) simplify();
            else if (should_restart()) restart();
            else decide();
        }
        LOG_H2("UNDEF (resource limit)");
        return l_undef;
    }

    dd::pdd_manager& solver::sz2pdd(unsigned sz) {
        m_pdd.reserve(sz + 1);
        if (!m_pdd[sz])
            m_pdd.set(sz, alloc(dd::pdd_manager, 1000, dd::pdd_manager::semantics::mod2N_e, sz));
        return *m_pdd[sz];
    }

    dd::pdd_manager& solver::var2pdd(pvar v) {
        return sz2pdd(size(v));
    }
        
    unsigned solver::add_var(unsigned sz) {
        pvar v = m_value.size();
        m_value.push_back(rational::zero());
        m_justification.push_back(justification::unassigned());
        m_viable.push(sz);
        m_pwatch.push_back({});
        m_activity.push_back(0);
        m_vars.push_back(sz2pdd(sz).mk_var(v));
        m_size.push_back(sz);
        m_trail.push_back(trail_instr_t::add_var_i);
        m_free_pvars.mk_var_eh(v);
        return v;
    }

    pdd solver::value(rational const& v, unsigned sz) {
        return sz2pdd(sz).mk_val(v);
    }

    void solver::del_var() {
        // TODO also remove v from all learned constraints.
        pvar v = m_value.size() - 1;
        m_viable.pop();
        m_value.pop_back();
        m_justification.pop_back();
        m_pwatch.pop_back();
        m_activity.pop_back();
        m_vars.pop_back();
        m_size.pop_back();
        m_free_pvars.del_var_eh(v);
    }

    void solver::assign_eh(signed_constraint c, unsigned dep) {
        SASSERT(at_base_level());
        SASSERT(c);
        if (is_conflict())
            return;  // no need to do anything if we already have a conflict at base level
        m_constraints.ensure_bvar(c.get());
        sat::literal lit = c.blit();
        LOG("New constraint: " << c);
        if (m_bvars.is_false(lit)) 
            set_conflict(c);
        else {
            m_bvars.asserted(lit, m_level, dep);
            m_trail.push_back(trail_instr_t::assign_bool_i);
            m_search.push_boolean(lit);
            if (c.is_currently_false(*this))
                set_conflict(c);
        }

#if ENABLE_LINEAR_SOLVER
        m_linear_solver.new_constraint(*c.get());
#endif
    }

    bool solver::can_propagate() {
        return m_qhead < m_search.size() && !is_conflict();
    }

    void solver::propagate() {
        if (!can_propagate())
            return;
        push_qhead();
        while (can_propagate()) {
            auto const& item = m_search[m_qhead++];
            if (item.is_assignment())
                propagate(item.var());
            else
                propagate(item.lit());
        }
        linear_propagate();
        SASSERT(wlist_invariant());
        SASSERT(assignment_invariant());
    }

    /**
    * Propagate assignment to a Boolean variable
    */
    void solver::propagate(sat::literal lit) {
        LOG_H2("Propagate bool " << lit << "@" << m_bvars.level(lit) << " " << m_level << " qhead: " << m_qhead);
        signed_constraint c = lit2cnstr(lit);
        SASSERT(c);
        if (c->is_active())
            return;
        activate_constraint(c);
        auto& wlist = m_bvars.watch(~lit);
        unsigned i = 0, j = 0, sz = wlist.size();
        for (; i < sz && !is_conflict(); ++i)
            if (!propagate(lit, *wlist[i]))
                wlist[j++] = wlist[i];
        for (; i < sz; ++i)
            wlist[j++] = wlist[i];
        wlist.shrink(j);
    }

    /**
    * Propagate assignment to a pvar
    */
    void solver::propagate(pvar v) {
        LOG_H2("Propagate v" << v);
        auto& wlist = m_pwatch[v];
        unsigned i = 0, j = 0, sz = wlist.size();
        for (; i < sz && !is_conflict(); ++i)
            if (!wlist[i].propagate(*this, v))
                wlist[j++] = wlist[i];
        for (; i < sz; ++i)
            wlist[j++] = wlist[i];
        wlist.shrink(j);
    }

    bool solver::propagate(sat::literal lit, clause& cl) {
        SASSERT(cl.size() >= 2);
        unsigned idx = cl[0] == ~lit ? 1 : 0;
        SASSERT(cl[1 - idx] == ~lit);
        if (m_bvars.is_true(cl[idx]))
            return false;
        unsigned i = 2;
        for (; i < cl.size() && m_bvars.is_false(cl[i]); ++i);
        if (i < cl.size()) {
            m_bvars.watch(cl[i]).push_back(&cl);
            std::swap(cl[1 - idx], cl[i]);
            return true;
        }
        if (m_bvars.is_false(cl[idx]))
            set_conflict(cl);
        else
            assign_propagate(cl[idx], cl);
        return false;
    }

    void solver::linear_propagate() {
#if ENABLE_LINEAR_SOLVER
        switch (m_linear_solver.check()) {
        case l_false:
            // TODO extract conflict
            break;
        default:
            break;
        }
#endif
    }

    void solver::propagate(pvar v, rational const& val, signed_constraint c) {
        LOG("Propagation: " << assignment_pp(*this, v, val) << ", due to " << c);
        if (m_viable.is_viable(v, val)) {
            m_free_pvars.del_var_eh(v);
            assign_core(v, val, justification::propagation(m_level));        
        }
        else 
            set_conflict(c);
    }

    void solver::push_level() {
        ++m_level;
        m_trail.push_back(trail_instr_t::inc_level_i);
#if ENABLE_LINEAR_SOLVER
        m_linear_solver.push();
#endif
    }

    /*
    * This is a place holder for in-processing simplification
    */
    bool solver::should_simplify() {
        return false;
    }

    void solver::simplify() {

    }

    /*
    * Basic restart functionality.
    * restarts make more sense when the order of variable 
    * assignments and the values assigned to variables can be diversified.
    */
    bool solver::should_restart() {
        if (m_stats.m_num_conflicts - m_conflicts_at_restart < m_restart_threshold)
            return false;
        if (base_level() + 2 > m_level)
            return false;
        return true;        
    }

    void solver::restart() {
        ++m_stats.m_num_restarts;
        pop_levels(m_level - base_level());
        m_conflicts_at_restart = m_stats.m_num_conflicts;
        m_restart_threshold = m_restart_init * get_luby(++m_luby_idx);
    }

    void solver::pop_levels(unsigned num_levels) {
        if (num_levels == 0)
            return;
        SASSERT(m_level >= num_levels);
        unsigned const target_level = m_level - num_levels;
        vector<sat::literal> replay;
        LOG("Pop " << num_levels << " levels (lvl " << m_level << " -> " << target_level << ")");
#if ENABLE_LINEAR_SOLVER
        m_linear_solver.pop(num_levels);
#endif
        while (num_levels > 0) {
            switch (m_trail.back()) {
            case trail_instr_t::qhead_i: {
                pop_qhead();
                break;
            }
            case trail_instr_t::add_var_i: {
                del_var();
                break;
            }
            case trail_instr_t::inc_level_i: {
                --m_level;
                --num_levels;
                break;
            }
            case trail_instr_t::viable_add_i: {
                m_viable.pop_viable();
                break;
            }
            case trail_instr_t::viable_rem_i: {
                m_viable.push_viable();
                break;
            }
            case trail_instr_t::assign_i: {
                auto v = m_search.back().var();
                LOG_V("Undo assign_i: v" << v);
                m_free_pvars.unassign_var_eh(v);
                m_justification[v] = justification::unassigned();
                m_search.pop();
                break;
            }
            case trail_instr_t::assign_bool_i: {
                sat::literal lit = m_search.back().lit();
                signed_constraint c = lit2cnstr(lit);
                LOG_V("Undo assign_bool_i: " << lit);
                unsigned active_level = m_bvars.level(lit);

                if (c->is_active())
                    deactivate_constraint(c);

                if (active_level <= target_level)
                    replay.push_back(lit);
                else 
                    m_bvars.unassign(lit);                
                m_search.pop();
                break;
            }
            default:
                UNREACHABLE();
            }
            m_trail.pop_back();
        }
        m_constraints.release_level(m_level + 1);
        SASSERT(m_level == target_level);
        for (unsigned j = replay.size(); j-- > 0; ) {
            sat::literal lit = replay[j];
            m_trail.push_back(trail_instr_t::assign_bool_i);
            m_search.push_boolean(lit);
        }
    }

    void solver::add_watch(signed_constraint c) {
        SASSERT(c);
        auto const& vars = c->vars();
        if (vars.size() > 0)
            add_watch(c, vars[0]);
        if (vars.size() > 1)
            add_watch(c, vars[1]);
    }

    void solver::add_watch(signed_constraint c, pvar v) {
        SASSERT(c);
        LOG("Watching v" << v << " in constraint " << c);
        m_pwatch[v].push_back(c);
    }

    void solver::erase_watch(signed_constraint c) {
        auto const& vars = c->vars();
        if (vars.size() > 0)
            erase_watch(vars[0], c);
        if (vars.size() > 1)
            erase_watch(vars[1], c);
    }

    void solver::erase_watch(pvar v, signed_constraint c) {
        if (v == null_var)
            return;
        auto& wlist = m_pwatch[v];
        unsigned sz = wlist.size();
        for (unsigned i = 0; i < sz; ++i) {
            if (c == wlist[i]) {
                wlist[i] = wlist.back();
                wlist.pop_back();
                return;
            }
        }
    }

    void solver::decide() {
        LOG_H2("Decide");
        SASSERT(can_decide());
        if (m_bvars.can_decide() && m_branch_bool)
            bdecide(m_bvars.next_var());
        else
            pdecide(m_free_pvars.next_var());
    }

    void solver::pdecide(pvar v) {
        LOG("Decide v" << v);
        IF_LOGGING(m_viable.log(v));
        rational val;
        switch (m_viable.find_viable(v, val)) {
        case dd::find_t::empty:
            // NOTE: all such cases should be discovered elsewhere (e.g., during propagation/narrowing)
            //       (fail here in debug mode so we notice if we miss some)
            DEBUG_CODE( UNREACHABLE(); );
            m_free_pvars.unassign_var_eh(v);
            set_conflict(v);
            break;
        case dd::find_t::singleton:
            // NOTE: this case may happen legitimately if all other possibilities were excluded by brute force search
            assign_core(v, val, justification::propagation(m_level));
            break;
        case dd::find_t::multiple:
            push_level();
            assign_core(v, val, justification::decision(m_level));
            break;
        }
    }   

    void solver::bdecide(sat::bool_var b) {
        decide_bool(sat::literal(b), nullptr);
    }

    void solver::assign_core(pvar v, rational const& val, justification const& j) {
        if (j.is_decision()) 
            ++m_stats.m_num_decisions;
        else 
            ++m_stats.m_num_propagations;
        LOG(assignment_pp(*this, v, val) << " by " << j);
        SASSERT(m_viable.is_viable(v, val));
        SASSERT(std::all_of(assignment().begin(), assignment().end(), [v](auto p) { return p.first != v; }));
        m_value[v] = val;
        m_search.push_assignment(v, val);
        m_trail.push_back(trail_instr_t::assign_i);
        m_justification[v] = j; 
#if ENABLE_LINEAR_SOLVER
        // TODO: convert justification into a format that can be tracked in a depdendency core.
        m_linear_solver.set_value(v, val, UINT_MAX);
#endif
    }

    /**
     * Conflict resolution.
     * - m_conflict are constraints that are infeasible in the current assignment.
     * 1. walk m_search from top down until last variable in m_conflict.
     * 2. resolve constraints in m_cjust to isolate lowest degree polynomials
     *    using variable.
     *    Use Olm-Seidl division by powers of 2 to preserve invertibility.
     * 3. resolve conflict with result of resolution.
     * 4. If the resulting lemma is still infeasible continue, otherwise bail out
     *    and undo the last assignment by accumulating conflict trail (but without resolution).
     * 5. When hitting the last decision, determine whether conflict polynomial is asserting,
     *    If so, apply propagation.
     * 6. Otherwise, add accumulated constraints to explanation for the next viable solution, prune
     *    viable solutions by excluding the previous guess.
     *
     */
    void solver::resolve_conflict() {        
        LOG_H2("Resolve conflict");
        LOG("\n" << *this);
        LOG("search state: " << m_search);
        for (pvar v = 0; v < m_justification.size(); ++v)
            LOG("v" << v << " " << viable::var_pp(m_viable, v));
        ++m_stats.m_num_conflicts;

        SASSERT(is_conflict());

        if (m_conflict.conflict_var() != null_var) {
            // This case corresponds to a propagation of conflict_var, except it's not explicitly on the stack.
            VERIFY(m_viable.resolve(m_conflict.conflict_var(), m_conflict));
            // TBD: make sure last value decision is blocked by this conflict.
            // A conflict in test_l5 reverts v1 = 2 more than once.
        }

        search_iterator search_it(m_search);
        while (search_it.next()) {
            LOG("search state: " << m_search);
            LOG("Conflict: " << m_conflict);
            auto const& item = *search_it;
            LOG_H2("Working on " << search_item_pp(m_search, item));
            if (item.is_assignment()) {
                // Resolve over variable assignment
                pvar v = item.var();
                if (!m_conflict.is_pmarked(v) && !m_conflict.is_bailout())
                    continue;
                justification& j = m_justification[v];
                LOG("Justification: " << j);
                if (j.level() > base_level() && !resolve_value(v) && j.is_decision()) {
                    revert_decision(v);
                    return;
                }
            }
            else {
                // Resolve over boolean literal
                SASSERT(item.is_boolean());
                sat::literal const lit = item.lit();
                sat::bool_var const var = lit.var();
                if (!m_conflict.is_bmarked(var))
                    continue;
                if (m_bvars.level(var) <= base_level())  
                    continue;
                if (m_bvars.is_decision(var)) {
                    revert_bool_decision(lit);
                    return;
                }
                SASSERT(m_bvars.is_propagation(var));
                resolve_bool(lit);
            }
        }
        // here we build conflict clause if it has free variables.
        // the last decision is reverted.
        report_unsat();
    }

    /** Conflict resolution case where propagation 'v := ...' is on top of the stack */
    bool solver::resolve_value(pvar v) {
        return m_conflict.resolve_value(v);
    }

    /**
    * Variable activity accounting.
    * As a placeholder we increment activity 
    * 1. when a variable assignment is used in a conflict.
    * 2. when a variable propagation is resolved against.
    * The hypothesis that this is useful should be tested against a 
    * broader suite of benchmarks and tested with micro-benchmarks.
    * It should be tested in conjunction with restarts.
    */
    void solver::inc_activity(pvar v) {
        unsigned& act = m_activity[v];
        act += m_activity_inc;
        m_free_pvars.activity_increased_eh(v);
        if (act > (1 << 24))
            rescale_activity();
    }

    void solver::decay_activity() {
        m_activity_inc *= m_variable_decay;
        m_activity_inc /= 100;
    }

    void solver::rescale_activity() {
        for (unsigned& act : m_activity) {
            act >>= 14;
        }
        m_activity_inc >>= 14;
    }


    /** Conflict resolution case where boolean literal 'lit' is on top of the stack 
    *   NOTE: boolean resolution should work normally even in bailout mode.
    */
    void solver::resolve_bool(sat::literal lit) {       
        SASSERT(m_bvars.is_propagation(lit.var()));
        clause const& other = *m_bvars.reason(lit.var());
        LOG_H3("resolve_bool: " << lit << " " << other);
        m_conflict.resolve(m_constraints, lit, other);
    }
    
    void solver::report_unsat() {
        backjump(base_level());
        SASSERT(!m_conflict.empty());
    }

    void solver::unsat_core(unsigned_vector& deps) {
        deps.reset();
        for (auto c : m_conflict) {
            auto d = m_bvars.dep(c.blit());
            if (d != null_dependency)
                deps.push_back(d);
        }
    }

    void solver::learn_lemma(clause& lemma) {
        LOG("Learning: "<< lemma);
        SASSERT(!lemma.empty());
        add_lemma(lemma);
        if (!is_conflict())
            decide_bool(lemma);
    }

    // Guess a literal from the given clause; returns the guessed constraint
    void solver::decide_bool(clause& lemma) {
        LOG_H3("Guessing literal in lemma: " << lemma);
        IF_LOGGING(m_viable.log());
        LOG("Boolean assignment: " << m_bvars);

        // To make a guess, we need to find an unassigned literal that is not false in the current model.

        sat::literal choice = sat::null_literal;
        unsigned num_choices = 0;  // TODO: should probably cache this? (or rather the suitability of each literal... it won't change until we backtrack beyond the current point)

        for (sat::literal lit : lemma) {
            switch (m_bvars.value(lit)) {
            case l_true:
                return;
            case l_false:
                break;
            case l_undef:               
                if (lit2cnstr(lit).is_currently_false(*this)) {
                    unsigned level = m_level; // TODO
                    assign_eval(level, lit);
                }
                else {
                    num_choices++;
                    choice = lit;
                }
                break;
            }
        }
        LOG_V("num_choices: " << num_choices);
        switch (num_choices) {
        case 0:
            set_conflict(lemma);
            break;
        case 1:
            assign_propagate(choice, lemma);
            break;
        default:
            push_level();
            assign_decision(choice, &lemma);
            break;
        }
    }

    /**
     * Revert a decision that caused a conflict.
     * Variable v was assigned by a decision at position i in the search stack.
     * 
     * C & v = val is conflict.
     * 
     * C => v != val
     * 
     * l1 \/ l2 \/ ... \/ lk \/ v != val     
     *      
     */
    void solver::revert_decision(pvar v) {
        rational val = m_value[v];
        LOG_H3("Reverting decision: pvar " << v << " := " << val);
        SASSERT(m_justification[v].is_decision());

        clause_ref lemma = m_conflict.build_lemma().build();
        if (lemma->empty())
            report_unsat();
        else {
            m_conflict.reset();
            backjump(get_level(v) - 1);
            learn_lemma(*lemma);
            narrow(v);
        }
    }

    bool solver::is_decision(search_item const& item) const {
        if (item.is_assignment())
            return m_justification[item.var()].is_decision();
        else
            return m_bvars.is_decision(item.lit().var());
    }

    // Current situation: we have a decision for boolean literal L on top of the stack, and a conflict core.
    //
    // In a CDCL solver, this means ~L is in the lemma (actually, as the asserting literal). We drop the decision and replace it by the propagation (~L)^lemma.
    //
    // - we know L must be false
    // - if L isn't in the core, we can still add it (weakening the lemma) to obtain "core => ~L"
    // - then we can add the propagation (~L)^lemma and continue with the next guess

    // Note that if we arrive at this point, the variables in L are "relevant" to the conflict (otherwise we would have skipped L).
    // So the subsequent steps must have contained one of these:
    // - propagation of some variable v from L (and maybe other constraints)
    //      (v := val)^{L, ...}
    //      this means L is in core, unless we core-reduced it away
    // - propagation of L' from L
    //      (L')^{L' \/ ¬L \/ ...}
    //      again L is in core, unless we core-reduced it away

    void solver::revert_bool_decision(sat::literal lit) {
        sat::bool_var const var = lit.var();
        LOG_H3("Reverting boolean decision: " << lit << " " << m_conflict);
        SASSERT(m_bvars.is_decision(var));

        clause_builder reason_builder = m_conflict.build_lemma();        

        SASSERT(std::find(reason_builder.begin(), reason_builder.end(), ~lit));
        clause_ref reason = reason_builder.build();

        if (reason->empty()) {
            report_unsat();
            return;
        }
        m_conflict.reset();

        // The lemma where 'lit' comes from.
        // Currently, boolean decisions always come from guessing a literal of a learned non-unit lemma.
        clause* lemma = m_bvars.lemma(var);  // need to grab this while 'lit' is still assigned

        backjump(m_bvars.level(var) - 1);

        add_lemma(*reason);

        if (!is_conflict() && lemma)
            decide_bool(*lemma);
    }

    void solver::decide_bool(sat::literal lit, clause* lemma) {
        SASSERT(!can_propagate());
        SASSERT(!is_conflict());
        push_level();        
        assign_decision(lit, lemma);
    }

    unsigned solver::level(clause const& cl) {
        unsigned lvl = base_level();
        for (auto lit : cl) {
            auto c = lit2cnstr(lit);
            if (m_bvars.is_false(lit) || c.is_currently_false(*this))
                lvl = std::max(lvl, c.level(*this));
        }
        return lvl;
    }

    void solver::assign_propagate(sat::literal lit, clause& reason) {
        m_bvars.propagate(lit, level(reason), reason);
        m_trail.push_back(trail_instr_t::assign_bool_i);
        m_search.push_boolean(lit);
    }

    void solver::assign_decision(sat::literal lit, clause* lemma) {
        m_bvars.decide(lit, m_level, lemma);
        m_trail.push_back(trail_instr_t::assign_bool_i);
        m_search.push_boolean(lit);
    }

    void solver::assign_eval(unsigned level, sat::literal lit) {
        m_bvars.eval(lit, level);
        m_trail.push_back(trail_instr_t::assign_bool_i);
        m_search.push_boolean(lit);
    }

    /** 
    * Activate constraint immediately
    * Activation and de-activation of constraints follows the scope controlled by push/pop.
    * constraints activated within the linear solver are de-activated when the linear
    * solver is popped.
    */
    void solver::activate_constraint(signed_constraint c) {
        SASSERT(c);
        LOG("Activating constraint: " << c);
        SASSERT(m_bvars.value(c.blit()) == l_true);
        SASSERT(!c->is_active());
        c->set_active(true);
        add_watch(c);
        c.narrow(*this);
#if ENABLE_LINEAR_SOLVER
        m_linear_solver.activate_constraint(c);
#endif
    }

    /// Deactivate constraint
    void solver::deactivate_constraint(signed_constraint c) {
        LOG("Deactivating constraint: " << c.blit());
        c->set_active(false);
        erase_watch(c);
    }

    void solver::backjump(unsigned new_level) {
        LOG_H3("Backjumping to level " << new_level << " from level " << m_level);
        pop_levels(m_level - new_level);
    }

    /**
     * placeholder for factoring/gcd common factors
     */
    void solver::narrow(pvar v) {
        if (is_conflict())
            return;
        // TODO
    }

    // Add lemma to storage
    void solver::add_lemma(clause& lemma) {
        LOG("Lemma: " << lemma);
        for (sat::literal lit : lemma) {
            LOG("   Literal " << lit << " is: " << lit2cnstr(lit));
            SASSERT(m_bvars.value(lit) != l_true);
        }
        SASSERT(!lemma.empty());
        m_constraints.store(&lemma, *this);
        propagate();
    }

    void solver::insert_constraint(signed_constraints& cs, signed_constraint c) {
        SASSERT(c);
        LOG_V("INSERTING: " << c);
        cs.push_back(c);
        SASSERT(invariant(cs)); 
    }   

    void solver::push() {
        LOG("Push user scope");
        push_level();
        m_base_levels.push_back(m_level);
    }

    void solver::pop(unsigned num_scopes) {
        unsigned base_level = m_base_levels[m_base_levels.size() - num_scopes];
        LOG("Pop " << num_scopes << " user scopes; lowest popped level = " << base_level << "; current level = " << m_level);
        pop_levels(m_level - base_level + 1);
        m_conflict.reset();   
    }

    bool solver::at_base_level() const {
        return m_level == base_level();
    }
    
    unsigned solver::base_level() const {
        return m_base_levels.empty() ? 0 : m_base_levels.back();
    }

    bool solver::try_eval(pdd const& p, rational& out_value) const {
        pdd r = p.subst_val(assignment());
        if (r.is_val())
            out_value = r.val();
        return r.is_val();
    }

    std::ostream& solver::display(std::ostream& out) const {
        out << "Search Stack:\n";
        for (auto item : m_search) {
            if (item.is_assignment()) {
                pvar v = item.var();
                auto const& j = m_justification[v];
                out << "\t" << assignment_pp(*this, v, get_value(v)) << " @" << j.level();   
                out << "\n";
            }
            else {
                sat::bool_var v = item.lit().var();
                out << "\t" << item.lit() << " @" << m_bvars.level(v);
                if (m_bvars.reason(v))
                    out << " " << *m_bvars.reason(v);
                out << "\n";
            }
        }
        out << "Constraints:\n";
        for (auto c : m_constraints)
            out << "\t" << c->bvar2string() << ": " << *c << "\n";
        out << "Clauses:\n";
        for (auto const& cls : m_constraints.clauses()) {
            for (auto const& cl : cls) {
                out << "\t" << *cl << "\n";
                for (auto lit : *cl) 
                    out << "\t\t" << lit << ": " << lit2cnstr(lit) << "\n";                
            }
        }
        return out;
    }

    std::ostream& assignments_pp::display(std::ostream& out) const {
        for (auto const& [var, val] : s.assignment())
            out << assignment_pp(s, var, val) << " ";
        return out;
    }

    std::ostream& assignment_pp::display(std::ostream& out) const {
        out << "v" << var << " := ";
        rational const& p = rational::power_of_two(s.size(var));
        if (val > mod(-val, p))
            return out << -mod(-val, p);
        else 
            return out << val;
    }
    

    void solver::collect_statistics(statistics& st) const {
        st.update("polysat iterations",   m_stats.m_num_iterations);
        st.update("polysat decisions",    m_stats.m_num_decisions);
        st.update("polysat conflicts",    m_stats.m_num_conflicts);
        st.update("polysat bailouts",     m_stats.m_num_bailouts);
        st.update("polysat propagations", m_stats.m_num_propagations);
        st.update("polysat restarts",     m_stats.m_num_restarts);
    }

    bool solver::invariant() {
        return true;
    }

    /**
     * levels are gone
     */
    bool solver::invariant(signed_constraints const& cs) {
        return true;
    }

    /**
     * Check that two variables of each constraint are watched.
     */
    bool solver::wlist_invariant() {
        // Skip boolean variables that aren't active yet
        uint_set skip;
        for (unsigned i = m_qhead; i < m_search.size(); ++i)
            if (m_search[i].is_boolean())
                skip.insert(m_search[i].lit().var());
        for (auto c : m_constraints) {
            if (!c->has_bvar())
                continue;
            if (skip.contains(c->bvar()))
                continue;

            lbool value = m_bvars.value(c->bvar());
            if (value == l_undef)
                continue;
            bool is_positive = value == l_true;
            int64_t num_watches = 0;
            signed_constraint sc(c, is_positive);
            for (auto const& wlist : m_pwatch) {
                auto n = std::count(wlist.begin(), wlist.end(), sc);
                if (n > 1)
                    std::cout << sc << "\n" << * this << "\n";
                VERIFY(n <= 1);  // no duplicates in the watchlist
                num_watches += n;
            }
            unsigned expected_watches = std::min(2u, c->vars().size());
            if (num_watches != expected_watches)
                LOG("wrong number of watches: " << c);
            SASSERT_EQ(num_watches, expected_watches);
        }
        return true;
    }

    /** Check that boolean assignment and constraint evaluation are consistent */
    bool solver::assignment_invariant() {
        if (is_conflict())
            return true;
        bool ok = true;
        for (sat::bool_var v = m_bvars.size(); v-- > 0; ) {
            sat::literal lit(v);            
            auto c = lit2cnstr(lit);  
            if (!std::all_of(c->vars().begin(), c->vars().end(), [&](auto v) { return is_assigned(v); }))
                continue;
            ok &= (m_bvars.value(lit) != l_true) || !c.is_currently_false(*this);
            ok &= (m_bvars.value(lit) != l_false) || !c.is_currently_true(*this);
            if (!ok) {
                LOG("assignment invariant is broken " << v << "\n" << *this);
                break;
            }
        }
        return ok;
    }

    /// Check that all constraints on the stack are satisfied by the current model.
    bool solver::verify_sat() {
        LOG_H1("Checking current model...");
        LOG("Assignment: " << assignments_pp(*this));
        bool all_ok = true;
        for (auto s : m_search) {
            if (s.is_boolean()) {
                bool ok = lit2cnstr(s.lit()).is_currently_true(*this);
                LOG((ok ? "PASS" : "FAIL") << ": " << s.lit());
                all_ok = all_ok && ok;
            }
        }
        if (all_ok) LOG("All good!");
        return true;
    }
}


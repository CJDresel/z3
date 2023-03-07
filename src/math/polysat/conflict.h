/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat conflict

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

Notes:

    A conflict state is of the form <Vars, Constraints, Lemmas>
    Where Vars are shorthand for the constraints v = value(v) for v in Vars and value(v) is the assignment.
    Lemmas provide justifications for newly created constraints.

    The conflict state is unsatisfiable under background clauses F.
    Dually, the negation is a consequence of F.

    Conflict resolution resolves an assignment in the search stack against the conflict state.

    Assignments are of the form:

    lit <- D => lit              - lit is propagated by the clause D => lit
    lit <- asserted              - lit is asserted
    lit <- Vars                  - lit is propagated from variable evaluation.

    v = value <- D               - v is assigned value by constraints D
    v = value <- ?               - v is a decision literal.

    - All literals should be assigned in the stack prior to their use;
      or justified by one of the side lemmas.
      (thus: all literals in the core must have bvalue == l_true)

    l <- D => l,       < Vars, { l } u C >   ===>   < Vars, C u D >
    l <- ?,            < Vars, { l } u C >   ===>   ~l <- (C & Vars = value(Vars) => ~l)
    l <- asserted,     < Vars, { l } u C >   ===>   < Vars, { l } u C >
    l <- Vars',        < Vars, { l } u C >   ===>   < Vars u Vars', C >          if all Vars' are propagated
    l <- Vars',        < Vars, { l } u C >   ===>   Mark < Vars, { l } u C > as bailout

    v = value <- D,  < Vars u { v }, C >     ===>   < Vars, D u C >
    v = value <- ?,  < Vars u { v }, C >     ===>   v != value <- (C & Vars = value(Vars) => v != value)


Example derivations:

Trail:       z <= y  <- asserted
             xz > xy <- asserted
             x = a   <- ?
             y = b   <- ?
             z = c   <- ?
Conflict:    < {x, y, z}, xz > xy > when ~O(a,b) and c <= b
Append       x <= a <- { x }
Append       y <= b <- { y }
Conflict:    < {}, y >= z, xz > xy, x <= a, y <= b >
Based on:    z <= y & x <= a & y <= b => xz <= xy
Resolve:     y <= b <- { y }, y is a decision variable.
Bailout:     lemma ~(y >= z & xz > xy & x <= a & y <= b) at decision level of lemma

With overflow predicate:
Append       ~O(x, y) <- { x, y }
Conflict:    < {}, y >= z, xz > xy, ~O(x,y) >
Based on     z <= y & ~O(x,y) => xz <= xy
Resolve:     ~O(x, y) <- { x, y } both x, y are decision variables
Lemma:       y < z or xz <= xy or O(x,y)

--*/
#pragma once
#include "math/polysat/types.h"
#include "math/polysat/constraint_manager.h"
#include "math/polysat/inference_logger.h"
#include <initializer_list>

namespace polysat {

    class solver;
    class conflict_iterator;
    class conflict_resolver;

    class conflict {
        solver& s;
        scoped_ptr<inference_logger> m_logger;
        scoped_ptr<conflict_resolver> m_resolver;

        // current conflict core consists of m_literals and m_vars
        indexed_uint_set m_literals;        // set of boolean literals in the conflict; TODO: why not sat::literal_set
        uint_set m_vars;                    // variable assignments used as premises, shorthand for literals (x := v)

        unsigned_vector m_var_occurrences;  // for each variable, the number of constraints in m_literals that contain it
        uint_set m_vars_occurring;          // set of variables that occur in at least one of the constraints in m_literals

        // Lemmas that been accumulated during conflict resolution
        clause_ref_vector m_lemmas;

        // Store constraints that should be narrowed after backjumping.
        // This allows us to perform propagations that are missed by the two-watched-variables scheme,
        // e.g., because one of the watched variables is unassigned but irrelevant (e.g., x is irrelevant in x*y if y := 0).
        sat::literal_vector m_narrow_queue;

        // Level at which the conflict was discovered
        unsigned m_level = UINT_MAX;
        dependency m_dep = null_dependency;
        sat::literal m_dep_literal = sat::null_literal;

    public:
        conflict(solver& s);
        ~conflict();

        inference_logger& logger();
        void log_inference(inference const& inf) { logger().log(inf); }

        bool empty() const;

        /** Reset to "no conflict" state. This is only appropriate when conflict resolution is complete or aborted. */
        void reset();

        using const_iterator = conflict_iterator;
        const_iterator begin() const;
        const_iterator end() const;

        uint_set const& vars() const { return m_vars; }

        unsigned level() const { return m_level; }

        bool is_relevant_pvar(pvar v) const;
        bool is_relevant(sat::literal lit) const;

        /** conflict due to obvious input inconsistency */
        void init_at_base_level(dependency dep);
        /** conflict due to obvious input inconsistency with literal */
        void init_at_base_level(dependency dep, sat::literal lit);
        /** conflict because the constraint c is false under current variable assignment */
        void init(signed_constraint c);
        /** boolean conflict with the given clause */
        void init(clause& cl);
        /** conflict because there is no viable value for the variable v, by interval reasoning */
        void init_by_viable_interval(pvar v);
        /** conflict because there is no viable value for the variable v, by fallback solver */
        void init_by_viable_fallback(pvar v, univariate_solver& us);

        bool contains(signed_constraint c) const { SASSERT(c); return contains(c.blit()); }
        bool contains(sat::literal lit) const;
        bool contains_pvar(pvar v) const { return m_vars.contains(v); }
        bool pvar_occurs_in_constraints(pvar v) const { return v < m_var_occurrences.size() && m_var_occurrences[v] > 0; }
        uint_set const& vars_occurring_in_constraints() const { return m_vars_occurring; }

        /**
         * Insert constraint c into conflict state.
         */
        void insert(signed_constraint c);

        /** Insert assigned variables of c */
        void insert_vars(signed_constraint c);

        /** Add a lemma to the conflict, to be added after conflict resolution */
        void add_lemma(char const* name, std::initializer_list<signed_constraint> cs);
        void add_lemma(char const* name, signed_constraint const* cs, size_t cs_len);
        void add_lemma(clause_ref lemma);
        void add_lemma(char const* name, clause_ref lemma) { lemma->set_name(name); add_lemma(lemma); }  // remove

        /** Re-add a lemma to the conflict that we were unable to add after the previous conflict. */
        void restore_lemma(clause_ref lemma);

        /** Remove c from core */
        void remove(signed_constraint c);

        /**
         * Remove all constraints and variables from the conflict state.
         * Use this during conflict resolution if the core needs to be replaced.
         * (It keeps the conflict level and side lemmas.)
         */
        void remove_all();

        /** Perform boolean resolution with the clause upon the given literal. */
        void resolve_bool(sat::literal lit, clause const& cl);

        /** lit was evaluated under the assignment. */
        void resolve_evaluated(sat::literal lit);

        /** Perform resolution with "v = value <- ..." */
        void resolve_value(pvar v);

        /** Revert variable assignment, add auxiliary lemmas for the reverted variable */
        void revert_pvar(pvar v);

        /** Convert the core into a lemma to be learned. */
        clause_ref build_lemma();

        /** Move the accumulated lemmas out of the conflict */
        clause_ref_vector take_lemmas();

        clause_ref_vector const& lemmas() const { return m_lemmas; }

        /** Move the literals to be narrowed out of the conflict */
        sat::literal_vector take_narrow_queue();

        /**
         * Collect external dependencies of the current conflict.
         * This only makes sense for base-level conflicts.
         */
        void find_deps(dependency_vector& out_deps) const;

        std::ostream& display(std::ostream& out) const;
    };

    inline std::ostream& operator<<(std::ostream& out, conflict const& c) { return c.display(out); }

    class conflict_iterator {
        friend class conflict;

        using inner_t = indexed_uint_set::iterator;

        constraint_manager* m_cm;
        inner_t m_inner;

        conflict_iterator(constraint_manager& cm, inner_t inner):
            m_cm(&cm), m_inner(inner) {}

        static conflict_iterator begin(constraint_manager& cm, indexed_uint_set const& lits) {
            return {cm, lits.begin()};
        }

        static conflict_iterator end(constraint_manager& cm, indexed_uint_set const& lits) {
            return {cm, lits.end()};
        }

    public:
        using value_type = signed_constraint;
        using difference_type = std::ptrdiff_t;
        using pointer = signed_constraint const*;
        using reference = signed_constraint const&;
        using iterator_category = std::input_iterator_tag;

        conflict_iterator& operator++() {
            ++m_inner;
            return *this;
        }

        signed_constraint operator*() const {
            return m_cm->lookup(sat::to_literal(*m_inner));
        }

        bool operator==(conflict_iterator const& other) const {
            return m_inner == other.m_inner;
        }

        bool operator!=(conflict_iterator const& other) const { return !operator==(other); }
    };
}

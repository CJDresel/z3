/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    maintain viable domains
    It uses the interval extraction functions from forbidden intervals.
    An empty viable set corresponds directly to a conflict that does not rely on
    the non-viable variable.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

--*/
#pragma once


#include <limits>
#include "util/dlist.h"
#include "util/map.h"
#include "util/small_object_allocator.h"
#include "math/polysat/types.h"
#include "math/polysat/conflict.h"
#include "math/polysat/constraint.h"
#include "math/polysat/forbidden_intervals.h"
#include <optional>

namespace polysat {

    class solver;
    class univariate_solver;
    class univariate_solver_factory;

    enum class find_t {
        empty,
        singleton,
        multiple,
        resource_out,
    };

    std::ostream& operator<<(std::ostream& out, find_t x);

    namespace viable_query {
        enum class query_t {
            has_viable,  // currently only used internally in resolve_viable
            min_viable,  // currently unused
            max_viable,  // currently unused
            find_viable,
        };

        template <query_t mode>
        struct query_result {
        };

        template <>
        struct query_result<query_t::min_viable> {
            using result_t = rational;
        };

        template <>
        struct query_result<query_t::max_viable> {
            using result_t = rational;
        };

        template <>
        struct query_result<query_t::find_viable> {
            using result_t = std::pair<rational&, rational&>;
        };
    }

    class viable {
        friend class test_fi;
        friend class test_polysat;

        solver& s;
        forbidden_intervals      m_forbidden_intervals;

        struct entry final : public dll_base<entry>, public fi_record {
            ptr_vector<entry const> refined;
        };
        enum class entry_kind { unit_e, equal_e, diseq_e };

        ptr_vector<entry>                    m_alloc;
        ptr_vector<entry>                    m_units;        // set of viable values based on unit multipliers
        ptr_vector<entry>                    m_equal_lin;    // entries that have non-unit multipliers, but are equal
        ptr_vector<entry>                    m_diseq_lin;    // entries that have distinct non-zero multipliers
        svector<std::tuple<pvar, entry_kind, entry*>>     m_trail; // undo stack

        bool well_formed(entry* e);

        entry* alloc_entry();

        bool intersect(pvar v, entry* e);

        template<bool FORWARD>
        bool refine_viable(pvar v, rational const& val, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications);

        template<bool FORWARD>
        bool refine_bits(pvar v, rational const& val, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications);

        bool refine_equal_lin(pvar v, rational const& val);

        bool refine_disequal_lin(pvar v, rational const& val);

        template<bool FORWARD>
        rational extend_by_bits(const pdd& var, const rational& bounds, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications, vector<signed_constraint>& src, vector<signed_constraint>& side_cond, ptr_vector<entry const>& refined) const;

        bool collect_bit_information(pvar v, bool add_conflict, svector<lbool>& fixed, vector<ptr_vector<entry>>& justifications);

        std::ostream& display_one(std::ostream& out, pvar v, entry const* e) const;
        std::ostream& display_all(std::ostream& out, pvar v, entry const* e, char const* delimiter = "") const;

        void insert(entry* e, pvar v, ptr_vector<entry>& entries, entry_kind k);

        void propagate(pvar v, rational const& val);

        /**
         * Interval-based queries
         * @return l_true on success, l_false on conflict, l_undef on refinement
         */
        lbool query_min(pvar v, rational& out_lo, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications);
        lbool query_max(pvar v, rational& out_hi, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications);
        lbool query_find(pvar v, rational& out_lo, rational& out_hi, const svector<lbool>& fixed, const vector<ptr_vector<entry>>& justifications);

        /**
         * Bitblasting-based queries.
         * The univariate solver has already been filled with all relevant constraints and check() returned l_true.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        lbool query_min_fallback(pvar v, univariate_solver& us, rational& out_lo);
        lbool query_max_fallback(pvar v, univariate_solver& us, rational& out_hi);
        lbool query_find_fallback(pvar v, univariate_solver& us, rational& out_lo, rational& out_hi);

        /**
         * Interval-based query with bounded refinement and fallback to bitblasting.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        template <viable_query::query_t mode>
        lbool query(pvar v, typename viable_query::query_result<mode>::result_t& out_result);

        /**
         * Bitblasting-based query.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        template <viable_query::query_t mode>
        lbool query_fallback(pvar v, typename viable_query::query_result<mode>::result_t& out_result);

    public:
        viable(solver& s);

        ~viable();

        // declare and remove var
        void push_var(unsigned bit_width);
        void pop_var();

        // undo adding/removing of entries
        void pop_viable();
        void push_viable();

        /**
         * update state of viable for pvar v
         * based on affine constraints.
         * Returns true if the state has been changed.
         */
        bool intersect(pvar v, signed_constraint const& c);

        /**
         * Extract remaining variable v from p and q and try updating viable state for v.
         * NOTE: does not require a particular constraint type (e.g., we call this for ule_constraint and umul_ovfl_constraint)
         */
        bool intersect(pdd const& p, pdd const& q, signed_constraint const& c);

        /**
         * Check whether variable v has any viable values left according to m_viable.
         */
        bool has_viable(pvar v);

        /**
         * check if value is viable according to m_viable.
         */
        bool is_viable(pvar v, rational const& val);

        /**
         * Extract min viable value for v.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        lbool min_viable(pvar v, rational& out_lo);

        /**
         * Extract max viable value for v.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        lbool max_viable(pvar v, rational& out_hi);


        /**
         * Query for an upper bound literal for v together with justification.
         * On success, the conjunction of out_c implies v <= out_hi.
         * @return true if a non-trivial upper bound is found, return justifying constraints.
         */
        bool has_upper_bound(pvar v, rational& out_hi, vector<signed_constraint>& out_c);

        /**
         * Query for an lower bound literal for v together with justification.
         * On success, the conjunction of out_c implies v >= out_hi.
         * @return true if a non-trivial lower bound is found, return justifying constraints.
         */
        bool has_lower_bound(pvar v, rational& out_lo, vector<signed_constraint>& out_c);

        /**
         * Query for a maximal interval based on fixed bounds where v is forbidden.
         * On success, the conjunction of out_c implies v \not\in [out_lo; out_hi[.
         */
        bool has_max_forbidden(pvar v, signed_constraint const& c, rational& out_lo, rational& out_hi, vector<signed_constraint>& out_c);
        
        /**
         * Find a next viable value for variable.
         */
        find_t find_viable(pvar v, rational& out_val);

        /**
         * Find a next viable value for variable. Attempts to find two different values, to distinguish propagation/decision.
         * @return l_true on success, l_false on conflict, l_undef on resource limit
         */
        lbool find_viable(pvar v, rational& out_lo, rational& out_hi);

        /**
         * Retrieve the unsat core for v,
         * and add the forbidden interval lemma for v (which eliminates v from the unsat core).
         * \pre there are no viable values for v (determined by interval reasoning)
         */
        bool resolve_interval(pvar v, conflict& core);

        /**
         * Retrieve the unsat core for v.
         * \pre there are no viable values for v (determined by fallback solver)
         */
        bool resolve_fallback(pvar v, univariate_solver& us, conflict& core);

        /** Log all viable values for the given variable.
         * (Inefficient, but useful for debugging small instances.)
         */
        void log(pvar v);

        /** Like log(v) but for all variables */
        void log();

        std::ostream& display(std::ostream& out, char const* delimiter = "") const;

        class iterator {
            entry* curr = nullptr;
            bool   visited = false;
            unsigned idx = 0;
        public:
            iterator(entry* curr, bool visited) :
                curr(curr), visited(visited || !curr) {}

            iterator& operator++() {
                if (idx < curr->side_cond.size() + curr->src.size() - 1)
                    ++idx;
                else {
                    idx = 0;
                    visited = true;
                    curr = curr->next();
                }
                return *this;
            }

            signed_constraint& operator*() {
                return idx < curr->side_cond.size() ? curr->side_cond[idx] : curr->src[idx - curr->side_cond.size()];
            }

            bool operator==(iterator const& other) const {
                return visited == other.visited && curr == other.curr;
            }

            bool operator!=(iterator const& other) const {
                return !(*this == other);
            }
        };

        class constraints {
            viable const& v;
            pvar var;
        public:
            constraints(viable const& v, pvar var) : v(v), var(var) {}
            iterator begin() const { return iterator(v.m_units[var], false); }
            iterator end() const { return iterator(v.m_units[var], true); }
        };

        constraints get_constraints(pvar v) const {
            return constraints(*this, v);
        }

        class int_iterator {
            entry* curr = nullptr;
            bool visited = false;
        public:
            int_iterator(entry* curr, bool visited) :
                curr(curr), visited(visited || !curr) {}
            int_iterator& operator++() {
                visited = true;
                curr = curr->next();
                return *this;
            }

            eval_interval const& operator*() {
                return curr->interval;
            }

            bool operator==(int_iterator const& other) const {
                return visited == other.visited && curr == other.curr;
            }

            bool operator!=(int_iterator const& other) const {
                return !(*this == other);
            }

        };

        class intervals {
            viable const& v;
            pvar var;
        public:
            intervals(viable const& v, pvar var): v(v), var(var) {}
            int_iterator begin() const { return int_iterator(v.m_units[var], false); }
            int_iterator end() const { return int_iterator(v.m_units[var], true); }
        };

        intervals units(pvar v) { return intervals(*this, v); }

        std::ostream& display(std::ostream& out, pvar v, char const* delimiter = "") const;

        struct var_pp {
            viable const& v;
            pvar var;
            var_pp(viable const& v, pvar var) : v(v), var(var) {}
        };

    };

    inline std::ostream& operator<<(std::ostream& out, viable const& v) {
        return v.display(out);
    }

    inline std::ostream& operator<<(std::ostream& out, viable::var_pp const& v) {
        return v.v.display(out, v.var);
    }

    class viable_fallback {
        friend class viable;

        solver& s;

        scoped_ptr<univariate_solver_factory>   m_usolver_factory;
        u_map<scoped_ptr<univariate_solver>>    m_usolver;  // univariate solver for each bit width
        vector<signed_constraints>              m_constraints;
        svector<unsigned>                       m_constraints_trail;

        univariate_solver* usolver(unsigned bit_width);

    public:
        viable_fallback(solver& s);

        // declare and remove var
        void push_var(unsigned bit_width);
        void pop_var();

        // add/remove constraints stored in the fallback solver
        void push_constraint(pvar v, signed_constraint const& c);
        void pop_constraint();

        // Check whether all constraints for 'v' are satisfied;
        // or find an arbitrary violated constraint.
        bool check_constraints(assignment const& a, pvar v) { return !find_violated_constraint(a, v); }
        signed_constraint find_violated_constraint(assignment const& a, pvar v);

        find_t find_viable(pvar v, rational& out_val);
    };

}



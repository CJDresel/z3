/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Op constraint.

    lshr: r == p >> q
    ashr: r == p >>a q
    lshl: r == p << q
    and:  r == p & q
    not:  r == ~p

Author:

    Jakob Rath, Nikolaj Bjorner (nbjorner) 2021-12-09

--*/
#pragma once
#include "math/polysat/constraint.h"
#include <optional>

namespace polysat {

    class solver;

    class op_constraint final : public constraint {
    public:
        enum class code {
            /// r is the logical right shift of p by q
            lshr_op,
            /// r is the arithmetic right shift of p by q
            ashr_op,
            /// r is the left shift of p by q
            shl_op,
            /// r is the bit-wise 'and' of p and q
            and_op,
            /// r is the smallest multiplicative pseudo-inverse of p;
            /// by definition we set r == 0 when p == 0.
            /// Note that in general, there are 2^parity(p) many pseudo-inverses of p.
            inv_op,
            // r is the quotient of dividing p by q 
            udiv_op,
            // r is the remainder of dividing p by q
            urem_op,
        };
    protected:
        friend class constraint_manager;

        code m_op;
        pdd m_p; // operand1
        pdd m_q; // operand2
        pdd m_r; // result
        
        op_constraint* m_linked; // for linking remainder/quotient

        op_constraint(code c, pdd const& p, pdd const& q, pdd const& r);
        lbool eval(pdd const& p, pdd const& q, pdd const& r) const;
        clause_ref produce_lemma(solver& s, assignment const& a);

        clause_ref lemma_lshr(solver& s, assignment const& a);
        static lbool eval_lshr(pdd const& p, pdd const& q, pdd const& r);
        bool propagate_bits_lshr(solver& s, bool is_positive);

        clause_ref lemma_shl(solver& s, assignment const& a);
        static lbool eval_shl(pdd const& p, pdd const& q, pdd const& r);
        bool propagate_bits_shl(solver& s, bool is_positive);

        clause_ref lemma_and(solver& s, assignment const& a);
        static lbool eval_and(pdd const& p, pdd const& q, pdd const& r);
        bool propagate_bits_and(solver& s, bool is_positive);

        clause_ref lemma_inv(solver& s, assignment const& a);
        static lbool eval_inv(pdd const& p, pdd const& r);
        
        clause_ref lemma_udiv(solver& s, assignment const& a);
        static lbool eval_udiv(pdd const& p, pdd const& q, pdd const& r);
        
        clause_ref lemma_urem(solver& s, assignment const& a);
        static lbool eval_urem(pdd const& p, pdd const& q, pdd const& r);

        std::ostream& display(std::ostream& out, char const* eq) const;

        void activate(solver& s);

        void activate_and(solver& s);
        void activate_udiv(solver& s);

    public:
        ~op_constraint() override {}
        pdd const& p() const { return m_p; }
        pdd const& q() const { return m_q; }
        pdd const& r() const { return m_r; }
        std::ostream& display(std::ostream& out, lbool status) const override;
        std::ostream& display(std::ostream& out) const override;
        lbool eval() const override;
        lbool eval(assignment const& a) const override;
        void narrow(solver& s, bool is_positive, bool first) override;
        bool propagate_bits(solver& s, bool is_positive) override;
        virtual clause_ref produce_lemma(solver& s, assignment const& a, bool is_positive) override;
        unsigned hash() const override;
        bool operator==(constraint const& other) const override;
        bool is_eq() const override { return false; }

        void add_to_univariate_solver(pvar v, solver& s, univariate_solver& us, unsigned dep, bool is_positive) const override;
    };

    struct op_constraint_args {
        op_constraint::code op;
        // NOTE: this is only optional because table2map requires a default constructor
        std::optional<std::pair<pdd, pdd>> args;

        op_constraint_args() = default;
        op_constraint_args(op_constraint::code op, pdd lhs, pdd rhs)
            : op(op), args({std::move(lhs), std::move(rhs)}) {}

        bool operator==(op_constraint_args const& other) const {
            return op == other.op && args == other.args;
        }

        unsigned hash() const {
            unsigned const lhs_hash = args ? args->first.hash() : 0;
            unsigned const rhs_hash = args ? args->second.hash() : 0;
            return mk_mix(static_cast<unsigned>(op), lhs_hash, rhs_hash);
        }
    };

}

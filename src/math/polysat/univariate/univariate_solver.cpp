/*++
Copyright (c) 2022 Microsoft Corporation

Module Name:

    polysat univariate solver

Abstract:

    Solve univariate constraints for polysat using bitblasting

Author:

    Nikolaj Bjorner (nbjorner) 2022-03-10
    Jakob Rath 2022-03-10

--*/

#include "math/polysat/univariate/univariate_solver.h"
#include "solver/solver.h"
#include "util/util.h"
#include "ast/ast.h"
#include "ast/reg_decl_plugins.h"
#include "ast/ast_smt2_pp.h"


namespace polysat {

    univariate_solver::dep_vector univariate_solver::unsat_core() {
        dep_vector deps;
        unsat_core(deps);
        return deps;
    }

    bool univariate_solver::find_min(rational& val) {
        val = model();
        push();
        // try reducing val by setting bits to 0, starting at the msb.
        for (unsigned k = bit_width; k-- > 0; ) {
            if (!val.get_bit(k)) {
                add_bit0(k, null_dep);
                continue;
            }
            // try decreasing k-th bit
            push();
            add_bit0(k, 0);
            lbool result = check();
            if (result == l_true) {
                SASSERT(model() < val);
                val = model();
            }
            pop(1);
            if (result == l_true)
                add_bit0(k, null_dep);
            else if (result == l_false)
                add_bit1(k, null_dep);
            else
                return false;
        }
        pop(1);
        return true;
    }

    bool univariate_solver::find_max(rational& val) {
        val = model();
        push();
        // try increasing val by setting bits to 1, starting at the msb.
        for (unsigned k = bit_width; k-- > 0; ) {
            if (val.get_bit(k)) {
                add_bit1(k, 0);
                continue;
            }
            // try increasing k-th bit
            push();
            add_bit1(k, 0);
            lbool result = check();
            if (result == l_true) {
                SASSERT(model() > val);
                val = model();
            }
            pop(1);
            if (result == l_true)
                add_bit1(k, null_dep);
            else if (result == l_false)
                add_bit0(k, null_dep);
            else
                return false;
        }
        pop(1);
        return true;
    }

    class univariate_bitblast_solver : public univariate_solver {
        // TODO: does it make sense to share m and bv between different solver instances?
        // TODO: consider pooling solvers to save setup overhead, see if solver/solver_pool.h can be used
        ast_manager m;
        scoped_ptr<bv_util> bv;
        scoped_ptr<solver> s;
        unsigned m_scope_level = 0;
        func_decl_ref m_x_decl;
        expr_ref m_x;
        vector<rational> model_cache;

    public:
        univariate_bitblast_solver(solver_factory& mk_solver, unsigned bit_width) :
            univariate_solver(bit_width),
            m_x_decl(m),
            m_x(m) {
            reg_decl_plugins(m);
            bv = alloc(bv_util, m);
            params_ref p;
            p.set_bool("bv.polysat", false);
            // p.set_bool("smt", true);
            s = mk_solver(m, p, false, true, true, symbol::null);
            m_x_decl = m.mk_const_decl("x", bv->mk_sort(bit_width));
            m_x = m.mk_const(m_x_decl);
            model_cache.push_back(rational(-1));
        }

        ~univariate_bitblast_solver() override = default;

        expr* x(unsigned num_bits) {
            SASSERT(1 <= num_bits);
            SASSERT(num_bits <= bit_width);
            if (num_bits == bit_width)
                return m_x;
            return bv->mk_extract(num_bits - 1, 0, m_x);
        }

        void reset_cache() {
            model_cache.back() = -1;
        }

        void push_cache() {
            rational v = model_cache.back();
            model_cache.push_back(v);
        }

        void pop_cache() {
            model_cache.pop_back();
        }

        void push() override {
            m_scope_level++;
            push_cache();
            s->push();
        }

        void pop(unsigned n) override {
            SASSERT(scope_level() >= n);
            m_scope_level -= n;
            pop_cache();
            s->pop(n);
        }

        unsigned scope_level() const override {
            return m_scope_level;
        }

        expr* mk_numeral(rational const& r, unsigned num_bits) const {
            return bv->mk_numeral(r, num_bits);
        }

        expr* mk_numeral(uint64_t u, unsigned num_bits) const {
            return bv->mk_numeral(u, num_bits);
        }

        rational get_offset(univariate const& p) const {
            return p.empty() ? rational::zero() : p[0];
        }

        bool is_constant(univariate const& p) const {
            return p.empty() || std::all_of(p.begin() + 1, p.end(), [](rational const& n) { return n.is_zero(); });
        }

        bool is_zero(univariate const& p) const {
            for (auto n : p)
                if (n != 0)
                    return false;
            return true;
        }

        bool is_zero(rational const& p) const {
            return p.is_zero();
        }

#if 0
        // [d,c,b,a]  -->  ((a*x + b)*x + c)*x + d
        expr* mk_poly(univariate const& p) const {
            if (p.empty()) {
                return mk_numeral(rational::zero());
            }
            else {
                expr* e = mk_numeral(p.back());
                for (unsigned i = p.size() - 1; i-- > 0; ) {
                    e = bv->mk_bv_mul(e, x);
                    if (!p[i].is_zero())
                        e = bv->mk_bv_add(e, mk_numeral(p[i]));
                }
                return e;
            }
        }
#else
        // 2^k*x  -->  x << k
        // n*x    -->  n * x
        expr* mk_poly_term(rational const& coeff, expr* xpow, unsigned num_bits) const {
            unsigned pow;
            SASSERT(!coeff.is_zero());
            if (coeff.is_one())
                return xpow;
            if (coeff.is_power_of_two(pow))
                return bv->mk_bv_shl(xpow, mk_numeral(rational(pow), num_bits));
            return bv->mk_bv_mul(mk_numeral(coeff, num_bits), xpow);
        }

        // [d,c,b,a]  -->  d + c*x + b*(x*x) + a*(x*x*x)
        expr_ref mk_poly(univariate const& p, unsigned num_bits) {
            expr_ref e(m);
            if (p.empty())
                e = mk_numeral(rational::zero(), num_bits);
            else {
                if (!p[0].is_zero())
                    e = mk_numeral(p[0], num_bits);
                expr_ref xpow{x(num_bits), m};
                for (unsigned i = 1; i < p.size(); ++i) {
                    if (!p[i].is_zero()) {
                        expr* t = mk_poly_term(p[i], xpow, num_bits);
                        e = e ? bv->mk_bv_add(e, t) : t;
                    }
                    if (i + 1 < p.size())
                        xpow = bv->mk_bv_mul(xpow, x(num_bits));
                }
                if (!e)
                    e = mk_numeral(p[0], num_bits);
            }
            return e;
        }

        expr_ref mk_poly(rational const& p, unsigned num_bits) {
            return {mk_numeral(p, num_bits), m};
        }
#endif

        void add(expr* e, bool sign, dep_t dep) {
            reset_cache();
            if (sign)
                e = m.mk_not(e);
            if (dep == null_dep) {
                s->assert_expr(e);
                IF_VERBOSE(10, verbose_stream() << "(assert " << expr_ref(e, m) << ")\n");
            }
            else {
                expr* a = m.mk_const(m.mk_const_decl(symbol(dep), m.mk_bool_sort()));
                s->assert_expr(e, a);
                IF_VERBOSE(10, verbose_stream() << "(assert (! " << expr_ref(e, m) << "      :named " << expr_ref(a, m) << "))\n");
            }
        }

        template <typename lhs_t, typename rhs_t>
        void add_ule_impl(lhs_t const& lhs, rhs_t const& rhs, bool sign, unsigned num_bits, dep_t dep) {
            if (is_zero(rhs))
                add(m.mk_eq(mk_poly(lhs, num_bits), mk_poly(rhs, num_bits)), sign, dep);
            else
                add(bv->mk_ule(mk_poly(lhs, num_bits), mk_poly(rhs, num_bits)), sign, dep);
        }

        void add_ule(univariate const& lhs, univariate const& rhs, bool sign, unsigned num_bits, dep_t dep) override { add_ule_impl(lhs, rhs, sign, num_bits, dep); }
        void add_ule(univariate const& lhs, rational   const& rhs, bool sign, unsigned num_bits, dep_t dep) override { add_ule_impl(lhs, rhs, sign, num_bits, dep); }
        void add_ule(rational   const& lhs, univariate const& rhs, bool sign, unsigned num_bits, dep_t dep) override { add_ule_impl(lhs, rhs, sign, num_bits, dep); }

        void add_umul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, unsigned num_bits, dep_t dep) override {
            add(bv->mk_bvumul_no_ovfl(mk_poly(lhs, num_bits), mk_poly(rhs, num_bits)), !sign, dep);
        }

        void add_smul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, unsigned num_bits, dep_t dep) override {
            add(bv->mk_bvsmul_no_ovfl(mk_poly(lhs, num_bits), mk_poly(rhs, num_bits)), !sign, dep);
        }

        void add_smul_udfl(univariate const& lhs, univariate const& rhs, bool sign, unsigned num_bits, dep_t dep) override {
            add(bv->mk_bvsmul_no_udfl(mk_poly(lhs, num_bits), mk_poly(rhs, num_bits)), !sign, dep);
        }

        void add_lshr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_lshr(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_ashr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_ashr(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_shl(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_shl(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_and(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_and(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_or(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_or(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_xor(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_xor(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_not(univariate const& in, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_not(mk_poly(in, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }

        void add_inv(univariate const& in, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            // out == smallest_pseudo_inverse(in)
            expr_ref v = mk_poly(in, num_bits);
            expr_ref v_inv = mk_poly(out, num_bits);
            expr_ref parity = mk_parity(v, in, num_bits);
            // 2^parity = v * v_inv
            add(m.mk_eq(bv->mk_bv_shl(mk_numeral(1, num_bits), parity), bv->mk_bv_mul(v, v_inv)), false, dep);
            // v_inv <= 2^(N - parity) - 1
            expr* v_inv_max = bv->mk_bv_sub(bv->mk_bv_shl(mk_numeral(1, num_bits), bv->mk_bv_sub(mk_numeral(num_bits, num_bits), parity)), mk_numeral(1, num_bits));
            add(bv->mk_ule(v_inv, v_inv_max), false, dep);
        }

        void add_udiv(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_udiv(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }
        
        void add_urem(univariate const& in1, univariate const& in2, univariate const& out, bool sign, unsigned num_bits, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_urem(mk_poly(in1, num_bits), mk_poly(in2, num_bits)), mk_poly(out, num_bits)), sign, dep);
        }
        
        void add_ule_const(rational const& val, bool sign, unsigned num_bits, dep_t dep) override {
            if (val == 0)
                add(m.mk_eq(x(num_bits), mk_poly(val, num_bits)), sign, dep);
            else
                add(bv->mk_ule(x(num_bits), mk_poly(val, num_bits)), sign, dep);
        }

        void add_uge_const(rational const& val, bool sign, unsigned num_bits, dep_t dep) override {
            add(bv->mk_ule(mk_poly(val, num_bits), x(num_bits)), sign, dep);
        }

        void add_bit(unsigned idx, bool sign, dep_t dep) override {
            add(bv->mk_bit2bool(x(bit_width), idx), sign, dep);
        }

        expr_ref mk_parity(expr* v, univariate const& v_coeff, unsigned num_bits) {
            expr_ref parity(m);
            if (is_constant(v_coeff)) {
                parity = mk_numeral(get_offset(v_coeff).parity(num_bits), num_bits);
                return parity;
            }
            parity = m.mk_fresh_const("parity", bv->mk_sort(num_bits), false);
            expr* parity_1 = bv->mk_bv_add(parity, mk_numeral(1, num_bits));
            // if v = 0
            //   then parity = N
            //   else v = (v >> parity) << parity
            //        && v != (v >> parity+1) << parity+1
            // TODO: what about:  v[k:] = 0  &&  v[k+1:] != 0  ==>  parity = k  for each k?
            // TODO: helper axioms like parity <= N etc.?
            add(m.mk_ite(
                    m.mk_eq(v, mk_numeral(0, num_bits)),
                    m.mk_eq(parity, mk_numeral(num_bits, num_bits)),
                    m.mk_and(
                        m.mk_eq(bv->mk_bv_shl(bv->mk_bv_lshr(v, parity), parity), v),
                        m.mk_not(m.mk_eq(bv->mk_bv_shl(bv->mk_bv_lshr(v, parity_1), parity_1), v))
                    )
                ), false, null_dep);
            return parity;
        }

        lbool check() override {
            return s->check_sat();
        }

        void unsat_core(dep_vector& deps) override {
            deps.reset();
            expr_ref_vector core(m);
            s->get_unsat_core(core);
            for (expr* a : core) {
                unsigned dep = to_app(a)->get_decl()->get_name().get_num();
                deps.push_back(dep);
            }
            IF_VERBOSE(10, verbose_stream() << "core " << deps << "\n");
            SASSERT(deps.size() > 0);
        }

        rational model() override {
            rational& cached_model = model_cache.back();
            if (cached_model.is_neg()) {
                model_ref model;
                s->get_model(model);
                SASSERT(model);
                app* val = to_app(model->get_const_interp(m_x_decl));
                unsigned sz;
                VERIFY(bv->is_numeral(val, cached_model, sz));
            }
            return cached_model;
        }



        bool find_two(rational& out1, rational& out2) override {
            out1 = model();
            bool ok = true;
            push();
            add(m.mk_eq(mk_numeral(out1, bit_width), x(bit_width)), true, null_dep);
            switch (check()) {
            case l_true:
                out2 = model();
                break;
            case l_false:
                out2 = out1;
                break;
            default:
                ok = false;
                break;
            }
            pop(1);
            IF_VERBOSE(10, verbose_stream() << "viable " << out1 << " " << out2 << "\n");
            return ok;
        }


        std::ostream& display(std::ostream& out) const override {
            return out << *s;
        }
    };

#if 0
    // stub for alternative int-blast solver.
    class univariate_intblast_solver : public univariate_solver {
        ast_manager m;
        scoped_ptr<arith_util> a;
        scoped_ptr<solver> s;
        rational m_mod;
        unsigned m_scope_level = 0;
        func_decl_ref x_decl;
        expr_ref x;
        vector<rational> model_cache;

        void add(expr* e, bool sign, dep_t dep) {
            reset_cache();
            if (sign)
                e = m.mk_not(e);
            if (dep == null_dep) {
                s->assert_expr(e);
                IF_VERBOSE(10, verbose_stream() << "(assert " << expr_ref(e, m) << ")\n");
            }
            else {
                expr* a = m.mk_const(m.mk_const_decl(symbol(dep), m.mk_bool_sort()));
                s->assert_expr(e, a);
                IF_VERBOSE(10, verbose_stream() << "(assert (! " << expr_ref(e, m) << "      :named " << expr_ref(a, m) << "))\n");
            }
        }

        bool is_zero(univariate const& p) const {
            for (auto n : p)
                if (n != 0)
                    return false;
            return true;
        }

        bool is_zero(rational const& p) const {
            return p.is_zero();
        }

    public:
        univariate_intblast_solver(solver_factory& mk_solver, unsigned bit_width) :
            univariate_solver(bit_width),
            m_mod(rational::power_of_two(bit_width)),
            x_decl(m),
            x(m) {
            reg_decl_plugins(m);
            a = alloc(arith_util, m);
            params_ref p;
            p.set_bool("bv.polysat", false);
            // p.set_bool("smt", true);
            s = mk_solver(m, p, false, true, true, symbol::null);
            x_decl = m.mk_const_decl("x", a->mk_int());
            x = m.mk_const(x_decl);
            model_cache.push_back(rational(-1));
            s->assert_expr(a->mk_le(mk_numeral(0), x));
            s->assert_expr(a->mk_lt(x, mk_numeral(m_mod)));
        }
        
        
        ~univariate_intblast_solver() override = default;

        void reset_cache() {
            model_cache.back() = -1;
        }

        void push_cache() {
            rational v = model_cache.back();
            model_cache.push_back(v);
        }

        void pop_cache() {
            model_cache.pop_back();
        }

        void push() override {
            m_scope_level++;
            push_cache();
            s->push();
        }

        void pop(unsigned n) override {
            SASSERT(scope_level() >= n);
            m_scope_level -= n;
            pop_cache();
            s->pop(n);
        }

        unsigned scope_level() const override {
            return m_scope_level;
        }

        expr* mk_numeral(rational const& r) const {
            // assert 0 <= r < 2^bit-width
            return a->mk_int(r);
        }

        expr* mk_numeral(uint64_t u) const {
            // assert u < 2^bit-width
            return a->mk_int(rational(u, rational::ui64()));
        }

        lbool check() override {
            return s->check_sat();
        }

        expr_ref mk_poly(rational const& p) {
            return {mk_numeral(p), m};
        }


        // 2^k*x  -->  x << k
        // n*x    -->  n * x
        expr* mk_poly_term(rational const& coeff, expr* xpow) const {
            SASSERT(!coeff.is_zero());
            if (coeff.is_one())
                return xpow;
            return a->mk_mul(mk_numeral(coeff), xpow);
        }
        
        // [d,c,b,a]  -->  d + c*x + b*(x*x) + a*(x*x*x)
        expr_ref mk_poly(univariate const& p) {
            expr_ref e(m);
            if (p.empty())
                e = mk_numeral(rational::zero());
            else {
                if (!p[0].is_zero())
                    e = mk_numeral(p[0]);
                expr_ref xpow = x;
                for (unsigned i = 1; i < p.size(); ++i) {
                    if (!p[i].is_zero()) {
                        expr* t = mk_poly_term(p[i], xpow);
                        e = e ? a->mk_add(e, t) : t;
                    }
                    if (i + 1 < p.size())
                        xpow = a->mk_mul(xpow, x);
                }
                if (!e)
                    e = mk_numeral(p[0]);
            }
            if (!a->is_numeral(e) && e != x)
                e = a->mk_mod(e, mk_numeral(m_mod));
            return e;
        }


        template <typename lhs_t, typename rhs_t>
        void add_ule_impl(lhs_t const& lhs, rhs_t const& rhs, bool sign, dep_t dep) {
            // todo: simplify x - k == 0 into x = k
            // or ensure that bounds simplification tactic is enabled.
            // without bounds simplification, int-blasting doesnt work.
            if (is_zero(rhs))
                add(m.mk_eq(mk_poly(lhs), mk_poly(rhs)), sign, dep);
            else
                add(a->mk_le(mk_poly(lhs), mk_poly(rhs)), sign, dep);
        }

        void add_ule(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override { add_ule_impl(lhs, rhs, sign, dep); }
        void add_ule(univariate const& lhs, rational   const& rhs, bool sign, dep_t dep) override { add_ule_impl(lhs, rhs, sign, dep); }
        void add_ule(rational   const& lhs, univariate const& rhs, bool sign, dep_t dep) override { add_ule_impl(lhs, rhs, sign, dep); }

        void add_umul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            auto c = a->mk_mul(mk_poly(lhs), mk_poly(rhs));
            if (sign) // or the other way around?
                add(a->mk_lt(c, mk_numeral(m_mod)), sign, dep);
            else
                add(a->mk_ge(c, mk_numeral(m_mod)), sign, dep);                
        }

        void add_smul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_smul_udfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_lshr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_ashr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_shl(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_and(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_or(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_xor(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_not(univariate const& in, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_inv(univariate const& in, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_udiv(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }
        
        void add_urem(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }
        
        void add_ule_const(rational const& val, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_uge_const(rational const& val, bool sign, dep_t dep) override {
            NOT_IMPLEMENTED_YET();
        }

        void add_bit(unsigned idx, bool sign, dep_t dep) override {
            add(m.mk_eq(mk_numeral(m_mod/2), a->mk_mul(mk_numeral(rational::power_of_two(bit_width - idx - 1)), x)), sign, dep);            
        }

        void unsat_core(dep_vector& deps) override {
            deps.reset();
            expr_ref_vector core(m);
            s->get_unsat_core(core);
            for (expr* a : core) {
                unsigned dep = to_app(a)->get_decl()->get_name().get_num();
                deps.push_back(dep);
            }
            IF_VERBOSE(10, verbose_stream() << "core " << deps << "\n");
            SASSERT(deps.size() > 0);
        }

        rational model() override {
            rational& cached_model = model_cache.back();
            if (cached_model.is_neg()) {
                model_ref model;
                s->get_model(model);
                SASSERT(model);
                app* val = to_app(model->get_const_interp(x_decl));
                VERIFY(a->is_numeral(val, cached_model));
            }
            return cached_model;
        }

        bool find_two(rational& out1, rational& out2) override {
            out1 = model();
            bool ok = true;
            push();
            add(m.mk_eq(mk_numeral(out1), x), true, null_dep);
            switch (check()) {
            case l_true:
                out2 = model();
                break;
            case l_false:
                out2 = out1;
                break;
            default:
                ok = false;
                break;
            }
            pop(1);
            IF_VERBOSE(10, verbose_stream() << "viable " << out1 << " " << out2 << "\n");
            return ok;
        }

        std::ostream& display(std::ostream& out) const override {
            return out << *s;
        }            
    };
#endif

    class univariate_bitblast_factory : public univariate_solver_factory {
        symbol m_logic;
        scoped_ptr<solver_factory> m_factory;

    public:
        univariate_bitblast_factory() :
#if 1
            m_logic("QF_BV")
#else
            m_logic("ALL")
#endif
        {
            m_factory = mk_smt_strategic_solver_factory(m_logic);
        }

        ~univariate_bitblast_factory() override = default;

        scoped_ptr<univariate_solver> operator()(unsigned bit_width) override {
            return alloc(univariate_bitblast_solver, *m_factory, bit_width);
        }
    };

    scoped_ptr<univariate_solver_factory> mk_univariate_bitblast_factory() {
        return alloc(univariate_bitblast_factory);
    }
}

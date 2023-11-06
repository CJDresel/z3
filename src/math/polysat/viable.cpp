/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    maintain viable domains

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

Notes:

TODO: Investigate in depth a notion of phase caching for variables.
The Linear solver can be used to supply a phase in some cases.
In other cases, the phase of a variable assignment across branches
might be used in a call to is_viable. With phase caching on, it may
just check if the cached phase is viable without detecting that it is a propagation.

TODO: plan to fix the FI "pumping":
    1. simple looping detection and bitblasting fallback.  -- done
    2. intervals at multiple bit widths
        - for equations, this will give us exact solutions for all coefficients
        - for inequalities, a coefficient 2^k*a means that intervals are periodic because the upper k bits of x are irrelevant;
          storing the interval for x[K-k:0] would take care of this.

--*/


#include "util/debug.h"
#include "math/polysat/viable.h"
#include "math/polysat/solver.h"
#include "math/polysat/number.h"

namespace polysat {

    using namespace viable_query;

    struct inf_fi : public inference {
        viable& v;
        pvar var;
        inf_fi(viable& v, pvar var) : v(v), var(var) {}
        std::ostream& display(std::ostream& out) const override {
            return out << "Forbidden intervals for v" << var << ": " << viable::var_pp(v, var);
        }
    };

    viable::viable(solver& s):
        s(s),
        m_forbidden_intervals(s) {
    }

    viable::~viable() {
        for (entry* e : m_alloc)
            dealloc(e);
    }

    void viable::push_var(unsigned bit_width) {
        m_units.push_back({});
        m_equal_lin.push_back(nullptr);
        m_diseq_lin.push_back(nullptr);
    }

    void viable::pop_var() {
        m_units.pop_back();
        m_equal_lin.pop_back();
        m_diseq_lin.pop_back();
    }

    viable::entry* viable::alloc_entry() {
        if (m_alloc.empty())
            return alloc(entry);
        auto* e = m_alloc.back();
        e->reset();
        m_alloc.pop_back();
        return e;
    }

    unsigned viable::size(pvar v) const {
        return s.size(v);
    }

    viable::layer& viable::layers::ensure_layer(unsigned bit_width) {
        for (unsigned i = 0; i < m_layers.size(); ++i) {
            layer& l = m_layers[i];
            if (l.bit_width == bit_width)
                return l;
            else if (l.bit_width < bit_width) {
                m_layers.push_back(layer(0));
                for (unsigned j = m_layers.size(); --j > i; )
                    m_layers[j] = m_layers[j - 1];
                m_layers[i] = layer(bit_width);
                return m_layers[i];
            }
        }
        m_layers.push_back(layer(bit_width));
        return m_layers.back();
    }

    viable::layer* viable::layers::get_layer(unsigned bit_width) {
        return const_cast<layer*>(std::as_const(*this).get_layer(bit_width));
    }

    viable::layer const* viable::layers::get_layer(unsigned bit_width) const {
        for (layer const& l : m_layers)
            if (l.bit_width == bit_width)
                return &l;
        return nullptr;
    }

    void viable::pop_viable() {
        auto const& [v, k, e] = m_trail.back();
        // display_one(verbose_stream() << "Pop entry:  ", v, e) << "\n";
        SASSERT(well_formed(m_units[v]));
        switch (k) {
        case entry_kind::unit_e:
            entry::remove_from(m_units[v].get_layer(e)->entries, e);
            SASSERT(well_formed(m_units[v]));
            break;
        case entry_kind::equal_e:
            entry::remove_from(m_equal_lin[v], e);
            break;
        case entry_kind::diseq_e:
            entry::remove_from(m_diseq_lin[v], e);
            break;
        default:
            UNREACHABLE();
            break;
        }
        m_alloc.push_back(e);
        m_trail.pop_back();
    }

    void viable::push_viable() {
        auto& [v, k, e] = m_trail.back();
        // display_one(verbose_stream() << "Push entry: ", v, e) << "\n";
        entry*& entries = m_units[v].get_layer(e)->entries;
        SASSERT(e->prev() != e || !entries);
        SASSERT(e->prev() != e || e->next() == e);
        SASSERT(k == entry_kind::unit_e);
        (void)k;
        SASSERT(well_formed(m_units[v]));
        if (e->prev() != e) {
            entry* pos = e->prev();
            e->init(e);
            pos->insert_after(e);
            if (e->interval.lo_val() < entries->interval.lo_val())
                entries = e;
        }
        else
            entries = e;
        SASSERT(well_formed(m_units[v]));
        m_trail.pop_back();
    }

    bool viable::intersect(pdd const& p, pdd const& q, signed_constraint const& sc) {
        pvar v = null_var;
        bool first = true;
        bool prop = false;
        if (p.is_unilinear())
            v = p.var();
        else if (q.is_unilinear())
            v = q.var(), first = false;
        else
            return prop;

    try_viable:
        if (intersect(v, sc)) {
            if (s.is_conflict())
                return true;
            rational val;
            switch (find_viable(v, val)) {
            case find_t::singleton:
                propagate(v, val);
                prop = true;
                break;
            case find_t::empty:
                SASSERT(s.is_conflict());
                return true;
            default:
                break;
            }
        }
        if (first && q.is_unilinear() && q.var() != v) {
            v = q.var();
            first = false;
            goto try_viable;
        }
        return prop;
    }

    void viable::propagate(pvar v, rational const& val) {
        // NOTE: all propagations must be justified by a prefix of \Gamma,
        //       otherwise dependencies may be missed during conflict resolution.
        //       The propagation reason for v := val consists of the following constraints:
        //       - source constraint (already on \Gamma)
        //       - side conditions
        //       - i.lo() == i.lo_val() for each unit interval i
        //       - i.hi() == i.hi_val() for each unit interval i

        // NSB review:
        // the bounds added by x < p and p < x in forbidden_intervals
        // match_non_max, match_non_zero
        // use values that are approximations. Then the propagations in
        // try_assign_eval are incorrect.
        // For example, x > p means x has forbidden interval [0, p + 1[,
        // the numeric interval is [0, 1[, but p + 1 == 1 is not ensured
        // even p may have free variables.
        // the proper side condition on p + 1 is -1 > p or -2 >= p or p + 1 != 0
        // I am disabling match_non_max and match_non_zero from forbidden_interval
        // The narrowing rules in ule_constraint already handle the bounds propagaitons
        // as it propagates p != -1 and 0 != q (p < -1, q > 0),
        //

        for (auto const& c : get_constraints(v)) {
            s.try_assign_eval(c);
        }
        for (auto const& i : units(v)) {
            s.try_assign_eval(s.eq(i.lo(), i.lo_val()));
            s.try_assign_eval(s.eq(i.hi(), i.hi_val()));
        }
        s.assign_propagate_by_viable(v, val);
    }

    bool viable::intersect(pvar v, signed_constraint const& c) {
        LOG("intersect v" << v << " in " << lit_pp(s, c));
        if (s.is_assigned(v)) {
            // this can happen e.g. for c = ovfl*(v2,v3); where intersect(pdd,pdd,signed_constraint) will try both variables.
            LOG("abort intersect because v" << v << " is already assigned");
            return false;
        }
        entry* ne = alloc_entry();
        if (!m_forbidden_intervals.get_interval(c, v, *ne)) {
            m_alloc.push_back(ne);
            return false;
        }
        if (ne->interval.is_currently_empty()) {
            m_alloc.push_back(ne);
            return false;
        }
        for (signed_constraint sc : ne->side_cond) {
            // side conditions must evaluate to true by definition
            VERIFY(sc.is_currently_true(s));
            switch (sc.bvalue(s)) {
            case l_false:
                // We have a bool/eval conflict with one of the side conditions.
                // This happens if the side condition was already bool-propagated, but appears in the propagation queue after c.
                // UNREACHABLE();  // since propagation now checks bool/eval conflicts before narrowing, this case should be impossible.
                // TODO: why does it still trigger?
                s.set_conflict(~sc);
                return true;
            case l_undef:
                s.assign_eval(sc.blit());
                break;
            case l_true:
                // ok
                break;
            }
            // any bool/eval conflicts should have been discovered before narrowing;
            VERIFY(sc.bvalue(s) != l_false);
            // side conditions should be eval'd
            VERIFY_EQ(sc.bvalue(s), l_true);
        }
        if (ne->coeff == 1) {
            return intersect(v, ne);
        }
        else if (ne->coeff == -1) {
            insert(ne, v, m_diseq_lin, entry_kind::diseq_e);
            return true;
        }
        else {
            insert(ne, v, m_equal_lin, entry_kind::equal_e);
            return true;
        }
    }

    void viable::insert(entry* e, pvar v, ptr_vector<entry>& entries, entry_kind k) {
        SASSERT(well_formed(m_units[v]));
        m_trail.push_back({ v, k, e });
        s.m_trail.push_back(trail_instr_t::viable_add_i);
        e->init(e);
        if (!entries[v])
            entries[v] = e;
        else
            e->insert_after(entries[v]);
        SASSERT(entries[v]->invariant());
        SASSERT(well_formed(m_units[v]));
    }

    bool viable::intersect(pvar v, entry* ne) {
        SASSERT(!s.is_assigned(v));
        SASSERT(!ne->src.empty());
        entry*& entries = m_units[v].ensure_layer(ne->bit_width).entries;
        entry* e = entries;
        if (e && e->interval.is_full()) {
            m_alloc.push_back(ne);
            return false;
        }

        if (ne->interval.is_currently_empty()) {
            m_alloc.push_back(ne);
            return false;
        }

        auto create_entry = [&]() {
            m_trail.push_back({ v, entry_kind::unit_e, ne });
            s.m_trail.push_back(trail_instr_t::viable_add_i);
            ne->init(ne);
            return ne;
        };

        auto remove_entry = [&](entry* e) {
            m_trail.push_back({ v, entry_kind::unit_e, e });
            s.m_trail.push_back(trail_instr_t::viable_rem_i);
            e->remove_from(entries, e);
        };

        if (ne->interval.is_full()) {
            while (entries)
                remove_entry(entries);
            entries = create_entry();
            return true;
        }

        if (!e)
            entries = create_entry();
        else {
            entry* first = e;
            do {
                if (e->interval.currently_contains(ne->interval)) {
                    m_alloc.push_back(ne);
                    return false;
                }
                while (ne->interval.currently_contains(e->interval)) {
                    entry* n = e->next();
                    remove_entry(e);
                    if (!entries) {
                        entries = create_entry();
                        return true;
                    }
                    if (e == first)
                        first = n;
                    e = n;
                }
                SASSERT(e->interval.lo_val() != ne->interval.lo_val());
                if (e->interval.lo_val() > ne->interval.lo_val()) {
                    if (first->prev()->interval.currently_contains(ne->interval)) {
                        m_alloc.push_back(ne);
                        return false;
                    }
                    e->insert_before(create_entry());
                    if (e == first)
                        entries = e->prev();
                    SASSERT(well_formed(m_units[v]));
                    return true;
                }
                e = e->next();
            }
            while (e != first);
            // otherwise, append to end of list
            first->insert_before(create_entry());
        }
        SASSERT(well_formed(m_units[v]));
        return true;
    }

    template<bool FORWARD>
    bool viable::refine_viable(pvar v, rational const& val, fixed_bits_info const& fbi) {
        return refine_bits<FORWARD>(v, val, fbi) && refine_equal_lin(v, val) && refine_disequal_lin(v, val);
    }

namespace {
    rational div_floor(rational const& a, rational const& b) {
        return floor(a / b);
    }

    rational div_ceil(rational const& a, rational const& b) {
        return ceil(a / b);
    }

    /**
     * Given a*y0 mod M \in [lo;hi], try to find the largest y_max >= y0 such that for all y \in [y0;y_max] . a*y mod M \in [lo;hi].
     * Result may not be optimal.
     * NOTE: upper bound is inclusive.
     */
    rational compute_y_max(rational const& y0, rational const& a, rational const& lo0, rational const& hi, rational const& M) {
        // verbose_stream() << "y0=" << y0 << " a=" << a << " lo0=" << lo0 << " hi=" << hi << " M=" << M << std::endl;
        // SASSERT(0 <= y0 && y0 < M);  // not required
        SASSERT(1 <= a && a < M);
        SASSERT(0 <= lo0 && lo0 < M);
        SASSERT(0 <= hi && hi < M);

        if (lo0 <= hi) {
            SASSERT(lo0 <= mod(a*y0, M) && mod(a*y0, M) <= hi);
        }
        else {
            SASSERT(mod(a*y0, M) <= hi || mod(a*y0, M) >= lo0);
        }

        // wrapping intervals are handled by replacing the lower bound lo by lo - M
        rational const lo = lo0 > hi ? (lo0 - M) : lo0;

        // the length of the interval is now hi - lo + 1.
        // full intervals shouldn't go through this computation.
        SASSERT(hi - lo + 1 < M);

        auto contained = [&lo, &hi] (rational const& a_y) -> bool {
            return lo <= a_y && a_y <= hi;
        };

        auto delta_h = [&a, &lo, &hi] (rational const& a_y) -> rational {
            SASSERT(lo <= a_y && a_y <= hi);
            (void)lo;  // avoid warning in release mode
            return div_floor(hi - a_y, a);
        };

        // minimal k such that lo <= a*y0 + k*M
        rational const k = div_ceil(lo - a * y0, M);
        rational const kM = k*M;
        rational const a_y0 = a*y0 + kM;
        SASSERT(contained(a_y0));

        // maximal y for [lo;hi]-interval around a*y0
        // rational const y0h = y0 + div_floor(hi - a_y0, a);
        rational const delta0 = delta_h(a_y0);
        rational const y0h = y0 + delta0;
        rational const a_y0h = a_y0 + a*delta0;
        SASSERT(y0 <= y0h);
        SASSERT(contained(a_y0h));

        // Check the first [lo;hi]-interval after a*y0
        rational const y1l = y0h + 1;
        rational const a_y1l = a_y0h + a - M;
        if (!contained(a_y1l))
            return y0h;
        rational const delta1 = delta_h(a_y1l);
        rational const y1h = y1l + delta1;
        rational const a_y1h = a_y1l + a*delta1;
        SASSERT(y1l <= y1h);
        SASSERT(contained(a_y1h));

        // Check the second [lo;hi]-interval after a*y0
        rational const y2l = y1h + 1;
        rational const a_y2l = a_y1h + a - M;
        if (!contained(a_y2l))
            return y1h;
        SASSERT(contained(a_y2l));

        // At this point, [y1l;y1h] must be a full y-interval that can be extended to the right.
        // Extending the interval can only be possible if the part not covered by [lo;hi] is smaller than the coefficient a.
        // The size of the gap is (lo + M) - (hi + 1).
        SASSERT(lo + M - hi - 1 < a);

        // The points a*[y0l;y0h] + k*M fall into the interval [lo;hi].
        // After the first overflow, the points a*[y1l .. y1h] + (k - 1)*M fall into [lo;hi].
        // With each overflow, these points drift by some offset alpha.
        rational const step = y1h - y0h;
        rational const alpha = a * step - M;

        if (alpha == 0) {
            // the points do not drift after overflow
            // => y_max is infinite
            return y0 + M;
        }

        rational const i =
            alpha < 0
            // alpha < 0:
            // With each overflow to the right, the points drift to the left.
            // We can continue overflowing while a * yil >= lo, where yil = y1l + i * step.
            ? div_floor(lo - a_y1l, alpha)
            // alpha > 0:
            // With each overflow to the right, the points drift to the right.
            // We can continue overflowing while a * yih <= hi, where yih = y1h + i * step.
            : div_floor(hi - a_y1h, alpha);

        // i is the number of overflows to the right
        SASSERT(i >= 0);

        // a * [yil;yih] is the right-most y-interval that is completely in [lo;hi].
        rational const yih = y1h + i * step;
        rational const a_yih = a_y1h + i * alpha;
        SASSERT_EQ(a_yih, a*yih + (k - i - 1)*M);
        SASSERT(contained(a_yih));

        // The next interval to the right may contain a few more values if alpha > 0
        // (because only the upper end moved out of the interval)
        rational const y_next = yih + 1;
        rational const a_y_next = a_yih + a - M;
        if (contained(a_y_next))
            return y_next + delta_h(a_y_next);
        else
            return yih;
    }

    /**
     * Given a*y0 mod M \in [lo;hi], try to find the smallest y_min <= y0 such that for all y \in [y_min;y0] . a*y mod M \in [lo;hi].
     * Result may not be optimal.
     * NOTE: upper bound is inclusive.
     */
    rational compute_y_min(rational const& y0, rational const& a, rational const& lo, rational const& hi, rational const& M) {
        // verbose_stream() << "y0=" << y0 << " a=" << a << " lo=" << lo << " hi=" << hi << " M=" << M << std::endl;
        // SASSERT(0 <= y0 && y0 < M);  // not required
        SASSERT(1 <= a && a < M);
        SASSERT(0 <= lo && lo < M);
        SASSERT(0 <= hi && hi < M);

        auto const negateM = [&M] (rational const& x) -> rational {
            if (x.is_zero())
                return x;
            else
                return M - x;
        };

        rational y_min = -compute_y_max(-y0, a, negateM(hi), negateM(lo), M);
        while (y_min > y0)
            y_min -= M;
        return y_min;
    }

    /**
     * Given a*y0 mod M \in [lo;hi],
     * find the largest interval [y_min;y_max] around y0 such that for all y \in [y_min;y_max] . a*y mod M \in [lo;hi].
     * Result may not be optimal.
     * NOTE: upper bounds are inclusive.
     */
    std::pair<rational, rational> compute_y_bounds(rational const& y0, rational const& a, rational const& lo, rational const& hi, rational const& M) {
        // verbose_stream() << "y0=" << y0 << " a=" << a << " lo=" << lo << " hi=" << hi << " M=" << M << std::endl;
        SASSERT(0 <= y0 && y0 < M);
        SASSERT(1 <= a && a < M);
        SASSERT(0 <= lo && lo < M);
        SASSERT(0 <= hi && hi < M);

        auto const is_valid = [&] (rational const& y) -> bool {
            rational const a_y = mod(a * y, M);
            if (lo <= hi)
                return lo <= a_y && a_y <= hi;
            else
                return a_y <= hi || lo <= a_y;
        };

        unsigned const max_refinements = 100;
        unsigned i = 0;
        rational const y_max_max = y0 + M - 1;
        rational y_max = compute_y_max(y0, a, lo, hi, M);
        while (y_max < y_max_max && is_valid(y_max + 1)) {
            y_max = compute_y_max(y_max + 1, a, lo, hi, M);
            if (++i == max_refinements) {
                // verbose_stream() << "y0=" << y0 << ", a=" << a << ", lo=" << lo << ", hi=" << hi << "\n";
                // verbose_stream() << "refined y_max: " << y_max << "\n";
                break;
            }
        }

        i = 0;
        rational const y_min_min = y_max - M + 1;
        rational y_min = y0;
        while (y_min > y_min_min && is_valid(y_min - 1)) {
            y_min = compute_y_min(y_min - 1, a, lo, hi, M);
            if (++i == max_refinements) {
                // verbose_stream() << "y0=" << y0 << ", a=" << a << ", lo=" << lo << ", hi=" << hi << "\n";
                // verbose_stream() << "refined y_min: " << y_min << "\n";
                break;
            }
        }

        SASSERT(y_min <= y0 && y0 <= y_max);
        rational const len = y_max - y_min + 1;
        if (len >= M)
            // full
            return { rational::zero(), M - 1 };
        else
            return { mod(y_min, M), mod(y_max, M) };
    }
}

    template<bool FORWARD>
    bool viable::refine_bits(pvar v, rational const& val, fixed_bits_info const& fbi) {

        pdd v_pdd = s.var(v);

        // TODO: We might also extend simultaneously up and downwards if we want the actual interval (however, this might make use of more fixed bits and is weaker - worse - therefore)
        entry* ne = alloc_entry();
        rational new_val = extend_by_bits<FORWARD>(v_pdd, val, fbi, ne->src, ne->side_cond);

        if (new_val == val) {
            m_alloc.push_back(ne);
            return true;
        }

        // TODO: Extend in both directions? (Less justifications vs. bigger intervals)
        // TODO: could also try to extend backwards as much as we can without introducing new justifications?
        rational new_val2 = extend_by_bits<!FORWARD>(v_pdd, val, fbi, ne->src, ne->side_cond);

        ne->refined = true;
        ne->coeff = 1;
        ne->bit_width = s.size(v);
        if (FORWARD) {
            LOG("refine-bits FORWARD for v" << v << " = " << val << " to [" << new_val2 << ", " << new_val << "[");
            ne->interval = eval_interval::proper(v_pdd.manager().mk_val(new_val2), new_val2, v_pdd.manager().mk_val(new_val), new_val);
        }
        else {
            LOG("refine-bits BACKWARD for v" << v << " = " << val << " to [" << new_val << ", " << new_val2 << "[");
            ne->interval = eval_interval::proper(v_pdd.manager().mk_val(new_val), new_val, v_pdd.manager().mk_val(new_val2), new_val2);
        }
        SASSERT(ne->interval.currently_contains(val));
        intersect(v, ne);
        return false;
    }

    /**
     * Traverse all interval constraints with coefficients to check whether current value 'val' for
     * 'v' is feasible. If not, extract a (maximal) interval to block 'v' from being assigned val.
     *
     * To investigate:
     * - side conditions are stronger than for unit intervals. They constrain the lower and upper bounds to
     *   be precisely the assigned values. This is to ensure that lo/hi that are computed based on lo_val
     *   and division with coeff are valid. Is there a more relaxed scheme?
     */
    bool viable::refine_equal_lin(pvar v, rational const& val) {
        // LOG_H2("refine-equal-lin with v" << v << ", val = " << val);
        entry const* e = m_equal_lin[v];
        if (!e)
            return true;
        entry const* first = e;
        auto& m = s.var2pdd(v);
        unsigned const N = m.power_of_2();
        rational const& max_value = m.max_value();
        rational const& mod_value = m.two_to_N();

        // Rotate the 'first' entry, to prevent getting stuck in a refinement loop
        // with an early entry when a later entry could give a better interval.
        m_equal_lin[v] = m_equal_lin[v]->next();

        do {
            rational coeff_val = mod(e->coeff * val, mod_value);
            if (e->interval.currently_contains(coeff_val)) {
                // IF_LOGGING(
                //     verbose_stream() << "refine-equal-lin for v" << v << " in src: ";
                //     for (const auto& src : e->src)
                //         verbose_stream() << lit_pp(s, src) << "\n";
                // );
                // LOG("forbidden interval v" << v << " " << num_pp(s, v, val) << "    " << num_pp(s, v, e->coeff, true) << " * " << e->interval);

                if (mod(e->interval.hi_val() + 1, mod_value) == e->interval.lo_val()) {
                    // We have an equation:  a * v == b
                    rational const a = e->coeff;
                    rational const b = e->interval.hi_val();
                    LOG("refine-equal-lin: equation detected: " << dd::val_pp(m, a, true) << " * v" << v << " == " << dd::val_pp(m, b, false));
                    unsigned const parity_a = get_parity(a, N);
                    unsigned const parity_b = get_parity(b, N);
                    if (parity_a > parity_b) {
                        // No solution
                        LOG("refined: no solution due to parity");
                        entry* ne = alloc_entry();
                        ne->refined = true;
                        ne->src = e->src;
                        ne->side_cond = e->side_cond;
                        ne->coeff = 1;
                        ne->bit_width = e->bit_width;
                        ne->interval = eval_interval::full();
                        intersect(v, ne);
                        return false;
                    }
                    if (parity_a == 0) {
                        // "fast path" for odd a
                        rational a_inv;
                        VERIFY(a.mult_inverse(N, a_inv));
                        rational const hi = mod(a_inv * b, mod_value);
                        rational const lo = mod(hi + 1, mod_value);
                        LOG("refined to [" << num_pp(s, v, lo) << ", " << num_pp(s, v, hi) << "[");
                        SASSERT_EQ(mod(a * hi, mod_value), b);  // hi is the solution
                        entry* ne = alloc_entry();
                        ne->refined = true;
                        ne->src = e->src;
                        ne->side_cond = e->side_cond;
                        ne->coeff = 1;
                        ne->bit_width = e->bit_width;
                        ne->interval = eval_interval::proper(m.mk_val(lo), lo, m.mk_val(hi), hi);
                        SASSERT(ne->interval.currently_contains(val));
                        intersect(v, ne);
                        return false;
                    }
                    // 2^k * v == a_inv * b
                    // 2^k solutions because only the lower N-k bits of v are fixed.
                    //
                    // Smallest solution is v0 == a_inv * (b >> k)
                    // Solutions are of the form v_i = v0 + 2^(N-k) * i for i in { 0, 1, ..., 2^k - 1 }.
                    // Forbidden intervals: [v_i + 1; v_{i+1}[  == [ v_i + 1; v_i + 2^(N-k) [
                    // We need the interval that covers val:
                    //      v_i + 1 <= val < v_i + 2^(N-k)
                    //
                    // TODO: create one interval for v[N-k:] instead of 2^k intervals for v.
                    unsigned const k = parity_a;
                    rational const a_inv = a.pseudo_inverse(N);
                    unsigned const N_minus_k = N - k;
                    rational const two_to_N_minus_k = rational::power_of_two(N_minus_k);
                    rational const v0 = mod(a_inv * machine_div2k(b, k), two_to_N_minus_k);
                    SASSERT(mod(val, two_to_N_minus_k) != v0);  // val is not a solution
                    rational const vi = v0 + clear_lower_bits(mod(val - v0, mod_value), N_minus_k);
                    rational const lo = mod(vi + 1, mod_value);
                    rational const hi = mod(vi + two_to_N_minus_k, mod_value);
                    LOG("refined to [" << num_pp(s, v, lo) << ", " << num_pp(s, v, hi) << "[");
                    SASSERT_EQ(mod(a * (lo - 1), mod_value), b);  // lo-1 is a solution
                    SASSERT_EQ(mod(a * hi, mod_value), b);  // hi is a solution
                    entry* ne = alloc_entry();
                    ne->refined = true;
                    ne->src = e->src;
                    ne->side_cond = e->side_cond;
                    ne->coeff = 1;
                    ne->bit_width = e->bit_width;
                    ne->interval = eval_interval::proper(m.mk_val(lo), lo, m.mk_val(hi), hi);
                    SASSERT(ne->interval.currently_contains(val));
                    intersect(v, ne);
                    return false;
                }

                // TODO: special handling for the even factors of e->coeff = 2^k * a', a' odd
                //       (create one interval for v[N-k:] instead of 2^k intervals for v)

                // compute_y_bounds calculates with inclusive upper bound, so we need to adjust argument and result accordingly.
                rational const hi_val_incl = e->interval.hi_val().is_zero() ? max_value : (e->interval.hi_val() - 1);
                auto [lo, hi] = compute_y_bounds(val, e->coeff, e->interval.lo_val(), hi_val_incl, mod_value);
                hi += 1;
                LOG("refined to [" << num_pp(s, v, lo) << ", " << num_pp(s, v, hi) << "[");
                // verbose_stream() << "lo=" << lo << " val=" << val << " hi=" << hi << "\n";
                if (lo <= hi) {
                    SASSERT(0 <= lo && lo <= val && val < hi && hi <= mod_value);
                } else {
                    SASSERT(0 < hi && hi < lo && lo < mod_value && (val < hi || lo <= val));
                }
                bool full = (lo == 0 && hi == mod_value);
                if (hi == mod_value)
                    hi = 0;
                entry* ne = alloc_entry();
                ne->refined = true;
                ne->src = e->src;
                ne->side_cond = e->side_cond;
                ne->coeff = 1;
                ne->bit_width = e->bit_width;
                if (full)
                    ne->interval = eval_interval::full();
                else
                    ne->interval = eval_interval::proper(m.mk_val(lo), lo, m.mk_val(hi), hi);
                SASSERT(ne->interval.currently_contains(val));
                intersect(v, ne);
                return false;
            }
            e = e->next();
        }
        while (e != first);
        return true;
    }

    bool viable::refine_disequal_lin(pvar v, rational const& val) {
        // LOG_H2("refine-disequal-lin with v" << v << ", val = " << val);
        entry const* e = m_diseq_lin[v];
        if (!e)
            return true;
        entry const* first = e;
        rational const& max_value = s.var2pdd(v).max_value();
        rational const mod_value = max_value + 1;

        // Rotate the 'first' entry, to prevent getting stuck in a refinement loop
        // with an early entry when a later entry could give a better interval.
        m_diseq_lin[v] = m_diseq_lin[v]->next();

        do {
            // IF_LOGGING(
            //         verbose_stream() << "refine-disequal-lin for v" << v << " in src: ";
            //         for (const auto& src : e->src)
            //             verbose_stream() << lit_pp(s, src) << "\n";
            // );

            // We compute an interval if the concrete value 'val' violates the constraint:
            //      p*val + q >  r*val + s  if e->src.is_positive()
            //      p*val + q >= r*val + s  if e->src.is_negative()
            // Note that e->interval is meaningless in this case,
            // we just use it to transport the values p,q,r,s
            rational const& p = e->interval.lo_val();
            rational const& q_ = e->interval.lo().val();
            rational const& r = e->interval.hi_val();
            rational const& s_ = e->interval.hi().val();
            SASSERT(p != r && p != 0 && r != 0);
            SASSERT(e->src.size() == 1);

            rational const a = mod(p * val + q_, mod_value);
            rational const b = mod(r * val + s_, mod_value);
            rational const np = mod_value - p;
            rational const nr = mod_value - r;
            int const corr = e->src[0].is_negative() ? 1 : 0;

            auto delta_l = [&](rational const& val) {
                rational num = a - b + corr;
                rational l1 = floor(b / r);
                rational l2 = val;
                if (p > r)
                    l2 = ceil(num / (p - r)) - 1;
                rational l3 = ceil(num / (p + nr)) - 1;
                rational l4 = ceil((mod_value - a) / np) - 1;
                rational d1 = l3;
                rational d2 = std::min(l1, l2);
                rational d3 = std::min(l1, l4);
                rational d4 = std::min(l2, l4);
                rational dmax = std::max(std::max(d1, d2), std::max(d3, d4));
                return std::min(val, dmax);
            };
            auto delta_u = [&](rational const& val) {
                rational num = a - b + corr;
                rational h1 = floor(b / nr);
                rational h2 = max_value - val;
                if (r > p)
                    h2 = ceil(num / (r - p)) - 1;
                rational h3 = ceil(num / (np + r)) - 1;
                rational h4 = ceil((mod_value - a) / p) - 1;
                rational d1 = h3;
                rational d2 = std::min(h1, h2);
                rational d3 = std::min(h1, h4);
                rational d4 = std::min(h2, h4);
                rational dmax = std::max(std::max(d1, d2), std::max(d3, d4));
                return std::min(max_value - val, dmax);
            };

            if (a > b || (e->src[0].is_negative() && a == b)) {
                rational lo = val - delta_l(val);
                rational hi = val + delta_u(val) + 1;

                LOG("refine-disequal-lin: " << " [" << lo << ", " << hi << "[");

                SASSERT(0 <= lo && lo <= val);
                SASSERT(val <= hi && hi <= mod_value);
                if (hi == mod_value) hi = 0;
                pdd lop = s.var2pdd(v).mk_val(lo);
                pdd hip = s.var2pdd(v).mk_val(hi);
                entry* ne = alloc_entry();
                ne->refined = true;
                ne->src = e->src;
                ne->side_cond = e->side_cond;
                ne->coeff = 1;
                ne->bit_width = e->bit_width;
                ne->interval = eval_interval::proper(lop, lo, hip, hi);
                intersect(v, ne);
                return false;
            }
            e = e->next();
        }
        while (e != first);
        return true;
    }

    // Skips all values that are not feasible w.r.t. fixed bits
    template<bool FORWARD>
    rational viable::extend_by_bits(pdd const& var, rational const& bound, fixed_bits_info const& fbi, vector<signed_constraint>& src, vector<signed_constraint>& side_cond) const {
        unsigned const k = var.power_of_2();
        if (fbi.is_empty())  // TODO: this check doesn't do anything.
            return bound;

        svector<lbool> const& fixed = fbi.fixed;

        SASSERT(k == fixed.size());

        sat::literal_set added_src;
        sat::literal_set added_side_cond;

        auto add_justification = [&](unsigned i) {
            SASSERT(!fbi.just_src[i].empty() || !fbi.just_slicing[i].empty());
            for (sat::literal lit : fbi.just_src[i]) {
                if (added_src.contains(lit))
                    continue;
                added_src.insert(lit);
                src.push_back(s.lit2cnstr(lit));
            }
            for (sat::literal lit : fbi.just_side_cond[i]) {
                if (added_side_cond.contains(lit))
                    continue;
                added_side_cond.insert(lit);
                side_cond.push_back(s.lit2cnstr(lit));
            }
            for (slicing::enode* n : fbi.just_slicing[i]) {
                s.m_slicing.explain_fixed(n, [&](sat::literal lit) {
                    if (!added_src.contains(lit)) {
                        added_src.insert(lit);
                        src.push_back(s.lit2cnstr(lit));
                    }
                }, [&](pvar v){
                    sat::literal lit = s.eq(s.var(v), s.get_value(v)).blit();
                    if (!s.m_bvars.is_assigned(lit))
                        s.assign_eval(lit);
                    if (!added_src.contains(lit)) {
                        added_src.insert(lit);
                        src.push_back(s.lit2cnstr(lit));
                    }
                });
            }
        };

        unsigned firstFail;
        for (firstFail = k; firstFail > 0; firstFail--) {
            if (fixed[firstFail - 1] != l_undef) {
                lbool current = to_lbool(bound.get_bit(firstFail - 1));
                if (current != fixed[firstFail - 1])
                    break;
            }
        }
        if (firstFail == 0)
            return bound; // the value is feasible according to fixed bits

        svector<lbool> new_bound(k);

        for (unsigned i = 0; i < firstFail; i++) {
            if (fixed[i] != l_undef) {
                SASSERT(fixed[i] == l_true || fixed[i] == l_false);
                new_bound[i] = fixed[i];
                if (i == firstFail - 1 || FORWARD != (fixed[i] == l_false))
                    add_justification(i); // Minimize number of responsible fixed bits; we only add those justifications we need for sure
            }
            else
                new_bound[i] = FORWARD ? l_false : l_true;
        }

        bool carry = fixed[firstFail - 1] == (FORWARD ? l_false : l_true);

        for (unsigned i = firstFail; i < new_bound.size(); i++) {
            if (fixed[i] == l_undef) {
                lbool current = to_lbool(bound.get_bit(i));
                if (carry) {
                    if (FORWARD) {
                        if (current == l_false) {
                            new_bound[i] = l_true;
                            carry = false;
                        }
                        else
                            new_bound[i] = l_false;
                    }
                    else {
                        if (current == l_true) {
                            new_bound[i] = l_false;
                            carry = false;
                        }
                        else
                            new_bound[i] = l_true;
                    }
                }
                else
                    new_bound[i] = current;
            }
            else {
                new_bound[i] = fixed[i];
                if (carry)
                    add_justification(i); // Again, we need this justification; if carry is false we don't need it
            }
        }
        SASSERT(!src.empty());
        if (carry) {
            // We covered everything
            /*if (FORWARD)
                return rational::power_of_two(k);
            else*/
                return rational::zero();
        }

        // TODO: Directly convert new_bound in rational?
        rational ret = rational::zero();
        for (unsigned i = new_bound.size(); i > 0; i--) {
            ret *= 2;
            SASSERT(new_bound[i - 1] != l_undef);
            ret += new_bound[i - 1] == l_true ? 1 : 0;
        }
        if (!FORWARD)
            return ret + 1;
        return ret;
    }

    // returns true iff no conflict was encountered
    bool viable::collect_bit_information(pvar v, bool add_conflict, fixed_bits_info& out_fbi) {

        pdd p = s.var(v);
        unsigned const v_sz = s.size(v);
        out_fbi.reset(v_sz);
        svector<lbool>& fixed = out_fbi.fixed;
        vector<sat::literal_vector>& just_src = out_fbi.just_src;
        vector<sat::literal_vector>& just_side_cond = out_fbi.just_side_cond;

        slicing::justified_fixed_bits_vector fbs;
        s.m_slicing.collect_fixed(v, fbs);

        for (auto const& fb : fbs) {
            LOG("slicing fixed bits: v" << v << "[" << fb.hi << ":" << fb.lo << "] = " << fb.value);
            for (unsigned i = fb.lo; i <= fb.hi; ++i) {
                SASSERT(out_fbi.just_src[i].empty());  // since we don't get overlapping ranges from collect_fixed.
                SASSERT(out_fbi.just_side_cond[i].empty());
                SASSERT(out_fbi.just_slicing[i].empty());
                out_fbi.fixed[i] = to_lbool(fb.value.get_bit(i - fb.lo));
                out_fbi.just_slicing[i].push_back(fb.just);
            }
        }

        entry* e1 = m_equal_lin[v];
        entry* e2 = m_units[v].get_entries(s.size(v));  // TODO: take other widths into account (will be done automatically by tracking fixed bits in the slicing egraph)
        entry* first = e1;
        if (!e1 && !e2)
            return true;

        clause_builder builder(s, "bit check");
        sat::literal_set added;
        vector<std::pair<entry*, trailing_bits>> postponed;

        auto add_literal = [&builder, &added](sat::literal lit) {
            if (added.contains(lit))
                return;
            added.insert(lit);
            builder.insert_eval(~lit);
        };

        auto add_literals = [&add_literal](sat::literal_vector const& lits) {
            for (sat::literal lit : lits)
                add_literal(lit);
        };

        auto add_entry = [&add_literal](entry* e) {
            for (const auto& sc : e->side_cond)
                add_literal(sc.blit());
            for (const auto& src : e->src)
                add_literal(src.blit());
        };

        auto add_slicing = [this, &add_literal](slicing::enode* n) {
            s.m_slicing.explain_fixed(n, [&](sat::literal lit) {
                add_literal(lit);
            }, [&](pvar v){
                LOG("from slicing: v" << v);
                add_literal(s.eq(s.var(v), s.get_value(v)).blit());
            });
        };

        auto add_bit_justification = [&add_literals, &add_slicing](fixed_bits_info const& fbi, unsigned i) {
            add_literals(fbi.just_src[i]);
            add_literals(fbi.just_side_cond[i]);
            for (slicing::enode* n : fbi.just_slicing[i])
                add_slicing(n);
        };

        if (e1) {
            unsigned largest_lsb = 0;
            do {
                if (e1->src.size() != 1) {
                    // We just consider the ordinary constraints and not already contracted ones
                    e1 = e1->next();
                    continue;
                }
                signed_constraint& src = e1->src[0];
                single_bit bit;
                trailing_bits lsb;
                if (src->is_ule() &&
                    simplify_clause::get_bit(s.subst(src->to_ule().lhs()), s.subst(src->to_ule().rhs()), p, bit, src.is_positive()) && p.is_var()) {
                    lbool prev = fixed[bit.position];
                    fixed[bit.position] = to_lbool(bit.positive);
                    //verbose_stream() << "Setting bit " << bit.position << " to " << bit.positive << " because of " << e->src << "\n";
                    if (prev != l_undef && fixed[bit.position] != prev) {
                        // LOG("Bit conflicting " << e1->src << " with " << just_src[bit.position][0]);  // NOTE: just_src may be empty if the justification is by slicing
                        if (add_conflict) {
                            add_bit_justification(out_fbi, bit.position);
                            add_entry(e1);
                            s.set_conflict(*builder.build());
                        }
                        return false;
                    }
                    // just override; we prefer bit constraints over parity as those are easier for subsumption to remove
                    // verbose_stream() << "Adding bit constraint: " <<  e->src[0] << " (" << bit.position << ")\n";
                    out_fbi.set_just(bit.position, e1);
                }
                else if (src->is_eq() &&
                         simplify_clause::get_lsb(s.subst(src->to_ule().lhs()), s.subst(src->to_ule().rhs()), p, lsb, src.is_positive()) && p.is_var()) {
                    if (src.is_positive()) {
                        for (unsigned i = 0; i < lsb.length; i++) {
                            lbool prev = fixed[i];
                            fixed[i] = to_lbool(lsb.bits.get_bit(i));
                            if (prev == l_undef) {
                                SASSERT(just_src[i].empty());
                                out_fbi.set_just(i, e1);
                                continue;
                            }
                            if (fixed[i] != prev) {
                                // LOG("Positive parity conflicting " << e1->src << " with " << just_src[i][0]);  // NOTE: just_src may be empty if the justification is by slicing
                                if (add_conflict) {
                                    add_bit_justification(out_fbi, i);
                                    add_entry(e1);
                                    s.set_conflict(*builder.build());
                                }
                                return false;
                            }
                            // Prefer justifications from larger masks (less premises)
                            // TODO: Check that we don't override justifications coming from bit constraints
                            if (largest_lsb < lsb.length)
                                out_fbi.set_just(i, e1);
                        }
                        largest_lsb = std::max(largest_lsb, lsb.length);
                    }
                    else
                        postponed.push_back({ e1, lsb });
                }
                e1 = e1->next();
            } while(e1 != first);
        }

        // so far every bit is justified by a single constraint
        SASSERT(all_of(just_src, [](auto const& vec) { return vec.size() <= 1; }));

#if 0 // is the benefit enough?
        if (e2) {
            unsigned largest_msb = 0;
            first = e2;
            do {
                if (e2->src.size() != 1) {
                    e2 = e2->next();
                    continue;
                }
                signed_constraint& src = e2->src[0];
                leading_bits msb;
                if (src->is_ule() &&
                    simplify_clause::get_msb(s.subst(src->to_ule().lhs()), s.subst(src->to_ule().rhs()), p, msb, src.is_positive()) && p.is_var()) {
                    for (unsigned i = fixed.size() - msb.length; i < fixed.size(); i++) {
                        lbool prev = fixed[i];
                        fixed[i] = msb.positive ? l_true : l_false;
                        if (prev != l_undef) {
                            if (fixed[i] != prev) {
                                LOG("msb conflicting " << e2->src << " with " << justifications[i][0]->src);
                                if (add_conflict) {
                                    add_entry_list(justifications[i]);
                                    add_entry(e2);
                                    s.set_conflict(*builder.build());
                                }
                                return false;
                            }
                            else {
                                if (largest_msb < msb.length) {
                                    justifications[i].clear();
                                    justifications[i].push_back(e2);
                                }
                            }
                        }
                        else {
                            SASSERT(justifications[i].empty());
                            justifications[i].push_back(e2);
                        }
                    }
                    largest_msb = std::max(largest_msb, msb.length);
                }
                e2 = e2->next();
            } while(e2 != first);
        }
#endif

        // TODO: Incomplete - e.g., if we know the trailing bits are not 00 not 10 not 01 and not 11 we could also detect a conflict
        // This would require partially clause solving (worth the effort?)
        bool_vector removed(postponed.size(), false);
        bool changed;
        do { // fixed-point required?
            changed = false;
            for (unsigned j = 0; j < postponed.size(); j++) {
                if (removed[j])
                    continue;
                const auto& neg = postponed[j];
                unsigned indet = 0;
                unsigned last_indet = 0;
                unsigned i = 0;
                for (; i < neg.second.length; i++) {
                    if (fixed[i] != l_undef) {
                        if (fixed[i] != to_lbool(neg.second.bits.get_bit(i))) {
                            removed[j] = true;
                            break; // this is already satisfied
                        }
                    }
                    else {
                        indet++;
                        last_indet = i;
                    }
                }
                if (i == neg.second.length) {
                    if (indet == 0) {
                        // Already false
                        LOG("Found conflict with constraint " << neg.first->src);
                        if (add_conflict) {
                            for (unsigned k = 0; k < neg.second.length; k++)
                                add_bit_justification(out_fbi, k);
                            add_entry(neg.first);
                            s.set_conflict(*builder.build());
                        }
                        return false;
                    }
                    else if (indet == 1) {
                        // Simple BCP
                        SASSERT(just_src[last_indet].empty());
                        SASSERT(just_side_cond[last_indet].empty());
                        for (unsigned k = 0; k < neg.second.length; k++) {
                            if (k != last_indet) {
                                SASSERT(fixed[k] != l_undef);
                                out_fbi.push_from_bit(last_indet, k);
                            }
                        }
                        out_fbi.push_just(last_indet, neg.first);
                        fixed[last_indet] = neg.second.bits.get_bit(last_indet) ? l_false : l_true;
                        removed[j] = true;
                        LOG("Applying fast BCP on bit " << last_indet << " from constraint " << neg.first->src);
                        changed = true;
                    }
                }
            }
        } while(changed);

        return true;
    }

#if 0
    bool viable::collect_bit_information(pvar v, bool add_conflict, const vector<signed_constraint>& cnstr, svector<lbool>& fixed, vector<vector<signed_constraint>>& justifications) {
        pdd p = s.var(v);
        fixed.clear();
        justifications.clear();
        fixed.resize(p.power_of_2(), l_undef);
        justifications.resize(p.power_of_2(), vector<signed_constraint>());
        if (cnstr.empty())
            return true;

        clause_builder builder(s, "bit check");
        uint_set already_added;
        vector<std::pair<signed_constraint, trailing_bits>> postponed;

        auto add_entry = [&builder, &already_added](const signed_constraint& src) {
            if (already_added.contains(src.bvar()))
                return;
            already_added.insert(src.bvar());
            builder.insert_eval(~src);
        };

        auto add_entry_list = [add_entry](const vector<signed_constraint>& list) {
            for (const auto& e : list)
                add_entry(e);
        };

        unsigned largest_mask = 0;

        for (unsigned i = 0; i < cnstr.size(); i++) {
            const signed_constraint& src = cnstr[i];

            single_bit bit;
            trailing_bits mask;
            if (src->is_ule() &&
                simplify_clause::get_bit(src->to_ule().lhs(), src->to_ule().rhs(), p, bit, src.is_positive()) && p.is_var()) {
                lbool prev = fixed[bit.position];
                fixed[bit.position] = bit.positive ? l_true : l_false;
                if (prev != l_undef && fixed[bit.position] != prev) {
                    LOG("Bit conflicting " << src << " with " << justifications[bit.position][0]);
                    if (add_conflict) {
                        add_entry_list(justifications[bit.position]);
                        add_entry(src);
                        s.set_conflict(*builder.build());
                    }
                    return false;
                }
                justifications[bit.position].clear();
                justifications[bit.position].push_back(src);
            }
            else if (src->is_eq() &&
                simplify_clause::get_lsb(src->to_ule().lhs(), src->to_ule().rhs(), p, mask, src.is_positive()) && p.is_var()) {
                if (src.is_positive()) {
                    for (unsigned i = 0; i < mask.length; i++) {
                        lbool prev = fixed[i];
                        fixed[i] = mask.bits.get_bit(i) ? l_true : l_false;
                        //verbose_stream() << "Setting bit " << i << " to " << mask.bits.get_bit(i) << " because of parity " << e->src << "\n";
                        if (prev != l_undef) {
                            if (fixed[i] != prev) {
                                LOG("Positive parity conflicting " << src << " with " << justifications[i][0]);
                                if (add_conflict) {
                                    add_entry_list(justifications[i]);
                                    add_entry(src);
                                    s.set_conflict(*builder.build());
                                }
                                return false;
                            }
                            else {
                                if (largest_mask < mask.length) {
                                    largest_mask = mask.length;
                                    justifications[i].clear();
                                    justifications[i].push_back(src);
                                }
                            }
                        }
                        else {
                            SASSERT(justifications[i].empty());
                            justifications[i].push_back(src);
                        }
                    }
                }
                else
                    postponed.push_back({ src, mask });
            }
        }

        bool_vector removed(postponed.size(), false);
        bool changed;
        do {
            changed = false;
            for (unsigned j = 0; j < postponed.size(); j++) {
                if (removed[j])
                    continue;
                const auto& neg = postponed[j];
                unsigned indet = 0;
                unsigned last_indet = 0;
                unsigned i = 0;
                for (; i < neg.second.length; i++) {
                    if (fixed[i] != l_undef) {
                        if (fixed[i] != (neg.second.bits.get_bit(i) ? l_true : l_false)) {
                            removed[j] = true;
                            break; // this is already satisfied
                        }
                    }
                    else {
                        indet++;
                        last_indet = i;
                    }
                }
                if (i == neg.second.length) {
                    if (indet == 0) {
                        // Already false
                        LOG("Found conflict with constraint " << neg.first);
                        if (add_conflict) {
                            for (unsigned k = 0; k < neg.second.length; k++)
                                add_entry_list(justifications[k]);
                            add_entry(neg.first);
                            s.set_conflict(*builder.build());
                        }
                        return false;
                    }
                    else if (indet == 1) {
                        // Simple BCP
                        auto& justification = justifications[last_indet];
                        SASSERT(justification.empty());
                        for (unsigned k = 0; k < neg.second.length; k++) {
                            if (k != last_indet) {
                                SASSERT(fixed[k] != l_undef);
                                for (const auto& just : justifications[k])
                                    justification.push_back(just);
                            }
                        }
                        justification.push_back(neg.first);
                        fixed[last_indet] = neg.second.bits.get_bit(last_indet) ? l_false : l_true;
                        removed[j] = true;
                        LOG("Applying fast BCP on bit " << last_indet << " from constraint " << neg.first);
                        changed = true;
                    }
                }
            }
        } while(changed);

        return true;
    }
#endif

    bool viable::has_viable(pvar v) {

        fixed_bits_info fbi;

        if (!collect_bit_information(v, false, fbi))
            return false;

        refined:
        entry* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account

#define CHECK_RETURN(val) { if (refine_viable<true>(v, val, fbi)) return true; else goto refined; }
        // return refine_viable(v, val) ? l_true : l_undef;

        if (!e)
            CHECK_RETURN(rational::zero());
        entry* first = e;
        entry* last = e->prev();

        if (e->interval.is_full())
            return false;
        // quick check: last interval doesn't wrap around, so hi_val
        // has not been covered
        if (last->interval.lo_val() < last->interval.hi_val())
            CHECK_RETURN(last->interval.hi_val());

        do {
            if (e->interval.is_full())
                return false;
            entry* n = e->next();
            if (n == e)
                CHECK_RETURN(e->interval.hi_val());
            if (!n->interval.currently_contains(e->interval.hi_val()))
                CHECK_RETURN(e->interval.hi_val());
            if (n == first) {
                if (e->interval.lo_val() > e->interval.hi_val())
                    return false;
                CHECK_RETURN(e->interval.hi_val());
            }
            e = n;
        }
        while (e != first);
        return false;
#undef CHECK_RETURN
    }

    bool viable::is_viable(pvar v, rational const& val) {

        fixed_bits_info fbi;

        if (!collect_bit_information(v, false, fbi))
            return false;
        entry* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        if (!e)
            return refine_viable<true>(v, val, fbi);
        entry* first = e;
        entry* last = first->prev();
        if (last->interval.currently_contains(val))
            return false;
        for (; e != last; e = e->next()) {
            if (e->interval.currently_contains(val))
                return false;
            if (val < e->interval.lo_val())
                return refine_viable<true>(v, val, fbi);
        }
        return refine_viable<true>(v, val, fbi);
    }

    find_t viable::find_viable(pvar v, rational& lo) {
        rational hi;
        switch (find_viable2(v, lo, hi)) {
        case l_true:
            if (hi < 0) {
                // fallback solver, treat propagations as decisions for now
                // (this is because the propagation justification currently always uses intervals, which is unsound in this case)
                return find_t::multiple;
            }
            return (lo == hi) ? find_t::singleton : find_t::multiple;
        case l_false:
            return find_t::empty;
        default:
            return find_t::resource_out;
        }
    }

    lbool viable::find_viable2(pvar v, rational& lo, rational& hi) {
        std::pair<rational&, rational&> args{lo, hi};
        return query<query_t::find_viable>(v, args);
    }

    lbool viable::min_viable(pvar v, rational& lo) {
        return query<query_t::min_viable>(v, lo);
    }

    lbool viable::max_viable(pvar v, rational& hi) {
        return query<query_t::max_viable>(v, hi);
    }

    bool viable::has_upper_bound(pvar v, rational& out_hi, vector<signed_constraint>& out_c) {
        entry const* first = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        entry const* e = first;
        bool found = false;
        out_c.reset();
        if (!e)
            return false;
        do {
            found = false;
            do {
                if (!e->refined && e->side_cond.empty()) {
                    auto const& lo = e->interval.lo();
                    auto const& hi = e->interval.hi();
                    if (lo.is_val() && hi.is_val()) {
                        if (out_c.empty() && lo.val() > hi.val()) {
                            for (signed_constraint src : e->src)
                                out_c.push_back(src);
                            out_hi = lo.val() - 1;
                            found = true;
                        }
                        else if (!out_c.empty() && lo.val() <= out_hi && out_hi < hi.val()) {
                            for (signed_constraint src : e->src)
                                out_c.push_back(src);
                            out_hi = lo.val() - 1;
                            found = true;
                        }
                    }
                }
                e = e->next();
            }
            while (e != first);
        }
        while (found);
        return !out_c.empty();
    }

    bool viable::has_lower_bound(pvar v, rational& out_lo, vector<signed_constraint>& out_c) {
        entry const* first = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        entry const* e = first;
        bool found = false;
        out_c.reset();
        if (!e)
            return false;
        do {
            found = false;
            do {
                if (!e->refined && e->side_cond.empty()) {
                    auto const& lo = e->interval.lo();
                    auto const& hi = e->interval.hi();
                    if (lo.is_val() && hi.is_val()) {
                        if (out_c.empty() && hi.val() != 0 && (lo.val() == 0 || lo.val() > hi.val())) {
                            for (signed_constraint src : e->src)
                                out_c.push_back(src);
                            out_lo = hi.val();
                            found = true;
                        }
                        else if (!out_c.empty() && lo.val() <= out_lo && out_lo < hi.val()) {
                            for (signed_constraint src : e->src)
                                out_c.push_back(src);
                            out_lo = hi.val();
                            found = true;
                        }
                    }
                }
                e = e->next();
            }
            while (e != first);
        }
        while (found);
        return !out_c.empty();
    }

    bool viable::has_max_forbidden(pvar v, signed_constraint const& c, rational& out_lo, rational& out_hi, vector<signed_constraint>& out_c) {
        // TODO:
        // - skip intervals adjacent to c's interval if they contain side conditions on y?
        //      constraints over y are allowed if level(c) < level(y) (e.g., boolean propagated)

        out_c.reset();
        entry const* first = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        entry const* e = first;
        if (!e)
            return false;

        bool found = false;

        do {
            found = e->src.contains(c);
            if (found)
                break;
            e = e->next();
        }
        while (e != first);

        if (!found)
            return false;
        entry const* e0 = e;

        if (e0->interval.is_full())
            return false;

        entry const* e0_prev = nullptr;
        entry const* e0_next = nullptr;

        do {
            entry const* n = e->next();
            while (n != e0) {
                entry const* n1 = n->next();
                if (n1 == e)
                    break;
                if (!n1->interval.currently_contains(e->interval.hi_val()))
                    break;
                n = n1;
            }
            if (e == n) {
                VERIFY_EQ(e, e0);
                return false;
            }
            if (!n->interval.currently_contains(e->interval.hi_val()))
                return false;  // gap
            if (e == e0) {
                e0_next = n;
                out_lo = n->interval.lo_val();
            }
            else if (n == e0) {
                e0_prev = e;
                out_hi = e->interval.hi_val();
            }
            else if (e->src.contains(c)) {
                // multiple intervals from the same constraint c
                // TODO: adjacent intervals would fine but they should be merged at insertion instead of considering them here.
                return false;
            }
            else {
                VERIFY(!e->interval.is_full());  // if e were full then there would be no e0
                signed_constraint c = s.m_constraints.elem(e->interval.hi(), n->interval.symbolic());
                out_c.push_back(c);
            }
            if (e != e0) {
                for (signed_constraint sc : e->side_cond)
                    out_c.push_back(sc);
                for (signed_constraint src : e->src)
                    out_c.push_back(src);
            }
            e = n;
        }
        while (e != e0);

        // Other intervals fully cover c's interval, e.g.:
        //              [---------[          e0 from c
        //         [---------[               e0_prev
        //                 [-------------[   e0_next
        if (e0_next->interval.currently_contains(e0_prev->interval.hi_val()))
            return false;

        // Conclusion:
        // v \not\in [out_lo; out_hi[, or equivalently
        // v \in [out_hi; out_lo[

        auto& m = s.var2pdd(v);

        // To justify the endpoints, pretend that instead of e0 (coming from constraint c) we have the interval [out_hi; out_lo[.
        out_c.push_back(s.m_constraints.elem(e0_prev->interval.hi(), m.mk_val(out_hi), m.mk_val(out_lo)));
        out_c.push_back(s.m_constraints.elem(m.mk_val(out_lo), e0_next->interval.symbolic()));

        IF_VERBOSE(2,
                   verbose_stream() << "has-max-forbidden " << e->src << "\n";
                   verbose_stream() << "v" << v << " " << out_lo << " " << out_hi << " " << out_c << "\n";
                   display(verbose_stream(), v) << "\n");
        return true;
    }


    template <query_t mode>
    lbool viable::query(pvar v, typename query_result<mode>::result_t& result) {

        fixed_bits_info fbi;

        if (!collect_bit_information(v, true, fbi))
            return l_false; // conflict already added

        pvar_vector overlaps;
        s.m_slicing.collect_simple_overlaps(v, overlaps);
        // TODO: (combining intervals across equivalence classes from slicing)
        //
        // When iterating over intervals:
        // - instead of only intervals of v, go over intervals of each entry of overlaps
        // - need a function to map interval from overlap into an interval over v
        //
        // Maybe combine only the "simple" overlaps in this method, and do the more comprehensive queries on demand, during conflict resolution (saturation.cpp).
        // Here, we should handle at least:
        // - direct equivalences (x = y); could just point one interval set to the other and store them together (may be annoying for bookkeeping)
        // - lower bits extractions (x[h:0]) and equivalent slices;
        //   (this is what Algorithm 3 in "Solving Bitvectors with MCSAT" does, and will also let us better handle even coefficients of inequalities).
        // - intervals with coefficient 2^k*a to be treated as intervals over x[|x|-k:0] with coefficient a (with odd a)
        //
        // Problem:
        // - the conflict clause will involve relations between different bit-widths
        // - can we avoid introducing new extract-terms? (if not, can we at least avoid additional slices?)
        //       e.g., multiply other terms by 2^k instead of introducing extract?
        // - NOTE: currently our clauses survive across backtracking points, but the slicing will be reset.
        //         It is currently unsafe to create extract/concat terms internally.
        //         (to be fixed when we re-internalize conflict clauses after backtracking)
        //
        // Problem:
        // - we want to iterate intervals in order. do we then need to perform the mapping in advance? (monotonic mapping -> only first one needs to be mapped in advance)
        // - should have some "cursor" class which abstracts the prev/next operation.
        //
        // (in addition to slices, some intervals may transfer by other operations. e.g. x = -y. but maybe it's better to handle these cases on demand by saturation.cpp)
        //
        // Refinement:
        // - is done when we find a "feasible" point, so not directly affected by changes to the algorithm.
        // - we don't know which constraint yields the "best" interval, so keep interleaving constraints

        // Mapping intervals (by example):
        //
        // A) Removing/appending LSB:
        //
        //      easy enough on numerals (have to be careful with rounding);
        //      using in conflict clause will probably involve new extract-terms...
        //
        //          x[6:0] \not\in [15;30[
        //      ==> x[6:1] \not\in [8;15[
        //      ==> x[6:2] \not\in [4;7[
        //
        //          x[6:2] \not\in [3;7[
        //      ==> x[6:1] \not\in [6;14[
        //      ==> x[6:0] \not\in [12;28[
        //
        // B) Removing/appending MSB:
        //
        //      When appending to the MSB, we get exponentially many copies
        //      of the interval because the upper bits are arbitrary.
        //      This is why the algorithm should support this case directly (i.e., lower-bits extractions of the query variable).
        //
        //          x[4:0] \not\in [3;7[
        //      ==> x[5:0] \not\in [3;7[ + 2^4 {0,1}
        //      ==> x[6:0] \not\in [3;7[ + 2^4 {0,1,2,3}
        //
        //      When shorting from the MSB side, we may not get an interval at all,
        //      because the bit-patterns of the remaining (lower) bits are allowed in another part of the domain.
        //
        //          x[6:0] \not\in [15;30[
        //      ==> x[5:0] \not\in \emptyset

        // max number of interval refinements before falling back to the univariate solver
        unsigned const refinement_budget = 1000;
        unsigned refinements = refinement_budget;

        while (refinements--) {
            lbool res = l_undef;

            if constexpr (mode == query_t::find_viable)
                res = query_find(v, result.first, result.second, fbi);
            else if constexpr (mode == query_t::min_viable)
                res = query_min(v, result, fbi);
            else if constexpr (mode == query_t::max_viable)
                res = query_max(v, result, fbi);
            else if constexpr (mode == query_t::has_viable) {
                NOT_IMPLEMENTED_YET();
            }
            else {
                UNREACHABLE();
            }
            IF_VERBOSE(10, {
                if (refinements % 100 == 0)
                    verbose_stream() << "Refinements " << refinements << "\n";
            });
            if (res != l_undef)
                return res;
        }
        IF_VERBOSE(10, verbose_stream() << "Fallback\n";);
        LOG("Refinement budget exhausted! Fall back to univariate solver.");
        return query_fallback<mode>(v, result);
    }

    lbool viable::query_find(pvar v, rational& lo, rational& hi, fixed_bits_info const& fbi) {
        auto const& max_value = s.var2pdd(v).max_value();
        lbool const refined = l_undef;

        // After a refinement, any of the existing entries may have been replaced
        // (if it is subsumed by the new entry created during refinement).
        // For this reason, we start chasing the intervals from the start again.
        lo = 0;
        hi = max_value;

        entry* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        if (!e && !refine_viable<true>(v, lo, fbi))
            return refined;
        if (!e && !refine_viable<false>(v, hi, fbi))
            return refined;
        if (!e)
            return l_true;
        if (e->interval.is_full()) {
            s.set_conflict_by_viable_interval(v);
            return l_false;
        }

        entry* first = e;
        entry* last = first->prev();

        // quick check: last interval does not wrap around
        // and has space for 2 unassigned values.
        if (last->interval.lo_val() < last->interval.hi_val() &&
            last->interval.hi_val() < max_value) {
            lo = last->interval.hi_val();
            if (!refine_viable<true>(v, lo, fbi))
                return refined;
            if (!refine_viable<false>(v, max_value, fbi))
                return refined;
            return l_true;
        }

        // find lower bound
        if (last->interval.currently_contains(lo))
            lo = last->interval.hi_val();
        do {
            if (!e->interval.currently_contains(lo))
                break;
            lo = e->interval.hi_val();
            e = e->next();
        }
        while (e != first);

        if (e->interval.currently_contains(lo)) {
            s.set_conflict_by_viable_interval(v);
            return l_false;
        }

        // find upper bound
        hi = max_value;
        e = last;
        do {
            if (!e->interval.currently_contains(hi))
                break;
            hi = e->interval.lo_val() - 1;
            e = e->prev();
        }
        while (e != last);

        if (!refine_viable<true>(v, lo, fbi))
            return refined;
        if (!refine_viable<false>(v, hi, fbi))
            return refined;
        return l_true;
    }

    lbool viable::query_min(pvar v, rational& lo, fixed_bits_info const& fbi) {
        // TODO: should be able to deal with UNSAT case; since also min_viable has to deal with it due to fallback solver
        lo = 0;
        entry* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        if (!e && !refine_viable<true>(v, lo, fbi))
            return l_undef;
        if (!e)
            return l_true;
        entry* first = e;
        entry* last = first->prev();
        if (last->interval.currently_contains(lo))
            lo = last->interval.hi_val();
        do {
            if (!e->interval.currently_contains(lo))
                break;
            lo = e->interval.hi_val();
            e = e->next();
        }
        while (e != first);
        if (!refine_viable<true>(v, lo, fbi))
            return l_undef;
        SASSERT(is_viable(v, lo));
        return l_true;
    }

    lbool viable::query_max(pvar v, rational& hi, fixed_bits_info const& fbi) {
        // TODO: should be able to deal with UNSAT case; since also max_viable has to deal with it due to fallback solver
        hi = s.var2pdd(v).max_value();
        entry* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        if (!e && !refine_viable<false>(v, hi, fbi))
            return l_undef;
        if (!e)
            return l_true;
        entry* last = e->prev();
        e = last;
        do {
            if (!e->interval.currently_contains(hi))
                break;
            hi = e->interval.lo_val() - 1;
            e = e->prev();
        }
        while (e != last);
        if (!refine_viable<false>(v, hi, fbi))
            return l_undef;
        SASSERT(is_viable(v, hi));
        return l_true;
    }

    template <query_t mode>
    lbool viable::query_fallback(pvar v, typename query_result<mode>::result_t& result) {
        unsigned const bit_width = s.size(v);
        univariate_solver* us = s.m_viable_fallback.usolver(bit_width);
        sat::literal_set added;

        // First step: only query the looping constraints and see if they alone are already UNSAT.
        // The constraints which caused the refinement loop will be reached from m_units.
        LOG_H3("Checking looping univariate constraints for v" << v << "...");
        LOG("Assignment: " << assignments_pp(s));
        entry const* first = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        entry const* e = first;
        do {
            // in the first step we're only interested in entries from refinement
            if (e->refined) {
                for (signed_constraint const src : e->src) {
                    sat::literal const lit = src.blit();
                    if (!added.contains(lit)) {
                        added.insert(lit);
                        LOG("Adding " << lit_pp(s, lit));
                        IF_VERBOSE(10, verbose_stream() << ";; " << lit_pp(s, lit) << "\n");
                        src.add_to_univariate_solver(v, s, *us, lit.to_uint());
                    }
                }
            }
            e = e->next();
        }
        while (e != first);

        switch (us->check()) {
        case l_false:
            s.set_conflict_by_viable_fallback(v, *us);
            return l_false;
        case l_true:
            // At this point we don't know much because we did not add all relevant constraints
            break;
        default:
            // resource limit
            return l_undef;
        }

        // Second step: looping constraints aren't UNSAT, so add the remaining relevant constraints
        LOG_H3("Checking all univariate constraints for v" << v << "...");
        auto const& cs = s.m_viable_fallback.m_constraints[v];
        for (unsigned i = cs.size(); i-- > 0; ) {
            sat::literal const lit = cs[i].blit();
            if (added.contains(lit))
                continue;
            LOG("Adding " << lit_pp(s, lit));
            IF_VERBOSE(10, verbose_stream() << ";; " << lit_pp(s, lit) << "\n");
            added.insert(lit);
            cs[i].add_to_univariate_solver(v, s, *us, lit.to_uint());
        }

        switch (us->check()) {
        case l_false:
            s.set_conflict_by_viable_fallback(v, *us);
            return l_false;
        case l_true:
            // pass solver to mode-specific query
            break;
        default:
            // resource limit
            return l_undef;
        }

        if constexpr (mode == query_t::find_viable)
            return query_find_fallback(v, *us, result.first, result.second);

        if constexpr (mode == query_t::min_viable)
            return query_min_fallback(v, *us, result);

        if constexpr (mode == query_t::max_viable)
            return query_max_fallback(v, *us, result);

        if constexpr (mode == query_t::has_viable) {
            NOT_IMPLEMENTED_YET();
            return l_undef;
        }

        UNREACHABLE();
        return l_undef;
    }

    lbool viable::query_find_fallback(pvar v, univariate_solver& us, rational& lo, rational& hi) {
        lo = us.model();
        hi = -1;
        return l_true;
        // return us.find_two(lo, hi) ? l_true : l_undef;
    }

    lbool viable::query_min_fallback(pvar v, univariate_solver& us, rational& lo) {
        return us.find_min(lo) ? l_true : l_undef;
    }

    lbool viable::query_max_fallback(pvar v, univariate_solver& us, rational& hi) {
        return us.find_max(hi) ? l_true : l_undef;
    }

    bool viable::resolve_fallback(pvar v, univariate_solver& us, conflict& core) {
        // The conflict is the unsat core of the univariate solver,
        // and the current assignment (under which the constraints are univariate in v)
        // TODO:
        // - currently we add variables directly, which is sound:
        //      e.g.:   v^2 + w^2 == 0;   w := 1
        // - but we could use side constraints on the coefficients instead (coefficients when viewed as polynomial over v):
        //      e.g.:   v^2 + w^2 == 0;   w^2 == 1
        for (unsigned dep : us.unsat_core()) {
            sat::literal lit = sat::to_literal(dep);
            signed_constraint c = s.lit2cnstr(lit);
            core.insert(c);
            core.insert_vars(c);
        }
        SASSERT(!core.vars().contains(v));
        core.add_lemma("viable unsat core", core.build_lemma());
        IF_VERBOSE(10, verbose_stream() << "unsat core " << core << "\n";);
        return true;
    }

#if 0
    void viable::make_bit_justification(pvar v) {
        if (!m_units[v] || m_units[v]->interval.is_full())
            return;
        // TODO: Maybe this helps? This prefers justifications from bits
        svector<lbool> fixed;
        vector<ptr_vector<entry>> justifications;
        if (!collect_bit_information(v, false, fixed, justifications))
            return;

        entry* first = m_units[v];
        entry* e = first;
        vector<eval_interval> intervals;
        do {
            intervals.push_back(e->interval);
            e = e->next();
        }
        while (e != first);
        int additional = 0;
        for (unsigned i = 0; i < intervals.size(); i++) { // Try to justify by bits as good as possible
            if (intervals[i].hi_val().is_zero())
                additional += refine_bits<true>(v, s.var(v).manager().max_value(), fixed, justifications);
            else
                additional += refine_bits<true>(v, intervals[i].hi_val() - 1, fixed, justifications);
        }
        verbose_stream() << "Found " << additional << " intervals\n";
    }

    void viable::get_bit_min_max(pvar v, conflict& core, rational& min, rational& max, vector<signed_constraint>& justifications_min, vector<signed_constraint>& justifications_max) {
        pdd v_pdd = s.var(v);
        min = 0;
        max = v_pdd.manager().max_value();
        svector<lbool> fixed;
        vector<vector<signed_constraint>> justifications;
        vector<signed_constraint> candidates;
        for (const auto& c : core) {
            if (!c->is_ule())
                continue;
            ule_constraint ule = c->to_ule();
            pdd sum = ule.lhs() + ule.rhs();
            if (sum.is_univariate_in(v) && sum.degree(v) == 1)
                candidates.push_back(c);
        }

        if (candidates.empty() || !collect_bit_information(v, false, candidates, fixed, justifications))
            return;

        for (unsigned i = 0; i < fixed.size(); i++) {
            verbose_stream() << (fixed[fixed.size() - 1] == l_true ? '1' : fixed[fixed.size() - 1] == l_false ? '0' : '?');
        }
        verbose_stream() << "\n";

        max = 0;

        for (unsigned i = fixed.size(); i > 0; i--) {
            min *= 2;
            max *= 2;
            lbool val = fixed[i - 1];
            if (val == l_true) {
                min++;
                max++;
                for (auto& add : justifications[i - 1])
                    justifications_min.push_back(add);
            }
            else if (val == l_undef)
                max++;
            else {
                SASSERT(val == l_false);
                for (auto& add : justifications[i - 1])
                    justifications_max.push_back(add);
            }
        }
    }
#endif

    bool viable::resolve_interval(pvar v, conflict& core) {
        DEBUG_CODE( log(v); );
        VERIFY(!has_viable(v));  // does a pass over interval refinement, making sure the intervals actually exist

#if 0
        // Prefer bit information as justifications
        make_bit_justification(v);
#endif

        entry const* e = m_units[v].get_entries(s.size(v));  // TODO: take other sizes into account
        // TODO: in the forbidden interval paper, they start with the longest interval. We should also try that at some point.
        entry const* first = e;
        SASSERT(e);
        // If there is a full interval, all others would have been removed
        clause_builder lemma(s);
        if (first->interval.is_full()) {
            SASSERT(first->next() == first);
            for (auto sc : first->side_cond)
                lemma.insert_eval(~sc);
            for (const auto& src : first->src) {
                lemma.insert(~src);
                core.insert(src);
                core.insert_vars(src);
            }
            core.add_lemma("viable", lemma.build());
            core.logger().log(inf_fi(*this, v));
            return true;
        }

        SASSERT(all_of(*first, [](entry const& f) { return !f.interval.is_full(); }));

        do {
            // Build constraint: upper bound of each interval is not contained in the next interval,
            // using the equivalence:  t \in [l;h[  <=>  t-l < h-l
            entry const* n = e->next();

            // Choose the next interval which furthest extends the covered region.
            // Example:
            //      covered:   [-------]
            //      e:           [-------]      <--- not required for the lemma because all points are also covered by other intervals
            //      n:              [-------]
            //
            // Note that intervals are sorted by their starting points,
            // so the intervals to be considered (i.e., those that
            // contain the current endpoint), form a prefix of the list.
            //
            // Furthermore, because we remove intervals that are subsets
            // of other intervals, also the endpoints must be increasing,
            // so the last interval of this prefix is the best choice.
            //
            // current:  [------[
            // next:       [---[        <--- impossible, would have been removed.
            //
            // current:  [------[
            // next:       [-------[    <--- thus, the next interval is always the better choice.
            //
            // The interval 'first' is always part of the lemma. If we reach first again here, we have covered the complete domain.
            while (n != first) {
                entry const* n1 = n->next();
                // Check if n1 is eligible; if yes, then n1 is better than n.
                //
                // Case 1, n1 overlaps e (unless n1 == e):
                //      e:  [------[
                //      n1:      [----[
                // Case 2, n1 connects to e:
                //      e:  [------[
                //      n1:        [----[
                if (n1 == e)
                    break;
                if (!n1->interval.currently_contains(e->interval.hi_val()))
                    break;
                n = n1;
            }

            signed_constraint c = s.m_constraints.elem(e->interval.hi(), n->interval.symbolic());
            // lemma.insert_try_eval(~c);
            VERIFY(c.is_currently_true(s));
            if (c.bvalue(s) == l_false) {
                core.reset();
                core.init(~c);
                return false;
            }
            lemma.insert_eval(~c);

            for (auto sc : e->side_cond)
                lemma.insert_eval(~sc);
            for (const auto& src : e->src) {
                lemma.insert(~src);
                core.insert(src);
                core.insert_vars(src);
            }
            e = n;
        }
        while (e != first);

        // TODO: violated in 5133-min.smt2:
        //
        //              viable lemma:
        //             35: -31 <= -1*v17 + -1*v11*v0 + -1*v5*v2 + 32  [ b:l_true  p:l_false bprop@0 idx:28 pwatched ]
        //            -22: v17 + v11*v0 + v6 + v5*v2 != 0  [ b:l_false p:l_undef assert@0 idx:8 pwatched dep:16 ]
        //             36: v17 + v11*v0 + v5*v2 + 1 == 0   [ b:l_false p:l_false eval@39 idx:75 ]
        //             -7: -31 > v6 + 32                   [ b:l_false p:l_undef assert@0 idx:17 pwatched dep:33 ]
        //      ASSERTION VIOLATION
        //      File: /Users/jakob/projects/z3/src/math/polysat/viable.cpp
        //      Line: 2036
        //      all_of(lemma, [this](sat::literal lit) { return s.m_bvars.value(lit) != l_true; })
        //
        //  Reason: there is an eval/bool conflict that we didn't discover before,
        //          because not-yet-assigned variables are watched but the constraint already evaluates due to cancellation of some terms.
        //
        // verbose_stream() << "viable lemma:\n";
        // for (auto lit : lemma)
        //     verbose_stream() << "    " << lit_pp(s, lit) << "\n";
        VERIFY(all_of(lemma, [this](sat::literal lit) { return s.m_bvars.value(lit) != l_true; }));

        core.add_lemma("viable", lemma.build());
        core.logger().log(inf_fi(*this, v));
        return true;
    }

    void viable::log(pvar v) {
#if 0
        if (!well_formed(m_units[v]))
            LOG("v" << v << " not well formed");
        auto* e = m_units[v];
        if (!e)
            return;
        entry* first = e;
        do {
            IF_LOGGING(
                    verbose_stream() << "v" << v << ": " << e->interval << " " << e->side_cond << " ";
                    for (const auto& src : e->src)
                        verbose_stream() << src << " ";
                    verbose_stream() << "\n";
            );
            e = e->next();
        }
        while (e != first);
#endif
    }

    void viable::log() {
        for (pvar v = 0; v < m_units.size(); ++v)
            log(v);
    }

    std::ostream& viable::display_one(std::ostream& out, pvar v, entry const* e) const {
        auto& m = s.var2pdd(v);
        if (e->coeff == -1) {
            //      p*val + q >  r*val + s  if e->src.is_positive()
            //      p*val + q >= r*val + s  if e->src.is_negative()
            // Note that e->interval is meaningless in this case,
            // we just use it to transport the values p,q,r,s
            rational const& p = e->interval.lo_val();
            rational const& q_ = e->interval.lo().val();
            rational const& r = e->interval.hi_val();
            rational const& s_ = e->interval.hi().val();
            out << "[ ";
            out << val_pp(m, p, true) << "*v" << v << " + " << val_pp(m, q_);
            out << (e->src[0].is_positive() ? " > " : " >= ");
            out << val_pp(m, r, true) << "*v" << v << " + " << val_pp(m, s_);
            out << " ] ";
        }
        else if (e->coeff != 1)
            out << e->coeff << " * v" << v << " " << e->interval << " ";
        else
            out << e->interval << " ";
        if (e->side_cond.size() <= 5)
            out << e->side_cond << " ";
        else
            out << e->side_cond.size() << " side-conditions ";
        unsigned count = 0;
        for (const auto& src : e->src) {
            ++count;
            out << src << "; ";
            if (count > 10) {
                out << " ...";
                break;
            }
        }
        return out;
    }

    std::ostream& viable::display_all(std::ostream& out, pvar v, entry const* e, char const* delimiter) const {
        if (!e)
            return out;
        entry const* first = e;
        unsigned count = 0;
        do {
            display_one(out, v, e) << delimiter;
            e = e->next();
            ++count;
            if (count > 10) {
                out << " ...";
                break;
            }
        }
        while (e != first);
        return out;
    }

    std::ostream& viable::display_all(std::ostream& out, pvar v, layers const& ls, char const* delimiter) const {
        // TODO
        return out;
    }

    std::ostream& viable::display(std::ostream& out, pvar v, char const* delimiter) const {
        display_all(out, v, m_units[v], delimiter);
        display_all(out, v, m_equal_lin[v], delimiter);
        display_all(out, v, m_diseq_lin[v], delimiter);
        return out;
    }

    std::ostream& viable::display(std::ostream& out, char const* delimiter) const {
        for (pvar v = 0; v < m_units.size(); ++v)
            display(out << "v" << v << ": ", v, delimiter) << "\n";
        return out;
    }

    /*
     * Lower bounds are strictly ascending.
     * Intervals don't contain each-other (since lower bounds are ascending, it suffices to check containment in one direction).
     */
    bool viable::well_formed(entry* e) {
        if (!e)
            return true;
        entry* first = e;
        while (true) {
            if (e->interval.is_full())
                return e->next() == e;
            if (e->interval.is_currently_empty())
                return false;

            auto* n = e->next();
            if (n != e && e->interval.currently_contains(n->interval))
                return false;

            if (n == first)
                break;
            if (e->interval.lo_val() >= n->interval.lo_val())
                return false;
            e = n;
        }
        return true;
    }

    /*
     * Layers are ordered in strictly descending bit-width.
     * Entries in each layer are well-formed.
     */
    bool viable::well_formed(layers const& ls) {
        unsigned prev_width = std::numeric_limits<unsigned>::max();
        for (layer const& l : ls.get_layers()) {
            if (!well_formed(l.entries))
                return false;
            if (!all_of(dll_elements(l.entries), [&l](entry const& e) { return e.bit_width == l.bit_width; }))
                return false;
            if (prev_width <= l.bit_width)
                return false;
            prev_width = l.bit_width;
        }
        return true;
    }

}

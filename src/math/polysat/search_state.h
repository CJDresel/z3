/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat search state

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

--*/
#pragma once
#include "math/polysat/types.h"
#include "math/polysat/assignment.h"

namespace polysat {

    enum class search_item_k
    {
        assignment,
        boolean,
    };

    class search_item {
        search_item_k m_kind;
        union {
            pvar m_var;
            sat::literal m_lit;
        };
        bool m_resolved = false; // when marked as resolved it is no longer valid to reduce the conflict state

        search_item(pvar var): m_kind(search_item_k::assignment), m_var(var) {}
        search_item(sat::literal lit): m_kind(search_item_k::boolean), m_lit(lit) {}
    public:
        static search_item assignment(pvar var) { return search_item(var); }
        static search_item boolean(sat::literal lit) { return search_item(lit); }
        bool is_assignment() const { return m_kind == search_item_k::assignment; }
        bool is_boolean() const { return m_kind == search_item_k::boolean; }
        bool is_resolved() const { return m_resolved; }
        search_item_k kind() const { return m_kind; }
        pvar var() const { SASSERT(is_assignment()); return m_var; }
        sat::literal lit() const { SASSERT(is_boolean()); return m_lit; }
        void set_resolved() { m_resolved = true; }
    };

    class search_state {
        solver&             s;

        vector<search_item> m_items;
        assignment          m_assignment;

        // store index into m_items
        unsigned_vector     m_pvar_to_idx;
        unsigned_vector     m_bool_to_idx;

        bool value(pvar v, rational& r) const;

    public:
        search_state(solver& s): s(s), m_assignment(s) {}
        unsigned size() const { return m_items.size(); }
        search_item const& back() const { return m_items.back(); }
        search_item const& operator[](unsigned i) const { return m_items[i]; }

        assignment const& get_assignment() const { return m_assignment; }
        substitution const& subst(unsigned sz) const { return m_assignment.subst(sz); }

        // TODO: implement the following method if we actually need the assignments without resolved items already during conflict resolution
        //       (no separate trail needed, just a second m_subst and an index into the trail, I think)
        //       (update on set_resolved? might be one iteration too early, looking at the old solver::resolve_conflict loop)
        substitution const& unresolved_assignment(unsigned sz) const;

        void push_assignment(pvar v, rational const& r);
        void push_boolean(sat::literal lit);
        void pop();

        unsigned get_pvar_index(pvar v) const;
        unsigned get_bool_index(sat::bool_var var) const;
        unsigned get_bool_index(sat::literal lit) const { return get_bool_index(lit.var()); }

        void set_resolved(unsigned i) { m_items[i].set_resolved(); }

        using const_iterator = decltype(m_items)::const_iterator;
        const_iterator begin() const { return m_items.begin(); }
        const_iterator end() const { return m_items.end(); }

        std::ostream& display(std::ostream& out) const;
        std::ostream& display(search_item const& item, std::ostream& out) const;
        std::ostream& display_verbose(std::ostream& out) const;
        std::ostream& display_verbose(search_item const& item, std::ostream& out) const;
    };

    struct search_state_pp {
        search_state const& s;
        bool verbose;
        search_state_pp(search_state const& s, bool verbose = false) : s(s), verbose(verbose) {}
    };

    struct search_item_pp {
        search_state const& s;
        search_item const& i;
        bool verbose;
        search_item_pp(search_state const& s, search_item const& i, bool verbose = false) : s(s), i(i), verbose(verbose) {}
    };

    inline std::ostream& operator<<(std::ostream& out, search_state const& s) { return s.display(out); }

    inline std::ostream& operator<<(std::ostream& out, search_state_pp const& p) { return p.verbose ? p.s.display_verbose(out) : p.s.display(out); }

    inline std::ostream& operator<<(std::ostream& out, search_item_pp const& p) { return p.verbose ? p.s.display_verbose(p.i, out) : p.s.display(p.i, out); }

}

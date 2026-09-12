// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_PROJECTION_H
#define MANGOS_COA_PROJECTION_H
#include <cstdint>
#include <functional>
#include <set>
#include <stdexcept>
#include <vector>

namespace coa
{
    inline bool OwnsProjectionAura(bool classSpell, bool selfCast, bool itemCast, bool passive, int32_t duration)
    {
        return classSpell && selfCast && !itemCast && (passive || duration < 0);
    }
    inline std::set<uint32_t> AcceptedQuestGrants(std::set<uint32_t> const& children,
        std::function<bool(uint32_t)> const& knowsSpell)
    {
        std::set<uint32_t> accepted;
        for (auto child : children)
        {
            if (child && knowsSpell(child))
            {
                accepted.insert(child);
            }
        }
        return accepted;
    }
    struct SpellLinks
    {
        uint32_t previous = 0;
        std::set<uint32_t> learned, triggered;
    };
    // Cycles and shared dependencies are finite set closure, never recursive
    // ownership counts. Triggered effects are NOT automatically spellbook grants.
    inline std::set<uint32_t> SpellClosure(std::set<uint32_t> const& seeds,
        std::function<SpellLinks(uint32_t)> const& resolve, bool auras = false)
    {
        auto result = seeds;
        std::vector<uint32_t> pending(seeds.begin(), seeds.end());
        auto add = [&](uint32_t id)
        {
            if (id && result.insert(id).second)
            {
                pending.push_back(id);
            }
        };
        for (size_t i = 0; i < pending.size(); ++i)
        {
            if (pending.size() > 100000)
            {
                throw std::runtime_error("CoA spell dependency closure exceeds bound");
            }
            auto links = resolve(pending[i]);
            add(links.previous);
            for (auto id : links.learned)
            {
                add(id);
            }
            if (auras)
            {
                for (auto id : links.triggered)
                {
                    add(id);
                }
            }
        }
        return result;
    }
}
#endif

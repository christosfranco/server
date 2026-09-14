// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_PROJECTION_H
#define MANGOS_COA_PROJECTION_H
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace coa
{
    inline bool OrdinaryClassSpell(uint32_t classFamily, uint32_t spellFamily, uint32_t spellLevel)
    {
        return classFamily && classFamily == spellFamily && spellLevel >= 1 && spellLevel <= 60;
    }

    struct TrainingRank { uint32_t id, firstSpell, spell, rank; };

    // Only candidates are authorized. Legacy rank rows supply prerequisites, never grants.
    // A prerequisite may come from a quest rather than this trainer; it must exist,
    // and the player must actually know it before buying the next rank.
    inline std::map<uint32_t, uint32_t> TrainingPreviousSpells(std::set<uint32_t> const& candidates,
        std::vector<TrainingRank> const& rows, std::function<bool(uint32_t)> const& exists)
    {
        std::map<uint32_t, std::map<uint32_t, uint32_t>> chains;
        std::map<uint32_t, TrainingRank> spells;
        std::set<uint32_t> invalid;
        std::map<uint32_t, uint32_t> ids;
        for (auto const& row : rows)
        {
            auto id = ids.emplace(row.id, row.firstSpell);
            auto spell = spells.emplace(row.spell, row);
            auto rank = chains[row.firstSpell].emplace(row.rank, row.spell);
            if (!row.id || !row.firstSpell || !row.spell || !row.rank || !id.second ||
                !spell.second || !rank.second || (row.rank == 1 && row.spell != row.firstSpell) ||
                (row.spell == row.firstSpell && row.rank != 1))
            {
                invalid.insert(row.firstSpell);
                if (!id.second)
                {
                    invalid.insert(id.first->second);
                }
                if (!spell.second)
                {
                    invalid.insert(spell.first->second.firstSpell);
                }
            }
        }
        std::map<uint32_t, uint32_t> result;
        for (auto candidate : candidates)
        {
            if (!exists(candidate))
            {
                continue;
            }
            auto found = spells.find(candidate);
            if (found == spells.end())
            {
                // A named but missing root is not an unranked spell.
                if (!chains.count(candidate))
                {
                    result.emplace(candidate, 0);
                }
                continue;
            }
            auto const& row = found->second;
            auto const& chain = chains.at(row.firstSpell);
            if (invalid.count(row.firstSpell) || row.rank > chain.size())
            {
                continue;
            }
            bool valid = true;
            uint32_t previous = 0;
            for (uint32_t rank = 1; rank <= row.rank; ++rank)
            {
                auto link = chain.find(rank);
                if (link == chain.end() || !exists(link->second))
                {
                    valid = false;
                    break;
                }
                if (rank < row.rank)
                {
                    previous = link->second;
                }
            }
            if (valid)
            {
                result.emplace(candidate, previous);
            }
        }
        return result;
    }

    inline bool TrainingRankKnown(std::map<uint32_t, uint32_t> const& previous, uint32_t spell,
        std::function<bool(uint32_t)> const& knows)
    {
        auto found = previous.find(spell);
        return found != previous.end() && (!found->second || knows(found->second));
    }

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

    // Revocation includes obsolete IDs even when their definitions no longer exist.
    // Grant validation still uses the strict SpellClosure path above.
    inline std::set<uint32_t> RevocationClosure(std::set<uint32_t> const& seeds,
        std::function<SpellLinks(uint32_t)> const& resolve,
        std::function<bool(uint32_t)> const& exists, bool auras = false)
    {
        return SpellClosure(seeds, [&](uint32_t id)
        {
            return exists(id) ? resolve(id) : SpellLinks{};
        }, auras);
    }
}
#endif

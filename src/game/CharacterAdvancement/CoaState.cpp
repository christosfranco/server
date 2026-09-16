// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaState.h"
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace coa
{
    Ranks EntryRanks(std::vector<analytic::CoaEntry> const& entries)
    {
        if (entries.size() > analytic::MaximumEntryCount)
        {
            throw std::invalid_argument("CoA too many entries");
        }
        Ranks result;
        for (auto const& e : entries)
        {
            if (!e.entryId || !e.rank || !e.spellRankLimit || e.learnedTime < 0 ||
                !result.emplace(e.entryId, e.rank).second)
            {
                throw std::invalid_argument("CoA invalid entry metadata");
            }
        }
        return result;
    }

    bool ValidateLoaded(Catalog const& catalog, State const& state, uint32_t guid,
        uint32_t playerClass, uint32_t level, Build& build)
    {
        if (!guid || guid != state.guid || !state.revision ||
            state.catalogRevision != catalog.Revision())
        {
            return false;
        }
        try
        {
            auto ranks = EntryRanks(state.entries);
            build = catalog.Authorize(playerClass, level, catalog.SelectedSpec(ranks), ranks);
            for (auto const& entry : state.entries)
            {
                if (entry.spellRankLimit != catalog.EntrySpell(entry.entryId, entry.rank, level).rank)
                {
                    return false;
                }
            }
            return build.entries == ranks;
        }
        catch (std::invalid_argument const&)
        {
            return false;
        }
    }

    ApplyStatus Replace(Catalog const& catalog, Store& store, State& current,
        uint32_t playerClass, uint32_t level, std::vector<analytic::CoaEntry> const& desired,
        int64_t now, Build& installed, std::string& error, MutationSource source,
        AuthorizationError* refusal)
    {
        State next;
        Build build;
        AuthorizationError authorized;
        if (refusal)
        {
            *refusal = AuthorizationError{};
        }
        try
        {
            if (!current.guid || now < 0 || current.revision == std::numeric_limits<uint64_t>::max() ||
                (current.revision && current.catalogRevision != catalog.Revision()))
            {
                throw std::invalid_argument("CoA state identity/revision invalid");
            }
            Ranks requested = EntryRanks(desired);
            std::map<uint32_t, analytic::CoaEntry> old;
            for (auto const& e : current.entries)
            {
                old.emplace(e.entryId, e);
                if (source != MutationSource::Reset && e.locked && (!requested.count(e.entryId) || requested.at(e.entryId) != e.rank))
                {
                    if (refusal)
                    {
                        refusal->entry = e.entryId;
                        refusal->rank = e.rank;
                    }
                    throw std::invalid_argument("CoA protected entry changed");
                }
            }
            for (auto const& e : desired)
            {
                auto prior = old.find(e.entryId);
                if ((prior == old.end() && e.locked) || (prior != old.end() &&
                    (e.locked != prior->second.locked || e.learnedTime != prior->second.learnedTime)))
                {
                    if (refusal)
                    {
                        refusal->entry = e.entryId;
                        refusal->rank = e.rank;
                    }
                    throw std::invalid_argument("CoA protected metadata changed");
                }
            }
            build = catalog.Authorize(playerClass, level, catalog.SelectedSpec(requested), requested, refusal ? &authorized : nullptr);
            next.guid = current.guid;
            next.catalogRevision = catalog.Revision();
            next.revision = current.revision + 1;
            for (auto const& rank : build.entries)
            {
                analytic::CoaEntry e;
                if (old.count(rank.first))
                {
                    e = old.at(rank.first);
                }
                else
                {
                    e.entryId = rank.first;
                    e.learnedTime = now;
                }
                e.rank = rank.second;
                e.spellRankLimit = catalog.EntrySpell(e.entryId, e.rank, level).rank;
                next.entries.push_back(e);
            }
            if (analytic::BuildEntriesStatePacket(next.entries).GetOpcode() != analytic::EntriesStateOpcode)
            {
                throw std::invalid_argument("CoA canonical state cannot be serialized");
            }
        }
        catch (std::invalid_argument const& e)
        {
            error = e.what();
            // Authorize names its offender structurally; a protected-metadata
            // refusal already set *refusal above, so only fill it in when the
            // authorizer blamed someone (budget and wire-shape refusals
            // legitimately stay 0).
            if (refusal && authorized.entry)
            {
                *refusal = authorized;
            }
            return ApplyStatus::Rejected;
        }
        if (source == MutationSource::Request && current.revision && current.entries.size() == next.entries.size() &&
            std::equal(current.entries.begin(), current.entries.end(), next.entries.begin(), [](auto const& a, auto const& b)
            {
                return a.entryId == b.entryId && a.rank == b.rank && a.spellRankLimit == b.spellRankLimit &&
                    a.locked == b.locked && a.learnedTime == b.learnedTime;
            }))
        {
            return ApplyStatus::NoChange;
        }
        if (!store.Commit(current, next))
        {
            error = "CoA commit failed; durable outcome uncertain, session must close";
            return ApplyStatus::Failed;
        }
        current = std::move(next);
        installed = std::move(build);
        return ApplyStatus::Applied;
    }
}

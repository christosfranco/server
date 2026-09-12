// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_CATALOG_H
#define MANGOS_COA_CATALOG_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace coa
{
    using Ranks = std::map<uint32_t, uint32_t>;
    struct SpellRank
    {
        uint32_t talentRank, spell, rank, level;
    };
    struct Entry
    {
        uint32_t id, playerClass, tab, flags, level, ae, te, maxRank, points, requiredPoints;
        std::array<uint32_t, 6> investment;
        std::set<uint32_t> owners, connections, excludes;
        Ranks requires;
        std::vector<SpellRank> spells;
        uint32_t group = 0;
    };
    struct Spec
    {
        uint32_t playerClass, marker, root;
    };
    struct Build
    {
        Ranks entries;
        std::set<uint32_t> spells;
    };

    // Only constructed after the complete bounded input and independent pin pass.
    class Catalog
    {
    public:
        static std::shared_ptr<Catalog const> Parse(std::string const& bytes,
            std::string const& expectedSha256);
        static std::shared_ptr<Catalog const> Load(std::string const& path,
            std::string const& expectedSha256);
        Build Authorize(uint32_t playerClass, uint32_t level, uint32_t spec,
            Ranks const& requested) const;
        uint32_t SelectedSpec(Ranks const& entries) const;
        bool HasClass(uint32_t id) const { return m_classes.count(id) != 0; }
        std::string const& Revision() const { return m_revision; }
        std::map<uint32_t, Entry> const& Entries() const { return m_entries; }
        std::map<uint32_t, Spec> const& Specs() const { return m_specs; }
        std::set<uint32_t> AllSpells(uint32_t playerClass = 0) const;
        SpellRank BaseSpell(uint32_t playerClass, uint32_t level) const;
        SpellRank EntrySpell(uint32_t entry, uint32_t rank, uint32_t level) const;

    private:
        std::string m_revision;
        std::map<uint32_t, uint32_t> m_classes;
        std::map<std::pair<uint32_t, uint32_t>, std::array<uint32_t, 2>> m_budgets;
        std::map<uint32_t, Entry> m_entries;
        std::map<uint32_t, Spec> m_specs;
        std::map<uint32_t, std::vector<SpellRank>> m_baseSpells;
    };
}
#endif

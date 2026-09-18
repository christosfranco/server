// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaCatalog.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <tuple>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coa
{
    namespace
    {
        constexpr size_t MaximumBytes = 16 * 1024 * 1024;
        void Require(bool condition, char const* error)
        {
            if (!condition)
            {
                throw std::invalid_argument(error);
            }
        }
        SpellRank SelectSpell(std::vector<SpellRank> const& spells, uint32_t rank, uint32_t level)
        {
            SpellRank const* selected = nullptr;
            for (auto const& spell : spells)
            {
                if (spell.talentRank == rank && spell.level <= level && (!selected || spell.rank > selected->rank))
                {
                    selected = &spell;
                }
            }
            Require(selected, "CoA no eligible spell rank");
            return *selected;
        }
    }

    std::shared_ptr<Catalog const> Catalog::Load(std::string const& path,
        std::string const& expectedSha256)
    {
        Require(std::filesystem::path(path).is_absolute(), "CoA catalog path must be absolute");
#ifndef _WIN32
        struct File
        {
            int fd;
            ~File() { if (fd >= 0) { close(fd); } }
        } file{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
        struct stat before{}, after{};
        Require(file.fd >= 0 && fstat(file.fd, &before) == 0 && S_ISREG(before.st_mode) &&
            before.st_size > 0 && uint64_t(before.st_size) <= MaximumBytes, "CoA catalog open/type/size invalid");
        std::string bytes(size_t(before.st_size) + 1, '\0');
        size_t size = 0;
        while (size < bytes.size())
        {
            auto count = read(file.fd, bytes.data() + size, bytes.size() - size);
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            Require(count >= 0, "CoA catalog read failed");
            if (!count)
            {
                break;
            }
            size += size_t(count);
        }
        Require(size == size_t(before.st_size) && fstat(file.fd, &after) == 0 &&
            before.st_size == after.st_size && before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime,
            "CoA catalog changed during read");
        bytes.resize(size);
#else
        Require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
            "CoA catalog must be a regular non-symlink file");
        auto size = std::filesystem::file_size(path);
        Require(size > 0 && size <= MaximumBytes, "CoA catalog size out of bounds");
        std::ifstream stream(path, std::ios::binary);
        std::string bytes(size_t(size), '\0');
        Require(bool(stream.read(bytes.data(), bytes.size())) && stream.peek() == EOF,
            "CoA catalog read failed or size changed");
#endif
        return Parse(bytes, expectedSha256);
    }

    std::shared_ptr<Catalog const> Catalog::Parse(std::string const& bytes,
        std::string const& expectedSha256)
    {
        Require(!bytes.empty() && bytes.size() <= MaximumBytes && bytes.back() == '\n',
            "CoA catalog size/termination invalid");
        Require(expectedSha256.size() == 64 && std::all_of(expectedSha256.begin(),
            expectedSha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
            "CoA catalog SHA256 pin required");
        unsigned char digest[32];
        size_t digestSize = sizeof(digest);
        Require(EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(), digest,
            &digestSize) == 1 && digestSize == 32, "CoA SHA256 failed");
        std::string actual;
        for (auto byte : digest)
        {
            actual += "0123456789abcdef"[byte >> 4];
            actual += "0123456789abcdef"[byte & 15];
        }
        Require(actual == expectedSha256, "CoA catalog SHA256 mismatch");
        auto catalog = std::shared_ptr<Catalog>(new Catalog);
        catalog->m_revision = actual;
        std::istringstream stream(bytes);
        std::string line;
        std::getline(stream, line);
        Require(line == "ASCENDER_COA_AUTHORIZATION\t1\t60\t10", "CoA catalog schema mismatch");
        std::map<std::string, size_t> const arity = {{"CLASS", 2}, {"BUDGET", 4},
            {"SPEC", 4}, {"ENTRY", 16}, {"OWNER", 2}, {"SPELL", 5}, {"REQUIRE", 3},
            {"CONNECT", 2}, {"EXCLUDE", 2}, {"GROUP", 2}, {"START", 2}, {"BASESPELL", 4}};
        std::vector<std::pair<std::string, std::vector<uint32_t>>> links;
        size_t records = 1;
        while (std::getline(stream, line))
        {
            Require(++records <= 100000 && !line.empty() && line.size() <= 1024,
                "CoA catalog record bound exceeded");
            auto end = line.find('\t');
            std::string tag = line.substr(0, end);
            Require(arity.count(tag) != 0 && end != std::string::npos, "CoA unknown record");
            std::vector<uint32_t> v;
            while (end != std::string::npos)
            {
                auto begin = end + 1;
                end = line.find('\t', begin);
                auto stop = end == std::string::npos ? line.size() : end;
                Require(stop > begin && stop - begin <= 10 &&
                    (stop - begin == 1 || line[begin] != '0'), "CoA invalid integer");
                uint32_t value = 0;
                auto parsed = std::from_chars(line.data() + begin, line.data() + stop, value);
                Require(parsed.ec == std::errc() && parsed.ptr == line.data() + stop &&
                    line[begin] >= '0' && line[begin] <= '9', "CoA invalid uint32");
                v.push_back(value);
            }
            Require(v.size() == arity.at(tag), "CoA wrong record arity");
            if (tag == "CLASS")
            {
                Require(v[0] >= 12 && v[0] <= 32 && v[1] &&
                    catalog->m_classes.emplace(v[0], v[1]).second, "CoA invalid/duplicate class");
            }
            else if (tag == "BUDGET")
            {
                Require(v[1] >= 1 && v[1] <= 60 && v[2] <= 1000 && v[3] <= 1000 &&
                    catalog->m_budgets.emplace(std::make_pair(v[0], v[1]),
                        std::array<uint32_t, 2>{v[2], v[3]}).second, "CoA invalid/duplicate budget");
            }
            else if (tag == "SPEC")
            {
                Require(v[0] && v[2] && v[3] && v[2] != v[3] &&
                    catalog->m_specs.emplace(v[0], Spec{v[1], v[2], v[3]}).second,
                    "CoA invalid/duplicate spec");
            }
            else if (tag == "ENTRY")
            {
                Require(v[0] && v[4] >= 1 && v[4] <= 60 && v[7] >= 1 && v[7] <= 9 &&
                    !(v[3] & 9), "CoA invalid entry");
                Entry entry{v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
                    {v[10], v[11], v[12], v[13], v[14], v[15]}, {}, {}, {}, {}, {}};
                Require(catalog->m_entries.emplace(v[0], std::move(entry)).second &&
                    catalog->m_entries.size() <= 8192, "CoA duplicate/excess entries");
            }
            else
            {
                links.emplace_back(tag, std::move(v));
            }
        }
        std::set<std::pair<std::string, std::vector<uint32_t>>> unique;
        for (auto const& link : links)
        {
            auto const& tag = link.first;
            auto const& v = link.second;
            Require(unique.insert(link).second, "CoA duplicate link");
            if (tag == "START" || tag == "BASESPELL")
            {
                Require(catalog->HasClass(v[0]), "CoA missing class reference");
                if (tag == "START")
                {
                    throw std::invalid_argument("CoA START entries are unsupported; use BASESPELL for starting kits");
                }
                else
                {
                    Require(v[1] && v[2] >= 1 && v[2] <= 255 && v[3] >= 1 && v[3] <= 255,
                        "CoA invalid base spell");
                    catalog->m_baseSpells[v[0]].push_back({1, v[1], v[2], v[3]});
                }
                continue;
            }
            Require(catalog->m_entries.count(v[0]), "CoA missing entry reference");
            auto& entry = catalog->m_entries.at(v[0]);
            if (tag == "OWNER")
            {
                Require(catalog->m_specs.count(v[1]) &&
                    catalog->m_specs.at(v[1]).playerClass == entry.playerClass,
                    "CoA invalid owner reference");
                entry.owners.insert(v[1]);
            }
            else if (tag == "SPELL")
            {
                Require(v[1] >= 1 && v[1] <= entry.maxRank && v[2] && v[3] >= 1 &&
                    v[3] <= 255 && v[4] >= 1 && v[4] <= 255, "CoA invalid spell rank");
                entry.spells.push_back({v[1], v[2], v[3], v[4]});
            }
            else if (tag == "GROUP")
            {
                Require(v[1] && !entry.group, "CoA duplicate/zero group");
                entry.group = v[1];
            }
            else
            {
                Require(v[1] != v[0] && catalog->m_entries.count(v[1]) &&
                    catalog->m_entries.at(v[1]).playerClass == entry.playerClass,
                    "CoA invalid edge");
                if (tag == "REQUIRE")
                {
                    Require(v[2] && v[2] <= catalog->m_entries.at(v[1]).maxRank &&
                        entry.requires.emplace(v[1], v[2]).second, "CoA invalid requirement");
                }
                else if (tag == "CONNECT")
                {
                    entry.connections.insert(v[1]);
                }
                else
                {
                    entry.excludes.insert(v[1]);
                }
            }
        }
        Require(!catalog->m_classes.empty() && !catalog->m_entries.empty(), "CoA empty catalog");
        std::set<uint32_t> types, markers;
        auto checkSpells = [](std::vector<SpellRank> const& spells, uint32_t ranks)
        {
            std::map<uint32_t, std::set<uint32_t>> chains;
            for (auto const& spell : spells)
            {
                Require(chains[spell.talentRank].insert(spell.rank).second, "CoA duplicate spell rank key");
            }
            Require(chains.size() == ranks, "CoA missing talent rank spells");
            for (auto const& chain : chains)
            {
                Require(!chain.second.empty() && *chain.second.begin() == 1 &&
                    *chain.second.rbegin() == chain.second.size(), "CoA noncontiguous spell ranks");
            }
        };
        for (auto const& cls : catalog->m_classes)
        {
            Require(types.insert(cls.second).second, "CoA duplicate class type");
            for (uint32_t level = 1; level <= 60; ++level)
            {
                Require(catalog->m_budgets.count({cls.first, level}), "CoA missing level budget");
            }
            auto const& base = catalog->m_baseSpells[cls.first];
            checkSpells(base, 1);
            Require(std::any_of(base.begin(), base.end(), [](SpellRank const& s)
                { return s.rank == 1 && s.level == 1; }), "CoA missing level one base kit");
        }
        for (auto const& budget : catalog->m_budgets)
        {
            Require(catalog->HasClass(budget.first.first), "CoA budget class missing");
        }
        for (auto const& pair : catalog->m_entries)
        {
            Require(catalog->HasClass(pair.second.playerClass), "CoA entry class missing");
            checkSpells(pair.second.spells, pair.second.maxRank);
        }
        for (auto const& pair : catalog->m_specs)
        {
            auto const& spec = pair.second;
            Require(catalog->HasClass(spec.playerClass) && markers.insert(spec.marker).second &&
                catalog->m_entries.count(spec.marker) && catalog->m_entries.count(spec.root),
                "CoA spec package missing/duplicate");
            auto const& marker = catalog->m_entries.at(spec.marker);
            auto const& root = catalog->m_entries.at(spec.root);
            Require(marker.playerClass == spec.playerClass && root.playerClass == spec.playerClass &&
                marker.owners == std::set<uint32_t>{pair.first} &&
                (root.owners.empty() || root.owners.count(pair.first)) &&
                marker.maxRank == 1 && root.maxRank == 1 && !marker.ae && !marker.te &&
                root.ae == 1 && !root.te, "CoA invalid spec package");
        }
        return catalog;
    }

    uint32_t Catalog::SelectedSpec(Ranks const& entries) const
    {
        uint32_t selected = 0;
        for (auto const& pair : m_specs)
        {
            if (entries.count(pair.second.marker))
            {
                Require(!selected && entries.at(pair.second.marker) == 1, "CoA multiple/invalid spec markers");
                selected = pair.first;
            }
        }
        return selected;
    }

    Build Catalog::Authorize(uint32_t playerClass, uint32_t level, uint32_t specId,
        Ranks const& requested) const
    {
        return Authorize(playerClass, level, specId, requested, nullptr);
    }

    Build Catalog::Authorize(uint32_t playerClass, uint32_t level, uint32_t specId,
        Ranks const& requested, AuthorizationError* offender) const
    {
        auto blame = [&](uint32_t entry, uint32_t rank)
        {
            if (offender)
            {
                offender->entry = entry;
                offender->rank = rank;
            }
        };
        Require(HasClass(playerClass) && level >= 1 && level <= 60, "CoA invalid character context");
        Require(!specId || (m_specs.count(specId) && m_specs.at(specId).playerClass == playerClass &&
            level >= 10), "CoA invalid specialization");
        Require(requested.size() <= 1024, "CoA oversized build");
        Require(specId || requested.empty(), "CoA choose specialization before purchases");
        Ranks desired = requested;
        for (auto const& pair : desired)
        {
            if (!m_entries.count(pair.first))
            {
                blame(pair.first, pair.second);
                Require(false, "CoA unknown entry");
            }
            auto const& e = m_entries.at(pair.first);
            if (!pair.second || pair.second > e.maxRank)
            {
                blame(pair.first, pair.second);
                Require(false, "CoA invalid entry rank");
            }
            if (!(e.playerClass == playerClass && (e.owners.empty() || e.owners.count(specId))))
            {
                blame(pair.first, pair.second);
                Require(false, "CoA entry not owned");
            }
        }
        if (specId)
        {
            desired[m_specs.at(specId).marker] = desired[m_specs.at(specId).root] = 1;
        }
        Require(SelectedSpec(desired) == specId, "CoA specialization mismatch");
        // [C] 2026-09-18 (PLAN 24.7 supersedes 24.5): the per-class per-level
        // {ae, te} row is the authoritative class-tree and spec-tree
        // talent-point budget, and each entry's `ae` / `te` is its cost. Sum
        // the desired build (marker + auto-added root are already in `desired`
        // above; the free-cost spec traversal added below is by construction
        // zero-cost and does not affect the sum) and refuse the whole set
        // when either aggregate exceeds its budget. Server-authoritative, so
        // a crafted or modified client cannot bypass the budget. The refusal
        // blames the whole set structurally (offender stays {0,0}, matching
        // CoaState.cpp:130-137's "budget and wire-shape refusals legitimately
        // stay 0"). Traversal string is verbatim `CoA point budget exceeded`;
        // the retired `CoA essence budget exceeded` remains retired. All
        // other ownership, prerequisite, traversal, tier-investment,
        // exclusion, group, state/rate and packet-bound checks are unchanged.
        // See `plans/character-foundations.md` 24.7.
        auto const& budget = m_budgets.at({playerClass, level});
        uint64_t desiredAe = 0, desiredTe = 0;
        for (auto const& pair : desired)
        {
            auto const& e = m_entries.at(pair.first);
            desiredAe += uint64_t(e.ae) * pair.second;
            desiredTe += uint64_t(e.te) * pair.second;
        }
        Require(desiredAe <= budget[0] && desiredTe <= budget[1],
            "CoA point budget exceeded");
        Ranks traversal = desired;
        for (auto const& pair : m_entries)
        {
            auto const& e = pair.second;
            if (specId && e.playerClass == playerClass && !e.ae && !e.te &&
                e.owners.count(specId) && !traversal.count(e.id))
            {
                traversal[e.id] = e.maxRank;
            }
        }
        Build build;
        bool changed;
        do
        {
            changed = false;
            for (auto const& pair : traversal)
            {
                auto const& e = m_entries.at(pair.first);
                uint32_t current = build.entries.count(e.id) ? build.entries.at(e.id) : 0;
                if (current >= pair.second || e.level > level || !std::any_of(e.spells.begin(),
                    e.spells.end(), [&](SpellRank const& s) { return s.talentRank == current + 1 && s.level <= level; }))
                {
                    continue;
                }
                bool eligible = true;
                for (auto const& req : e.requires)
                {
                    if (!build.entries.count(req.first) || build.entries.at(req.first) < req.second)
                    {
                        eligible = false;
                    }
                }
                if (!e.connections.empty() && !std::any_of(e.connections.begin(), e.connections.end(),
                    [&](uint32_t id) { return build.entries.count(id) != 0; }))
                {
                    eligible = false;
                }
                std::array<uint64_t, 6> invested{};
                uint64_t points = 0;
                for (auto const& prior : build.entries)
                {
                    auto const& p = m_entries.at(prior.first);
                    if (e.excludes.count(p.id) || p.excludes.count(e.id) ||
                        (e.id != p.id && e.group && e.group == p.group))
                    {
                        eligible = false;
                    }
                    if (e.id == p.id)
                    {
                        continue;
                    }
                    points += uint64_t(p.points) * prior.second;
                    for (size_t i = 0; i < invested.size(); ++i)
                    {
                        if (i < 4 || p.tab == e.tab)
                        {
                            invested[i] += uint64_t(i % 2 ? p.te : p.ae) * prior.second;
                        }
                    }
                }
                bool anyRequirement = false, anyMet = false, allMet = true;
                for (size_t i = 0; i < invested.size(); ++i)
                {
                    if (e.investment[i])
                    {
                        anyRequirement = true;
                        anyMet |= invested[i] >= e.investment[i];
                        allMet &= invested[i] >= e.investment[i];
                    }
                }
                if (!eligible || points < e.requiredPoints ||
                    (anyRequirement && !(e.flags & 0x4000 ? anyMet : allMet)))
                {
                    continue;
                }
                build.entries[e.id] = current + 1;
                changed = true;
            }
        } while (changed);
        for (auto const& pair : desired)
        {
            if (!(build.entries.count(pair.first) && build.entries.at(pair.first) >= pair.second))
            {
                blame(pair.first, pair.second);
                Require(false, "CoA build not traversable");
            }
        }
        Require(build.entries.size() <= 1024, "CoA canonical build exceeds packet bound");
        for (auto const& pair : build.entries)
        {
            build.spells.insert(EntrySpell(pair.first, pair.second, level).spell);
        }
        // Taking Vows (300331), granted by Sun Cleric node 34025
        // "Spiteful" (ca_talents.lua). Learning it unlocks the seven Vow
        // self-buffs, which have no other grant path on this realm (not
        // starting, not level-up, no other entry references them):
        // 803489 Radiance, 803491 Dawn, 803494 Benediction, 803719 Grace,
        // 807435 Eclipse, 807547 Light, 807749 Valkyr. Vow exclusivity
        // stays out (PLAN 22.22); the periodic +2/+1 covers live in the
        // combat rules.
        if (build.spells.count(300331))
        {
            for (uint32_t vow : {803489u, 803491u, 803494u, 803719u, 807435u, 807547u, 807749u})
            {
                build.spells.insert(vow);
            }
        }
        build.spells.insert(BaseSpell(playerClass, level).spell);
        return build;
    }

    SpellRank Catalog::BaseSpell(uint32_t playerClass, uint32_t level) const
    {
        Require(HasClass(playerClass) && level >= 1 && level <= 60, "CoA invalid starter context");
        return SelectSpell(m_baseSpells.at(playerClass), 1, level);
    }

    SpellRank Catalog::EntrySpell(uint32_t entry, uint32_t rank, uint32_t level) const
    {
        Require(m_entries.count(entry) && level >= 1 && level <= 60, "CoA invalid entry context");
        return SelectSpell(m_entries.at(entry).spells, rank, level);
    }

    std::set<uint32_t> Catalog::AllSpells(uint32_t playerClass) const
    {
        std::set<uint32_t> result;
        for (auto const& e : m_entries)
        {
            if (playerClass && e.second.playerClass != playerClass)
            {
                continue;
            }
            for (auto const& s : e.second.spells)
            {
                result.insert(s.spell);
            }
        }
        for (auto const& cls : m_baseSpells)
        {
            if (playerClass && cls.first != playerClass)
            {
                continue;
            }
            for (auto const& s : cls.second)
            {
                result.insert(s.spell);
            }
        }
        return result;
    }
}

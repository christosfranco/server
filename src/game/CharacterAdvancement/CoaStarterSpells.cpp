// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaStarter.h"

#include <array>
#include <utility>

namespace coa
{
    namespace
    {
        // The full per-class starting book, verbatim from
        // research/ascension-reference/<class>.json starting_spells[] (the
        // 21.1 offline capture, the contract until 22.10's live pass). This is
        // what tools/make-class-spells.py wrote into playercreateinfo_spell;
        // the native path grants the same set on create and at login
        // reconcile, so the generator stops being the source. Class 10 Hero
        // is not CoA-managed and keeps its SQL rows. Every id below exists in
        // the staged Spell.dbc; an unknown id here would fail creation.
        std::array<std::pair<uint32_t, std::vector<uint32_t>>, 21> const& StarterBooks()
        {
            static const std::array<std::pair<uint32_t, std::vector<uint32_t>>, 21> books = {{
                {12, {801576, 200, 2567, 9116, 15590, 804280}}, // Barbarian
                {13, {500017, 5009, 15590, 804280}}, // Witch Doctor
                {14, {800222, 801901, 804334, 804280}}, // Felsworn
                {15, {500082, 800165, 800665, 802001, 264, 266, 5011, 804280, 804404, 804657, 803165}}, // Witch Hunter
                {16, {500929, 804018, 804020, 5009, 804280}}, // Stormbringer
                {17, {801016, 197, 750, 804280, 704452}}, // Knight of Xoroth
                {18, {500155, 500168, 802305, 200, 202, 750, 3124, 9116, 804280}}, // Guardian
                {19, {15590, 804280, 805401, 805402, 806014, 802992, 704572}}, // Templar
                {20, {500125, 804280, 704664, 800486}}, // Bloodmage
                {21, {500074, 804331, 75, 3124, 5011, 804280, 805278, 803165}}, // Ranger
                {22, {800852, 804464, 5009, 804280}}, // Chronomancer
                {23, {500970, 804558, 804280, 804360}}, // Necromancer
                {24, {800790, 5009, 804280, 805650, 800408}}, // Pyromancer
                {25, {500720, 801964, 197, 200, 227, 5009, 8737, 9116, 804280}}, // Cultist
                {26, {800510, 801127, 801976, 75, 197, 227, 674, 5009, 9116, 804280}}, // Starcaller
                {27, {800611, 5009, 8737, 9116, 804280, 800764}}, // Sun Cleric
                {28, {500234, 500237, 801648, 200, 266, 8737, 9116, 804280}}, // Tinker
                {29, {800869, 227, 5009, 804280, 805731, 805900}}, // Venomancer
                {30, {500357, 197, 200, 8737, 804280, 804311, 805684}}, // Reaper
                {31, {500939, 800093, 804344, 107, 8737, 9116, 15590, 804280}}, // Primalist
                {32, {802202, 197, 199, 200, 202, 5009, 804280}}, // Runemaster
            }};
            return books;
        }
    }

    std::set<uint32_t> StarterSpells(uint32_t playerClass)
    {
        for (auto const& book : StarterBooks())
        {
            if (book.first == playerClass) { return {book.second.begin(), book.second.end()}; }
        }
        return {};
    }
}

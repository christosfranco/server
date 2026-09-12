// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaStanceRules.h"
#include <set>

TEST(Coa_stance_sets_cover_the_eleven_lua_sets_disjointly)
{
    // class_resources.lua STANCE_SETS, same member ids. Sets are disjoint by
    // construction: one cast triggers at most one rule.
    uint32_t const members[] = {801681, 801682, 801684, 801685,
        803489, 803491, 803494, 803719, 807435, 807547, 807749,
        500103, 800260, 800261, 800262, 801069, 800317, 803130, 803417,
        802282, 802293, 802276, 802277, 802278, 802279, 500982, 500983, 500985,
        704444, 706195, 806614, 500260, 500263, 800510, 801123, 801128, 802203,
        806290, 806291, 806292, 806293};
    std::set<uint32_t> seen;
    for (auto spell : members)
    {
        auto const* set = coa::StanceSetForSpell(spell);
        REQUIRE(set);
        bool listed = false;
        for (std::size_t i = 0; i < set->size; ++i) { listed = listed || set->members[i] == spell; }
        CHECK(listed);
        CHECK(seen.insert(spell).second);
    }
    CHECK_EQ(seen.size(), 41u);
    CHECK(!coa::StanceSetForSpell(0));
    CHECK(!coa::StanceSetForSpell(800764));
}

TEST(Coa_stance_set_names_match_the_lua_table)
{
    CHECK(std::string(coa::StanceSetForSpell(801681)->name) == "devotions");
    CHECK(std::string(coa::StanceSetForSpell(803489)->name) == "vows");
    CHECK(std::string(coa::StanceSetForSpell(500103)->name) == "quivers");
    CHECK(std::string(coa::StanceSetForSpell(800317)->name) == "formations");
    CHECK(std::string(coa::StanceSetForSpell(802282)->name) == "poses");
    CHECK(std::string(coa::StanceSetForSpell(802276)->name) == "tonics");
    CHECK(std::string(coa::StanceSetForSpell(500982)->name) == "undead");
    CHECK(std::string(coa::StanceSetForSpell(704444)->name) == "augments");
    CHECK(std::string(coa::StanceSetForSpell(500260)->name) == "standards");
    CHECK(std::string(coa::StanceSetForSpell(800510)->name) == "aspects");
    CHECK(std::string(coa::StanceSetForSpell(806290)->name) == "aeons");
    CHECK_EQ(coa::StanceSetForSpell(803489)->size, 7u);
    CHECK_EQ(coa::StanceSetForSpell(500103)->size, 5u);
}

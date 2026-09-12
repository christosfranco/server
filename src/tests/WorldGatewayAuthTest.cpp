/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "TestHarness.h"

#include "Common/Locales.h"
#include "WorldGatewayAuth.h"

#include <array>
#include <stdexcept>

TEST(WorldGatewayAuth_revision_marker_grammar_and_limits)
{
    const std::vector<std::string> valid = {
        "AscenderUpdate_local_update_4", "AscenderUpdate_a_1",
        "AscenderUpdate___9007199254740992",
        "AscenderUpdate_" + std::string(32, 'a') + "_9007199254740992"};
    for (const auto& marker : valid)
    {
        CHECK(IsValidClientUpdateMarker(marker));
    }
    const std::vector<std::string> invalid = {
        "", " ", "AscenderUpdate_", "AscenderUpdate__1", "AscenderUpdate_a_",
        "ascenderUpdate_a_1", "AscenderUpdate_A_1", "AscenderUpdate_a-b_1",
        "AscenderUpdate_a_0", "AscenderUpdate_a_01", "AscenderUpdate_a_-1",
        "AscenderUpdate_a_+1", "AscenderUpdate_a_1.0", "AscenderUpdate_a_1x",
        "AscenderUpdate_a_9007199254740993", "AscenderUpdate_a_10000000000000000",
        "AscenderUpdate_" + std::string(33, 'a') + "_1",
        "AscenderUpdate_a_1/", "AscenderUpdate_a_1\\", "AscenderUpdate_a_1\n",
        "AscenderUpdate_a_1 ", "AscenderUpdate_a\t_1", "AscenderUpdate_a\x7f_1",
        "AscenderUpdate_a\x80_1", std::string("AscenderUpdate_a_1\0", 19)
    };
    for (const auto& marker : invalid)
    {
        CHECK(!IsValidClientUpdateMarker(marker));
    }
}

TEST(WorldGatewayAuth_known_addons_exact_wire_and_explicit_flags)
{
    CHECK(!proto::AuthLookup().knownAddons);
    CHECK(!BuildAscensionKnownAddons(""));
    CHECK(!BuildAscensionKnownAddons("   "));
    auto packet = BuildAscensionKnownAddons(" A:0, B_2:1 ");
    REQUIRE(packet);
    CHECK_EQ(packet->GetOpcode(), uint16(0x94E));
    CHECK_BYTES(packet->contents(), packet->size(),
        {2, 0, 0, 0, 'A', 0, 0, 'B', '_', '2', 0, 1});
    // An override replaces the packaged defaults rather than appending to them.
    auto single = BuildAscensionKnownAddons("Operator_UI-1:0");
    REQUIRE(single);
    CHECK_EQ(single->read<uint32>(0), 1u);
}

TEST(WorldGatewayAuth_known_addons_reject_unsafe_or_ambiguous_manifest)
{
    const std::vector<std::string> invalid = {
        "A", "A:", ":1", "A:2", "A:01", "A:-1", "A:true", "A:1:0",
        ",A:1", "A:1,", "A:1,,B:1", "A:1,  ,B:1", "A:1,A:0",
        "A B:1", "A :1", "A: 1", "../A:1", "A/B:1", "A\\B:1",
        "*:1", "Ascension_*:1", "A;B:1", "A\t:1", "A:1\n", "A:1\r",
        std::string("A\0B:1", 6), "A\x7f:1", "A\x80:1",
        std::string(65, 'A') + ":1", std::string(8577, 'A')
    };
    for (const auto& manifest : invalid)
    {
        bool rejected = false;
        try
        {
            BuildAscensionKnownAddons(manifest);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        CHECK(rejected);
    }
}

TEST(WorldGatewayAuth_known_addons_limits_and_pinned_defaults)
{
    auto longest = BuildAscensionKnownAddons(std::string(64, 'A') + ":1");
    REQUIRE(longest);
    CHECK_EQ(longest->size(), size_t(70));
    std::string manifest;
    for (size_t i = 0; i < 128; ++i)
    {
        manifest += (i ? "," : "") + std::string("Addon_") + std::to_string(i) + ":0";
    }
    auto packet = BuildAscensionKnownAddons(manifest);
    REQUIRE(packet);
    CHECK_EQ(packet->read<uint32>(0), 128u);
    CHECK_BYTES(packet->contents(), 4, {128, 0, 0, 0});
    bool rejected = false;
    try
    {
        BuildAscensionKnownAddons(manifest + ",Extra:1");
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    CHECK(rejected);

    WorldPacket defaults(*BuildAscensionKnownAddons(ASCENSION_KNOWN_ADDONS_DEFAULT));
    uint32 count;
    defaults >> count;
    CHECK_EQ(defaults.GetOpcode(), uint16(0x94E));
    CHECK_EQ(count, 11u);
    CHECK_BYTES(defaults.contents(), 4, {11, 0, 0, 0});
    size_t expectedSize = 4;
    for (const char* expected : {"Ascension_Collections", "AscensionUI",
                                "Ascension_TalentUI", "Ascension_CoATalents",
                                "Ascension_Warmode", "Ascension_NamePlates",
                                "Ascension_NewPlayerExperience", "Ascension_MythicPlus",
                                "Ascension_Manastorm", "Ascension_VanityCollection",
                                "Ascension_AppearanceUI"})
    {
        std::string name;
        uint8 secure;
        defaults >> name >> secure;
        CHECK_STR(name, expected);
        CHECK_EQ(secure, uint8(1));
        expectedSize += std::string(expected).size() + 2;
    }
    CHECK_EQ(defaults.size(), expectedSize);
    CHECK_EQ(defaults.rpos(), defaults.size());
}

TEST(WorldGatewayAuth_accepts_only_supported_account_operating_systems)
{
    CHECK(IsSupportedAccountClientOS("Win"));
    CHECK(IsSupportedAccountClientOS("OSX"));
    CHECK(!IsSupportedAccountClientOS(""));
    CHECK(!IsSupportedAccountClientOS("win"));
    CHECK(!IsSupportedAccountClientOS("Linux"));
}

TEST(WorldGatewayAuth_stock_auth_candidate_requires_operator_opt_in_and_exact_build)
{
    CHECK(proto::AuthLookup().profile == proto::ConnectionProfile::Stock);
    CHECK(SelectConnectionProfile(12344, false) ==
        proto::ConnectionProfile::AscensionClearHeaders);
    CHECK(SelectConnectionProfile(12344, true) ==
        proto::ConnectionProfile::AscensionStockAuthCoA);
    for (uint32 build : {0u, 12340u, 12343u, 12345u, 15595u})
    {
        for (bool optIn : {false, true})
        {
            CHECK(SelectConnectionProfile(build, optIn) ==
                proto::ConnectionProfile::Stock);
        }
    }
}

TEST(ExactLocaleName_recognizes_all_client_locale_tokens_without_aliasing)
{
    std::array<char const*, 10> const locales =
    {{
        "enUS", "enGB", "koKR", "frFR", "deDE",
        "zhCN", "zhTW", "esES", "esMX", "ruRU"
    }};
    for (char const* locale : locales)
    {
        char const* exact = GetExactLocaleName(locale);
        REQUIRE(exact != nullptr);
        CHECK_STR(exact, locale);
    }
    CHECK_STR(GetExactLocaleName("enGB"), "enGB");
    CHECK(GetLocaleByName("enGB") == LOCALE_enUS);
}

TEST(ExactLocaleName_rejects_missing_or_malformed_state_without_fallback)
{
    CHECK(GetExactLocaleName("") == nullptr);
    CHECK(GetExactLocaleName("enus") == nullptr);
    CHECK(GetExactLocaleName("enUS ") == nullptr);
    CHECK(GetExactLocaleName("unknown") == nullptr);
}

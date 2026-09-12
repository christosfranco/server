/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "TestHarness.h"

#include "CoaProtocol.h"
#include "WorldPacket.h"

#include <limits>
#include <vector>

namespace
{
    using coa::analytic::CoaEntry;
    using coa::analytic::ParseStatus;

    WorldPacket Packet(uint16 opcode, std::initializer_list<uint8> bytes)
    {
        WorldPacket packet(opcode, bytes.size());
        for (uint8 byte : bytes)
        {
            packet << byte;
        }
        return packet;
    }

    void AppendUint32(std::vector<uint8>& bytes, uint32 value)
    {
        bytes.push_back(uint8(value));
        bytes.push_back(uint8(value >> 8));
        bytes.push_back(uint8(value >> 16));
        bytes.push_back(uint8(value >> 24));
    }

    void AppendUint64(std::vector<uint8>& bytes, uint64 value)
    {
        for (uint8 index = 0; index < 8; ++index)
        {
            bytes.push_back(uint8(value >> (index * 8)));
        }
    }

    WorldPacket EntryPacket(uint16 opcode, std::vector<CoaEntry> const& entries)
    {
        std::vector<uint8> bytes;
        AppendUint32(bytes, uint32(entries.size()));
        for (CoaEntry const& entry : entries)
        {
            AppendUint32(bytes, entry.entryId);
            AppendUint32(bytes, entry.rank);
            AppendUint32(bytes, entry.spellRankLimit);
            bytes.push_back(uint8(entry.locked ? 1 : 0));
            AppendUint64(bytes, static_cast<uint64>(entry.learnedTime));
        }

        WorldPacket packet(opcode, bytes.size());
        packet.append(bytes.data(), bytes.size());
        return packet;
    }

    CoaEntry FirstEntry()
    {
        return {0x01020304, 2, 0x11223344, true, -0x0102030405060708LL};
    }

    CoaEntry SecondEntry()
    {
        return {0xAABBCCDD, 9, 1, false, 0x0102030405060708LL};
    }
}

TEST(CoaProtocol_decodes_active_state_little_endian_and_bounds)
{
    WorldPacket packet = Packet(coa::analytic::ActiveStateOpcode,
        {0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00});
    auto const parsed = coa::analytic::ParseActiveState(packet);
    REQUIRE(parsed.status == ParseStatus::Ok);
    CHECK_EQ(parsed.value.activeIndex, uint32(1));
    CHECK_EQ(parsed.value.count, uint32(2));
    CHECK_EQ(packet.rpos(), size_t(0));

    for (size_t size = 0; size < 8; ++size)
    {
        WorldPacket truncated(coa::analytic::ActiveStateOpcode, size);
        truncated.append(packet.contents(), size);
        CHECK(coa::analytic::ParseActiveState(truncated).status ==
            ParseStatus::WrongSize);
    }

    CHECK(coa::analytic::ParseActiveState(Packet(coa::analytic::ActiveStateOpcode,
        {0, 0, 0, 0, 0, 0, 0, 0})).status == ParseStatus::InvalidValue);
    CHECK(coa::analytic::ParseActiveState(Packet(coa::analytic::ActiveStateOpcode,
        {1, 0, 0, 0, 1, 0, 0, 0})).status == ParseStatus::InvalidValue);
    CHECK(coa::analytic::ParseActiveState(Packet(coa::analytic::ActiveStateOpcode,
        {0, 0, 0, 0, 21, 0, 0, 0})).status == ParseStatus::InvalidValue);
    CHECK(coa::analytic::ParseActiveState(Packet(0x726,
        {0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00})).status ==
        ParseStatus::WrongOpcode);
}

TEST(CoaProtocol_decodes_two_entries_without_padding)
{
    WorldPacket packet = Packet(coa::analytic::EntriesStateOpcode,
        {0x02, 0x00, 0x00, 0x00,
         0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00,
         0x44, 0x33, 0x22, 0x11, 0x01,
         0xF8, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
         0xDD, 0xCC, 0xBB, 0xAA, 0x09, 0x00, 0x00, 0x00,
         0x01, 0x00, 0x00, 0x00, 0x00,
         0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});
    auto const parsed = coa::analytic::ParseEntriesState(packet);
    REQUIRE(parsed.status == ParseStatus::Ok);
    REQUIRE(parsed.value.entries.size() == 2);
    CHECK_EQ(parsed.value.entries[0].entryId, uint32(0x01020304));
    CHECK_EQ(parsed.value.entries[0].rank, uint32(2));
    CHECK_EQ(parsed.value.entries[0].spellRankLimit, uint32(0x11223344));
    CHECK(parsed.value.entries[0].locked);
    CHECK_EQ(parsed.value.entries[0].learnedTime, -0x0102030405060708LL);
    CHECK_EQ(parsed.value.entries[1].entryId, uint32(0xAABBCCDD));
    CHECK_EQ(parsed.value.entries[1].learnedTime, 0x0102030405060708LL);
    CHECK_EQ(packet.size(), size_t(46));
    CHECK_EQ(packet.rpos(), size_t(0));
}

TEST(CoaProtocol_entry_decoding_rejects_every_truncation_and_trailing_byte)
{
    WorldPacket valid = EntryPacket(coa::analytic::EntriesStateOpcode,
        {FirstEntry(), SecondEntry()});
    for (size_t size = 0; size < valid.size(); ++size)
    {
        WorldPacket truncated(coa::analytic::EntriesStateOpcode, size);
        truncated.append(valid.contents(), size);
        CHECK(coa::analytic::ParseEntriesState(truncated).status != ParseStatus::Ok);
    }

    valid << uint8(0);
    CHECK(coa::analytic::ParseEntriesState(valid).status == ParseStatus::WrongSize);
    CHECK(coa::analytic::ParseEntriesState(Packet(coa::analytic::EntriesStateOpcode,
        {3, 0, 0, 0})).status == ParseStatus::WrongSize);
}

TEST(CoaProtocol_entry_decoding_rejects_hostile_counts_and_descriptors)
{
    CHECK(coa::analytic::ParseEntriesState(Packet(coa::analytic::EntriesStateOpcode,
        {0xFF, 0xFF, 0xFF, 0xFF})).status == ParseStatus::InvalidValue);

    CoaEntry invalid = FirstEntry();
    invalid.entryId = 0;
    CHECK(coa::analytic::ParseEntriesState(EntryPacket(
        coa::analytic::EntriesStateOpcode, {invalid})).status ==
        ParseStatus::InvalidValue);
    invalid = FirstEntry();
    invalid.rank = 0;
    CHECK(coa::analytic::ParseEntriesState(EntryPacket(
        coa::analytic::EntriesStateOpcode, {invalid})).status ==
        ParseStatus::InvalidValue);
    invalid.rank = 10;
    CHECK(coa::analytic::ParseEntriesState(EntryPacket(
        coa::analytic::EntriesStateOpcode, {invalid})).status ==
        ParseStatus::InvalidValue);
    invalid = FirstEntry();
    invalid.spellRankLimit = 0;
    CHECK(coa::analytic::ParseEntriesState(EntryPacket(
        coa::analytic::EntriesStateOpcode, {invalid})).status ==
        ParseStatus::InvalidValue);
    invalid = FirstEntry();
    invalid.locked = false;
    WorldPacket badLocked = EntryPacket(coa::analytic::EntriesStateOpcode, {invalid});
    badLocked.put(16, uint8(2));
    CHECK(coa::analytic::ParseEntriesState(badLocked).status ==
        ParseStatus::InvalidValue);
}

TEST(CoaProtocol_entry_decoding_rejects_duplicates_and_accepts_signed_times)
{
    CoaEntry negative = FirstEntry();
    negative.learnedTime = std::numeric_limits<int64>::min();
    WorldPacket valid = EntryPacket(coa::analytic::EntriesStateOpcode, {negative});
    auto const parsed = coa::analytic::ParseEntriesState(valid);
    REQUIRE(parsed.status == ParseStatus::Ok);
    CHECK_EQ(parsed.value.entries[0].learnedTime,
        std::numeric_limits<int64>::min());

    CoaEntry duplicate = FirstEntry();
    duplicate.rank = 3;
    CHECK(coa::analytic::ParseEntriesState(EntryPacket(
        coa::analytic::EntriesStateOpcode, {FirstEntry(), duplicate})).status ==
        ParseStatus::InvalidValue);
}

TEST(CoaProtocol_entry_vectors_are_bounded_and_replacement_has_own_opcode)
{
    WorldPacket empty = coa::analytic::BuildEntriesStatePacket({});
    CHECK_HEX(empty.contents(), empty.size(), "00000000");
    CHECK(coa::analytic::ParseEntriesState(empty).status == ParseStatus::Ok);

    std::vector<CoaEntry> entries;
    entries.reserve(coa::analytic::MaximumEntryCount + 1);
    for (uint32 index = 0; index < coa::analytic::MaximumEntryCount + 1; ++index)
    {
        entries.push_back({index + 1, 1, 1, false, 0});
    }
    WorldPacket oversized = coa::analytic::BuildEntriesStatePacket(entries);
    CHECK_EQ(oversized.GetOpcode(), uint16(0));
    CHECK(oversized.empty());
    CoaEntry invalid = FirstEntry();
    invalid.rank = 10;
    WorldPacket invalidPacket = coa::analytic::BuildEntriesStatePacket({invalid});
    CHECK_EQ(invalidPacket.GetOpcode(), uint16(0));
    CHECK(invalidPacket.empty());

    std::vector<CoaEntry> const one = {FirstEntry()};
    WorldPacket replacement = coa::analytic::BuildEntriesReplacementPacket(one);
    CHECK_EQ(replacement.GetOpcode(), coa::analytic::EntriesReplacementOpcode);
    auto const parsed = coa::analytic::ParseEntriesReplacement(replacement);
    REQUIRE(parsed.status == ParseStatus::Ok);
    CHECK_EQ(parsed.value.entries.size(), size_t(1));
}

TEST(CoaProtocol_update_result_requires_known_token_and_exact_strings)
{
    coa::analytic::ResultToken const tokens[] =
    {
        coa::analytic::ResultToken::UpdateEntriesOk,
        coa::analytic::ResultToken::Unknown,
        coa::analytic::ResultToken::NoBuild,
        coa::analytic::ResultToken::BadEntry,
        coa::analytic::ResultToken::NoDiff,
        coa::analytic::ResultToken::NotTraversible,
        coa::analytic::ResultToken::BadUpdateCosts,
        coa::analytic::ResultToken::MissingTokens,
        coa::analytic::ResultToken::GameModeNotAllowed
    };
    char const* const expected[] =
    {
        "CA_UPDATE_ENTRIES_OK",
        "CA_UPDATE_ENTRIES_UNKNOWN",
        "CA_UPDATE_ENTRIES_NO_BUILD",
        "CA_UPDATE_ENTRIES_BAD_ENTRY",
        "CA_UPDATE_ENTRIES_NO_DIFF",
        "CA_UPDATE_ENTRIES_NOT_TRAVERSIBLE",
        "CA_UPDATE_ENTRIES_BAD_UPDATE_COSTS",
        "CA_UPDATE_ENTRIES_MISSING_TOKENS",
        "CA_UPDATE_GAME_MODE_NOT_ALLOWED"
    };
    for (size_t index = 0; index < sizeof(tokens) / sizeof(tokens[0]); ++index)
    {
        CHECK_STR(coa::analytic::ResultTokenText(tokens[index]), expected[index]);
        CHECK(coa::analytic::ParseUpdateResult(
            coa::analytic::BuildUpdateResultPacket(tokens[index], "", 0, 0)).status ==
            ParseStatus::Ok);
    }

    WorldPacket packet = coa::analytic::BuildUpdateResultPacket(
        coa::analytic::ResultToken::GameModeNotAllowed, "CA_TRAVERSE_OK", 0x01020304, 9);
    auto const parsed = coa::analytic::ParseUpdateResult(packet);
    REQUIRE(parsed.status == ParseStatus::Ok);
    CHECK_STR(parsed.value.result, "CA_UPDATE_GAME_MODE_NOT_ALLOWED");
    CHECK_STR(parsed.value.traversal, "CA_TRAVERSE_OK");
    CHECK_EQ(parsed.value.errorEntry, uint32(0x01020304));
    CHECK_EQ(parsed.value.rank, uint32(9));

    for (size_t size = 0; size < packet.size(); ++size)
    {
        WorldPacket truncated(coa::analytic::UpdateResultOpcode, size);
        truncated.append(packet.contents(), size);
        CHECK(coa::analytic::ParseUpdateResult(truncated).status != ParseStatus::Ok);
    }

    WorldPacket wrong = Packet(coa::analytic::UpdateResultOpcode,
        {'N', 'O', 'T', '_', 'K', 'N', 'O', 'W', 'N',
         0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    CHECK(coa::analytic::ParseUpdateResult(wrong).status ==
        ParseStatus::InvalidResultToken);
    WorldPacket embeddedNul = coa::analytic::BuildUpdateResultPacket(
        coa::analytic::ResultToken::UpdateEntriesOk, std::string("bad\0tail", 8), 0, 0);
    CHECK_EQ(embeddedNul.GetOpcode(), uint16(0));
}

TEST(CoaProtocol_builders_emit_exact_wire_shapes)
{
    WorldPacket active = coa::analytic::BuildActiveStatePacket(1, 2);
    CHECK_HEX(active.contents(), active.size(), "0100000002000000");
    WorldPacket entries = coa::analytic::BuildEntriesStatePacket({FirstEntry()});
    CHECK_HEX(entries.contents(), entries.size(),
        "0100000004030201020000004433221101"
        "f8f8f9fafbfcfdfe");
}

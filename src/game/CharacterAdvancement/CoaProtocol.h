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

#ifndef MANGOS_GAME_CHARACTERADVANCEMENT_COAPROTOCOL_H
#define MANGOS_GAME_CHARACTERADVANCEMENT_COAPROTOCOL_H

#include "Platform/Define.h"
#include "WorldPacket.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coa
{
    namespace analytic
    {
        // These are recovered wire values. They intentionally are not added to
        // the vendor opcode table until the game integration owns that mapping.
        uint16 constexpr ActiveStateOpcode = 0x725;
        uint16 constexpr EntriesStateOpcode = 0x726;
        uint16 constexpr EntriesReplacementOpcode = 0x727;
        uint16 constexpr UpdateResultOpcode = 0x72C;

        uint32 constexpr MaximumActiveEntryCount = 20;
        // Server policy bound; native evidence established the vector shape,
        // not a native maximum. The packet budget is independently 64 KiB.
        uint32 constexpr MaximumEntryCount = 1024;
        size_t constexpr EntryWireBytes = 21;
        size_t constexpr MaximumWireBytes = 64 * 1024;

        enum class ParseStatus : uint8
        {
            Ok,
            WrongOpcode,
            WrongSize,
            InvalidValue,
            InvalidResultToken
        };

        enum class ResultToken : uint8
        {
            UpdateEntriesOk,
            Unknown,
            NoBuild,
            BadEntry,
            NoDiff,
            NotTraversible,
            BadUpdateCosts,
            MissingTokens,
            GameModeNotAllowed
        };

        struct CoaEntry
        {
            uint32 entryId = 0;
            uint32 rank = 0;
            uint32 spellRankLimit = 1;
            bool locked = false;
            int64 learnedTime = 0;
        };

        struct ActiveState
        {
            uint32 activeIndex = 0;
            uint32 count = 0;
        };

        struct EntriesState
        {
            std::vector<CoaEntry> entries;
        };

        struct UpdateResult
        {
            std::string result;
            std::string traversal;
            uint32 errorEntry = 0;
            uint32 rank = 0;
        };

        struct ActiveStateParseResult
        {
            ParseStatus status = ParseStatus::WrongSize;
            ActiveState value;
        };

        struct EntriesParseResult
        {
            ParseStatus status = ParseStatus::WrongSize;
            EntriesState value;
        };

        struct UpdateResultParseResult
        {
            ParseStatus status = ParseStatus::WrongSize;
            UpdateResult value;
        };

        ActiveStateParseResult ParseActiveState(WorldPacket const& packet);
        EntriesParseResult ParseEntriesState(WorldPacket const& packet);
        EntriesParseResult ParseEntriesReplacement(WorldPacket const& packet);
        UpdateResultParseResult ParseUpdateResult(WorldPacket const& packet);

        // Invalid builder input returns a null/empty WorldPacket; no partially
        // validated wire data is emitted.
        WorldPacket BuildActiveStatePacket(uint32 activeIndex, uint32 count);
        WorldPacket BuildEntriesStatePacket(std::vector<CoaEntry> const& entries);
        WorldPacket BuildEntriesReplacementPacket(std::vector<CoaEntry> const& entries);
        WorldPacket BuildUpdateResultPacket(ResultToken result, std::string const& traversal,
            uint32 errorEntry, uint32 rank);

        char const* ResultTokenText(ResultToken token);
    }
}

#endif

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

#include "CoaProtocol.h"

#include "Utilities/ByteBuffer.h"
#include "WorldPacket.h"

#include <utility>

namespace
{
    using namespace coa::analytic;

    bool HasBytes(ByteBuffer const& packet, size_t offset, size_t bytes)
    {
        return offset <= packet.size() && bytes <= packet.size() - offset;
    }

    uint32 ReadUint32LE(ByteBuffer const& packet, size_t offset)
    {
        return uint32(packet[offset]) |
            (uint32(packet[offset + 1]) << 8) |
            (uint32(packet[offset + 2]) << 16) |
            (uint32(packet[offset + 3]) << 24);
    }

    int64 ReadInt64LE(ByteBuffer const& packet, size_t offset)
    {
        uint64 value = uint64(packet[offset]) |
            (uint64(packet[offset + 1]) << 8) |
            (uint64(packet[offset + 2]) << 16) |
            (uint64(packet[offset + 3]) << 24) |
            (uint64(packet[offset + 4]) << 32) |
            (uint64(packet[offset + 5]) << 40) |
            (uint64(packet[offset + 6]) << 48) |
            (uint64(packet[offset + 7]) << 56);
        return static_cast<int64>(value);
    }

    bool IsValidEntry(CoaEntry const& entry)
    {
        return entry.entryId != 0 && entry.rank >= 1 && entry.rank <= 9 &&
            entry.spellRankLimit != 0;
    }

    bool IsValidEntries(std::vector<CoaEntry> const& entries)
    {
        if (entries.size() > MaximumEntryCount)
        {
            return false;
        }

        for (size_t first = 0; first < entries.size(); ++first)
        {
            if (!IsValidEntry(entries[first]))
            {
                return false;
            }
            for (size_t second = first + 1; second < entries.size(); ++second)
            {
                if (entries[first].entryId == entries[second].entryId)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsKnownResultToken(std::string const& result)
    {
        for (uint8 token = 0; token <= uint8(ResultToken::GameModeNotAllowed); ++token)
        {
            if (result == ResultTokenText(static_cast<ResultToken>(token)))
            {
                return true;
            }
        }
        return false;
    }

    bool IsNulTerminated(ByteBuffer const& packet, size_t start, size_t& end)
    {
        if (start > packet.size())
        {
            return false;
        }

        for (size_t offset = start; offset < packet.size(); ++offset)
        {
            if (packet[offset] == 0)
            {
                end = offset;
                return true;
            }
        }
        return false;
    }

    std::string ReadString(ByteBuffer const& packet, size_t start, size_t end)
    {
        std::string result;
        result.reserve(end - start);
        for (size_t offset = start; offset < end; ++offset)
        {
            result.push_back(static_cast<char>(packet[offset]));
        }
        return result;
    }

    EntriesParseResult ParseEntries(WorldPacket const& packet, uint16 expectedOpcode)
    {
        EntriesParseResult result;
        if (packet.GetOpcode() != expectedOpcode)
        {
            result.status = ParseStatus::WrongOpcode;
            return result;
        }
        if (!HasBytes(packet, 0, sizeof(uint32)))
        {
            return result;
        }

        uint32 count = ReadUint32LE(packet, 0);
        if (count > MaximumEntryCount || count >
            (MaximumWireBytes - sizeof(uint32)) / EntryWireBytes)
        {
            result.status = ParseStatus::InvalidValue;
            return result;
        }

        size_t const expectedSize = sizeof(uint32) + size_t(count) * EntryWireBytes;
        if (expectedSize > MaximumWireBytes || packet.size() != expectedSize)
        {
            return result;
        }

        std::vector<CoaEntry> entries;
        entries.reserve(count);
        for (uint32 index = 0; index < count; ++index)
        {
            size_t const offset = sizeof(uint32) + size_t(index) * EntryWireBytes;
            CoaEntry entry;
            entry.entryId = ReadUint32LE(packet, offset);
            entry.rank = ReadUint32LE(packet, offset + 4);
            entry.spellRankLimit = ReadUint32LE(packet, offset + 8);
            uint8 const locked = packet[offset + 12];
            entry.learnedTime = ReadInt64LE(packet, offset + 13);
            if (locked > 1)
            {
                result.status = ParseStatus::InvalidValue;
                return result;
            }
            entry.locked = locked != 0;
            if (!IsValidEntry(entry))
            {
                result.status = ParseStatus::InvalidValue;
                return result;
            }
            entries.push_back(entry);
        }

        if (!IsValidEntries(entries))
        {
            result.status = ParseStatus::InvalidValue;
            return result;
        }

        result.status = ParseStatus::Ok;
        result.value.entries = std::move(entries);
        return result;
    }

    WorldPacket BuildEntriesPacket(uint16 opcode, std::vector<CoaEntry> const& entries)
    {
        if (!IsValidEntries(entries) ||
            entries.size() > (MaximumWireBytes - sizeof(uint32)) / EntryWireBytes)
        {
            return WorldPacket();
        }

        WorldPacket packet(opcode, sizeof(uint32) + entries.size() * EntryWireBytes);
        packet << static_cast<uint32>(entries.size());
        for (CoaEntry const& entry : entries)
        {
            packet << entry.entryId;
            packet << entry.rank;
            packet << entry.spellRankLimit;
            packet << static_cast<uint8>(entry.locked ? 1 : 0);
            packet << static_cast<uint64>(entry.learnedTime);
        }
        return packet;
    }
}

namespace coa
{
    namespace analytic
    {
        char const* ResultTokenText(ResultToken token)
        {
            switch (token)
            {
                case ResultToken::UpdateEntriesOk:
                    return "CA_UPDATE_ENTRIES_OK";
                case ResultToken::Unknown:
                    return "CA_UPDATE_ENTRIES_UNKNOWN";
                case ResultToken::NoBuild:
                    return "CA_UPDATE_ENTRIES_NO_BUILD";
                case ResultToken::BadEntry:
                    return "CA_UPDATE_ENTRIES_BAD_ENTRY";
                case ResultToken::NoDiff:
                    return "CA_UPDATE_ENTRIES_NO_DIFF";
                case ResultToken::NotTraversible:
                    return "CA_UPDATE_ENTRIES_NOT_TRAVERSIBLE";
                case ResultToken::BadUpdateCosts:
                    return "CA_UPDATE_ENTRIES_BAD_UPDATE_COSTS";
                case ResultToken::MissingTokens:
                    return "CA_UPDATE_ENTRIES_MISSING_TOKENS";
                case ResultToken::GameModeNotAllowed:
                    return "CA_UPDATE_GAME_MODE_NOT_ALLOWED";
            }
            return "";
        }

        ActiveStateParseResult ParseActiveState(WorldPacket const& packet)
        {
            ActiveStateParseResult result;
            if (packet.GetOpcode() != ActiveStateOpcode)
            {
                result.status = ParseStatus::WrongOpcode;
                return result;
            }
            if (packet.size() != sizeof(uint32) * 2)
            {
                return result;
            }

            uint32 const activeIndex = ReadUint32LE(packet, 0);
            uint32 const count = ReadUint32LE(packet, sizeof(uint32));
            if (count == 0 || count > MaximumActiveEntryCount || activeIndex >= count)
            {
                result.status = ParseStatus::InvalidValue;
                return result;
            }

            result.status = ParseStatus::Ok;
            result.value = {activeIndex, count};
            return result;
        }

        EntriesParseResult ParseEntriesState(WorldPacket const& packet)
        {
            return ParseEntries(packet, EntriesStateOpcode);
        }

        EntriesParseResult ParseEntriesReplacement(WorldPacket const& packet)
        {
            return ParseEntries(packet, EntriesReplacementOpcode);
        }

        UpdateResultParseResult ParseUpdateResult(WorldPacket const& packet)
        {
            UpdateResultParseResult result;
            if (packet.GetOpcode() != UpdateResultOpcode)
            {
                result.status = ParseStatus::WrongOpcode;
                return result;
            }
            if (packet.size() > MaximumWireBytes)
            {
                return result;
            }

            size_t resultEnd = 0;
            if (!IsNulTerminated(packet, 0, resultEnd))
            {
                return result;
            }
            size_t traversalEnd = 0;
            if (!IsNulTerminated(packet, resultEnd + 1, traversalEnd) ||
                !HasBytes(packet, traversalEnd + 1, sizeof(uint32) * 2) ||
                packet.size() != traversalEnd + 1 + sizeof(uint32) * 2)
            {
                return result;
            }

            std::string const resultText = ReadString(packet, 0, resultEnd);
            if (!IsKnownResultToken(resultText))
            {
                result.status = ParseStatus::InvalidResultToken;
                return result;
            }

            result.status = ParseStatus::Ok;
            result.value.result = resultText;
            result.value.traversal = ReadString(packet, resultEnd + 1, traversalEnd);
            result.value.errorEntry = ReadUint32LE(packet, traversalEnd + 1);
            result.value.rank = ReadUint32LE(packet, traversalEnd + 1 + sizeof(uint32));
            return result;
        }

        WorldPacket BuildActiveStatePacket(uint32 activeIndex, uint32 count)
        {
            if (count == 0 || count > MaximumActiveEntryCount || activeIndex >= count)
            {
                return WorldPacket();
            }

            WorldPacket packet(ActiveStateOpcode, sizeof(uint32) * 2);
            packet << activeIndex;
            packet << count;
            return packet;
        }

        WorldPacket BuildEntriesStatePacket(std::vector<CoaEntry> const& entries)
        {
            return BuildEntriesPacket(EntriesStateOpcode, entries);
        }

        WorldPacket BuildEntriesReplacementPacket(std::vector<CoaEntry> const& entries)
        {
            return BuildEntriesPacket(EntriesReplacementOpcode, entries);
        }

        WorldPacket BuildUpdateResultPacket(ResultToken result, std::string const& traversal,
            uint32 errorEntry, uint32 rank)
        {
            std::string const resultText = ResultTokenText(result);
            if (resultText.empty() || traversal.find('\0') != std::string::npos ||
                resultText.size() + 1 + traversal.size() + 1 + sizeof(uint32) * 2 >
                    MaximumWireBytes)
            {
                return WorldPacket();
            }

            WorldPacket packet(UpdateResultOpcode,
                resultText.size() + 1 + traversal.size() + 1 +
                sizeof(uint32) * 2);
            packet << ResultTokenText(result);
            packet << traversal;
            packet << errorEntry;
            packet << rank;
            return packet;
        }
    }
}

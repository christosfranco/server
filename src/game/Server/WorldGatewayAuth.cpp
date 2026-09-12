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

#include "WorldGatewayAuth.h"
#include "Utilities/ByteBuffer.h"

#include <openssl/crypto.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    constexpr std::string_view CLIENT_UPDATE_PREFIX = "AscenderUpdate_";
}

bool IsSupportedAccountClientOS(const std::string& os)
{
    return os == "Win" || os == "OSX";
}

proto::ConnectionProfile SelectConnectionProfile(uint32 build,
    bool ascensionStockAuthCompatibility)
{
    if (build != 12344)
    {
        return proto::ConnectionProfile::Stock;
    }
    return ascensionStockAuthCompatibility
        ? proto::ConnectionProfile::AscensionStockAuthCoA
        : proto::ConnectionProfile::AscensionClearHeaders;
}

std::shared_ptr<const WorldPacket> BuildAscensionKnownAddons(
    const std::string& manifest)
{
    // Operator limits, not inferred client limits. Bound before token allocation.
    constexpr size_t MAX_NAMES = 128;
    constexpr size_t MAX_NAME_LENGTH = 64;
    constexpr uint16 SMSG_KNOWN_ADDON_LIST = 0x94E;
    if (manifest.size() > MAX_NAMES * (MAX_NAME_LENGTH + 3)
        || std::any_of(manifest.begin(), manifest.end(), [](unsigned char c)
            { return c < 0x20 || c >= 0x7F; }))
    {
        throw std::invalid_argument("manifest too long or contains unsafe bytes");
    }
    if (manifest.find_first_not_of(' ') == std::string::npos)
    {
        return nullptr;
    }

    auto packet = std::make_shared<WorldPacket>(SMSG_KNOWN_ADDON_LIST, 256);
    *packet << uint32(0);
    std::set<std::string> names;
    size_t start = 0;
    while (start <= manifest.size())
    {
        const size_t end = manifest.find(',', start);
        std::string entry = manifest.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        const size_t first = entry.find_first_not_of(' ');
        if (first == std::string::npos)
        {
            throw std::invalid_argument("empty manifest entry");
        }
        entry = entry.substr(first, entry.find_last_not_of(' ') - first + 1);
        const size_t colon = entry.find(':');
        if (colon == std::string::npos || colon == 0 || colon > MAX_NAME_LENGTH
            || entry.size() != colon + 2
            || (entry.back() != '0' && entry.back() != '1'))
        {
            throw std::invalid_argument("expected name:0 or name:1 (name 1..64 bytes)");
        }
        const std::string name = entry.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c)
            { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-'; }))
        {
            throw std::invalid_argument("unsafe addon name");
        }
        if (!names.insert(name).second || names.size() > MAX_NAMES)
        {
            throw std::invalid_argument("duplicate addon or more than 128 names");
        }
        *packet << name << uint8(entry.back() - '0');
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    packet->put<uint32>(0, uint32(names.size()));
    return packet;
}

bool IsValidClientUpdateMarker(const std::string& marker)
{
    const std::string_view name(marker);
    if (name.size() > 64 || name.substr(0, CLIENT_UPDATE_PREFIX.size()) !=
        CLIENT_UPDATE_PREFIX)
    {
        return false;
    }
    const size_t separator = name.rfind('_');
    if (separator <= CLIENT_UPDATE_PREFIX.size() ||
        separator - CLIENT_UPDATE_PREFIX.size() > 32)
    {
        return false;
    }
    const auto channel = name.substr(CLIENT_UPDATE_PREFIX.size(),
        separator - CLIENT_UPDATE_PREFIX.size());
    if (!std::all_of(channel.begin(), channel.end(), [](unsigned char c)
        { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
    {
        return false;
    }
    const auto sequence = name.substr(separator + 1);
    if (sequence.empty() || sequence.size() > 16 || sequence.front() == '0')
    {
        return false;
    }
    uint64 value = 0;
    for (unsigned char c : sequence)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    return value <= (uint64(1) << 53);
}

std::string ClientUpdateAdmission::Diagnostic() const
{
    const char* reason;
    switch (status)
    {
        case ClientUpdateStatus::Disabled:
        case ClientUpdateStatus::Accepted: return {};
        case ClientUpdateStatus::InvalidRequirement: reason = "invalid_requirement"; break;
        case ClientUpdateStatus::MissingMarker: reason = "missing_marker"; break;
        case ClientUpdateStatus::WrongMarker: reason = "wrong_marker"; break;
        case ClientUpdateStatus::DuplicateMarker: reason = "duplicate_marker"; break;
        case ClientUpdateStatus::CompressedFormat: reason = "compressed_format"; break;
        case ClientUpdateStatus::Size: reason = "size"; break;
        case ClientUpdateStatus::Count: reason = "count"; break;
        case ClientUpdateStatus::RecordFormat: reason = "record_format"; break;
        case ClientUpdateStatus::Trailer: reason = "trailer"; break;
        default: reason = "invalid_reason"; break;
    }
    return std::string("world_auth stage=client_update reason=") + reason +
        " addon_count=" + std::to_string(addonCount) +
        " marker_count=" + std::to_string(markerCount) +
        " matched=" + (matched ? "1" : "0");
}

ClientUpdateAdmission CheckRequiredClientUpdate(const std::vector<uint8>& addonData,
    const std::string& required)
{
    if (required.empty())
    {
        return {ClientUpdateStatus::Disabled};
    }
    if (!IsValidClientUpdateMarker(required))
    {
        return {ClientUpdateStatus::InvalidRequirement};
    }
    if (addonData.empty())
    {
        return {ClientUpdateStatus::MissingMarker};
    }
    if (addonData.size() < 4)
    {
        return {ClientUpdateStatus::CompressedFormat};
    }
    // Preserve the existing reader's inflated limit; bound input before zlib too.
    constexpr size_t MAX_ADDON_BYTES = 0xFFFFF;
    if (addonData.size() - 4 > MAX_ADDON_BYTES)
    {
        return {ClientUpdateStatus::Size};
    }
    const uint32 size = uint32(addonData[0]) | (uint32(addonData[1]) << 8) |
        (uint32(addonData[2]) << 16) | (uint32(addonData[3]) << 24);
    if (size == 0 && addonData.size() == 4)
    {
        return {ClientUpdateStatus::MissingMarker};
    }
    if (size < 8 || size > MAX_ADDON_BYTES)
    {
        return {ClientUpdateStatus::Size};
    }
    ByteBuffer decoded;
    decoded.resize(size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(addonData.data() + 4);
    stream.avail_in = uInt(addonData.size() - 4);
    stream.next_out = const_cast<Bytef*>(decoded.contents());
    stream.avail_out = size;
    if (inflateInit(&stream) != Z_OK)
    {
        return {ClientUpdateStatus::CompressedFormat};
    }
    const int inflated = inflate(&stream, Z_FINISH);
    const bool outputFull = stream.avail_out == 0;
    const bool sizeMatches = stream.total_out == size;
    const bool inputConsumed = stream.avail_in == 0;
    inflateEnd(&stream);
    if (inflated != Z_STREAM_END)
    {
        return {inflated == Z_BUF_ERROR && outputFull
            ? ClientUpdateStatus::Size : ClientUpdateStatus::CompressedFormat};
    }
    if (!sizeMatches)
    {
        return {ClientUpdateStatus::Size};
    }
    if (!inputConsumed)
    {
        return {ClientUpdateStatus::CompressedFormat};
    }

    const uint32 count = decoded.read<uint32>(0);
    // Count and final uint32 trailer; each record needs a NUL plus 9 fixed bytes.
    if (count > (size - 8) / 10)
    {
        return {ClientUpdateStatus::Count};
    }
    size_t pos = 4;
    ClientUpdateAdmission result{ClientUpdateStatus::RecordFormat, count};
    for (uint32 i = 0; i < count; ++i)
    {
        const char* start = reinterpret_cast<const char*>(decoded.contents() + pos);
        const char* end = static_cast<const char*>(
            std::memchr(start, 0, size - pos));
        if (!end)
        {
            return result;
        }
        const std::string_view name(start, size_t(end - start));
        pos += name.size() + 1;
        if (size - pos < 9)
        {
            return result;
        }
        // Unknown/disabled addons may be listed before the post-AUTH_OK bootstrap.
        // Count every reserved name, independent of enabled, CRC and unknown flags.
        if (name.substr(0, CLIENT_UPDATE_PREFIX.size()) == CLIENT_UPDATE_PREFIX)
        {
            ++result.markerCount;
            result.matched = result.matched || name == required;
        }
        pos += 9;
    }
    if (size - pos != 4)
    {
        result.status = ClientUpdateStatus::Trailer;
    }
    else if (result.markerCount == 0)
    {
        result.status = ClientUpdateStatus::MissingMarker;
    }
    else if (result.markerCount > 1)
    {
        result.status = ClientUpdateStatus::DuplicateMarker;
    }
    else
    {
        result.status = result.matched ? ClientUpdateStatus::Accepted : ClientUpdateStatus::WrongMarker;
    }
    return result;
}

warden::AdmissionData BuildWardenAdmissionData(uint32 build,
    std::string platform, std::string clientLocale, BigNumber& sessionKey)
{
    warden::AdmissionData admission;
    admission.build = build;
    admission.platform = std::move(platform);
    admission.clientLocale = std::move(clientLocale);

    // BigNumber owns this buffer and will replace it on the next serialization.
    // Transfer the bytes, cleanse in place, and neither retain nor free it.
    uint8* const serialized = sessionKey.AsByteArray(
        static_cast<int>(admission.sessionKey.size()));
    if (!serialized)
    {
        admission.Clear();
        return admission;
    }
    std::copy(serialized, serialized + admission.sessionKey.size(),
        admission.sessionKey.begin());
    OPENSSL_cleanse(serialized, admission.sessionKey.size());
    admission.available = true;
    return admission;
}

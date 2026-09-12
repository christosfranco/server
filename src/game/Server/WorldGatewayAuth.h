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

#ifndef MANGOS_WORLDGATEWAYAUTH_H
#define MANGOS_WORLDGATEWAYAUTH_H

#include "Auth/BigNumber.h"
#include "IWorldGateway.h"
#include "WardenProtocol.h"

#include <cstddef>
#include <string>
#include <vector>

/** Append-only account projection used by WorldGateway::LookupAccount. */
enum class WorldGatewayAccountField : std::size_t
{
    Id = 0,
    Security = 1,
    SessionKey = 2,
    LastIp = 3,
    Locked = 4,
    Expansion = 5,
    MuteTime = 6,
    DbcLocale = 7,
    ClientOS = 8,
    ClientLocale = 9,
    Count = 10
};

constexpr std::size_t WorldGatewayAccountFieldIndex(
    WorldGatewayAccountField field)
{
    return static_cast<std::size_t>(field);
}

bool IsSupportedAccountClientOS(const std::string& os);

proto::ConnectionProfile SelectConnectionProfile(uint32 build,
    bool ascensionStockAuthCompatibility);

// Pinned patch-B realm-1 UIParent lifecycle + CoA Collections tabs + TOC closure.
// :1 is explicit local client-UI trust policy, not a recovered vendor flag.
inline constexpr char ASCENSION_KNOWN_ADDONS_DEFAULT[] =
    "Ascension_Collections:1,AscensionUI:1,Ascension_TalentUI:1,Ascension_CoATalents:1,"
    "Ascension_Warmode:1,Ascension_NamePlates:1,Ascension_NewPlayerExperience:1,"
    "Ascension_MythicPlus:1,Ascension_Manastorm:1,Ascension_VanityCollection:1,"
    "Ascension_AppearanceUI:1";

/** Empty disables; invalid manifests throw std::invalid_argument, never truncate. */
std::shared_ptr<const WorldPacket> BuildAscensionKnownAddons(
    const std::string& manifest);

/** Nonempty managed addon name: bounded channel token and canonical sequence. */
bool IsValidClientUpdateMarker(const std::string& marker);

enum class ClientUpdateStatus
{
    Disabled,
    Accepted,
    InvalidRequirement,
    MissingMarker,
    WrongMarker,
    DuplicateMarker,
    CompressedFormat,
    Size,
    Count,
    RecordFormat,
    Trailer
};

struct ClientUpdateAdmission
{
    ClientUpdateStatus status;
    uint32 addonCount = 0;  ///< Only a count that passed the inflated-size bound.
    uint32 markerCount = 0;
    bool matched = false;

    explicit operator bool() const
    {
        return status == ClientUpdateStatus::Disabled || status == ClientUpdateStatus::Accepted;
    }

    /** Fixed codes and bounded counts only; no input strings retained or logged. */
    std::string Diagnostic() const;
};

/** Empty requirement preserves legacy parsing; otherwise validate the full block. */
ClientUpdateAdmission CheckRequiredClientUpdate(const std::vector<uint8>& addonData,
    const std::string& required);

/** Copies one fixed-width Warden key while retaining the BigNumber for HMAC. */
warden::AdmissionData BuildWardenAdmissionData(uint32 build,
    std::string platform, std::string clientLocale, BigNumber& sessionKey);

#endif

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
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include <cstdint>
#include <array>
#include <chrono>
#include <utility>
#include <vector>
#include <memory>
#include <mutex>
#include "ClientConnection.h"

#include "Opcodes.h"

#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "Log/Log.h"
#include "Utilities/ByteBuffer.h"

#include <cstring>

namespace proto
{
    namespace
    {
        /// Number of bytes in the client's SHA-1 login proof.
        const size_t AUTH_DIGEST_SIZE = 20;

        /**
         * @brief Draw the server authentication nonce from the cryptographic RNG.
         *
         * The old code used the general-purpose PRNG for this. The value is hashed
         * into the client's proof, so a predictable seed narrows the search space
         * for anyone replaying a captured login.
         */
        uint32 MakeAuthSeed()
        {
            BigNumber seed;
            seed.SetRand(32);
            return seed.AsDword();
        }
    }

    std::atomic<uint32> ClientConnection::s_openConnections{0};

    ClientConnection::ClientConnection(IWorldGateway& gateway)
        : m_gateway(gateway),
          m_codec(),
          m_seed(MakeAuthSeed()),
          m_session(INVALID_SESSION_ID),
          m_traceSession(INVALID_SESSION_ID),
          m_closed(false)
    {
        s_openConnections.fetch_add(1, std::memory_order_relaxed);
    }

    ClientConnection::~ClientConnection()
    {
        s_openConnections.fetch_sub(1, std::memory_order_relaxed);
    }

    std::vector<uint8_t> ClientConnection::onConnect()
    {
        // SMSG_AUTH_CHALLENGE: one-slot shuffle marker, the server nonce, and two
        // 16-byte seeds the 3.3.5a client folds into its own crypt setup.
        WorldPacket packet(SMSG_AUTH_CHALLENGE, 40);
        packet << uint32(1);
        packet << m_seed;

        BigNumber seed1;
        seed1.SetRand(16 * 8);
        packet.append(seed1.AsByteArray(16), 16);

        BigNumber seed2;
        seed2.SetRand(16 * 8);
        packet.append(seed2.AsByteArray(16), 16);

        // The crypt is not armed yet, so this goes out in clear text -- which is
        // exactly right: the client cannot key its cipher until it has this packet.
        m_gateway.TracePacket(INVALID_SESSION_ID, packet, false);
        return PacketCodec::Encode(packet, PacketCodec::HeaderEncryptor());
    }

    std::vector<uint8_t> ClientConnection::onData(const uint8_t* data, size_t len)
    {
        if (closed())
        {
            return std::vector<uint8_t>();
        }

        // A short read anywhere below unwinds onto a network worker thread, where
        // nothing else would catch it and the process would abort. Dropping the
        // peer has to be the worst a malformed packet can do.
        try
        {
            // Auth must install the decryptor before a coalesced next header is
            // decoded. A rejected proof must not decode or dispatch its suffix.
            const DecodeStatus status = m_codec.Feed(data, len,
                [this](WorldPacket&& packet)
                {
                    m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                                          packet, true);
                    if (!HandlePacket(std::move(packet)))
                    {
                        Close();
                        return false;
                    }
                    return !closed();
                });
            if (status == DecodeStatus::Malformed)
            {
                sLog.outError("proto: malformed packet framing from %s, dropping",
                              m_address.c_str());
                Close();
            }
        }
        catch (ByteBufferException&)
        {
            sLog.outError("proto: short read handling packet from %s, dropping",
                          m_address.c_str());
            Close();
        }

        // Everything this class sends goes through SendPacket() (and therefore the
        // transport's Sender), because a reply may be produced on the world thread
        // long after this call returned. Nothing is ever returned inline.
        return std::vector<uint8_t>();
    }

    bool ClientConnection::HandlePacket(WorldPacket&& packet)
    {
        const uint16 opcode = uint16(packet.GetOpcode());

        switch (opcode)
        {
            case CMSG_AUTH_SESSION:
                if (m_session != INVALID_SESSION_ID)
                {
                    sLog.outError("proto: repeated CMSG_AUTH_SESSION from %s",
                                  m_address.c_str());
                    return false;
                }
                return HandleAuthSession(packet);

            default:
                break;
        }

        if (m_session == INVALID_SESSION_ID)
        {
            sLog.outError("proto: opcode %u from unauthenticated peer %s",
                          opcode, m_address.c_str());
            return false;
        }

        TraceKeepalive(packet, true);
        m_gateway.Deliver(m_session, std::move(packet));
        return true;
    }

    /// Wire width of the SRP6 session key. The value's own byte count is
    /// not the same thing and must never be used in its place.
    static const int SRP_SESSION_KEY_WIDTH = 40;

    bool ClientConnection::HandleAuthSession(WorldPacket& packet)
    {
        AuthRequest request;
        request.peerAddress = m_address;

        try
        {
            packet >> request.build;
            packet.read_skip<uint32>();
            packet >> request.account;
            packet.read_skip<uint32>();
            packet >> request.clientSeed;
            packet.read_skip<uint32>();
            packet.read_skip<uint32>();
            packet.read_skip<uint32>();
            packet.read_skip<uint64>();
            packet.read(request.digest, AUTH_DIGEST_SIZE);
        }
        catch (ByteBufferException&)
        {
            sLog.outError("proto: truncated CMSG_AUTH_SESSION from %s",
                          m_address.c_str());
            return false;
        }

        // Whatever is left is the addon block. It is opaque here -- the world
        // parses it, because the reply depends on the addon registry it owns.
        if (packet.rpos() < packet.size())
        {
            request.addonData.assign(packet.contents() + packet.rpos(),
                                     packet.contents() + packet.size());
        }

        // Policy and persistence: account row, bans, IP lock, allowed build,
        // security level. None of it belongs on this side of the seam.
        const AuthLookup lookup = m_gateway.LookupAccount(request);

        if (lookup.status != AuthStatus::Ok)
        {
            sLog.outError("world_auth stage=lookup reason=lookup_rejected status=%u",
                          uint32(lookup.status));
            SendAuthStatus(lookup.status);
            return false;
        }

        // Cryptography stays on this side. The client proves it holds the same
        // session key realmd handed it, over both halves of the nonce.
        BigNumber sessionKey = lookup.sessionKey;
        const uint8 zero[4] = { 0, 0, 0, 0 };
        const uint32 clientSeed = request.clientSeed;
        const uint32 serverSeed = m_seed;

        Sha1Hash sha;
        sha.UpdateData(request.account);
        sha.UpdateData(zero, 4);
        sha.UpdateData(reinterpret_cast<const uint8*>(&clientSeed), 4);
        sha.UpdateData(reinterpret_cast<const uint8*>(&serverSeed), 4);
        // FORTY BYTES, not sessionKey.GetNumBytes(). The client hashes the
        // full-width key; a BigNumber whose top byte is zero is 39 bytes wide
        // and produces a digest that cannot match, refusing about one login in
        // 256 as a bad proof. See the note in AuthCrypt::Init.
        sha.UpdateData(sessionKey.AsByteArray(SRP_SESSION_KEY_WIDTH),
                       SRP_SESSION_KEY_WIDTH);
        sha.Finalize();

        if (std::memcmp(sha.GetDigest(), request.digest, AUTH_DIGEST_SIZE) != 0)
        {
            sLog.outError("world_auth stage=proof reason=proof_failed");
            SendAuthStatus(AuthStatus::Failed);
            return false;
        }

        // Apply the cipher policy BEFORE the world is told: the world answers with
        // SMSG_AUTH_RESPONSE (or a queue position) the moment it accepts the
        // session, and that reply must already use the selected framing.
        // The gateway selects the profile from operator policy, not build alone.
        // No profile takes effect until the proof above has passed.
        {
            std::lock_guard<std::mutex> lock(m_cryptSendLock);
            if (lookup.profile != ConnectionProfile::AscensionClearHeaders)
            {
                m_crypt.Init(&sessionKey);
                m_codec.SetHeaderDecryptor(
                    [this](uint8* header, size_t len) { m_crypt.DecryptRecv(header, len); });
            }
            m_coaBootstrapPending =
                lookup.profile == ConnectionProfile::AscensionStockAuthCoA;
            if (m_coaBootstrapPending)
            {
                m_knownAddons = lookup.knownAddons;
            }
        }

        // Hand the world a share of our own lifetime. net::ISession is held by
        // shared_ptr from the moment the transport accepts, so this is well
        // formed here, and it is what allows a WorldSession to outlive its socket
        // long enough to unwind the player from the map.
        std::shared_ptr<IClientLink> link =
            std::static_pointer_cast<ClientConnection>(shared_from_this());

        const AttachResult attached = m_gateway.Attach(request, link, lookup.context);
        if (attached.session == INVALID_SESSION_ID)
        {
            SendAuthStatus(attached.failure);
            return false;
        }

        m_session = attached.session;
        m_traceSession.store(attached.session, std::memory_order_relaxed);

        DEBUG_LOG("proto: account '%s' authenticated from %s session=%u",
                  request.account.c_str(), m_address.c_str(), attached.session);
        return true;
    }

    void ClientConnection::SendAuthStatus(AuthStatus status)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(status);
        SendPacket(packet);
    }

    void ClientConnection::SendPacket(const WorldPacket& packet)
    {
        if (m_closed.load(std::memory_order_acquire) || !m_sender)
        {
            return;
        }

        // Keep encryption AND transport enqueue in the same order across threads.
        // AUTH_OK and its optional bootstrap must precede subsequent replies.
        std::lock_guard<std::mutex> lock(m_cryptSendLock);
        const auto encryptHeader = [this](uint8* header, size_t len)
        {
            if (m_crypt.IsInitialized())
            {
                m_crypt.EncryptSend(header, len);
            }
        };
        m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed), packet, false);
        const std::vector<uint8_t> wire = PacketCodec::Encode(packet, encryptHeader);
        m_sender(wire.data(), wire.size());
        TraceKeepalive(packet, false);

        if (m_coaBootstrapPending && packet.GetOpcode() == SMSG_AUTH_RESPONSE
            && !packet.empty() && packet.contents()[0] == uint8(AuthStatus::Ok))
        {
            m_coaBootstrapPending = false;
            // Exact minimal payload from coa_realm_info_frame: realm 1,
            // expansion/unknowns zero, CoA flag at byte 42, empty A/B data paths.
            // This outbound-only uint16 opcode needs no inbound table entry.
            constexpr uint16 SMSG_COA_REALM_INFO = 0x9BC;
            std::array<uint8, 47> payload{};
            payload[0] = 1;
            payload[36] = 1; // Ordinary Live catalog selector, separate from CoA.
            payload[42] = 1;
            WorldPacket bootstrap(SMSG_COA_REALM_INFO, payload.size());
            bootstrap.append(payload.data(), payload.size());
            m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                                  bootstrap, false);
            const std::vector<uint8_t> bootstrapWire =
                PacketCodec::Encode(bootstrap, encryptHeader);
            m_sender(bootstrapWire.data(), bootstrapWire.size());
            if (m_knownAddons)
            {
                m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                                      *m_knownAddons, false);
                const auto addonWire = PacketCodec::Encode(*m_knownAddons, encryptHeader);
                m_sender(addonWire.data(), addonWire.size());
                m_knownAddons.reset();
            }
        }
    }

    void ClientConnection::TraceKeepalive(const WorldPacket& packet, bool incoming)
    {
        if (!sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG)
            || sLog.HasLogFilter(LOG_FILTER_PLAYER_STATS)
            || packet.GetOpcode() != (incoming ? CMSG_PING : SMSG_PONG)
            || packet.size() != (incoming ? 8u : 4u))
        {
            return;
        }

        const SessionId session = m_traceSession.load(std::memory_order_relaxed);
        uint32& count = incoming ? m_pingTraceCount : m_pongTraceCount;
        // Bound even privileged/disabled-flood-policy sessions; never affect traffic.
        if (session == INVALID_SESSION_ID || count >= 64)
        {
            return;
        }
        ++count;
        const uint64 now = uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        // Sender is void and may discard during teardown. This is a handoff,
        // NOT a socket-write acknowledgement or evidence of client processing.
        DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_STATS,
            "proto: keepalive %s session=%u sequence=%u latency_ms=%u unix_ms=" UI64FMTD " encrypted=%u",
            incoming ? "ping_received" : "pong_handoff", session,
            packet.read<uint32>(0), incoming ? packet.read<uint32>(4) : 0,
            now, uint32(m_crypt.IsInitialized()));
    }

    void ClientConnection::Close()
    {
        m_closed.store(true, std::memory_order_release);
        if (m_closer)
        {
            m_closer();
        }
    }

    void ClientConnection::onClose()
    {
        m_closed.store(true, std::memory_order_release);

        if (m_session != INVALID_SESSION_ID)
        {
            m_gateway.Detach(m_session);
            m_session = INVALID_SESSION_ID;
        }
        m_traceSession.store(INVALID_SESSION_ID, std::memory_order_relaxed);
    }
}

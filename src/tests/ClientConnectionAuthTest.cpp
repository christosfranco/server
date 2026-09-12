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

#include "TestHarness.h"

#include "Auth/AuthCrypt.h"
#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "ClientConnection.h"
#include "Log/Log.h"
#include "Opcodes.h"
#include "PacketCodec.h"
#include "WorldGatewayAuth.h"

#include <zlib.h>

#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    const std::string ACCOUNT = "CLIENTCONNECTIONTEST";
    const uint32 CLIENT_SEED = 0xA1B2C3D4;
    const int SESSION_KEY_WIDTH = 40;
    // Explicit little-endian SRP wire key: 39 significant bytes, padded to 40.
    const std::array<uint8, SESSION_KEY_WIDTH> SESSION_KEY_BYTES =
    {{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 0
    }};

    BigNumber MakeSessionKey()
    {
        BigNumber key;
        key.SetBinary(SESSION_KEY_BYTES.data(), int(SESSION_KEY_BYTES.size()));
        return key;
    }

    uint32 ReadLittleEndian32(const std::vector<uint8>& bytes, size_t offset)
    {
        return uint32(bytes[offset])
             | (uint32(bytes[offset + 1]) << 8)
             | (uint32(bytes[offset + 2]) << 16)
             | (uint32(bytes[offset + 3]) << 24);
    }

    std::array<uint8, 20> MakeLoginProof(uint32 serverSeed)
    {
        const uint8 zero[4] = { 0, 0, 0, 0 };

        Sha1Hash sha;
        sha.UpdateData(ACCOUNT);
        sha.UpdateData(zero, sizeof(zero));
        sha.UpdateData(reinterpret_cast<const uint8*>(&CLIENT_SEED),
                       sizeof(CLIENT_SEED));
        sha.UpdateData(reinterpret_cast<const uint8*>(&serverSeed),
                       sizeof(serverSeed));
        sha.UpdateData(SESSION_KEY_BYTES.data(), SESSION_KEY_BYTES.size());
        sha.Finalize();

        std::array<uint8, 20> proof{};
        std::memcpy(proof.data(), sha.GetDigest(), proof.size());
        return proof;
    }

    std::vector<uint8> ClientFrame(const WorldPacket& packet)
    {
        const uint32 size = uint32(packet.size()) + 4;
        const uint32 opcode = packet.GetOpcode();
        std::vector<uint8> wire =
        {
            uint8((size >> 8) & 0xFF),
            uint8(size & 0xFF),
            uint8(opcode & 0xFF),
            uint8((opcode >> 8) & 0xFF),
            uint8((opcode >> 16) & 0xFF),
            uint8((opcode >> 24) & 0xFF)
        };
        if (!packet.empty())
        {
            wire.insert(wire.end(), packet.contents(),
                        packet.contents() + packet.size());
        }
        return wire;
    }

    std::vector<uint8> AuthFrame(uint32 build, uint32 serverSeed, bool validProof,
        const std::vector<uint8>& addonData = {})
    {
        std::array<uint8, 20> proof = MakeLoginProof(serverSeed);
        if (!validProof)
        {
            proof[0] ^= 0xFF;
        }

        WorldPacket packet(CMSG_AUTH_SESSION, 80);
        packet << build;
        packet << uint32(0);
        packet << ACCOUNT;
        packet << uint32(0);
        packet << CLIENT_SEED;
        packet << uint32(0);
        packet << uint32(0);
        packet << uint32(0);
        packet << uint64(0);
        packet.append(proof.data(), proof.size());
        if (!addonData.empty())
        {
            packet.append(addonData.data(), addonData.size());
        }
        return ClientFrame(packet);
    }

    const std::string UPDATE_MARKER = "AscenderUpdate_local_update_4";

    ByteBuffer AddonInfo(std::initializer_list<std::pair<std::string, uint8>> addons)
    {
        ByteBuffer data;
        data << uint32(addons.size());
        for (const auto& addon : addons)
        {
            data << addon.first << addon.second << uint32(0x4c1c776d) << uint32(0);
        }
        data << uint32(0);
        return data;
    }

    std::vector<uint8> CompressAddonInfo(const ByteBuffer& data)
    {
        uLongf length = compressBound(data.size());
        std::vector<uint8> block(4 + length);
        const uint32 size = uint32(data.size());
        for (size_t i = 0; i < 4; ++i)
        {
            block[i] = uint8(size >> (8 * i));
        }
        if (compress2(block.data() + 4, &length, data.contents(),
            data.size(), Z_BEST_COMPRESSION) != Z_OK)
        {
            throw std::runtime_error("synthetic addon compression failed");
        }
        block.resize(4 + length);
        return block;
    }

    WorldPacket AuthResponse(proto::AuthStatus status)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(status);
        return packet;
    }

    class FakeGateway final : public proto::IWorldGateway
    {
        public:

            FakeGateway(const BigNumber& sessionKey, bool stockAuthCompatibility)
                : m_sessionKey(sessionKey),
                  m_stockAuthCompatibility(stockAuthCompatibility)
            {
            }

            proto::AuthLookup LookupAccount(
                const proto::AuthRequest& request) override
            {
                ++m_lookupCount;
                m_build = request.build;
                proto::AuthLookup lookup;
                lookup.status = m_lookupStatus;
                lookup.sessionKey = m_sessionKey;
                lookup.profile = SelectConnectionProfile(request.build,
                    m_stockAuthCompatibility);
                lookup.knownAddons = m_knownAddons;
                return lookup;
            }

            proto::AttachResult Attach(
                const proto::AuthRequest& request,
                const std::shared_ptr<proto::IClientLink>& link,
                const std::shared_ptr<proto::AuthContext>&) override
            {
                ++m_attachCount;
                if (m_rejectAttach)
                {
                    return {};
                }
                if (!CheckRequiredClientUpdate(request.addonData, m_requiredClientUpdate))
                {
                    return {proto::INVALID_SESSION_ID, proto::AuthStatus::Failed};
                }
                ++m_admissionCount;
                link->SendPacket(AuthResponse(m_attachStatus));
                return {42};
            }

            void TracePacket(proto::SessionId, const WorldPacket&, bool) override
            {
            }

            void Deliver(proto::SessionId session, WorldPacket&& packet) override
            {
                ++m_deliverCount;
                m_deliveredSession = session;
                m_deliveredOpcode = packet.GetOpcode();
                m_deliveredPayload.clear();
                if (!packet.empty())
                {
                    m_deliveredPayload.assign(packet.contents(),
                        packet.contents() + packet.size());
                }
            }

            void Detach(proto::SessionId) override
            {
            }

            int LookupCount() const { return m_lookupCount; }
            int AttachCount() const { return m_attachCount; }
            int AdmissionCount() const { return m_admissionCount; }
            int DeliverCount() const { return m_deliverCount; }
            uint32 Build() const { return m_build; }
            proto::SessionId DeliveredSession() const { return m_deliveredSession; }
            uint16 DeliveredOpcode() const { return m_deliveredOpcode; }
            const std::vector<uint8>& DeliveredPayload() const { return m_deliveredPayload; }

            proto::AuthStatus m_lookupStatus = proto::AuthStatus::Ok;
            proto::AuthStatus m_attachStatus = proto::AuthStatus::Ok;
            bool m_rejectAttach = false;
            std::string m_requiredClientUpdate;
            std::shared_ptr<const WorldPacket> m_knownAddons;

        private:

            BigNumber m_sessionKey;
            bool m_stockAuthCompatibility;
            int m_lookupCount = 0;
            int m_attachCount = 0;
            int m_admissionCount = 0;
            int m_deliverCount = 0;
            uint32 m_build = 0;
            proto::SessionId m_deliveredSession = proto::INVALID_SESSION_ID;
            uint16 m_deliveredOpcode = 0;
            std::vector<uint8> m_deliveredPayload;
    };

    class TestPeer
    {
        public:

            explicit TestPeer(bool stockAuthCompatibility = false)
                : m_sessionKey(MakeSessionKey()),
                  m_gateway(m_sessionKey, stockAuthCompatibility),
                  m_connection(std::make_shared<proto::ClientConnection>(m_gateway))
            {
                m_connection->setPeerAddress("127.0.0.1");
                m_connection->setSender(
                    [this](const uint8* data, size_t length)
                    {
                        m_sent.emplace_back(data, data + length);
                    });
                m_connection->setCloser([this]() { ++m_closeCount; });
            }

            bool Authenticate(uint32 build, bool validProof = true,
                const std::vector<uint8>& addonData = {})
            {
                const std::vector<uint8> challenge = m_connection->onConnect();
                if (challenge.size() < 12)
                {
                    return false;
                }
                const uint32 serverSeed = ReadLittleEndian32(challenge, 8);
                const std::vector<uint8> auth =
                    AuthFrame(build, serverSeed, validProof, addonData);
                m_connection->onData(auth.data(), auth.size());
                return true;
            }

            BigNumber& SessionKey() { return m_sessionKey; }
            FakeGateway& Gateway() { return m_gateway; }
            proto::ClientConnection& Connection() { return *m_connection; }
            const std::vector<std::vector<uint8>>& Sent() const { return m_sent; }
            int CloseCount() const { return m_closeCount; }

        private:

            BigNumber m_sessionKey;
            FakeGateway m_gateway;
            std::shared_ptr<proto::ClientConnection> m_connection;
            std::vector<std::vector<uint8>> m_sent;
            int m_closeCount = 0;
    };

    class KeepaliveLogScope
    {
        public:
            explicit KeepaliveLogScope(bool enabled)
                : m_level(sLog.GetLogLevel()),
                  m_filtered(sLog.HasLogFilter(LOG_FILTER_PLAYER_STATS))
            {
                char debug[] = "3";
                sLog.SetLogLevel(debug);
                sLog.SetLogFilter(LOG_FILTER_PLAYER_STATS, !enabled);
            }

            ~KeepaliveLogScope()
            {
                std::string level = std::to_string(m_level);
                sLog.SetLogLevel(level.data());
                sLog.SetLogFilter(LOG_FILTER_PLAYER_STATS, m_filtered);
            }

        private:
            uint32 m_level;
            bool m_filtered;
    };

    void ExerciseKeepalive(bool enabled)
    {
        KeepaliveLogScope logging(enabled);
        TestPeer peer(true);
        REQUIRE(peer.Authenticate(12344));
        AuthCrypt clientCrypt;
        clientCrypt.Init(&peer.SessionKey());
        REQUIRE(peer.Sent().size() == 2);
        for (const auto& sent : peer.Sent())
        {
            auto header = sent;
            clientCrypt.EncryptSend(header.data(), 4);
        }
        for (uint32 sequence = 0; sequence < 1000; ++sequence)
        {
            WorldPacket ping(CMSG_PING, 8);
            ping << sequence << uint32(23);
            auto wire = ClientFrame(ping);
            clientCrypt.DecryptRecv(wire.data(), proto::CLIENT_HEADER_SIZE);
            peer.Connection().onData(wire.data(), wire.size());
            CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_PING));
            CHECK_EQ(ReadLittleEndian32(peer.Gateway().DeliveredPayload(), 0), sequence);
            CHECK_EQ(ReadLittleEndian32(peer.Gateway().DeliveredPayload(), 4), 23u);

            WorldPacket pong(SMSG_PONG, 4);
            pong << sequence;
            peer.Connection().SendPacket(pong);
            auto reply = peer.Sent().back();
            clientCrypt.EncryptSend(reply.data(), 4);
            CHECK(reply == proto::PacketCodec::Encode(pong, {}));
        }
        CHECK_EQ(peer.Gateway().DeliverCount(), 1000);
        CHECK_EQ(peer.Sent().size(), size_t(1002));
        CHECK(!peer.Connection().closed());
    }
}

TEST(ClientConnection_keepalive_quiet)
{
    CHECK(sLog.HasLogFilter(LOG_FILTER_PLAYER_STATS));
    ExerciseKeepalive(false);
}

TEST(ClientConnection_keepalive_enabled)
{
    ExerciseKeepalive(true);
}

TEST(ClientConnection_keepalive_closed)
{
    KeepaliveLogScope logging(true);
    TestPeer peer(true);
    REQUIRE(peer.Authenticate(12344));
    AuthCrypt crypt;
    crypt.Init(&peer.SessionKey());
    WorldPacket ping(CMSG_PING, 8);
    ping << uint32(7) << uint32(0);
    auto wire = ClientFrame(ping);
    crypt.DecryptRecv(wire.data(), proto::CLIENT_HEADER_SIZE);
    peer.Connection().onData(wire.data(), wire.size());
    peer.Connection().Close();
    WorldPacket pong(SMSG_PONG, 4);
    pong << uint32(7);
    peer.Connection().SendPacket(pong);
    CHECK_EQ(peer.Sent().size(), size_t(2));
}

TEST(ClientConnection_keepalive_malformed)
{
    KeepaliveLogScope logging(true);
    TestPeer peer(true);
    REQUIRE(peer.Authenticate(12344));
    AuthCrypt crypt;
    crypt.Init(&peer.SessionKey());
    for (size_t size : {size_t(0), size_t(7), size_t(9)})
    {
        WorldPacket ping(CMSG_PING, size);
        ping.resize(size);
        auto wire = ClientFrame(ping);
        crypt.DecryptRecv(wire.data(), proto::CLIENT_HEADER_SIZE);
        peer.Connection().onData(wire.data(), wire.size());
        WorldPacket pong(SMSG_PONG, size);
        pong.resize(size);
        peer.Connection().SendPacket(pong);
    }
    CHECK_EQ(peer.Gateway().DeliverCount(), 3);
    CHECK_EQ(peer.Sent().size(), size_t(5));
}

TEST(ClientConnection_keepalive_no_sender)
{
    KeepaliveLogScope logging(true);
    TestPeer peer(true);
    REQUIRE(peer.Authenticate(12344));
    peer.Connection().setSender({});
    WorldPacket pong(SMSG_PONG, 4);
    pong << uint32(7);
    peer.Connection().SendPacket(pong);
    CHECK_EQ(peer.Sent().size(), size_t(2));
}

TEST(ClientConnection_keepalive_low_level)
{
    KeepaliveLogScope logging(true);
    char quiet[] = "0";
    sLog.SetLogLevel(quiet);
    TestPeer peer(true);
    REQUIRE(peer.Authenticate(12344));
    WorldPacket pong(SMSG_PONG, 4);
    pong << uint32(7);
    peer.Connection().SendPacket(pong);
    CHECK_EQ(peer.Sent().size(), size_t(3));
}

TEST(ClientConnection_build_12344_authenticates_with_clear_world_headers)
{
    TestPeer peer;
    REQUIRE(peer.Authenticate(12344));
    CHECK_EQ(peer.Gateway().LookupCount(), 1);
    CHECK_EQ(peer.Gateway().AttachCount(), 1);
    CHECK_EQ(peer.Gateway().Build(), 12344u);
    CHECK(!peer.Connection().closed());

    const WorldPacket response = AuthResponse(proto::AuthStatus::Ok);
    const std::vector<uint8> clearResponse = proto::PacketCodec::Encode(
        response, proto::PacketCodec::HeaderEncryptor());

    REQUIRE(peer.Sent().size() == 1);
    CHECK(peer.Sent()[0] == clearResponse);

    const WorldPacket ping(CMSG_PING, 0);
    const std::vector<uint8> clearPing = ClientFrame(ping);
    peer.Connection().onData(clearPing.data(), clearPing.size());

    CHECK_EQ(peer.Gateway().DeliverCount(), 1);
    CHECK_EQ(peer.Gateway().DeliveredSession(), 42u);
    CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_PING));
    CHECK(!peer.Connection().closed());
}

TEST(ClientConnection_build_12340_authenticates_with_rc4_world_headers)
{
    for (bool optIn : {false, true})
    {
        TestPeer peer(optIn);
        REQUIRE(peer.Authenticate(12340));
        CHECK_EQ(peer.Gateway().LookupCount(), 1);
        CHECK_EQ(peer.Gateway().AttachCount(), 1);
        CHECK_EQ(peer.Gateway().Build(), 12340u);
        CHECK(!peer.Connection().closed());

        AuthCrypt clientCrypt;
        clientCrypt.Init(&peer.SessionKey());

        const WorldPacket response = AuthResponse(proto::AuthStatus::Ok);
        const std::vector<uint8> clearResponse = proto::PacketCodec::Encode(
            response, proto::PacketCodec::HeaderEncryptor());

        REQUIRE(peer.Sent().size() == 1);
        std::vector<uint8> decryptedResponse = peer.Sent()[0];
        CHECK(decryptedResponse != clearResponse);
        const size_t responseHeaderSize = clearResponse.size() - response.size();
        clientCrypt.EncryptSend(decryptedResponse.data(), responseHeaderSize);
        CHECK(decryptedResponse == clearResponse);

        const WorldPacket ping(CMSG_PING, 0);
        const std::vector<uint8> clearPing = ClientFrame(ping);
        std::vector<uint8> encryptedPing = clearPing;
        clientCrypt.DecryptRecv(encryptedPing.data(), proto::CLIENT_HEADER_SIZE);
        CHECK(encryptedPing != clearPing);
        peer.Connection().onData(encryptedPing.data(), encryptedPing.size());

        CHECK_EQ(peer.Gateway().DeliverCount(), 1);
        CHECK_EQ(peer.Gateway().DeliveredSession(), 42u);
        CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_PING));
        CHECK(!peer.Connection().closed());
    }
}

TEST(ClientConnection_all_profiles_reject_a_bad_login_proof)
{
    for (uint32 build : {12340u, 12344u})
    {
        for (bool optIn : {false, true})
        {
            TestPeer peer(optIn);
            REQUIRE(peer.Authenticate(build, false));

            CHECK_EQ(peer.Gateway().LookupCount(), 1);
            CHECK_EQ(peer.Gateway().AttachCount(), 0);
            CHECK_EQ(peer.Gateway().DeliverCount(), 0);
            CHECK(peer.Connection().closed());
            CHECK_EQ(peer.CloseCount(), 1);

            const WorldPacket failure = AuthResponse(proto::AuthStatus::Failed);
            const std::vector<uint8> expected = proto::PacketCodec::Encode(
                failure, proto::PacketCodec::HeaderEncryptor());
            REQUIRE(peer.Sent().size() == 1);
            CHECK(peer.Sent()[0] == expected);
        }
    }
}

TEST(ClientConnection_stock_auth_candidate_encrypts_bootstrap_once_before_characters)
{
    TestPeer peer(true);
    peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
    REQUIRE(peer.Authenticate(12344, true,
        CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 0}}))));
    CHECK_EQ(peer.Gateway().AttachCount(), 1);
    REQUIRE(peer.Sent().size() == 2);

    AuthCrypt clientCrypt;
    clientCrypt.Init(&peer.SessionKey());
    std::vector<uint8> response = peer.Sent()[0];
    clientCrypt.EncryptSend(response.data(), 4);
    CHECK(response == proto::PacketCodec::Encode(AuthResponse(proto::AuthStatus::Ok), {}));

    // Golden frame from the existing probe, not the production serializer.
    std::vector<uint8> expectedBootstrap(51, 0);
    expectedBootstrap[1] = 49;
    expectedBootstrap[2] = 0xBC;
    expectedBootstrap[3] = 0x09;
    expectedBootstrap[4] = 1;
    expectedBootstrap[40] = 1; // Live selector at payload byte 36.
    expectedBootstrap[46] = 1;
    CHECK(0x9BC >= NUM_MSG_TYPES);
    std::vector<uint8> bootstrap = peer.Sent()[1];
    CHECK(bootstrap != expectedBootstrap);
    clientCrypt.EncryptSend(bootstrap.data(), 4);
    CHECK(bootstrap == expectedBootstrap);

    WorldPacket characters(SMSG_CHAR_ENUM, 1);
    characters << uint8(0);
    peer.Connection().SendPacket(characters);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::Ok));
    REQUIRE(peer.Sent().size() == 4);
    std::vector<uint8> characterResponse = peer.Sent()[2];
    clientCrypt.EncryptSend(characterResponse.data(), 4);
    CHECK(characterResponse == proto::PacketCodec::Encode(characters, {}));
    response = peer.Sent()[3];
    clientCrypt.EncryptSend(response.data(), 4);
    CHECK(response == proto::PacketCodec::Encode(AuthResponse(proto::AuthStatus::Ok), {}));

    // Both cipher directions must stay aligned, including fragmented headers.
    std::vector<uint8> request = ClientFrame(WorldPacket(CMSG_CHAR_ENUM, 0));
    clientCrypt.DecryptRecv(request.data(), proto::CLIENT_HEADER_SIZE);
    peer.Connection().onData(request.data(), 2);
    CHECK_EQ(peer.Gateway().DeliverCount(), 0);
    peer.Connection().onData(request.data() + 2, request.size() - 2);
    CHECK_EQ(peer.Gateway().DeliverCount(), 1);
    CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_CHAR_ENUM));
    CHECK(!peer.Connection().closed());

    request = ClientFrame(WorldPacket(CMSG_AUTH_SESSION, 0));
    clientCrypt.DecryptRecv(request.data(), proto::CLIENT_HEADER_SIZE);
    peer.Connection().onData(request.data(), request.size());
    CHECK(peer.Connection().closed());
    CHECK_EQ(peer.Gateway().AttachCount(), 1);
    CHECK_EQ(peer.Sent().size(), size_t(4));
}

TEST(ClientConnection_known_addons_after_coa_before_characters_once_and_snapshot)
{
    TestPeer peer(true);
    peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
    peer.Gateway().m_knownAddons = BuildAscensionKnownAddons("A:0,B:1");
    peer.Gateway().m_attachStatus = proto::AuthStatus::WaitQueue;
    REQUIRE(peer.Authenticate(12344, true,
        CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 1}}))));
    REQUIRE(peer.Sent().size() == 1);
    // Changing world policy while queued must not change this login's snapshot.
    peer.Gateway().m_knownAddons = BuildAscensionKnownAddons("Other:1");
    peer.Connection().SendPacket(WorldPacket(SMSG_AUTH_RESPONSE, 0));
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::WaitQueue));
    REQUIRE(peer.Sent().size() == 3);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::Ok));
    REQUIRE(peer.Sent().size() == 6);
    WorldPacket characters(SMSG_CHAR_ENUM, 1);
    characters << uint8(0);
    peer.Connection().SendPacket(characters);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::Ok));
    REQUIRE(peer.Sent().size() == 8);
    AuthCrypt crypt;
    crypt.Init(&peer.SessionKey());
    for (size_t i = 0; i < peer.Sent().size(); ++i)
    {
        auto frame = peer.Sent()[i];
        crypt.EncryptSend(frame.data(), 4);
        const uint16 opcode = uint16(frame[2]) | (uint16(frame[3]) << 8);
        CHECK_EQ(opcode, uint16(i == 4 ? 0x9BC : i == 5 ? 0x94E
            : i == 6 ? SMSG_CHAR_ENUM : SMSG_AUTH_RESPONSE));
        if (i == 5)
        {
            // Independent full-frame golden: BE length, LE opcode/count, cstring/u8.
            CHECK_BYTES(frame.data(), frame.size(),
                {0, 12, 0x4E, 9, 2, 0, 0, 0, 'A', 0, 0, 'B', 0, 1});
        }
        if (i == 6)
        {
            CHECK(frame == proto::PacketCodec::Encode(characters, {}));
        }
    }
    auto request = ClientFrame(WorldPacket(CMSG_CHAR_ENUM, 0));
    crypt.DecryptRecv(request.data(), proto::CLIENT_HEADER_SIZE);
    peer.Connection().onData(request.data(), 2);
    peer.Connection().onData(request.data() + 2, request.size() - 2);
    CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_CHAR_ENUM));
    CHECK(!peer.Connection().closed());
}

TEST(ClientConnection_known_addons_ignored_outside_opt_in_and_on_failures)
{
    for (uint32 build : {12340u, 12344u})
    {
        for (bool optIn : {false, true})
        {
            for (int failure = 0; failure < 4; ++failure)
            {
                TestPeer peer(optIn);
                peer.Gateway().m_knownAddons =
                    BuildAscensionKnownAddons(ASCENSION_KNOWN_ADDONS_DEFAULT);
                if (failure == 1)
                {
                    peer.Gateway().m_lookupStatus = proto::AuthStatus::Banned;
                }
                peer.Gateway().m_rejectAttach = failure == 3;
                REQUIRE(peer.Authenticate(build, failure != 2));
                CHECK_EQ(peer.Sent().size(), size_t(
                    failure == 0 && build == 12344 && optIn ? 3 : 1));
                CHECK(peer.Connection().closed() == (failure != 0));
            }
        }
    }
}

TEST(ClientConnection_stock_auth_candidate_waits_for_queue_success)
{
    TestPeer peer(true);
    peer.Gateway().m_attachStatus = proto::AuthStatus::WaitQueue;
    REQUIRE(peer.Authenticate(12344));
    REQUIRE(peer.Sent().size() == 1);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::WaitQueue));
    REQUIRE(peer.Sent().size() == 2);
    peer.Connection().SendPacket(WorldPacket(SMSG_AUTH_RESPONSE, 0));
    REQUIRE(peer.Sent().size() == 3);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::Ok));
    REQUIRE(peer.Sent().size() == 5);
    peer.Connection().SendPacket(AuthResponse(proto::AuthStatus::Ok));
    CHECK_EQ(peer.Sent().size(), size_t(6));

    AuthCrypt clientCrypt;
    clientCrypt.Init(&peer.SessionKey());
    for (size_t i = 0; i < peer.Sent().size(); ++i)
    {
        std::vector<uint8> frame = peer.Sent()[i];
        clientCrypt.EncryptSend(frame.data(), 4);
        const uint16 opcode = uint16(frame[2]) | (uint16(frame[3]) << 8);
        CHECK_EQ(opcode, uint16(i == 4 ? 0x9BC : SMSG_AUTH_RESPONSE));
    }
}

TEST(ClientConnection_stock_auth_candidate_does_not_bootstrap_rejected_accounts)
{
    TestPeer peer(true);
    peer.Gateway().m_lookupStatus = proto::AuthStatus::Banned;
    REQUIRE(peer.Authenticate(12344));
    CHECK_EQ(peer.Gateway().AttachCount(), 0);
    CHECK(peer.Connection().closed());
    REQUIRE(peer.Sent().size() == 1);
    CHECK(peer.Sent()[0] == proto::PacketCodec::Encode(
        AuthResponse(proto::AuthStatus::Banned), {}));
}

TEST(ClientConnection_stock_auth_candidate_does_not_bootstrap_failed_attach)
{
    TestPeer peer(true);
    peer.Gateway().m_rejectAttach = true;
    REQUIRE(peer.Authenticate(12344));
    CHECK_EQ(peer.Gateway().AttachCount(), 1);
    CHECK(peer.Connection().closed());
    REQUIRE(peer.Sent().size() == 1);
    AuthCrypt clientCrypt;
    clientCrypt.Init(&peer.SessionKey());
    std::vector<uint8> failure = peer.Sent()[0];
    clientCrypt.EncryptSend(failure.data(), 4);
    CHECK(failure == proto::PacketCodec::Encode(
        AuthResponse(proto::AuthStatus::SystemError), {}));
}

TEST(ClientConnection_coalesced_auth_dispatches_before_encrypted_ping_at_every_split)
{
    for (uint32 build : {12340u, 12344u})
    {
        // Include both one-buffer delivery and every two-buffer split through
        // the clear auth header/body and the following encrypted header/body.
        const size_t authSize = AuthFrame(build, 0, true).size();
        WorldPacket ping(CMSG_PING, 8);
        ping << uint32(0x11223344) << uint32(50);
        const std::vector<uint8> clearPing = ClientFrame(ping);
        for (size_t cut = 0; cut <= authSize + clearPing.size(); ++cut)
        {
            TestPeer peer(true);
            CHECK_EQ(peer.SessionKey().GetNumBytes(), 39);
            const auto challenge = peer.Connection().onConnect();
            REQUIRE(challenge.size() >= 12);
            auto stream = AuthFrame(build, ReadLittleEndian32(challenge, 8), true);
            AuthCrypt clientCrypt;
            clientCrypt.Init(&peer.SessionKey());
            auto encryptedPing = clearPing;
            clientCrypt.DecryptRecv(encryptedPing.data(), proto::CLIENT_HEADER_SIZE);
            stream.insert(stream.end(), encryptedPing.begin(), encryptedPing.end());

            peer.Connection().onData(stream.data(), cut);
            CHECK_EQ(peer.Gateway().AttachCount(), cut >= authSize ? 1 : 0);
            CHECK_EQ(peer.Gateway().DeliverCount(), cut == stream.size() ? 1 : 0);
            peer.Connection().onData(stream.data() + cut, stream.size() - cut);
            REQUIRE(!peer.Connection().closed());
            CHECK_EQ(peer.Gateway().LookupCount(), 1);
            CHECK_EQ(peer.Gateway().AttachCount(), 1);
            CHECK_EQ(peer.Gateway().DeliverCount(), 1);
            CHECK_EQ(peer.Gateway().DeliveredSession(), 42u);
            CHECK_EQ(peer.Gateway().DeliveredOpcode(), uint16(CMSG_PING));
            CHECK(peer.Gateway().DeliveredPayload() ==
                std::vector<uint8>(ping.contents(), ping.contents() + ping.size()));
            REQUIRE(peer.Sent().size() == (build == 12344 ? 2u : 1u));
            auto response = peer.Sent()[0];
            clientCrypt.EncryptSend(response.data(), 4);
            CHECK(response == proto::PacketCodec::Encode(
                AuthResponse(proto::AuthStatus::Ok), {}));

            // A subsequent header uses the next keystream bytes, not a reset.
            encryptedPing = clearPing;
            clientCrypt.DecryptRecv(encryptedPing.data(), proto::CLIENT_HEADER_SIZE);
            for (uint8 byte : encryptedPing)
            {
                peer.Connection().onData(&byte, 1);
            }
            CHECK_EQ(peer.Gateway().DeliverCount(), 2);
            CHECK(!peer.Connection().closed());
        }
    }
}

TEST(ClientConnection_coalesced_bad_proof_discards_appended_traffic)
{
    for (uint32 build : {12340u, 12344u})
    {
        const size_t authSize = AuthFrame(build, 0, false).size();
        for (size_t cut : {size_t(0), size_t(5), authSize - 1,
                           authSize, authSize + 2, authSize + 6})
        {
            TestPeer peer(true);
            const auto challenge = peer.Connection().onConnect();
            REQUIRE(challenge.size() >= 12);
            auto stream = AuthFrame(build, ReadLittleEndian32(challenge, 8), false);
            AuthCrypt clientCrypt;
            clientCrypt.Init(&peer.SessionKey());
            auto ping = ClientFrame(WorldPacket(CMSG_PING, 0));
            clientCrypt.DecryptRecv(ping.data(), proto::CLIENT_HEADER_SIZE);
            stream.insert(stream.end(), ping.begin(), ping.end());
            peer.Connection().onData(stream.data(), cut);
            peer.Connection().onData(stream.data() + cut, stream.size() - cut);
            CHECK(peer.Connection().closed());
            CHECK_EQ(peer.CloseCount(), 1);
            CHECK_EQ(peer.Gateway().LookupCount(), 1);
            CHECK_EQ(peer.Gateway().AttachCount(), 0);
            CHECK_EQ(peer.Gateway().DeliverCount(), 0);
            REQUIRE(peer.Sent().size() == 1);
            CHECK(peer.Sent()[0] == proto::PacketCodec::Encode(
                AuthResponse(proto::AuthStatus::Failed), {}));
        }
    }
}

TEST(WorldGatewayAuth_revision_empty_requirement_preserves_legacy_bypass)
{
    CHECK(CheckRequiredClientUpdate({}, ""));
    CHECK(CheckRequiredClientUpdate({0xff, 0xff, 0xff, 0xff}, ""));
    CHECK(CheckRequiredClientUpdate(std::vector<uint8>(0x100004, 0), ""));
    const auto block = CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 0}, {UPDATE_MARKER, 1}}));
    CHECK(CheckRequiredClientUpdate(block, ""));
}

TEST(WorldGatewayAuth_revision_requires_exact_unique_marker_including_disabled)
{
    CHECK(!CheckRequiredClientUpdate({}, UPDATE_MARKER));
    CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo({})), UPDATE_MARKER));
    const std::vector<std::string> wrong = {
        "OrdinaryAddon", "AscenderUpdate_local_update_3", "AscenderUpdate_local_update_5",
        "AscenderUpdate_other_4", "AscenderUpdate_local_update_04",
        "AscenderUpdate_" + std::string(200, 'a'), "ascenderUpdate_local_update_4"
    };
    for (uint8 enabled : {0, 1})
    {
        const auto correct = CompressAddonInfo(AddonInfo({{"OrdinaryAddon", 1},
            {UPDATE_MARKER, enabled}, {"OtherAddon", 0}}));
        CHECK(CheckRequiredClientUpdate(correct, UPDATE_MARKER));
        CHECK(!CheckRequiredClientUpdate(correct, "invalid requirement"));
        for (const auto& name : wrong)
        {
            CHECK(!CheckRequiredClientUpdate(
                CompressAddonInfo(AddonInfo({{name, enabled}})), UPDATE_MARKER));
        }
        for (uint8 secondEnabled : {0, 1})
        {
            CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo(
                {{UPDATE_MARKER, enabled}, {UPDATE_MARKER, secondEnabled}})), UPDATE_MARKER));
            for (const char* stale : {"AscenderUpdate_local_update_3", "AscenderUpdate_other_4"})
            {
                CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo(
                    {{UPDATE_MARKER, enabled}, {stale, secondEnabled}})), UPDATE_MARKER));
                CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo(
                    {{stale, secondEnabled}, {UPDATE_MARKER, enabled}})), UPDATE_MARKER));
            }
        }
    }
}

TEST(WorldGatewayAuth_revision_parses_existing_fields_not_toc_version_or_crc)
{
    auto info = AddonInfo({{UPDATE_MARKER, 0}});
    const size_t flags = 4 + UPDATE_MARKER.size() + 1;
    info.put<uint8>(flags, 0xff);
    info.put<uint32>(flags + 1, 0x01020304);
    info.put<uint32>(flags + 5, 0xffffffff);
    info.put<uint32>(info.size() - 4, 0x55667788);
    CHECK(CheckRequiredClientUpdate(CompressAddonInfo(info), UPDATE_MARKER));
    // Ordinary names keep the existing reader's encoding and empty-name tolerance.
    CHECK(CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo(
        {{"NonAscii\xc3\xa9", 1}, {"", 0}, {UPDATE_MARKER, 1}})), UPDATE_MARKER));
    auto wrongName = AddonInfo({{"OrdinaryAddon", 1}});
    wrongName.put<uint32>(4 + std::string("OrdinaryAddon").size() + 2, 4);
    CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(wrongName), UPDATE_MARKER));
}

TEST(WorldGatewayAuth_revision_rejects_every_truncation_and_extra_record_bytes)
{
    const auto info = AddonInfo({{UPDATE_MARKER, 1}, {"AfterMarker", 0}});
    for (size_t size = 0; size < info.size(); ++size)
    {
        ByteBuffer truncated(info);
        truncated.resize(size);
        CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(truncated), UPDATE_MARKER));
    }
    const auto compressed = CompressAddonInfo(info);
    for (size_t size = 0; size < compressed.size(); ++size)
    {
        const std::vector<uint8> truncated(compressed.begin(), compressed.begin() + size);
        CHECK(!CheckRequiredClientUpdate(truncated, UPDATE_MARKER));
    }
    for (uint32 count : {0u, 1u, 3u, 0xffffffffu})
    {
        ByteBuffer wrongCount(info);
        wrongCount.put<uint32>(0, count);
        CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(wrongCount), UPDATE_MARKER));
    }
    ByteBuffer extra(info);
    extra << uint8(0);
    CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(extra), UPDATE_MARKER));
    ByteBuffer noTerminator;
    noTerminator << uint32(1);
    for (size_t i = 0; i < 100; ++i)
    {
        noTerminator << uint8('A');
    }
    noTerminator << uint32(0);
    CHECK(!CheckRequiredClientUpdate(CompressAddonInfo(noTerminator), UPDATE_MARKER));
}

TEST(WorldGatewayAuth_revision_rejects_bad_zlib_sizes_suffixes_and_bombs)
{
    const auto info = AddonInfo({{UPDATE_MARKER, 1}});
    const auto compressed = CompressAddonInfo(info);
    for (uint32 declared : {0u, 7u, uint32(info.size() - 1),
        uint32(info.size() + 1), 0x100000u, 0xffffffffu})
    {
        auto wrongSize = compressed;
        for (size_t i = 0; i < 4; ++i)
        {
            wrongSize[i] = uint8(declared >> (8 * i));
        }
        CHECK(!CheckRequiredClientUpdate(wrongSize, UPDATE_MARKER));
    }
    auto corrupt = compressed;
    corrupt.back() ^= 0xff;
    CHECK(!CheckRequiredClientUpdate(corrupt, UPDATE_MARKER));
    auto suffix = compressed;
    suffix.push_back(0);
    CHECK(!CheckRequiredClientUpdate(suffix, UPDATE_MARKER));
    auto concatenated = compressed;
    concatenated.insert(concatenated.end(), compressed.begin() + 4, compressed.end());
    CHECK(!CheckRequiredClientUpdate(concatenated, UPDATE_MARKER));
    CHECK(!CheckRequiredClientUpdate(std::vector<uint8>(0x100004, 0), UPDATE_MARKER));

    ByteBuffer expanded;
    expanded.resize(0x100000);
    auto bomb = CompressAddonInfo(expanded);
    REQUIRE(bomb.size() < 4096);
    // A small compressed input exceeds the maximum even when it lies about size.
    bomb[0] = 0xff;
    bomb[1] = 0xff;
    bomb[2] = 0x0f;
    bomb[3] = 0;
    CHECK(!CheckRequiredClientUpdate(bomb, UPDATE_MARKER));
    bomb[0] = 8;
    bomb[1] = 0;
    bomb[2] = 0;
    CHECK(!CheckRequiredClientUpdate(bomb, UPDATE_MARKER));
}

TEST(WorldGatewayAuth_revision_accepts_exact_inflated_limit)
{
    const size_t fixed = AddonInfo({{UPDATE_MARKER, 1}, {"", 0}}).size();
    const auto info = AddonInfo({{UPDATE_MARKER, 1},
        {std::string(0xFFFFF - fixed, 'a'), 0}});
    REQUIRE(info.size() == 0xFFFFF);
    CHECK(CheckRequiredClientUpdate(CompressAddonInfo(info), UPDATE_MARKER));
}

TEST(ClientConnection_revision_failure_sends_one_auth_failed_before_queue_or_bootstrap)
{
    const std::vector<std::vector<uint8>> rejected = {
        {}, CompressAddonInfo(AddonInfo({})),
        CompressAddonInfo(AddonInfo({{"AscenderUpdate_local_update_3", 1}})),
        CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 1}, {UPDATE_MARKER, 0}})),
        {0xff, 0xff, 0xff, 0xff}, {8, 0, 0, 0, 0xff}
    };
    for (uint32 build : {12340u, 12344u})
    {
        for (bool optIn : {false, true})
        {
            for (const auto& block : rejected)
            {
                TestPeer peer(optIn);
                peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
                peer.Gateway().m_attachStatus = proto::AuthStatus::WaitQueue;
                peer.Gateway().m_knownAddons = BuildAscensionKnownAddons("A:1");
                REQUIRE(peer.Authenticate(build, true, block));
                CHECK_EQ(peer.Gateway().AttachCount(), 1);
                CHECK_EQ(peer.Gateway().AdmissionCount(), 0);
                CHECK_EQ(peer.Gateway().DeliverCount(), 0);
                CHECK(peer.Connection().closed());
                CHECK_EQ(peer.CloseCount(), 1);
                REQUIRE(peer.Sent().size() == 1);
                auto failure = peer.Sent()[0];
                if (build != 12344 || optIn)
                {
                    AuthCrypt crypt;
                    crypt.Init(&peer.SessionKey());
                    crypt.EncryptSend(failure.data(), 4);
                }
                CHECK_BYTES(failure.data(), failure.size(), {0, 3, 0xee, 1, 0x0d});
            }
        }
    }
}

TEST(ClientConnection_revision_correct_marker_uses_existing_queue_and_auth_packets)
{
    for (uint32 build : {12340u, 12344u})
    {
        for (bool optIn : {false, true})
        {
            for (auto status : {proto::AuthStatus::Ok, proto::AuthStatus::WaitQueue})
            {
                TestPeer peer(optIn);
                peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
                peer.Gateway().m_attachStatus = status;
                REQUIRE(peer.Authenticate(build, true,
                    CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 0}}))));
                CHECK_EQ(peer.Gateway().AdmissionCount(), 1);
                CHECK(!peer.Connection().closed());
                REQUIRE(peer.Sent().size() == size_t(
                    build == 12344 && optIn && status == proto::AuthStatus::Ok ? 2 : 1));
                auto response = peer.Sent()[0];
                if (build != 12344 || optIn)
                {
                    AuthCrypt crypt;
                    crypt.Init(&peer.SessionKey());
                    crypt.EncryptSend(response.data(), 4);
                }
                CHECK(response == proto::PacketCodec::Encode(AuthResponse(status), {}));
            }
        }
    }
}

TEST(ClientConnection_revision_coalesced_rejection_discards_suffix_at_every_split)
{
    const auto stale = CompressAddonInfo(AddonInfo({{"AscenderUpdate_local_update_3", 1}}));
    for (bool optIn : {false, true})
    {
        const size_t authSize = AuthFrame(12344, 0, true, stale).size();
        auto clearNext = ClientFrame(WorldPacket(CMSG_CHAR_ENUM, 0));
        for (size_t cut = 0; cut <= authSize + clearNext.size(); ++cut)
        {
            TestPeer peer(optIn);
            peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
            const auto challenge = peer.Connection().onConnect();
            REQUIRE(challenge.size() >= 12);
            auto stream = AuthFrame(12344, ReadLittleEndian32(challenge, 8), true, stale);
            auto next = clearNext;
            if (optIn)
            {
                AuthCrypt crypt;
                crypt.Init(&peer.SessionKey());
                crypt.DecryptRecv(next.data(), proto::CLIENT_HEADER_SIZE);
            }
            stream.insert(stream.end(), next.begin(), next.end());
            peer.Connection().onData(stream.data(), cut);
            peer.Connection().onData(stream.data() + cut, stream.size() - cut);
            CHECK_EQ(peer.Gateway().LookupCount(), 1);
            CHECK_EQ(peer.Gateway().AttachCount(), 1);
            CHECK_EQ(peer.Gateway().AdmissionCount(), 0);
            CHECK_EQ(peer.Gateway().DeliverCount(), 0);
            CHECK(peer.Connection().closed());
            CHECK_EQ(peer.CloseCount(), 1);
            CHECK_EQ(peer.Sent().size(), size_t(1));
        }
    }
}

TEST(ClientConnection_revision_does_not_exempt_or_precede_login_proof)
{
    for (const auto& block : {std::vector<uint8>{},
        CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 1}}))})
    {
        TestPeer peer(true);
        peer.Gateway().m_requiredClientUpdate = UPDATE_MARKER;
        REQUIRE(peer.Authenticate(12344, false, block));
        CHECK_EQ(peer.Gateway().AttachCount(), 0);
        CHECK_EQ(peer.Gateway().AdmissionCount(), 0);
        REQUIRE(peer.Sent().size() == 1);
        // Existing bad-proof path is still cleartext, never a post-proof cipher.
        CHECK_BYTES(peer.Sent()[0].data(), peer.Sent()[0].size(), {0, 3, 0xee, 1, 0x0d});
    }
}

TEST(WorldGatewayAuth_revision_diagnostic_fixed_codes_and_silent_success)
{
    for (auto status : {ClientUpdateStatus::Disabled, ClientUpdateStatus::Accepted})
    {
        const ClientUpdateAdmission result{status};
        CHECK(bool(result));
        CHECK(result.Diagnostic().empty());
    }
    const std::vector<std::pair<ClientUpdateStatus, std::string>> reasons = {
        {ClientUpdateStatus::InvalidRequirement, "invalid_requirement"},
        {ClientUpdateStatus::MissingMarker, "missing_marker"},
        {ClientUpdateStatus::WrongMarker, "wrong_marker"},
        {ClientUpdateStatus::DuplicateMarker, "duplicate_marker"},
        {ClientUpdateStatus::CompressedFormat, "compressed_format"},
        {ClientUpdateStatus::Size, "size"},
        {ClientUpdateStatus::Count, "count"},
        {ClientUpdateStatus::RecordFormat, "record_format"},
        {ClientUpdateStatus::Trailer, "trailer"},
        {static_cast<ClientUpdateStatus>(255), "invalid_reason"}
    };
    for (const auto& reason : reasons)
    {
        const ClientUpdateAdmission result{reason.first, 3, 2, true};
        CHECK(!result);
        CHECK_STR(result.Diagnostic(), "world_auth stage=client_update reason=" + reason.second +
            " addon_count=3 marker_count=2 matched=1");
        for (unsigned char c : result.Diagnostic())
        {
            CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ' ' || c == '=');
        }
    }
}

TEST(WorldGatewayAuth_revision_diagnostic_marker_counts_and_no_input_strings)
{
    const auto correct = CompressAddonInfo(AddonInfo({{UPDATE_MARKER, 0}}));
    auto result = CheckRequiredClientUpdate(correct, UPDATE_MARKER);
    CHECK(result.status == ClientUpdateStatus::Accepted);
    CHECK_EQ(result.addonCount, 1u);
    CHECK_EQ(result.markerCount, 1u);
    CHECK(result.matched);
    CHECK(result.Diagnostic().empty());
    result = CheckRequiredClientUpdate(correct, "untrusted requirement\r\n\x80");
    CHECK(result.status == ClientUpdateStatus::InvalidRequirement);
    CHECK_STR(result.Diagnostic(), "world_auth stage=client_update reason=invalid_requirement addon_count=0 marker_count=0 matched=0");
    for (const char* input : {"untrusted addon\r\n\x80", "AscenderUpdate_untrusted\r\n\x80"})
    {
        result = CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo({{input, 0}})), UPDATE_MARKER);
        CHECK(!result);
        CHECK_EQ(result.addonCount, 1u);
        CHECK_EQ(result.markerCount, input[0] == 'A' ? 1u : 0u);
        CHECK(!result.matched);
        CHECK(result.status == (input[0] == 'A' ? ClientUpdateStatus::WrongMarker : ClientUpdateStatus::MissingMarker));
        CHECK(result.Diagnostic().find("untrusted") == std::string::npos);
        CHECK(result.Diagnostic().find(UPDATE_MARKER) == std::string::npos);
        for (unsigned char c : result.Diagnostic())
        {
            CHECK(c >= 0x20 && c < 0x7f);
        }
    }
    // Duplicate means the whole reserved namespace, independent of order/enablement.
    for (bool staleFirst : {false, true})
    {
        const std::string stale = "AscenderUpdate_local_update_3";
        result = CheckRequiredClientUpdate(CompressAddonInfo(AddonInfo({
            {staleFirst ? stale : UPDATE_MARKER, 0}, {"Ordinary", 1},
            {staleFirst ? UPDATE_MARKER : stale, 1}})), UPDATE_MARKER);
        CHECK(result.status == ClientUpdateStatus::DuplicateMarker);
        CHECK_EQ(result.addonCount, 3u);
        CHECK_EQ(result.markerCount, 2u);
        CHECK(result.matched);
        CHECK_STR(result.Diagnostic(), "world_auth stage=client_update reason=duplicate_marker addon_count=3 marker_count=2 matched=1");
    }
    result = CheckRequiredClientUpdate({0xff, 0xff, 0xff, 0xff}, "");
    CHECK(result.status == ClientUpdateStatus::Disabled);
    CHECK(result.Diagnostic().empty());
}

TEST(WorldGatewayAuth_revision_diagnostic_compressed_size_count_record_and_trailer)
{
    for (const auto& block : {std::vector<uint8>{}, std::vector<uint8>{0, 0, 0, 0},
        CompressAddonInfo(AddonInfo({}))})
    {
        const auto result = CheckRequiredClientUpdate(block, UPDATE_MARKER);
        CHECK(result.status == ClientUpdateStatus::MissingMarker);
        CHECK_EQ(result.addonCount, 0u);
        CHECK_EQ(result.markerCount, 0u);
    }
    for (const auto& block : {std::vector<uint8>{1, 2, 3}, std::vector<uint8>{8, 0, 0, 0, 0xff}})
    {
        const auto result = CheckRequiredClientUpdate(block, UPDATE_MARKER);
        CHECK(result.status == ClientUpdateStatus::CompressedFormat);
        CHECK_EQ(result.addonCount, 0u);
    }
    const auto valid = AddonInfo({{UPDATE_MARKER, 1}});
    auto compressed = CompressAddonInfo(valid);
    compressed.push_back(0);
    CHECK(CheckRequiredClientUpdate(compressed, UPDATE_MARKER).status == ClientUpdateStatus::CompressedFormat);
    for (uint32 size : {7u, uint32(valid.size() + 1), 0x100000u, 0xffffffffu})
    {
        compressed = CompressAddonInfo(valid);
        for (size_t i = 0; i < 4; ++i)
        {
            compressed[i] = uint8(size >> (8 * i));
        }
        const auto result = CheckRequiredClientUpdate(compressed, UPDATE_MARKER);
        CHECK(result.status == ClientUpdateStatus::Size);
        CHECK_EQ(result.addonCount, 0u);
    }
    auto count = valid;
    count.put<uint32>(0, 0xffffffff);
    auto result = CheckRequiredClientUpdate(CompressAddonInfo(count), UPDATE_MARKER);
    CHECK(result.status == ClientUpdateStatus::Count);
    CHECK_EQ(result.addonCount, 0u); // Unbounded declared counts never enter diagnostics.
    CHECK_EQ(result.markerCount, 0u);
    ByteBuffer record;
    record << uint32(1);
    for (size_t i = 0; i < 100; ++i)
    {
        record << uint8('A');
    }
    record << uint32(0);
    result = CheckRequiredClientUpdate(CompressAddonInfo(record), UPDATE_MARKER);
    CHECK(result.status == ClientUpdateStatus::RecordFormat);
    CHECK_EQ(result.addonCount, 1u);
    CHECK_EQ(result.markerCount, 0u);
    for (size_t size : {valid.size() - 4, valid.size() - 1, valid.size() + 1})
    {
        auto trailer = valid;
        trailer.resize(size);
        result = CheckRequiredClientUpdate(CompressAddonInfo(trailer), UPDATE_MARKER);
        CHECK(result.status == ClientUpdateStatus::Trailer);
        CHECK_EQ(result.addonCount, 1u);
        CHECK_EQ(result.markerCount, 1u);
        CHECK(result.matched);
    }
    ByteBuffer bomb;
    bomb.resize(0x100000);
    compressed = CompressAddonInfo(bomb);
    compressed[0] = 0xff;
    compressed[1] = 0xff;
    compressed[2] = 0x0f;
    compressed[3] = 0;
    result = CheckRequiredClientUpdate(compressed, UPDATE_MARKER);
    CHECK(result.status == ClientUpdateStatus::Size);
    CHECK_EQ(result.addonCount, 0u);
}

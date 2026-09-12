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
#include "SessionMailbox.h"
#include "Opcodes.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace
{
std::unique_ptr<WorldPacket> MakePacket(uint16 opcode, uint8 value)
{
    std::unique_ptr<WorldPacket> packet =
        std::make_unique<WorldPacket>(opcode, 1);
    *packet << value;
    return packet;
}
}

TEST(SessionMailbox_transfers_fifo_ownership)
{
    SessionMailbox mailbox;
    CHECK(mailbox.Enqueue(MakePacket(1, 0x11)));
    CHECK(mailbox.Enqueue(MakePacket(2, 0x22)));

    WorldPacket* raw = nullptr;
    REQUIRE(mailbox.Next(raw));
    std::unique_ptr<WorldPacket> first(raw);
    CHECK_EQ(first->GetOpcode(), 1);
    CHECK_EQ((*first)[0], 0x11);

    REQUIRE(mailbox.Next(raw));
    std::unique_ptr<WorldPacket> second(raw);
    CHECK_EQ(second->GetOpcode(), 2);
    CHECK_EQ((*second)[0], 0x22);
    CHECK(!mailbox.Next(raw));
}

TEST(SessionMailbox_close_is_idempotent_and_drains)
{
    SessionMailbox mailbox;
    CHECK(mailbox.Enqueue(MakePacket(3, 0x33)));
    CHECK(mailbox.Enqueue(MakePacket(4, 0x44)));

    mailbox.Close();
    mailbox.Close();

    WorldPacket* raw = nullptr;
    CHECK(mailbox.IsClosed());
    CHECK(!mailbox.Next(raw));
    CHECK(!mailbox.Enqueue(MakePacket(5, 0x55)));
}

TEST(SessionMailbox_accepts_last_opcode_and_transfers_ownership)
{
    CHECK_EQ(NUM_MSG_TYPES, 0x529);
    SessionMailbox mailbox;
    std::unique_ptr<WorldPacket> packet = MakePacket(0x528, 0x28);
    WorldPacket* original = packet.get();
    REQUIRE(mailbox.Enqueue(std::move(packet)));
    CHECK(!packet);

    WorldPacket* raw = nullptr;
    REQUIRE(mailbox.Next(raw));
    std::unique_ptr<WorldPacket> received(raw);
    CHECK(received.get() == original);
    CHECK_EQ(received->GetOpcode(), 0x528);
    CHECK_EQ((*received)[0], 0x28);
    CHECK(!mailbox.Next(raw));
}

TEST(SessionMailbox_rejects_unknown_opcodes_before_filtering)
{
    struct Filter
    {
        unsigned calls = 0;

        bool Process(WorldPacket* packet)
        {
            ++calls;
            CHECK(packet->GetOpcode() < NUM_MSG_TYPES);
            return true;
        }
    } filter;

    SessionMailbox mailbox;
    CHECK(mailbox.Enqueue(MakePacket(1, 0x11)));
    for (uint16 opcode : {0x529, 0x53b, 0xffff})
    {
        std::unique_ptr<WorldPacket> packet = MakePacket(opcode, 0xff);
        CHECK(!mailbox.Enqueue(std::move(packet)));
        CHECK(!packet);
    }
    CHECK(!mailbox.IsClosed());
    CHECK(mailbox.Enqueue(MakePacket(0x528, 0x28)));

    WorldPacket* raw = nullptr;
    REQUIRE(mailbox.Next(raw, filter));
    std::unique_ptr<WorldPacket> first(raw);
    CHECK_EQ(first->GetOpcode(), 1);
    REQUIRE(mailbox.Next(raw, filter));
    std::unique_ptr<WorldPacket> second(raw);
    CHECK_EQ(second->GetOpcode(), 0x528);
    while (mailbox.Next(raw, filter))
    {
        delete raw;
        CHECK(false);
    }
    CHECK_EQ(filter.calls, 2u);
}

TEST(SessionMailbox_rejected_packets_are_not_queued)
{
    SessionMailbox mailbox;
    CHECK(!mailbox.Enqueue(nullptr));
    for (uint16 opcode : {0x529, 0x53b, 0xffff})
    {
        CHECK(!mailbox.Enqueue(MakePacket(opcode, 0xff)));
        WorldPacket* raw = nullptr;
        const bool received = mailbox.Next(raw);
        std::unique_ptr<WorldPacket> unexpected(raw);
        CHECK(!received);
        CHECK(!unexpected);
    }
    CHECK(!mailbox.IsClosed());
}

TEST(SessionMailbox_close_racing_producers_leaves_no_packets)
{
    SessionMailbox mailbox;
    std::atomic<bool> start{false};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> raceClose{false};
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 4; ++producer)
    {
        producers.emplace_back(
            [&mailbox, &start, &ready, &raceClose, producer]()
        {
            while (!start.load())
                std::this_thread::yield();

            mailbox.Enqueue(MakePacket(uint16(producer + 1), 0));
            ready.fetch_add(1);
            while (!raceClose.load())
                std::this_thread::yield();

            for (unsigned packet = 1; packet < 200; ++packet)
            {
                mailbox.Enqueue(MakePacket(
                    uint16(producer + 1), uint8(packet)));
            }
        });
    }

    start.store(true);
    while (ready.load() != producers.size())
        std::this_thread::yield();
    raceClose.store(true);
    mailbox.Close();
    for (std::thread& producer : producers)
        producer.join();

    WorldPacket* raw = nullptr;
    CHECK(!mailbox.Next(raw));
    CHECK(!mailbox.Enqueue(MakePacket(9, 0x99)));
}

TEST(SessionMailbox_closed_old_route_cannot_reach_replacement)
{
    std::shared_ptr<SessionMailbox> oldMailbox =
        std::make_shared<SessionMailbox>();
    std::shared_ptr<SessionMailbox> retainedDelivery = oldMailbox;
    oldMailbox->Close();

    std::shared_ptr<SessionMailbox> replacement =
        std::make_shared<SessionMailbox>();
    CHECK(!retainedDelivery->Enqueue(MakePacket(6, 0x66)));
    CHECK(replacement->Enqueue(MakePacket(7, 0x77)));

    WorldPacket* raw = nullptr;
    REQUIRE(replacement->Next(raw));
    std::unique_ptr<WorldPacket> packet(raw);
    CHECK_EQ(packet->GetOpcode(), 7);
    CHECK(!replacement->Next(raw));
}

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
#include "SessionProtocolPolicy.h"

using namespace std::chrono;

TEST(SessionPingTracker_first_ping_is_not_fast)
{
    SessionPingTracker tracker;
    CHECK_EQ(tracker.Record(
        SessionPingTracker::Clock::time_point(seconds(100))), 0);
    CHECK(!tracker.ShouldKick(1, true));
}

TEST(SessionPingTracker_counts_and_resets_fast_runs)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point start(seconds(100));
    CHECK_EQ(tracker.Record(start), 0);
    CHECK_EQ(tracker.Record(start + seconds(10)), 1);
    CHECK_EQ(tracker.Record(start + seconds(20)), 2);
    CHECK_EQ(tracker.Record(start + seconds(50)), 0);
}

TEST(SessionPingTracker_enforces_threshold_only_for_players)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point start(seconds(100));
    tracker.Record(start);
    tracker.Record(start + seconds(1));
    tracker.Record(start + seconds(2));

    CHECK(tracker.ShouldKick(1, true));
    CHECK(!tracker.ShouldKick(0, true));
    CHECK(!tracker.ShouldKick(1, false));
}

TEST(SessionPingTracker_build12344_regular_player_five_second_cadence)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point start(seconds(100));
    for (int ping = 0; ping <= 12; ++ping)
    {
        CHECK_EQ(tracker.Record(start + seconds(5 * ping), seconds(4)), 0);
        CHECK(!tracker.ShouldKick(2, true));
    }
}

TEST(SessionPingTracker_build12344_rapid_abuse_still_kicks_players)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point start(seconds(100));
    CHECK_EQ(tracker.Record(start, seconds(4)), 0);
    CHECK_EQ(tracker.Record(start + seconds(1), seconds(4)), 1);
    CHECK(!tracker.ShouldKick(2, true));
    CHECK_EQ(tracker.Record(start + seconds(2), seconds(4)), 2);
    CHECK(!tracker.ShouldKick(2, true));
    CHECK_EQ(tracker.Record(start + seconds(3), seconds(4)), 3);
    CHECK(tracker.ShouldKick(2, true));
    CHECK(!tracker.ShouldKick(0, true));
    CHECK(!tracker.ShouldKick(2, false));
    CHECK_EQ(tracker.Record(start + seconds(8), seconds(4)), 0);
    CHECK(!tracker.ShouldKick(2, true));
}

TEST(SessionPingTracker_build12344_four_second_boundary)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point now(seconds(100));
    CHECK_EQ(tracker.Record(now, seconds(4)), 0);
    now += seconds(4) - milliseconds(1);
    CHECK_EQ(tracker.Record(now, seconds(4)), 1);
    now += seconds(4);
    CHECK_EQ(tracker.Record(now, seconds(4)), 0);
    now += seconds(4) + milliseconds(1);
    CHECK_EQ(tracker.Record(now, seconds(4)), 0);
    CHECK(!tracker.ShouldKick(2, true));
}

TEST(SessionPingTracker_stock_five_second_cadence_still_kicks)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point start(seconds(100));
    CHECK_EQ(tracker.Record(start), 0);
    CHECK_EQ(tracker.Record(start + seconds(5)), 1);
    CHECK_EQ(tracker.Record(start + seconds(10)), 2);
    CHECK(!tracker.ShouldKick(2, true));
    CHECK_EQ(tracker.Record(start + seconds(15)), 3);
    CHECK(tracker.ShouldKick(2, true));
}

TEST(SessionPingTracker_stock_twenty_seven_second_boundary)
{
    SessionPingTracker tracker;
    SessionPingTracker::Clock::time_point now(seconds(100));
    CHECK_EQ(tracker.Record(now), 0);
    now += seconds(27) - milliseconds(1);
    CHECK_EQ(tracker.Record(now), 1);
    now += seconds(27);
    CHECK_EQ(tracker.Record(now), 0);
    now += seconds(27) + milliseconds(1);
    CHECK_EQ(tracker.Record(now), 0);
}

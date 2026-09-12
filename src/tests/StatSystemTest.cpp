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
#include "StatSystem.h"

#include <cmath>

TEST(StatSystem_UncalibratedDodgeContributionHasNoInventedCap)
{
    const float diminishing = 7.25f;

    CHECK_EQ(StatSystem::CalculateDiminishingContribution(
        diminishing, 0.0f, 0.9560f), diminishing);
    CHECK_EQ(StatSystem::CalculateDiminishingContribution(
        diminishing, 88.129021f, 0.0f), diminishing);
}

TEST(StatSystem_StockDodgeFormulaIsUnchanged)
{
    const float nondiminishing = 5.0f;
    const float diminishing = 12.5f;
    const float cap = 88.129021f;
    const float coefficient = 0.9560f;
    const float expected = nondiminishing + diminishing * cap /
        (diminishing + cap * coefficient);
    const float actual = nondiminishing +
        StatSystem::CalculateDiminishingContribution(
            diminishing, cap, coefficient);

    CHECK_EQ(actual, expected);
}

TEST(StatSystem_Class10DodgeIsFiniteWithZeroCalibration)
{
    // Class 10's entries in both stock calibration arrays are zero.
    const float nondiminishing = 3.5f;
    const float value = nondiminishing +
        StatSystem::CalculateDiminishingContribution(
            0.0f, 0.0f, 0.0f);

    CHECK(std::isfinite(value));
    CHECK_EQ(value, nondiminishing);
}

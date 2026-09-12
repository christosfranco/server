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

#include "Database/Database.h"
#include "Utilities/ByteBuffer.h"

#include <cstring>
#include <string>

namespace
{
    class DefaultConnection final : public SqlConnection
    {
        public:
            explicit DefaultConnection(Database& database) : SqlConnection(database) {}

            bool Initialize(const char*) override { return true; }
            QueryResult* Query(const char*) override { return nullptr; }
            QueryNamedResult* QueryNamed(const char*) override { return nullptr; }
            bool Execute(const char*) override { return true; }
    };

    class DefaultDatabase final : public Database
    {
        public:
            DefaultDatabase()
            {
                m_pQueryConnections.push_back(new DefaultConnection(*this));
                m_nQueryConnPoolSize = 1;
            }

        protected:
            SqlConnection* CreateConnection() override
            {
                return new DefaultConnection(*this);
            }
    };
}

TEST(SqlConnection_default_escape_copies_exact_length_and_terminates)
{
    DefaultDatabase database;
    DefaultConnection connection(database);
    const char input[] = {'o', 'k', '!'};
    const char sentinel = char(0x5A);
    char output[sizeof(input) * 2 + 3];
    std::memset(output, sentinel, sizeof(output));

    CHECK_EQ(connection.escape_string(output + 1, input, sizeof(input)),
        static_cast<unsigned long>(sizeof(input)));
    CHECK_EQ(int(output[0]), int(sentinel));
    CHECK(std::memcmp(output + 1, input, sizeof(input)) == 0);
    CHECK_EQ(int(output[sizeof(input) + 1]), 0);
    CHECK_EQ(int(output[sizeof(input) + 2]), int(sentinel));
}

TEST(SqlConnection_default_escape_handles_empty_input)
{
    DefaultDatabase database;
    DefaultConnection connection(database);
    const char sentinel = char(0x5A);
    char output[3] = {sentinel, sentinel, sentinel};

    CHECK_EQ(connection.escape_string(output + 1, "", 0), 0UL);
    CHECK_EQ(int(output[0]), int(sentinel));
    CHECK_EQ(int(output[1]), 0);
    CHECK_EQ(int(output[2]), int(sentinel));
}

TEST(Database_escape_string_terminates_default_connection_output)
{
    DefaultDatabase database;
    std::string value = "nonterminated";

    database.escape_string(value);

    CHECK_STR(value, "nonterminated");
}

TEST(ByteBuffer_reads_scalars_at_odd_offsets)
{
    const uint8 bytes[] = {
        0xAA,
        0x34, 0x12,
        0xEF, 0xCD, 0xAB, 0x89,
        0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01
    };
    ByteBuffer buffer;
    buffer.append(bytes, sizeof(bytes));

    CHECK_EQ(buffer.read<uint16>(1), uint16(0x1234));
    CHECK_EQ(buffer.read<uint32>(3), uint32(0x89ABCDEF));
    CHECK_EQ(buffer.read<uint64>(7), uint64(0x0123456789ABCDEFULL));
}

TEST(ByteBuffer_reads_signed_and_floating_scalars)
{
    const uint8 bytes[] = {0xBB, 0xFE, 0xFF, 0x00, 0x00, 0x20, 0xC0};
    ByteBuffer buffer;
    buffer.append(bytes, sizeof(bytes));

    CHECK_EQ(buffer.read<int16>(1), int16(-2));
    CHECK(buffer.read<float>(3) == -2.5f);
}

TEST(ByteBuffer_truncated_scalar_read_does_not_advance)
{
    const uint8 bytes[] = {0x01, 0x02, 0x03};
    ByteBuffer buffer;
    buffer.append(bytes, sizeof(bytes));
    const size_t position = buffer.rpos();
    uint32 value = 0xDEADBEEF;
    bool threw = false;

    try
    {
        value = buffer.read<uint32>();
    }
    catch (const ByteBufferException&)
    {
        threw = true;
    }

    CHECK(threw);
    CHECK_EQ(buffer.rpos(), position);
    CHECK_EQ(value, uint32(0xDEADBEEF));
}

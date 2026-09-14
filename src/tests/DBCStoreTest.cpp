// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "DBCStore.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>

TEST(Coa_trainer_dbc_storage_initialization_and_reload)
{
    namespace fs = std::filesystem;
    auto directory = fs::temp_directory_path() / ("ascender-dbc-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
    struct Cleanup
    {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{directory};
    auto file = directory / "trainer.dbc";
    // Sparse IDs and an empty string table, like native NPCTrainer.dbc.
    std::array<uint32_t, 13> words{{0x43424457, 2, 4, 16, 0, 7, 500232, 0, 0, 100, 706742, 0, 0}};
    {
        std::ofstream output(file, std::ios::binary);
        for (auto word : words)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                output.put(char(word >> shift));
            }
        }
        REQUIRE(bool(output));
    }
    struct Entry { uint32_t id, spell; };
    using Store = DBCStorage<Entry>;
    for (unsigned char fill : {0, 1})
    {
        alignas(Store) std::array<unsigned char, sizeof(Store)> bytes;
        bytes.fill(fill);
        auto destroy = [](Store* store) { store->~Store(); };
        std::unique_ptr<Store, decltype(destroy)> store(new(bytes.data()) Store("nixx"), destroy);
        REQUIRE(store->Load(file.string().c_str()));
        CHECK_EQ(store->GetNumRows(), 101u);
        REQUIRE(store->LookupEntry(7));
        CHECK_EQ(store->LookupEntry(7)->spell, 500232u);
        CHECK(!store->LookupEntry(8));
        Entry extra{101, 801707};
        store->SetEntry(101, &extra);
        CHECK_EQ(store->LookupEntry(101)->spell, 801707u);
        store->Clear();
        CHECK_EQ(store->GetNumRows(), 0u);
        REQUIRE(store->Load(file.string().c_str()));
        CHECK_EQ(store->GetNumRows(), 101u);
    }
}

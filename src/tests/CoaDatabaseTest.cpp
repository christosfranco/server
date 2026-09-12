// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaState.h"
#include "Database/Database.h"
#include "Database/QueryResult.h"
#include "Database/Field.h"
#include "PlayerSavePreflight.h"
#include "InventoryTransaction.h"
#include "GuildBankCleanup.h"
#include "Mail.h"
#include <optional>
#include <cstdio>
#include <algorithm>
#include <thread>

namespace
{
    struct InventoryItem
    {
        enum State { New, Unchanged, Removed };
        State state = New;
        unsigned slot = 0;
        uint32 duration = 0;
        State GetState() const { return state; }
        uint32 GetEnchantmentDuration(unsigned) const { return duration; }
    };
    struct InventoryEnchantment
    {
        InventoryItem* item;
        unsigned slot;
        uint32 leftduration;
    };
    using Row = std::vector<std::optional<std::string>>;
    struct Rows : QueryResult
    {
        std::vector<Row> rows;
        std::vector<Field> fields;
        size_t position = 0;
        explicit Rows(std::vector<Row> values)
            : QueryResult(values.size(), values.front().size()), rows(std::move(values)), fields(mFieldCount)
        {
            Install();
        }
        void Install()
        {
            for (size_t i = 0; i < fields.size(); ++i)
            {
                fields[i].SetValue(rows[position][i] ? rows[position][i]->c_str() : nullptr);
            }
            mCurrentRow = fields.data();
        }
        bool NextRow() override
        {
            if (++position >= rows.size()) { return false; }
            Install(); return true;
        }
    };
    struct Backend
    {
        bool readFailure = false, commitFailure = false, beginFailure = false, countFailure = false;
        size_t failStatement = 0;
        std::string affected = "1";
        std::vector<Row> rows{Row(12)};
        std::vector<std::string> statements, events;
        uint32_t level = 19, xp = 123, flags = 3;
        uint32_t savedLevel = 19, savedXp = 123, savedFlags = 3;
        bool schema = false;
        uint32_t engines = 3;
        std::string rankDefault = "1";
        uint32_t creationEngines = 26;
        bool creation = false, loseCommitReply = false;
        struct Records
        {
            bool character = false, header = false, item = false, inventory = false, skill = false;
        } records, savedRecords;
        struct InventoryRecords
        {
            uint32 senderMoney = 100, receiverMoney = 100;
            uint32 itemOwner = 0, cod = 25, payment = 0;
            bool attachment = true;
            bool itemExists = true, guildExists = true;
        } inventory, savedInventory;
        std::function<void()> executing;
    };
    class Connection : public SqlConnection
    {
        Backend& m_backend;
    public:
        Connection(Database& db, Backend& backend) : SqlConnection(db), m_backend(backend) {}
        ~Connection() override { FreePreparedStatements(); }
        bool Initialize(char const*) override { return true; }
        QueryResult* Query(char const* sql) override
        {
            if (m_backend.schema)
            {
                std::string query = sql;
                if (query.find("information_schema.TABLES") != std::string::npos)
                {
                    return new Rows({{std::to_string(query.find("'item_instance'") != std::string::npos
                        ? m_backend.creationEngines : m_backend.engines)}});
                }
                if (query.find("information_schema.COLUMNS") != std::string::npos)
                {
                    std::vector<Row> columns;
                    auto add = [&](std::string table, std::string column, std::string type, bool isUnsigned = true)
                    {
                        columns.push_back({table, column, type, type + (isUnsigned ? " unsigned" : ""), "NO",
                            type == "char" ? std::optional<std::string>("64") : std::nullopt,
                            type == "char" ? std::optional<std::string>("ascii_bin") : std::nullopt,
                            column == "spell_rank_limit" ? m_backend.rankDefault : "0"});
                    };
                    add("character_coa_state", "guid", "int");
                    add("character_coa_state", "active_loadout", "tinyint");
                    add("character_coa_state", "loadout_count", "tinyint");
                    add("character_coa_state", "catalog_revision", "char", false);
                    add("character_coa_state", "state_revision", "bigint");
                    add("character_coa_entry", "guid", "int");
                    add("character_coa_entry", "loadout", "tinyint");
                    add("character_coa_entry", "entry_id", "int");
                    add("character_coa_entry", "rank", "int");
                    add("character_coa_entry", "spell_rank_limit", "int");
                    add("character_coa_entry", "locked", "tinyint");
                    add("character_coa_entry", "entry_time", "bigint", false);
                    return new Rows(std::move(columns));
                }
                if (query.find("information_schema.STATISTICS") != std::string::npos)
                {
                    return new Rows({{"character_coa_entry", "guid,loadout,entry_id"}, {"character_coa_state", "guid"}});
                }
            }
            if (std::string(sql) == "SELECT ROW_COUNT()")
            {
                return m_backend.countFailure ? nullptr : new Rows({{m_backend.affected}});
            }
            return m_backend.readFailure ? nullptr : new Rows(m_backend.rows);
        }
        QueryNamedResult* QueryNamed(char const*) override { return nullptr; }
        bool Execute(char const* sql) override
        {
            if (m_backend.executing)
            {
                m_backend.executing();
            }
            m_backend.statements.push_back(sql);
            if (m_backend.failStatement && m_backend.statements.size() == m_backend.failStatement)
            {
                return false;
            }
            if (m_backend.creation)
            {
                std::string query(sql);
                if (query.find("INSERT INTO `characters`") == 0)
                {
                    if (m_backend.records.character)
                    {
                        return false;
                    }
                    m_backend.records.character = true;
                }
                else if (query.find("INSERT INTO character_coa_state") == 0)
                {
                    if (m_backend.records.header)
                    {
                        return false;
                    }
                    if (!m_backend.records.character)
                    {
                        m_backend.affected = "0";
                        return true;
                    }
                    m_backend.records.header = true;
                    m_backend.affected = "1";
                }
                else if (query.find("INSERT INTO item_instance") == 0)
                {
                    m_backend.records.item = true;
                }
                else if (query.find("INSERT INTO character_inventory") == 0)
                {
                    m_backend.records.inventory = true;
                }
                else if (query.find("INSERT INTO character_skills") == 0)
                {
                    m_backend.records.skill = true;
                }
            }
            unsigned money, owner;
            if (std::sscanf(sql, "UPDATE characters SET money=%u WHERE guid=%u", &money, &owner) == 2)
            {
                (owner == 1 ? m_backend.inventory.senderMoney : m_backend.inventory.receiverMoney) = money;
            }
            if (std::sscanf(sql, "UPDATE item_instance SET owner_guid=%u WHERE guid=70", &owner) == 1)
            {
                m_backend.inventory.itemOwner = owner;
            }
            if (std::string(sql) == "DELETE FROM mail_items WHERE item_guid=70")
            {
                m_backend.inventory.attachment = false;
            }
            if (std::string(sql) == "DELETE FROM item_instance WHERE guid=70")
            {
                m_backend.inventory.itemExists = false;
            }
            if (std::string(sql) == "DELETE FROM guild WHERE guildid=7")
            {
                m_backend.inventory.guildExists = false;
            }
            if (std::string(sql) == "UPDATE mail SET cod=0 WHERE id=42")
            {
                m_backend.inventory.cod = 0;
            }
            if (std::sscanf(sql, "INSERT INTO mail (money) VALUES (%u)", &money) == 1)
            {
                m_backend.inventory.payment = money;
            }
            unsigned level, xp, mask, guid, playerClass;
            if (std::sscanf(sql, "UPDATE characters SET level=%u,xp=%u,at_login=(at_login & %u) WHERE guid=%u AND class=%u",
                &level, &xp, &mask, &guid, &playerClass) == 5)
            {
                m_backend.level = level;
                m_backend.xp = xp;
                m_backend.flags &= mask;
            }
            return true;
        }
        bool BeginTransaction() override
        {
            m_backend.events.push_back("begin");
            m_backend.savedLevel = m_backend.level;
            m_backend.savedXp = m_backend.xp;
            m_backend.savedFlags = m_backend.flags;
            m_backend.savedRecords = m_backend.records;
            m_backend.savedInventory = m_backend.inventory;
            return !m_backend.beginFailure;
        }
        bool CommitTransaction() override
        {
            m_backend.events.push_back("commit");
            if (m_backend.loseCommitReply)
            {
                // COMMIT reached the database, but the success reply was lost.
                m_backend.savedRecords = m_backend.records;
                m_backend.savedInventory = m_backend.inventory;
                return false;
            }
            return !m_backend.commitFailure;
        }
        bool RollbackTransaction() override
        {
            m_backend.events.push_back("rollback");
            m_backend.level = m_backend.savedLevel;
            m_backend.xp = m_backend.savedXp;
            m_backend.flags = m_backend.savedFlags;
            m_backend.records = m_backend.savedRecords;
            m_backend.inventory = m_backend.savedInventory;
            return true;
        }
    };
    class FakeDatabase : public Database
    {
    public:
        Backend backend;
        SqlConnection* CreateConnection() override { return new Connection(*this, backend); }
        // End the delay thread while the backend still exists (base dtor is later).
        ~FakeDatabase() override { HaltDelayThread(); }
    };
    // Replace only the world-bound participant at this seam. Begin, both preflight
    // passes, checked save propagation, commit/rollback and callbacks are the same
    // production InventoryTransaction API used by every migrated handler.
    struct InventoryParticipant
    {
        Database& db;
        unsigned guid;
        uint32 money = 100;
        InventoryItem item;
        std::vector<InventoryItem*> queue{&item};
        InventoryItem* stored = &item;
        bool blocked = false, failSave = false, takesAttachment = false;
        unsigned saves = 0, kicks = 0;
        std::function<void()> onDisconnect;
        PlayerPersistence::InventoryTransaction<InventoryParticipant>* transaction = nullptr;
        auto* GetInventoryTransaction() const { return transaction; }
        void SetInventoryTransaction(PlayerPersistence::InventoryTransaction<InventoryParticipant>* owner) { transaction = owner; }

        bool CanSaveInventory()
        {
            std::array<InventoryItem*,0> buyback{};
            std::vector<InventoryEnchantment> enchantments;
            return !blocked && PlayerPersistence::WithInventoryPreflight(buyback.begin(), buyback.end(), queue,
                enchantments, InventoryItem::Removed, [&](InventoryItem const*) { return stored; }, [] { return true; });
        }
        bool SaveInventoryAndGoldToDB()
        {
            if (!CanSaveInventory() || failSave)
            {
                return false;
            }
            ++saves;
            queue.clear();
            item.state = InventoryItem::Unchanged;
            return (!takesAttachment || db.PExecute("UPDATE item_instance SET owner_guid=%u WHERE guid=70", guid)) &&
                db.PExecute("UPDATE characters SET money=%u WHERE guid=%u", money, guid);
        }
        void BlockSavesAndDisconnect()
        {
            blocked = true;
            ++kicks;
            if (onDisconnect)
            {
                onDisconnect();
            }
        }
    };
    coa::State Next()
    {
        coa::State s;
        s.guid = 7; s.revision = 2; s.catalogRevision = std::string(64, 'a');
        s.entries = {{100, 1, 1, false, 123}};
        return s;
    }
}

TEST(Coa_sql_checked_commit_cas_failure_and_statement_rollback)
{
    FakeDatabase db;
    REQUIRE(db.Initialize("synthetic-no-network"));
    coa::SqlStore store(db, {12,20,0,1});
    auto next = Next(), previous = next;
    previous.revision = 1;
    REQUIRE(store.Commit(previous, next));
    CHECK((db.backend.events == std::vector<std::string>{"begin", "commit"}));
    REQUIRE(db.backend.statements.size() == 4);
    CHECK(db.backend.statements[1].find("AND state_revision=1") != std::string::npos);
    CHECK(db.backend.statements[1].find("WHERE guid=7") != std::string::npos);
    CHECK(db.backend.statements[1].find("AND level=20 AND xp=0") != std::string::npos);
    CHECK_EQ(db.backend.level, 20u); CHECK_EQ(db.backend.xp, 0u); CHECK_EQ(db.backend.flags, 2u);
    for (int failure = 0; failure < 5; ++failure)
    {
        db.backend.statements.clear(); db.backend.events.clear();
        db.backend.level = 19; db.backend.xp = 123; db.backend.flags = 3;
        db.backend.affected = failure == 0 ? "0" : "1";
        db.backend.commitFailure = failure == 1;
        db.backend.failStatement = failure == 2 ? 3 : 0;
        db.backend.countFailure = failure == 3;
        db.backend.beginFailure = failure == 4;
        CHECK(!store.Commit(previous, next));
        CHECK_EQ(db.backend.level, 19u); CHECK_EQ(db.backend.xp, 123u); CHECK_EQ(db.backend.flags, 3u);
        if (failure == 0 || failure == 3)
        {
            CHECK_EQ(db.backend.statements.size(), 2u);
        }
        if (failure != 4)
        {
            CHECK_STR(db.backend.events.back(), "rollback");
        }
    }
    db.backend.affected = "1";
    db.backend.countFailure = db.backend.beginFailure = false;
    db.backend.failStatement = 0;
    db.AllowAsyncTransactions();
    db.backend.events.clear();
    db.backend.commitFailure = true;
    CHECK(!store.Commit(previous, next));
    CHECK((db.backend.events == std::vector<std::string>{"begin", "commit", "rollback"}));
    db.backend.commitFailure = false;
    CHECK(store.Commit(previous, next));
}

TEST(Coa_sql_read_failure_is_not_missing_header_and_loadout_is_checked)
{
    FakeDatabase db;
    REQUIRE(db.Initialize("synthetic-no-network"));
    coa::SqlStore store(db, {12,10,0});
    coa::State loaded; loaded.guid = 999;
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Missing);
    CHECK_EQ(loaded.guid, 999u);
    db.backend.readFailure = true;
    CHECK(!coa::SqlStore::CheckSchema(db));
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Failed);
    CHECK_EQ(loaded.guid, 999u);
    db.backend.readFailure = false;
    db.backend.rows[0][5] = "7"; // Orphan entry is corruption, not a new player.
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Failed);
    db.backend.rows = {{"7", "0", "1", std::string(64, 'a'), "5", "7", "0", "100", "1", "1", "0", "123"}};
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Found);
    CHECK_EQ(loaded.guid, 7u); CHECK_EQ(loaded.revision, 5u);
    REQUIRE(loaded.entries.size() == 1);
    CHECK_EQ(loaded.entries[0].entryId, 100u); CHECK_EQ(loaded.entries[0].learnedTime, 123);
    auto valid = db.backend.rows;
    for (auto alteration : {std::pair<size_t, std::string>{1, "1"}, {2, "2"}, {6, "1"}, {9, "0"}, {10, "2"}, {11, "-1"}, {11, "9223372036854775808"}})
    {
        db.backend.rows = valid; db.backend.rows[0][alteration.first] = alteration.second;
        CHECK(store.Load(7, loaded) == coa::LoadStatus::Failed);
    }
    db.backend.rows.assign(1025, valid[0]);
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Failed);
}

TEST(Coa_sql_schema_requires_character_atomicity_and_positive_default)
{
    FakeDatabase db;
    REQUIRE(db.Initialize("synthetic-no-network"));
    db.backend.schema = true;
    CHECK(coa::SqlStore::CheckSchema(db));
    db.backend.engines = 2;
    CHECK(!coa::SqlStore::CheckSchema(db));
    db.backend.engines = 3;
    db.backend.rankDefault = "0";
    CHECK(!coa::SqlStore::CheckSchema(db));
    db.backend.rankDefault = "1";
    CHECK(coa::SqlStore::CheckSchema(db));
    CHECK(coa::SqlStore::CheckCreationSchema(db));
    db.backend.creationEngines = 25;
    CHECK(!coa::SqlStore::CheckCreationSchema(db));
}

TEST(Coa_creation_preflight_append_and_sql_failures_never_orphan_records)
{
    // Production SqlStore::Create owns both the preparation/append ordering and
    // the REAL checked transaction. Only the Player callbacks/connection are fake.
    for (int fault = 0; fault <= 15; ++fault)
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        db.backend.creation = true;
        coa::SqlStore store(db, {12,1,0});
        coa::State state;
        bool ready = false, appended = false, acknowledged = false;
        db.backend.executing = [&]
        {
            CHECK(ready); CHECK(appended); CHECK_EQ(state.revision, 0u);
        };
        if (fault >= 6 && fault <= 12)
        {
            db.backend.failStatement = size_t(fault - 5);
        }
        db.backend.commitFailure = fault == 13;
        db.backend.loseCommitReply = fault == 14;
        db.backend.beginFailure = fault == 15;
        auto result = store.Create(state, [&]
        {
            state.guid = 7;
            state.catalogRevision = std::string(64, 'a');
            // Identity, proficiency, equipment, final readiness faults.
            for (int stage = 1; stage <= 4; ++stage)
            {
                if (fault == stage)
                {
                    // Even incidental writes during preparation must stay queued.
                    db.Execute("INSERT INTO character_skills VALUES (7,43,1,5)");
                    return false;
                }
            }
            ready = true;
            return true;
        }, [&]
        {
            CHECK(ready); CHECK(db.backend.events.empty());
            SqlStatementID insert;
            auto character = db.CreateStatement(insert,
                "INSERT INTO `characters` (`guid`,`class`,`level`,`xp`) VALUES (?,?,?,?)");
            bool ok = character.PExecute(uint32(7), uint32(12), uint32(1), uint32(0));
            ok = ok && db.Execute("INSERT INTO item_instance VALUES (70,7)");
            ok = ok && db.Execute("INSERT INTO character_inventory VALUES (7,70)");
            ok = ok && db.Execute("INSERT INTO character_skills VALUES (7,43,1,5)");
            appended = ok;
            return ok && fault != 5;
        });
        acknowledged = result == coa::CreationStatus::Created;
        CHECK(acknowledged == (fault == 0));
        CHECK_EQ(state.revision, fault == 0 ? 1u : 0u);
        auto const& rows = db.backend.records;
        bool complete = fault == 0 || fault == 14;
        CHECK(rows.character == complete); CHECK(rows.header == complete);
        CHECK(rows.item == complete); CHECK(rows.inventory == complete); CHECK(rows.skill == complete);
        if (fault >= 1 && fault <= 5)
        {
            CHECK(db.backend.events.empty()); CHECK(db.backend.statements.empty());
        }
        for (auto const& sql : db.backend.statements)
        {
            CHECK(sql.find("DELETE FROM `characters`") == std::string::npos);
            CHECK(sql.find("DELETE FROM character_coa_state") == std::string::npos);
        }
    }
}

TEST(Coa_creation_collision_preserves_existing_character_or_header)
{
    for (bool existingCharacter : {false, true})
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        db.backend.creation = true;
        db.backend.records.header = true;
        db.backend.records.character = existingCharacter;
        db.backend.records.item = existingCharacter;
        coa::SqlStore store(db, {12,1,0});
        coa::State state; state.guid = 7; state.catalogRevision = std::string(64, 'a');
        auto result = store.Create(state, [] { return true; }, [&]
        {
            return db.Execute("INSERT INTO `characters` (`guid`,`class`,`level`,`xp`) VALUES (7,12,1,0)") &&
                db.Execute("INSERT INTO item_instance VALUES (71,7)");
        });
        CHECK(result == coa::CreationStatus::Failed);
        CHECK(db.backend.records.header);
        CHECK(db.backend.records.character == existingCharacter);
        CHECK(db.backend.records.item == existingCharacter);
        CHECK_EQ(state.revision, 0u);
    }
}

TEST(Coa_shared_save_preflight_preserves_mail_until_inventory_repair)
{
    for (bool creation : {false, true})
    {
        for (bool wrongItem : {false, true})
        {
            FakeDatabase db;
            REQUIRE(db.Initialize("synthetic-no-network"));
            db.backend.creation = creation;
            InventoryItem first{InventoryItem::New,0}, second{InventoryItem::New,1};
            InventoryItem removed{InventoryItem::Removed,2}, unrelated{InventoryItem::New,1};
            std::array<InventoryItem*,2> slots{{&first, wrongItem ? &unrelated : nullptr}};
            std::array<InventoryItem*,0> buyback{};
            std::vector<InventoryEnchantment> enchantments;
            std::vector<InventoryItem*> pendingItems{nullptr, &removed, &first, &second};
            Mail changed;
            changed.messageID = 42;
            changed.state = MAIL_STATE_CHANGED;
            changed.removedItems = {101};
            auto deleted = std::make_unique<Mail>();
            deleted->messageID = 43;
            deleted->state = MAIL_STATE_DELETED;
            bool mailsUpdated = true, spellsDirty = true;
            unsigned saveCalls = 0;
            coa::State state;
            state.guid = 7;
            state.catalogRevision = std::string(64, 'a');
            coa::SqlStore store(db, {12,1,0});
            auto queueSave = [&]
            {
                return PlayerPersistence::WithInventoryPreflight(buyback.begin(), buyback.end(), pendingItems, enchantments,
                    InventoryItem::Removed, [&](InventoryItem const* item)
                    {
                        return item->slot < slots.size() ? slots[item->slot] : nullptr;
                    }, [&]
                    {
                        ++saveCalls;
                        // Real Mail objects and the real DB queue, with save callbacks
                        // standing in for Player's dirty-state consumers in this seam.
                        bool ok = db.Execute("INSERT INTO `characters` (`guid`,`class`,`level`,`xp`) VALUES (7,12,1,0)");
                        ok = ok && db.PExecute("UPDATE mail SET checked=1 WHERE id=%u", changed.messageID);
                        for (auto attachment : changed.removedItems)
                        {
                            ok = ok && db.PExecute("DELETE FROM mail_items WHERE item_guid=%u", attachment);
                        }
                        changed.removedItems.clear();
                        changed.state = MAIL_STATE_UNCHANGED;
                        if (deleted)
                        {
                            ok = ok && db.PExecute("DELETE FROM mail WHERE id=%u", deleted->messageID);
                            deleted.reset();
                        }
                        mailsUpdated = false;
                        first.state = second.state = InventoryItem::Unchanged;
                        pendingItems.clear();
                        spellsDirty = false;
                        return ok;
                    });
            };
            auto save = [&]
            {
                if (creation)
                {
                    return store.Create(state, [] { return true; }, queueSave) == coa::CreationStatus::Created;
                }
                if (!db.BeginTransaction())
                {
                    return false;
                }
                if (!queueSave())
                {
                    db.RollbackTransaction();
                    return false;
                }
                return db.CommitTransaction();
            };
            CHECK(!save());
            CHECK_EQ(saveCalls, 0u);
            CHECK(mailsUpdated && spellsDirty);
            CHECK(changed.state == MAIL_STATE_CHANGED);
            CHECK(changed.removedItems == std::vector<uint32>{101});
            REQUIRE(deleted != nullptr);
            CHECK(deleted->state == MAIL_STATE_DELETED);
            CHECK_EQ(pendingItems.size(), 4u);
            CHECK(first.state == InventoryItem::New && second.state == InventoryItem::New);
            CHECK(db.backend.statements.empty() && db.backend.events.empty());
            CHECK_EQ(state.revision, 0u);

            slots[1] = &second;
            REQUIRE(save());
            CHECK_EQ(saveCalls, 1u);
            CHECK(!mailsUpdated && !spellsDirty && !deleted);
            CHECK(changed.state == MAIL_STATE_UNCHANGED && changed.removedItems.empty());
            CHECK(pendingItems.empty());
            CHECK(std::find(db.backend.statements.begin(), db.backend.statements.end(),
                "UPDATE mail SET checked=1 WHERE id=42") != db.backend.statements.end());
            CHECK(std::find(db.backend.statements.begin(), db.backend.statements.end(),
                "DELETE FROM mail_items WHERE item_guid=101") != db.backend.statements.end());
            CHECK(std::find(db.backend.statements.begin(), db.backend.statements.end(),
                "DELETE FROM mail WHERE id=43") != db.backend.statements.end());
            CHECK_EQ(state.revision, creation ? 1u : 0u);
        }
    }
}

TEST(Coa_shared_save_preflight_checks_buyback_before_state_normalization)
{
    InventoryItem item{InventoryItem::Removed,0};
    std::array<InventoryItem*,1> buyback{{&item}};
    std::vector<InventoryItem*> pending{nullptr, &item};
    std::vector<InventoryEnchantment> enchantments;
    InventoryItem* stored = nullptr;
    unsigned saves = 0;
    auto save = [&]
    {
        return PlayerPersistence::WithInventoryPreflight(buyback.begin(), buyback.end(), pending, enchantments,
            InventoryItem::Removed, [&](InventoryItem const*) { return stored; }, [&]
            {
                ++saves;
                item.state = InventoryItem::New;
                return true;
            });
    };
    CHECK(!save());
    CHECK_EQ(saves, 0u);
    CHECK(item.state == InventoryItem::Removed);
    stored = &item;
    CHECK(save());
    CHECK_EQ(saves, 1u);
    CHECK(item.state == InventoryItem::New);
    pending.clear();
    stored = nullptr;
    CHECK(save()); // Normal detached, unqueued buyback inventory must remain saveable.
    CHECK_EQ(saves, 2u);
}

TEST(Coa_shared_save_preflight_checks_items_queued_by_enchantment_updates)
{
    InventoryItem item{InventoryItem::Unchanged,0};
    std::array<InventoryItem*,0> buyback{};
    std::vector<InventoryItem*> pending;
    std::vector<InventoryEnchantment> enchantments{{&item,0,50}};
    InventoryItem* stored = nullptr;
    unsigned saves = 0;
    auto save = [&]
    {
        return PlayerPersistence::WithInventoryPreflight(buyback.begin(), buyback.end(), pending, enchantments,
            InventoryItem::Removed, [&](InventoryItem const*) { return stored; }, [&]
            {
                ++saves;
                item.duration = 50;
                item.state = InventoryItem::Unchanged;
                return true;
            });
    };
    CHECK(!save());
    CHECK_EQ(item.duration, 0u);
    CHECK(item.state == InventoryItem::Unchanged);
    CHECK_EQ(saves, 0u);
    stored = &item;
    CHECK(save());
    CHECK_EQ(saves, 1u);
    CHECK_EQ(item.duration, 50u);
}

TEST(Coa_inventory_transaction_trade_rejects_either_participant_before_mutation_and_retries)
{
    for (bool invalidSender : {false, true})
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        InventoryParticipant sender{db,1}, receiver{db,2};
        (invalidSender ? sender : receiver).stored = nullptr;
        bool acknowledged = false;
        auto trade = [&]
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> transaction(db, {&sender, &receiver});
            if (!transaction.Begin())
            {
                return false;
            }
            sender.money -= 25;
            receiver.money += 25;
            if (!transaction.Commit())
            {
                return false;
            }
            acknowledged = true;
            return true;
        };
        CHECK(!trade());
        CHECK(!acknowledged && !sender.blocked && !receiver.blocked);
        CHECK_EQ(sender.money, 100u); CHECK_EQ(receiver.money, 100u);
        CHECK_EQ(sender.saves + receiver.saves, 0u);
        CHECK_EQ(sender.queue.size(), 1u); CHECK_EQ(receiver.queue.size(), 1u);
        CHECK(db.backend.events.empty() && db.backend.statements.empty());
        sender.stored = &sender.item; receiver.stored = &receiver.item;
        REQUIRE(trade());
        CHECK(acknowledged);
        CHECK_EQ(db.backend.inventory.senderMoney, 75u);
        CHECK_EQ(db.backend.inventory.receiverMoney, 125u);
        CHECK((db.backend.events == std::vector<std::string>{"begin", "commit"}));
    }
}

TEST(Coa_inventory_transaction_late_trade_failures_block_both_saves_and_acknowledgements)
{
    for (unsigned fault = 0; fault < 9; ++fault)
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        db.AllowAsyncTransactions();
        InventoryParticipant sender{db,1}, receiver{db,2};
        bool published = false, cacheBlocked = false;
        auto noLogoutSave = [&](InventoryParticipant& participant)
        {
            participant.stored = &participant.item; // Even repair cannot unpoison this live object.
            CHECK(!participant.SaveInventoryAndGoldToDB());
        };
        sender.onDisconnect = [&] { noLogoutSave(sender); };
        receiver.onDisconnect = [&] { noLogoutSave(receiver); };
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> transaction(db, {&sender, &receiver});
            REQUIRE(transaction.Begin());
            transaction.OnCommit([&] { published = true; });
            transaction.OnFailure([&] { cacheBlocked = true; });
            sender.money -= 25; receiver.money += 25;
            if (fault == 0) { sender.stored = nullptr; }
            if (fault == 1) { receiver.stored = nullptr; }
            sender.failSave = fault == 2;
            receiver.failSave = fault == 3;
            db.backend.failStatement = fault == 4 ? 1 : fault == 5 ? 2 : 0;
            db.backend.beginFailure = fault == 6;
            db.backend.commitFailure = fault == 7;
            db.backend.loseCommitReply = fault == 8;
            CHECK(!transaction.Commit());
            CHECK(!transaction.Commit());
        }
        CHECK(!published && cacheBlocked && sender.blocked && receiver.blocked);
        CHECK_EQ(sender.kicks, 1u); CHECK_EQ(receiver.kicks, 1u);
        CHECK_EQ(db.backend.inventory.senderMoney, fault == 8 ? 75u : 100u);
        CHECK_EQ(db.backend.inventory.receiverMoney, fault == 8 ? 125u : 100u);
        if (fault < 4)
        {
            CHECK(db.backend.statements.empty() && db.backend.events.empty());
        }
        PlayerPersistence::InventoryTransaction<InventoryParticipant> retry(db, {&sender, &receiver});
        CHECK(!retry.Begin());
    }
}

TEST(Coa_inventory_transaction_mail_cod_is_atomic_and_delivery_follows_commit)
{
    // Success, early/late queue rejection, a false shortcut, each SQL statement,
    // failed SQL BEGIN/COMMIT and a lost COMMIT reply.
    for (unsigned fault = 0; fault < 12; ++fault)
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        db.AllowAsyncTransactions();
        InventoryParticipant receiver{db,2};
        Mail mail;
        mail.messageID = 42; mail.COD = 25; mail.AddItem(70,100);
        bool delivered = false, acknowledged = false;
        db.backend.executing = [&] { CHECK(!delivered && !acknowledged); };
        receiver.stored = fault == 1 ? nullptr : &receiver.item;
        db.backend.failStatement = fault >= 4 && fault <= 8 ? fault - 3 : 0;
        db.backend.beginFailure = fault == 9;
        db.backend.commitFailure = fault == 10;
        db.backend.loseCommitReply = fault == 11;
        auto takeAttachment = [&]
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> transaction(db, {&receiver});
            if (!transaction.Begin())
            {
                return false;
            }
            mail.RemoveItem(70); mail.removedItems.push_back(70);
            CHECK(db.PExecute("INSERT INTO mail (money) VALUES (%u)", mail.COD));
            transaction.OnCommit([&]
            {
                CHECK_STR(db.backend.events.back(), "commit");
                CHECK(!db.backend.inventory.attachment);
                CHECK_EQ(db.backend.inventory.receiverMoney, 75u);
                delivered = true;
            });
            receiver.money -= mail.COD;
            mail.COD = 0; mail.state = MAIL_STATE_CHANGED;
            receiver.takesAttachment = true;
            CHECK(db.Execute("DELETE FROM mail_items WHERE item_guid=70"));
            CHECK(db.Execute("UPDATE mail SET cod=0 WHERE id=42"));
            mail.removedItems.clear(); mail.state = MAIL_STATE_UNCHANGED;
            if (fault == 2) { receiver.stored = nullptr; }
            receiver.failSave = fault == 3;
            if (!transaction.Commit())
            {
                return false;
            }
            acknowledged = true;
            return true;
        };
        CHECK(takeAttachment() == (fault == 0));
        bool complete = fault == 0 || fault == 11;
        CHECK(delivered == (fault == 0)); CHECK(acknowledged == (fault == 0));
        CHECK(receiver.blocked == (fault > 1));
        CHECK_EQ(db.backend.inventory.receiverMoney, complete ? 75u : 100u);
        CHECK_EQ(db.backend.inventory.itemOwner, complete ? 2u : 0u);
        CHECK_EQ(db.backend.inventory.cod, complete ? 0u : 25u);
        CHECK_EQ(db.backend.inventory.payment, complete ? 25u : 0u);
        CHECK(db.backend.inventory.attachment == !complete);
        if (fault == 1)
        {
            CHECK_EQ(mail.COD, 25u); CHECK_EQ(mail.items.size(), 1u);
            CHECK_EQ(receiver.money, 100u); CHECK_EQ(receiver.saves, 0u);
            CHECK(db.backend.statements.empty());
        }
        if (receiver.blocked)
        {
            receiver.stored = &receiver.item;
            CHECK(!receiver.SaveInventoryAndGoldToDB());
        }
    }
}

TEST(Coa_inventory_transaction_abandonment_and_unavailable_begin)
{
    FakeDatabase db;
    InventoryParticipant sender{db,1}, receiver{db,2};
    {
        PlayerPersistence::InventoryTransaction<InventoryParticipant> unavailable(db, {&sender});
        CHECK(!unavailable.Begin());
        CHECK(!unavailable.Commit());
    }
    CHECK(!sender.blocked);
    REQUIRE(db.Initialize("synthetic-no-network"));
    bool published = false, discarded = false;
    {
        PlayerPersistence::InventoryTransaction<InventoryParticipant> abandoned(db, {&sender, nullptr, &receiver, &sender});
        REQUIRE(abandoned.Begin());
        CHECK(!abandoned.Begin()); // Does not detach/replace the owned queue.
        REQUIRE(db.Execute("UPDATE characters SET money=75 WHERE guid=1"));
        abandoned.OnCommit([&] { published = true; });
        abandoned.OnFailure([&] { discarded = true; });
    }
    CHECK(!published && discarded);
    CHECK_EQ(sender.kicks, 1u); CHECK_EQ(receiver.kicks, 1u);
    CHECK_EQ(db.backend.inventory.senderMoney, 100u);
    CHECK(db.backend.statements.empty());
    InventoryParticipant fresh{db,1}; // A new participant represents authoritative relog.
    PlayerPersistence::InventoryTransaction<InventoryParticipant> recovered(db, {&fresh, &fresh});
    REQUIRE(recovered.Begin());
    REQUIRE(recovered.Commit());
    CHECK_EQ(fresh.saves, 1u);
}

TEST(Coa_inventory_transaction_checked_sync_commit_fault)
{
    FakeDatabase db;
    REQUIRE(db.Initialize("synthetic-no-network"));
    InventoryParticipant sender{db,1}, receiver{db,2};
    PlayerPersistence::InventoryTransaction<InventoryParticipant> transaction(db, {&sender, &receiver});
    REQUIRE(transaction.Begin());
    sender.money = 75; receiver.money = 125;
    bool published = false;
    transaction.OnCommit([&] { published = true; });
    db.backend.commitFailure = true;
    CHECK(!transaction.Commit());
    CHECK(!published && sender.blocked && receiver.blocked);
    CHECK_EQ(db.backend.inventory.senderMoney, 100u);
    CHECK_EQ(db.backend.inventory.receiverMoney, 100u);
}

TEST(Coa_inventory_transaction_scopes_nested_saves_and_post_commit_rewards)
{
    for (bool failure : {false, true})
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        InventoryParticipant participant{db,1};
        bool reward = false;
        PlayerPersistence::InventoryTransaction<InventoryParticipant> transaction(db, {&participant});
        REQUIRE(transaction.Begin());
        CHECK(participant.GetInventoryTransaction() == &transaction);
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> nested(db, {&participant});
            CHECK(!nested.Begin());
            CHECK(!nested.Commit());
        }
        participant.GetInventoryTransaction()->OnCommit([&]
        {
            CHECK(participant.GetInventoryTransaction() == nullptr);
            PlayerPersistence::InventoryTransaction<InventoryParticipant> rewardMail(db, {});
            CHECK(rewardMail.Begin());
            CHECK(db.Execute("INSERT INTO mail (money) VALUES (25)"));
            CHECK(rewardMail.Commit());
            reward = true;
        });
        db.backend.commitFailure = failure;
        CHECK(transaction.Commit() == !failure);
        CHECK(reward == !failure);
        CHECK(participant.GetInventoryTransaction() == nullptr);
        CHECK_EQ(db.backend.inventory.payment, failure ? 0u : 25u);
    }
}

TEST(Coa_inventory_transaction_nested_database_ownership_rejects_without_touching_outer)
{
    for (bool ambient : {false, true})
    for (bool abortOuter : {false, true})
    for (unsigned innerKind = 0; innerKind < 3; ++innerKind)
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        InventoryParticipant participant{db,1}, other{db,2};
        bool innerPublished = false, innerFailed = false, outerPublished = false;
        CHECK(!db.IsTransactionActive());
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> outer(db, {&participant});
            REQUIRE(ambient ? db.BeginTransaction() : outer.Begin());
            outer.OnCommit([&] { outerPublished = true; });
            REQUIRE(db.Execute("UPDATE characters SET money=75 WHERE guid=1"));
            {
                // Overlap, disjoint and standalone-mail-shaped empty owners must
                // all reject an ambient queue, including non-inventory owners.
                PlayerPersistence::InventoryTransaction<InventoryParticipant> inner(db, innerKind == 0
                    ? std::initializer_list<InventoryParticipant*>{&participant}
                    : innerKind == 1 ? std::initializer_list<InventoryParticipant*>{&other}
                                    : std::initializer_list<InventoryParticipant*>{});
                inner.OnCommit([&] { innerPublished = true; });
                inner.OnFailure([&] { innerFailed = true; });
                bool begun = inner.Begin();
                CHECK(!begun);
                if (begun)
                {
                    other.money -= 25;
                    CHECK(db.Execute("INSERT INTO mail (money) VALUES (25)"));
                }
                CHECK(!inner.Commit());
            }
            CHECK(!innerPublished && !innerFailed);
            CHECK(db.IsTransactionActive());
            CHECK(participant.GetInventoryTransaction() == (ambient ? nullptr : &outer));
            CHECK(other.GetInventoryTransaction() == nullptr);
            CHECK_EQ(other.money, 100u);
            CHECK_EQ(participant.saves + other.saves + participant.kicks + other.kicks, 0u);
            CHECK(db.backend.events.empty() && db.backend.statements.empty());
            REQUIRE(db.Execute("UPDATE characters SET money=125 WHERE guid=2"));
            participant.money = 75;
            if (!abortOuter)
            {
                REQUIRE(ambient ? db.CommitTransactionChecked() : outer.Commit());
            }
            else if (ambient)
            {
                REQUIRE(db.RollbackTransaction());
            }
        }
        CHECK(!db.IsTransactionActive());
        CHECK(!innerPublished && !innerFailed);
        CHECK(outerPublished == (!ambient && !abortOuter));
        CHECK_EQ(db.backend.inventory.senderMoney, abortOuter ? 100u : 75u);
        CHECK_EQ(db.backend.inventory.receiverMoney, abortOuter ? 100u : 125u);
        CHECK_EQ(db.backend.inventory.payment, 0u);
        if (abortOuter)
        {
            CHECK(db.backend.events.empty() && db.backend.statements.empty());
        }
        else
        {
            REQUIRE(db.backend.statements.size() >= 2);
            CHECK_STR(db.backend.statements[0], "UPDATE characters SET money=75 WHERE guid=1");
            CHECK_STR(db.backend.statements[1], "UPDATE characters SET money=125 WHERE guid=2");
        }
    }
}

TEST(Coa_inventory_transaction_queue_ownership_is_per_database_and_thread)
{
    FakeDatabase db, independent;
    REQUIRE(db.Initialize("synthetic-no-network"));
    REQUIRE(independent.Initialize("synthetic-no-network"));
    REQUIRE(db.BeginTransaction());
    REQUIRE(db.Execute("UPDATE characters SET money=75 WHERE guid=1"));
    CHECK(db.IsTransactionActive());
    CHECK(!independent.IsTransactionActive());
    PlayerPersistence::InventoryTransaction<InventoryParticipant> otherDatabase(independent, {});
    REQUIRE(otherDatabase.Begin());
    REQUIRE(otherDatabase.Commit());
    bool threadInitiallyEmpty = false, threadBegan = false, threadCleared = false;
    std::thread thread([&]
    {
        DbThreadGuard guard(&db);
        threadInitiallyEmpty = !db.IsTransactionActive();
        threadBegan = db.BeginTransaction();
        if (threadBegan)
        {
            db.Execute("INSERT INTO mail (money) VALUES (25)");
            threadCleared = db.RollbackTransaction() && !db.IsTransactionActive();
        }
    });
    thread.join();
    CHECK(threadInitiallyEmpty && threadBegan && threadCleared);
    CHECK(db.IsTransactionActive());
    REQUIRE(db.CommitTransactionChecked());
    CHECK_EQ(db.backend.inventory.senderMoney, 75u);
    CHECK_EQ(db.backend.inventory.payment, 0u);
    REQUIRE(db.backend.statements.size() == 1);
    CHECK_STR(db.backend.statements[0], "UPDATE characters SET money=75 WHERE guid=1");
}

namespace
{
    struct CachedBankItem
    {
        FakeDatabase& db;
        unsigned& deletes;
        unsigned& freed;
        unsigned owner = 1;
        void RemoveFromWorld() {}
        void DeleteFromDB()
        {
            ++deletes;
            CHECK(db.Execute("DELETE FROM item_instance WHERE guid=70"));
        }
        ~CachedBankItem() { ++freed; }
    };
    struct CachedBankTab
    {
        std::array<CachedBankItem*,2> Slots{};
    };
}

TEST(Coa_guild_bank_quarantine_blocks_disband_and_durable_cache_deletion)
{
    for (bool commitFailure : {false, true})
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        db.backend.inventory.itemOwner = 1;
        unsigned deletes = 0, freed = 0, notifications = 0;
        bool bankBlocked = false, managerDropped = false;
        std::vector<CachedBankTab*> tabs{new CachedBankTab};
        auto* item = new CachedBankItem{db, deletes, freed};
        InventoryParticipant depositor{db,1}, otherLeader{db,2};
        {
            PlayerPersistence::InventoryTransaction<InventoryParticipant> deposit(db, {&depositor});
            REQUIRE(deposit.Begin());
            deposit.OnFailure([&] { bankBlocked = true; });
            tabs[0]->Slots[0] = item;
            item->owner = 0;
            REQUIRE(db.Execute("UPDATE item_instance SET owner_guid=0 WHERE guid=70"));
            db.backend.commitFailure = commitFailure;
            db.backend.failStatement = commitFailure ? 0 : 2;
            CHECK(!deposit.Commit());
        }
        CHECK(bankBlocked && depositor.blocked && !otherLeader.blocked);
        CHECK_EQ(item->owner, 0u);
        CHECK_EQ(db.backend.inventory.itemOwner, 1u);
        CHECK(db.backend.inventory.itemExists);
        db.backend.commitFailure = false;
        db.backend.failStatement = 0;
        auto statementsBefore = db.backend.statements.size();
        auto disband = [&]
        {
            // Same pre-metadata policy as Guild::CanDisband; item iteration and
            // GUID-only deletion below are the actual production cleanup helper.
            if (!GuildBankPersistence::CanCleanup(bankBlocked, true))
            {
                return false;
            }
            ++notifications;
            if (!db.Execute("DELETE FROM guild WHERE guildid=7") ||
                !GuildBankPersistence::DeleteItems(tabs, bankBlocked, true))
            {
                return false;
            }
            managerDropped = true;
            return true;
        };
        CHECK(!disband());
        CHECK(!GuildBankPersistence::DeleteItems(tabs, bankBlocked, true));
        CHECK_EQ(deletes, 0u); CHECK_EQ(freed, 0u); CHECK_EQ(notifications, 0u);
        CHECK(!managerDropped && db.backend.inventory.guildExists);
        CHECK(db.backend.inventory.itemExists);
        CHECK_EQ(db.backend.inventory.itemOwner, 1u);
        CHECK_EQ(db.backend.statements.size(), statementsBefore);
        CHECK_EQ(tabs.size(), 1u);
        // Destruction of stale memory neither deletes durable player items nor
        // clears quarantine to permit a subsequent destructive operation.
        CHECK(GuildBankPersistence::DeleteItems(tabs, bankBlocked, false));
        CHECK(tabs.empty()); CHECK_EQ(freed, 1u); CHECK_EQ(deletes, 0u);
        CHECK(!disband());
        CHECK(!GuildBankPersistence::DeleteItems(tabs, bankBlocked, true));
        CHECK(db.backend.inventory.itemExists && db.backend.inventory.guildExists);
        CHECK_EQ(db.backend.statements.size(), statementsBefore);
    }
}

TEST(Coa_guild_bank_cleanup_keeps_normal_durable_and_memory_only_behavior)
{
    for (bool durable : {false, true})
    {
        FakeDatabase db;
        REQUIRE(db.Initialize("synthetic-no-network"));
        unsigned deletes = 0, freed = 0;
        std::vector<CachedBankTab*> tabs{new CachedBankTab};
        tabs[0]->Slots[0] = new CachedBankItem{db, deletes, freed, 0};
        REQUIRE(GuildBankPersistence::DeleteItems(tabs, false, durable));
        CHECK(tabs.empty()); CHECK_EQ(freed, 1u); CHECK_EQ(deletes, durable ? 1u : 0u);
        CHECK(db.backend.inventory.itemExists == !durable);
        CHECK(GuildBankPersistence::DeleteItems(tabs, false, durable));
        CHECK_EQ(freed, 1u);
    }
}

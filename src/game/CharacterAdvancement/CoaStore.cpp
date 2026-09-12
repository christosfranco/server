// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaState.h"
#include "Database/DatabaseEnv.h"
#include <sstream>
#include <limits>
#include <stdexcept>

namespace coa
{
    bool SqlStore::CheckSchema(Database& m_database)
    {
        // Empty tables still produce a result. Never interpret a null query as empty.
        std::unique_ptr<QueryResult> columns(m_database.Query(
            "SELECT h.guid,h.active_loadout,h.loadout_count,h.catalog_revision,h.state_revision,"
            "e.guid,e.loadout,e.entry_id,e.rank,e.spell_rank_limit,e.locked,e.entry_time "
            "FROM (SELECT 1) s LEFT JOIN character_coa_state h ON 0 "
            "LEFT JOIN character_coa_entry e ON 0"));
        if (!columns)
        {
            return false;
        }
        std::unique_ptr<QueryResult> engines(m_database.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME IN ('characters','character_coa_state','character_coa_entry') AND ENGINE='InnoDB'"));
        if (!engines || engines->Fetch()[0].GetUInt32() != 3)
        {
            return false;
        }
        std::map<std::pair<std::string, std::string>, std::string> expected;
        for (auto const& table : {std::string("character_coa_state"), std::string("character_coa_entry")})
        {
            expected[{table, "guid"}] = "int";
        }
        for (auto const& name : {"active_loadout", "loadout_count"})
        {
            expected[{"character_coa_state", name}] = "tinyint";
        }
        expected[{"character_coa_state", "catalog_revision"}] = "char";
        expected[{"character_coa_state", "state_revision"}] = "bigint";
        for (auto const& name : {"entry_id", "rank", "spell_rank_limit"})
        {
            expected[{"character_coa_entry", name}] = "int";
        }
        expected[{"character_coa_entry", "loadout"}] = "tinyint";
        expected[{"character_coa_entry", "locked"}] = "tinyint";
        expected[{"character_coa_entry", "entry_time"}] = "bigint";
        std::unique_ptr<QueryResult> types(m_database.Query(
            "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE,COLUMN_TYPE,IS_NULLABLE,CHARACTER_MAXIMUM_LENGTH,COLLATION_NAME,COLUMN_DEFAULT "
            "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME IN ('character_coa_state','character_coa_entry')"));
        if (!types || types->GetRowCount() != expected.size())
        {
            return false;
        }
        do
        {
            auto f = types->Fetch();
            auto key = std::make_pair(f[0].GetCppString(), f[1].GetCppString());
            auto found = expected.find(key);
            if (found == expected.end() || f[2].GetCppString() != found->second || f[4].GetCppString() != "NO")
            {
                return false;
            }
            if (key.second == "spell_rank_limit" && f[7].GetCppString() != "1")
            {
                return false;
            }
            if (found->second == "char")
            {
                if (f[5].GetUInt32() != 64 || f[6].GetCppString() != "ascii_bin")
                {
                    return false;
                }
            }
            else if ((f[3].GetCppString().find("unsigned") != std::string::npos) != (key.second != "entry_time"))
            {
                return false;
            }
            expected.erase(found);
        } while (types->NextRow());
        std::unique_ptr<QueryResult> keys(m_database.Query(
            "SELECT TABLE_NAME,GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX SEPARATOR ',') "
            "FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME IN ('character_coa_state','character_coa_entry') AND INDEX_NAME='PRIMARY' "
            "GROUP BY TABLE_NAME ORDER BY TABLE_NAME"));
        if (!keys || keys->GetRowCount() != 2 || keys->Fetch()[0].GetCppString() != "character_coa_entry" ||
            keys->Fetch()[1].GetCppString() != "guid,loadout,entry_id" || !keys->NextRow())
        {
            return false;
        }
        return keys->Fetch()[0].GetCppString() == "character_coa_state" &&
            keys->Fetch()[1].GetCppString() == "guid";
    }

    LoadStatus SqlStore::Load(uint32_t guid, State& state)
    {
        std::unique_ptr<QueryResult> result(m_database.PQuery(
            "SELECT h.guid,h.active_loadout,h.loadout_count,h.catalog_revision,h.state_revision,"
            "e.guid,e.loadout,e.entry_id,e.rank,e.spell_rank_limit,e.locked,e.entry_time "
            "FROM (SELECT %u AS guid) s LEFT JOIN character_coa_state h ON h.guid=s.guid "
            "LEFT JOIN character_coa_entry e ON e.guid=s.guid ORDER BY e.loadout,e.entry_id LIMIT 1025", guid));
        if (!guid || !result || result->GetRowCount() > analytic::MaximumEntryCount)
        {
            return LoadStatus::Failed;
        }
        auto f = result->Fetch();
        if (f[0].IsNULL())
        {
            return f[5].IsNULL() ? LoadStatus::Missing : LoadStatus::Failed;
        }
        State loaded;
        loaded.guid = f[0].GetUInt32();
        loaded.catalogRevision = f[3].GetCppString();
        loaded.revision = f[4].GetUInt64();
        do
        {
            f = result->Fetch();
            if (f[0].GetUInt32() != guid || f[1].GetUInt32() != 0 || f[2].GetUInt32() != 1 ||
                !loaded.revision || f[3].IsNULL() || f[4].IsNULL())
            {
                return LoadStatus::Failed;
            }
            if (f[5].IsNULL())
            {
                continue;
            }
            if (f[5].GetUInt32() != guid || f[6].GetUInt32() || f[10].GetUInt32() > 1 ||
                f[11].GetCppString().empty() || f[11].GetCppString()[0] == '-' ||
                f[11].GetUInt64() > uint64_t(std::numeric_limits<int64_t>::max()))
            {
                return LoadStatus::Failed;
            }
            for (unsigned i = 6; i <= 11; ++i)
            {
                if (f[i].IsNULL())
                {
                    return LoadStatus::Failed;
                }
            }
            loaded.entries.push_back({f[7].GetUInt32(), f[8].GetUInt32(), f[9].GetUInt32(),
                f[10].GetUInt32() == 1, int64_t(f[11].GetUInt64())});
        } while (result->NextRow());
        try
        {
            (void)EntryRanks(loaded.entries);
        }
        catch (std::invalid_argument const&)
        {
            return LoadStatus::Failed;
        }
        state = std::move(loaded);
        return LoadStatus::Found;
    }

    bool SqlStore::CheckCreationSchema(Database& database)
    {
        // Every target of the initial full save must roll back together. The
        // normal post-save statistics and pet saves do not run during creation.
        std::unique_ptr<QueryResult> result(database.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND ENGINE='InnoDB' "
            "AND TABLE_NAME IN ('characters','character_coa_state','character_coa_entry',"
            "'character_inventory','item_instance','item_loot','character_gifts','character_spell',"
            "'character_spell_cooldown','character_action','character_aura','character_skills',"
            "'character_achievement','character_achievement_progress','character_reputation',"
            "'character_equipmentsets','character_glyphs','character_talent','character_battleground_data',"
            "'character_queststatus','character_queststatus_daily','character_queststatus_weekly',"
            "'character_queststatus_monthly','character_tutorial','mail','mail_items')"));
        return result && result->Fetch()[0].GetUInt32() == 26;
    }

    CreationStatus SqlStore::Create(State& state, std::function<bool()> const& prepare,
        std::function<bool()> const& appendCharacter)
    {
        if (state.revision || !prepare || !appendCharacter || !m_database.BeginTransaction())
        {
            return CreationStatus::Failed;
        }
        // BeginTransaction only starts the thread-local queue. No SQL executes
        // until preparation/readiness and the full new-character save are staged.
        try
        {
            if (!prepare() || state.revision || !state.entries.empty())
            {
                m_database.RollbackTransaction();
                return CreationStatus::Rejected;
            }
            State next = state;
            next.revision = 1;
            if (!appendCharacter() || !Append(state, next))
            {
                m_database.RollbackTransaction();
                return CreationStatus::Rejected;
            }
            if (!m_database.CommitTransactionChecked())
            {
                return CreationStatus::Failed;
            }
            state = std::move(next);
            return CreationStatus::Created;
        }
        catch (std::exception const& error)
        {
            m_database.RollbackTransaction();
            sLog.outError("CoA creation staging failed: %s", error.what());
            return CreationStatus::Failed;
        }
    }

    bool SqlStore::Append(State const& previous, State const& next)
    {
        if (!previous.guid || next.guid != previous.guid || next.revision != previous.revision + 1 ||
            next.catalogRevision.size() != 64 || next.entries.size() > analytic::MaximumEntryCount ||
            m_progress.level < 1 || m_progress.level > 60 || m_progress.playerClass < 12 || m_progress.playerClass > 32 ||
            analytic::BuildEntriesStatePacket(next.entries).GetOpcode() != analytic::EntriesStateOpcode)
        {
            return false;
        }
        std::string hash = next.catalogRevision;
        m_database.escape_string(hash);
        std::ostringstream context;
        // New characters must already have their INSERT queued, never a DELETE
        // or upsert. Existing characters update context atomically with the CAS.
        std::ostringstream character;
        character << "UPDATE characters SET level=" << m_progress.level << ",xp=" << m_progress.xp
            << ",at_login=(at_login & " << ~m_progress.clearAtLogin << ") WHERE guid=" << next.guid
            << " AND class=" << m_progress.playerClass;
        bool ok = m_database.Execute(character.str().c_str());
        context << " EXISTS (SELECT 1 FROM characters WHERE guid=" << next.guid
            << " AND class=" << m_progress.playerClass << " AND level=" << m_progress.level
            << " AND xp=" << m_progress.xp << " AND (at_login & " << m_progress.clearAtLogin << ")=0)";
        std::ostringstream header;
        if (!previous.revision)
        {
            header << "INSERT INTO character_coa_state (guid,active_loadout,loadout_count,catalog_revision,state_revision) SELECT "
                << next.guid << ",0,1,'" << hash << "',1 FROM DUAL";
            header << " WHERE " << context.str()
                << " AND NOT EXISTS (SELECT 1 FROM character_coa_entry WHERE guid=" << next.guid << ')';
        }
        else
        {
            header << "UPDATE character_coa_state SET state_revision=" << next.revision
                << " WHERE guid=" << next.guid << " AND state_revision=" << previous.revision
                << " AND active_loadout=0 AND loadout_count=1 AND catalog_revision='" << hash << "'";
            header << " AND " << context.str();
        }
        ok = ok && m_database.ExecuteExpectedRows(header.str().c_str(), 1);
        ok = ok && m_database.PExecute("DELETE FROM character_coa_entry WHERE guid=%u", next.guid);
        for (auto const& e : next.entries)
        {
            std::ostringstream sql;
            sql << "INSERT INTO character_coa_entry (guid,loadout,entry_id,`rank`,spell_rank_limit,locked,entry_time) VALUES ("
                << next.guid << ",0," << e.entryId << ',' << e.rank << ',' << e.spellRankLimit
                << ',' << unsigned(e.locked) << ',' << e.learnedTime << ')';
            ok = ok && m_database.Execute(sql.str().c_str());
        }
        return ok;
    }

    bool SqlStore::Commit(State const& previous, State const& next)
    {
        if (!m_database.BeginTransaction())
        {
            return false;
        }
        if (!Append(previous, next))
        {
            m_database.RollbackTransaction();
            return false;
        }
        return m_database.CommitTransactionChecked();
    }
}

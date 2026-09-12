// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_STATE_H
#define MANGOS_COA_STATE_H
#include "CoaCatalog.h"
#include "CoaProtocol.h"
#include <functional>
class Database;

namespace coa
{
    struct State
    {
        uint32_t guid = 0;
        uint64_t revision = 0;
        std::string catalogRevision;
        std::vector<analytic::CoaEntry> entries;
    };
    enum class LoadStatus { Found, Missing, Failed };
    enum class ApplyStatus { Applied, NoChange, Rejected, Failed };
    enum class MutationSource { Request, LevelChange, Reset };
    enum class CreationStatus { Created, Rejected, Failed };
    struct CharacterProgress
    {
        uint32_t playerClass, level, xp;
        uint32_t clearAtLogin = 0;
    };
    class Store
    {
    public:
        virtual ~Store() = default;
        virtual LoadStatus Load(uint32_t guid, State& state) = 0;
        virtual bool Commit(State const& previous, State const& next) = 0;
    };
    class SqlStore final : public Store
    {
    public:
        SqlStore(Database& database, CharacterProgress progress) : m_database(database), m_progress(progress) {}
        static bool CheckSchema(Database& database);
        static bool CheckCreationSchema(Database& database);
        CreationStatus Create(State& state, std::function<bool()> const& prepare,
            std::function<bool()> const& appendCharacter);
        LoadStatus Load(uint32_t guid, State& state) override;
        bool Commit(State const& previous, State const& next) override;
    private:
        bool Append(State const& previous, State const& next);
        Database& m_database;
        CharacterProgress m_progress;
    };
    Ranks EntryRanks(std::vector<analytic::CoaEntry> const& entries);
    // No Player or SQL globals: tests inject a store and verify the commit boundary.
    ApplyStatus Replace(Catalog const& catalog, Store& store, State& current,
        uint32_t playerClass, uint32_t level, std::vector<analytic::CoaEntry> const& desired,
        int64_t now, Build& installed, std::string& error, MutationSource source = MutationSource::Request);
    bool ValidateLoaded(Catalog const& catalog, State const& state, uint32_t guid,
        uint32_t playerClass, uint32_t level, Build& build);
}
#endif

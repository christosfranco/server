// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_INVENTORY_TRANSACTION_H
#define MANGOS_INVENTORY_TRANSACTION_H
#include "Database/Database.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <vector>

namespace PlayerPersistence
{
    // The operation owns one queue, including mail and shared-storage writes.
    // Begin precedes mutations; an abandoned/failed operation cannot be autosaved.
    template<class Participant>
    class InventoryTransaction
    {
    public:
        InventoryTransaction(Database& database, std::initializer_list<Participant*> participants)
            : m_database(database)
        {
            for (auto* participant : participants)
            {
                if (participant && std::find(m_participants.begin(), m_participants.end(), participant) == m_participants.end())
                {
                    m_participants.push_back(participant);
                }
            }
        }
        InventoryTransaction(InventoryTransaction const&) = delete;
        InventoryTransaction& operator=(InventoryTransaction const&) = delete;
        ~InventoryTransaction() { Abort(); }

        [[nodiscard]] bool Begin()
        {
            if (m_started)
            {
                return false;
            }
            m_started = true;
            // Participants do not identify queue ownership: standalone mail has
            // none, and a script may name an entirely different player.
            if (m_database.IsTransactionActive())
            {
                return false;
            }
            for (auto* participant : m_participants)
            {
                if (participant->GetInventoryTransaction() || !participant->CanSaveInventory())
                {
                    return false;
                }
            }
            m_active = m_database.BeginTransaction();
            if (m_active)
            {
                for (auto* participant : m_participants)
                {
                    participant->SetInventoryTransaction(this);
                }
            }
            return m_active;
        }

        bool IsActive() const { return m_active; }
        void OnCommit(std::function<void()> action) { m_committed.push_back(std::move(action)); }
        void OnFailure(std::function<void()> action) { m_failed.push_back(std::move(action)); }

        [[nodiscard]] bool Commit()
        {
            if (!m_active)
            {
                return false;
            }
            // Recheck every participant before consuming even the first queue.
            for (auto* participant : m_participants)
            {
                if (!participant->CanSaveInventory())
                {
                    Abort();
                    return false;
                }
            }
            for (auto* participant : m_participants)
            {
                if (!participant->SaveInventoryAndGoldToDB())
                {
                    Abort();
                    return false;
                }
            }
            if (!m_database.CommitTransactionChecked())
            {
                Abort();
                return false;
            }
            m_active = false;
            for (auto* participant : m_participants)
            {
                participant->SetInventoryTransaction(nullptr);
            }
            m_failed.clear();
            for (auto& action : m_committed)
            {
                action();
            }
            m_committed.clear();
            return true;
        }

    private:
        void Abort()
        {
            if (!m_active)
            {
                return;
            }
            m_active = false;
            m_database.RollbackTransaction();
            for (auto* participant : m_participants)
            {
                participant->SetInventoryTransaction(nullptr);
            }
            // Also covers an ambiguous COMMIT reply. Only a fresh authoritative
            // load may enable saving again, never a repaired queue on this object.
            for (auto* participant : m_participants)
            {
                participant->BlockSavesAndDisconnect();
            }
            for (auto& action : m_failed)
            {
                action();
            }
            m_failed.clear();
            m_committed.clear();
        }

        Database& m_database;
        std::vector<Participant*> m_participants;
        std::vector<std::function<void()>> m_committed, m_failed;
        bool m_started = false, m_active = false;
    };
}

class Player;
using PlayerInventoryTransaction = PlayerPersistence::InventoryTransaction<Player>;
#endif

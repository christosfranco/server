// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_COMBAT_INTEGRATION_H
#define MANGOS_COA_COMBAT_INTEGRATION_H

#include "CoaCombatRules.h"
#include "SharedDefines.h"
#include <memory>
#include <vector>

class Unit;
class Player;
class Spell;
class SpellAuraHolder;

// These objects exist only during synchronous map-thread preparation/publication.
// Delayed work lives exclusively as weak identities in combat::State::pending.
class CoaCombatTransaction
{
public:
    CoaCombatTransaction(Player& owner, coa::combat::Event event);
    CoaCombatTransaction(Player& owner, std::vector<coa::combat::Event> events);
    ~CoaCombatTransaction();
    SpellCastResult Prepare();
    bool Publish();
    void ReleaseCasts();
    void ReservePower(int32 type, uint32 cost);
    uint32 SuppressSlots() const { return m_plan.SuppressSlots(); }
private:
    struct HolderChange
    {
        Unit* target;
        coa::combat::Identity targetLife;
        uint32 spell;
        SpellAuraHolder* previous;
        std::unique_ptr<SpellAuraHolder> replacement;
    };
    Player& m_owner;
    std::vector<coa::combat::Event> m_events;
    coa::combat::Context m_context;
    coa::combat::Plan m_plan;
    std::vector<coa::combat::Plan> m_plans;
    std::array<uint64, MAX_POWERS + 1> m_powerCosts{};
    std::vector<HolderChange> m_holders;
    std::vector<std::unique_ptr<Spell>> m_casts;
    bool m_ready = false;
};

namespace CoaCombatIntegration
{
    coa::combat::Identity Identify(Unit const& unit);
    Unit* Resolve(Unit& owner, coa::combat::Identity const& identity);
    SpellCastResult CastResult(coa::combat::Status status);
    bool IsCounterOnlyComponent(uint32 spell, uint32 slot, uint32 aura, int32 value);
}
#endif

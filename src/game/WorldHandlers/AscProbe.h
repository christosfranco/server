/**
 * AscProbe -- the world's own account of what a cast, a hit or a hand-in did,
 * written by the core, one JSON object per line.
 *
 * This is the C++ successor of the Eluna script `spellprobe.lua`: the same
 * event names, the same field names, the same file shape, so the gates that
 * read `spellprobe.jsonl` read this without change. It exists because a
 * screenshot proves the client drew something; it cannot prove the spell
 * existed on the server, hit anything, or killed it. A line here is the
 * server saying so.
 *
 * Off unless `Ascension.Probe.Path` names a file in mangosd.conf. Every hook
 * is a plain function call at the point the core already informs its script
 * engines; none of them alters what it observes.
 */
#ifndef MANGOS_H_ASC_PROBE
#define MANGOS_H_ASC_PROBE

#include "Platform/Define.h"

class Creature;
class GameObject;
class Item;
class Player;
class Quest;
class Spell;
class Unit;

namespace AscProbe
{
    /// Read `Ascension.Probe.Path` from the loaded config; empty disables.
    void Init();
    bool Enabled();

    // Combat -- what test/ascension_client_combat_test.py and the bot gate read.
    void Cast(Player* caster, Spell* spell);
    void SpellHit(Creature* victim, Unit* caster, uint32 spellId);
    void Damage(Creature* victim, Unit* attacker, uint32 amount);
    void Death(Creature* victim, Unit* killer);
    void Kill(Player* killer, Creature* victim);
    void EnterCombat(Creature* creature, Unit* target);

    // Friendly interaction -- the things a player does that are not a fight.
    void LootItem(Player* looter, Item* item, uint32 count);
    void MoneyChange(Player* player, int32 amount);
    void QuestAbandon(Player* player, uint32 questId);
    void QuestAccept(Player* player, Creature* giver, Quest const* quest);
    void QuestReward(Player* player, Creature* giver, Quest const* quest, uint32 opt);
    void GossipHello(Player* player, Creature* npc);
    void GossipSelect(Player* player, Creature* npc, uint32 sender, uint32 action);
    void GameObjectUse(Player* player, GameObject* go);
    void LootStateChanged(GameObject* go, uint32 state);
}

#endif

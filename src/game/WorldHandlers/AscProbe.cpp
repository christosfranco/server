/**
 * AscProbe -- see AscProbe.h. The successor of tools/eluna/spellprobe.lua.
 *
 * Field names and event kinds are the Lua's, verbatim, because the gates
 * that read the file were written against them:
 *
 *   cast          spell, cast_ms, caster_*, target_*
 *   spellhit      spell, victim_*, caster_*
 *   damage        amount, victim_*, attacker_*
 *   death         victim_*, killer_*
 *   kill          killer_*, victim_*
 *   enter_combat  creature_*, target_*
 *   loot_item     item, item_name, count, looter_*
 *   money_change  amount, player_*, player_x/y/z/money
 *   quest_abandon quest, player_*
 *   quest_accept  quest, quest_title, player_*, giver_*
 *   quest_reward  quest, quest_title, opt, player_*, giver_*
 *   gossip_hello  player_*, npc_*
 *   gossip_select sender, action, player_*, npc_*
 *   go_use        player_*, player_x/y/z, go_*
 *   go_loot_state state, go_*
 *
 * where unit fields are `<prefix>_name/_entry/_guid/_hp/_maxhp` and
 * gameobject fields `<prefix>_name/_entry/_guid/_x/_y/_z/_loot_state/_go_state`.
 * Numbers are numbers; strings are JSON-escaped. A null pointer produces no
 * fields for that prefix, never a lost line.
 */
#include "AscProbe.h"

#include "Config/Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Item.h"
#include "Player.h"
#include "QuestDef.h"
#include "Spell.h"
#include "Unit.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace
{
    std::string g_path;
    std::mutex g_mutex;

    void Escape(std::string& out, char const* s)
    {
        for (; s && *s; ++s)
        {
            unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"') { out += "\\\""; }
            else if (c == '\\') { out += "\\\\"; }
            else if (c == '\n') { out += "\\n"; }
            else if (c == '\r') { out += "\\r"; }
            else if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else { out += static_cast<char>(c); }
        }
    }

    class Line
    {
        public:
            explicit Line(char const* kind)
            {
                char stamp[16];
                std::time_t now = std::time(nullptr);
                std::tm tm;
                localtime_r(&now, &tm);
                std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
                m_text = "{\"t\":\"";
                m_text += stamp;
                m_text += "\",\"kind\":\"";
                m_text += kind;
                m_text += "\"";
            }
            Line& Num(char const* key, long long v)
            {
                m_text += ",\"";
                m_text += key;
                m_text += "\":";
                m_text += std::to_string(v);
                return *this;
            }
            Line& Flt(char const* key, double v)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6g", v);
                m_text += ",\"";
                m_text += key;
                m_text += "\":";
                m_text += buf;
                return *this;
            }
            Line& Str(char const* key, char const* v)
            {
                m_text += ",\"";
                m_text += key;
                m_text += "\":\"";
                Escape(m_text, v);
                m_text += "\"";
                return *this;
            }
            Line& Unit(char const* prefix, ::Unit const* u)
            {
                if (!u) { return *this; }
                std::string p(prefix);
                Str((p + "_name").c_str(), u->GetName());
                Num((p + "_entry").c_str(), u->GetEntry());
                Num((p + "_guid").c_str(), u->GetGUIDLow());
                Num((p + "_hp").c_str(), u->GetHealth());
                Num((p + "_maxhp").c_str(), u->GetMaxHealth());
                return *this;
            }
            Line& Go(char const* prefix, ::GameObject const* g)
            {
                if (!g) { return *this; }
                std::string p(prefix);
                Str((p + "_name").c_str(), g->GetName());
                Num((p + "_entry").c_str(), g->GetEntry());
                Num((p + "_guid").c_str(), g->GetGUIDLow());
                Flt((p + "_x").c_str(), g->Where().X());
                Flt((p + "_y").c_str(), g->Where().Y());
                Flt((p + "_z").c_str(), g->Where().Z());
                Num((p + "_loot_state").c_str(), g->getLootState());
                Num((p + "_go_state").c_str(), g->GetGoState());
                return *this;
            }
            Line& WhereIs(char const* prefix, ::Player const* p)
            {
                if (!p) { return *this; }
                std::string s(prefix);
                Flt((s + "_x").c_str(), p->Where().X());
                Flt((s + "_y").c_str(), p->Where().Y());
                Flt((s + "_z").c_str(), p->Where().Z());
                Num((s + "_money").c_str(), p->GetMoney());
                return *this;
            }
            void Write()
            {
                m_text += "}\n";
                std::lock_guard<std::mutex> lock(g_mutex);
                if (FILE* f = std::fopen(g_path.c_str(), "a"))
                {
                    std::fputs(m_text.c_str(), f);
                    std::fclose(f);
                }
            }
        private:
            std::string m_text;
    };
}

namespace AscProbe
{
    void Init()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = sConfig.GetStringDefault("Ascension.Probe.Path", "");
        if (!g_path.empty())
        {
            sLog.outString("Ascension probe: writing world events to %s", g_path.c_str());
        }
    }

    bool Enabled() { return !g_path.empty(); }

    void Cast(Player* caster, Spell* spell)
    {
        if (!Enabled() || !caster || !spell || !spell->m_spellInfo) { return; }
        Line("cast").Num("spell", spell->m_spellInfo->ID).Num("cast_ms", spell->GetCastTime())
            .Unit("caster", caster).Unit("target", spell->m_targets.getUnitTarget()).Write();
    }

    void SpellHit(Creature* victim, Unit* caster, uint32 spellId)
    {
        if (!Enabled() || !victim) { return; }
        Line("spellhit").Num("spell", spellId).Unit("victim", victim).Unit("caster", caster).Write();
    }

    void Damage(Creature* victim, Unit* attacker, uint32 amount)
    {
        if (!Enabled() || !victim) { return; }
        Line("damage").Num("amount", amount).Unit("victim", victim).Unit("attacker", attacker).Write();
    }

    void Death(Creature* victim, Unit* killer)
    {
        if (!Enabled() || !victim) { return; }
        Line("death").Unit("victim", victim).Unit("killer", killer).Write();
    }

    void Kill(Player* killer, Creature* victim)
    {
        if (!Enabled() || !killer || !victim) { return; }
        Line("kill").Unit("killer", killer).Unit("victim", victim).Write();
    }

    void EnterCombat(Creature* creature, Unit* target)
    {
        if (!Enabled() || !creature) { return; }
        Line("enter_combat").Unit("creature", creature).Unit("target", target).Write();
    }

    void LootItem(Player* looter, Item* item, uint32 count)
    {
        if (!Enabled() || !looter) { return; }
        Line line("loot_item");
        if (item)
        {
            line.Num("item", item->GetEntry());
            if (ItemPrototype const* proto = item->GetProto()) { line.Str("item_name", proto->Name1); }
        }
        line.Num("count", count).Unit("looter", looter).Write();
    }

    void MoneyChange(Player* player, int32 amount)
    {
        if (!Enabled() || !player) { return; }
        Line("money_change").Num("amount", amount).Unit("player", player).WhereIs("player", player).Write();
    }

    void QuestAbandon(Player* player, uint32 questId)
    {
        if (!Enabled() || !player) { return; }
        Line("quest_abandon").Num("quest", questId).Unit("player", player).Write();
    }

    void QuestAccept(Player* player, Creature* giver, Quest const* quest)
    {
        if (!Enabled() || !player || !quest) { return; }
        Line("quest_accept").Num("quest", quest->GetQuestId()).Str("quest_title", quest->GetTitle().c_str())
            .Unit("player", player).WhereIs("player", player).Unit("giver", giver).Write();
    }

    void QuestReward(Player* player, Creature* giver, Quest const* quest, uint32 opt)
    {
        if (!Enabled() || !player || !quest) { return; }
        Line("quest_reward").Num("quest", quest->GetQuestId()).Str("quest_title", quest->GetTitle().c_str())
            .Num("opt", opt).Unit("player", player).WhereIs("player", player).Unit("giver", giver).Write();
    }

    void GossipHello(Player* player, Creature* npc)
    {
        if (!Enabled() || !player) { return; }
        Line("gossip_hello").Unit("player", player).WhereIs("player", player).Unit("npc", npc).Write();
    }

    void GossipSelect(Player* player, Creature* npc, uint32 sender, uint32 action)
    {
        if (!Enabled() || !player) { return; }
        Line("gossip_select").Num("sender", sender).Num("action", action)
            .Unit("player", player).Unit("npc", npc).Write();
    }

    void GameObjectUse(Player* player, GameObject* go)
    {
        if (!Enabled() || !player) { return; }
        Line("go_use").Unit("player", player).WhereIs("player", player).Go("go", go).Write();
    }

    void LootStateChanged(GameObject* go, uint32 state)
    {
        if (!Enabled() || !go) { return; }
        Line("go_loot_state").Num("state", state).Go("go", go).Write();
    }
}

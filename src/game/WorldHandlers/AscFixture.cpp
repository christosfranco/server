/**
 * AscFixture -- see AscFixture.h. The three Eluna fixtures, in the core.
 *
 * PLAN 23.6. Command words, argument order, JSON keys and file paths are the
 * Lua's, so no gate changed. Where the Lua called an Eluna binding this calls
 * the same core API the binding wrapped; where the Lua guarded with pcall this
 * checks the pointer, because a fixture that throws inside a chat handler
 * would take the session down and the gate would read a disconnect instead of
 * a red.
 */

#include "AscFixture.h"

#include "Config/Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "MapManager.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "TemporarySummon.h"
#include "SpellAuras.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::mutex g_mutex;
    std::string g_resources;
    std::string g_items;
    std::string g_dungeons;

    // The stock 3.3.5a weapon/armour proficiency SkillLine ids, verbatim from
    // items_probe.lua's GEAR_SKILLS. A custom CoA class is never granted them,
    // so CanUseItem would refuse a fixture-granted weapon with
    // EQUIP_ERR_NO_REQUIRED_PROFICIENCY before the slot machinery is reached.
    uint16 const GEAR_SKILLS[] = {
        43,  44,  45,  46,  54,  55,  95,  136, 160, 172,
        173, 176, 226, 227, 228, 229,
        293, 413, 414, 415, 433,
    };

    // The Lua spawned its target at the melee distance by default.
    float const MELEE_DIST = 4.0f;

    void Escape(std::string& out, char const* text)
    {
        for (unsigned char const* p = (unsigned char const*)text; p && *p; ++p)
        {
            unsigned char c = *p;
            if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
            else if (c == '\n') { out += "\\n"; }
            else if (c == '\r') { out += "\\r"; }
            else if (c == '\t') { out += "\\t"; }
            else if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else { out += char(c); }
        }
    }

    /// One JSON object per line, the shape the Lua's emit() wrote.
    class Line
    {
        public:
            Line(std::string const& path, char const* kind) : m_path(path)
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
                m_text += ",\""; m_text += key; m_text += "\":";
                m_text += std::to_string(v);
                return *this;
            }
            Line& Flt(char const* key, double v)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6g", v);
                m_text += ",\""; m_text += key; m_text += "\":"; m_text += buf;
                return *this;
            }
            Line& Bool(char const* key, bool v)
            {
                m_text += ",\""; m_text += key; m_text += "\":";
                m_text += v ? "true" : "false";
                return *this;
            }
            Line& Str(char const* key, char const* v)
            {
                m_text += ",\""; m_text += key; m_text += "\":\"";
                Escape(m_text, v ? v : "");
                m_text += "\"";
                return *this;
            }
            void Write()
            {
                m_text += "}\n";
                std::lock_guard<std::mutex> lock(g_mutex);
                if (m_path.empty()) { return; }
                if (FILE* f = std::fopen(m_path.c_str(), "a"))
                {
                    std::fputs(m_text.c_str(), f);
                    std::fclose(f);
                }
            }
        private:
            std::string m_path;
            std::string m_text;
    };

    std::vector<std::string> Split(std::string const& s)
    {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string word;
        while (in >> word) { out.push_back(word); }
        return out;
    }

    long long Int(std::vector<std::string> const& a, size_t i, long long fallback = 0)
    {
        if (i >= a.size()) { return fallback; }
        try { return std::stoll(a[i]); } catch (...) { return fallback; }
    }

    float Flt(std::vector<std::string> const& a, size_t i, float fallback = 0.0f)
    {
        if (i >= a.size()) { return fallback; }
        try { return std::stof(a[i]); } catch (...) { return fallback; }
    }

    /// The creature this fixture spawned for `player`, re-fetched in THIS
    /// event: the Lua learned the hard way that a stored userdata is invalid
    /// in a later event, so the id is what is kept, never the pointer.
    // The FULL ObjectGuid, not the counter: a creature's guid carries its
    // entry (HIGHGUID_UNIT | entry | counter), so a counter alone does not
    // address it and GetAnyTypeCreature returns nothing.
    std::map<uint32, ObjectGuid> g_spawned;   // player guidlow -> creature guid

    Creature* FindSpawned(Player* player)
    {
        auto it = g_spawned.find(player->GetGUIDLow());
        if (it == g_spawned.end()) { return nullptr; }
        return player->GetMap()->GetAnyTypeCreature(it->second);
    }

    /// Nearest creature of `entry` within `range` of the player.
    Creature* NearestOf(Player* player, uint32 entry, float range)
    {
        Creature* best = nullptr;
        float bestDist = range;
        std::list<Creature*> found;
        MaNGOS::AllCreaturesOfEntryInRangeCheck check(player, entry, range);
        MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(found, check);
        Cell::VisitGridObjects(player, searcher, range);
        for (Creature* c : found)
        {
            if (!c || !c->IsInWorld()) { continue; }
            float d = player->Where().DistanceTo(c->Where());
            if (!best || d < bestDist) { best = c; bestDist = d; }
        }
        return best;
    }

    // ---------------------------------------------------------------- rp
    bool Resources(Player* player, std::string const& verb,
                   std::vector<std::string> const& a, std::string const& rest)
    {
        std::string const& log = g_resources;
        if (verb == "ping")
        {
            Line(log, "pong").Str("nonce", rest.c_str())
                .Num("map", player->GetMapId())
                .Flt("x", player->Where().X()).Flt("y", player->Where().Y())
                .Flt("z", player->Where().Z()).Write();
            return true;
        }
        if (verb == "tele")
        {
            if (a.size() < 4)
            {
                Line(log, "tele").Bool("ok", false)
                    .Str("error", ("bad args: " + rest).c_str()).Write();
                return true;
            }
            uint32 map = uint32(Int(a, 0));
            float x = Flt(a, 1), y = Flt(a, 2), z = Flt(a, 3);
            bool ok = player->TeleportTo(map, x, y, z, player->Where().Facing());
            Line(log, "tele").Bool("ok", ok).Num("asked_map", map)
                .Flt("x", x).Flt("y", y).Flt("z", z)
                .Num("landed_map", player->GetMapId()).Write();
            return true;
        }
        if (verb == "spawn")
        {
            if (a.empty())
            {
                Line(log, "spawn").Bool("ok", false)
                    .Str("error", ("bad args: " + rest).c_str()).Write();
                return true;
            }
            uint32 entry = uint32(Int(a, 0));
            uint32 hp = uint32(Int(a, 1, 5000000));
            float dist = a.size() > 2 ? Flt(a, 2, MELEE_DIST) : MELEE_DIST;
            // ClosePointNear is the modern spelling of GetClosePoint (the
            // compat shim is Eluna-only, PLAN 23.6).
            float x, y, z;
            ClosePointNear(*player, x, y, z, 0.0f, dist, 0.0f, nullptr);
            Creature* c = player->SummonCreature(entry, x, y, z,
                                                 player->Where().Facing(),
                                                 TEMPSPAWN_MANUAL_DESPAWN, 0);
            if (!c)
            {
                Line(log, "spawn").Bool("ok", false).Num("entry", entry)
                    .Str("error", "SummonCreature returned nothing").Write();
                return true;
            }
            // Pacify exactly as the Lua did: hostile enough to be a legal
            // target for a "Strike an enemy" generator, but it never picks a
            // victim, never flees and never dies mid-sequence.
            c->setFaction(14);
            c->SetMaxHealth(hp);
            c->SetHealth(hp);
            // The Lua called Eluna's Creature:SetAggroEnabled(false), which
            // sets UNIT_FLAG_IMMUNE_TO_NPC -- a CMangos constant this fork's
            // UnitFlags does not carry. UNIT_FLAG_PASSIVE is this core's flag
            // for the same intent (Unit.h: "will ignore its surroundings and
            // not engage in combat unless called upon"), which is what the
            // fixture needs: a target that never picks a victim but can still
            // be attacked by the character driving the gate.
            c->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PASSIVE);
            c->GetMotionMaster()->MoveIdle();
            player->SetFacingToObject(c);
            g_spawned[player->GetGUIDLow()] = c->GetObjectGuid();
            Line(log, "spawn").Bool("ok", true).Num("entry", entry)
                .Num("guidlow", c->GetGUIDLow())
                .Flt("x", c->Where().X()).Flt("y", c->Where().Y())
                .Flt("z", c->Where().Z())
                .Num("hp", c->GetHealth()).Num("maxhp", c->GetMaxHealth())
                .Num("faction", c->getFaction())
                .Bool("passive", true)
                .Flt("dist_req", dist).Flt("dist", player->Where().DistanceTo(c->Where())).Write();
            return true;
        }
        if (verb == "despawn")
        {
            Creature* c = FindSpawned(player);
            bool removed = false;
            uint32 guidlow = 0;
            if (c)
            {
                guidlow = c->GetGUIDLow();
                c->ForcedDespawn();
                removed = true;
            }
            g_spawned.erase(player->GetGUIDLow());
            Line(log, "despawn").Bool("ok", true).Bool("found", c != nullptr)
                .Bool("removed", removed).Num("guidlow", guidlow).Write();
            return true;
        }
        if (verb == "fillpower")
        {
            Powers p = player->GetPowerType();
            player->SetPower(p, player->GetMaxPower(p));
            Line(log, "fillpower").Bool("ok", true).Num("ptype", uint32(p))
                .Num("power", player->GetPower(p))
                .Num("maxpower", player->GetMaxPower(p)).Write();
            return true;
        }
        if (verb == "give")
        {
            uint32 entry = uint32(Int(a, 0));
            uint32 count = uint32(Int(a, 1, 1));
            uint32 skills = 0;
            for (uint16 sid : GEAR_SKILLS)
            {
                player->SetSkill(sid, 1, 375, 375);
                ++skills;
            }
            Item* item = entry ? player->StoreNewItemInInventorySlot(entry, count) : nullptr;
            Line(log, "give").Num("entry", entry).Num("count", count)
                .Bool("ok", item != nullptr)
                .Num("guidlow", item ? item->GetGUIDLow() : 0)
                .Num("skills_set", skills).Write();
            return true;
        }
        if (verb == "auras")
        {
            uint32 spell = uint32(Int(a, 0));
            Creature* c = FindSpawned(player);
            bool has = c && c->HasAura(spell);
            uint32 stack = 0;
            if (c)
            {
                if (SpellAuraHolder* h = c->GetSpellAuraHolder(spell))
                {
                    stack = h->GetStackAmount();
                }
            }
            // Field set is the Lua's exactly: `ok` is what the gate keys on
            // (felsworn_test A1: `if au and au.get("ok")`), and a failure
            // carries `ok:false` plus `error` instead of a reading.
            if (!c)
            {
                Line(log, "auras").Bool("ok", false).Num("spell", spell)
                    .Str("error", "no live spawned target for this caller").Write();
                return true;
            }
            Line(log, "auras").Bool("ok", true).Num("spell", spell)
                .Bool("has", has).Num("stack", stack)
                .Num("guidlow", c->GetGUIDLow()).Write();
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- ip
    bool Items(Player* player, std::string const& verb,
               std::vector<std::string> const& a, std::string const& rest)
    {
        std::string const& log = g_items;
        if (verb == "additem")
        {
            uint32 entry = uint32(Int(a, 0));
            uint32 count = uint32(Int(a, 1, 1));
            Item* item = entry ? player->StoreNewItemInInventorySlot(entry, count) : nullptr;
            Line(log, "additem").Num("entry", entry).Num("count", count)
                .Bool("ok", item != nullptr)
                .Num("guidlow", item ? item->GetGUIDLow() : 0).Write();
            return true;
        }
        if (verb == "removeitem")
        {
            uint32 entry = uint32(Int(a, 0));
            uint32 had = player->GetItemCount(entry, true);
            player->DestroyItemCount(entry, 1000, true, false);
            Line(log, "removeitem").Num("entry", entry).Num("had", had)
                .Num("left", player->GetItemCount(entry, true))
                .Bool("ok", true).Write();
            return true;
        }
        if (verb == "gearskills")
        {
            uint32 n = 0;
            for (uint16 sid : GEAR_SKILLS)
            {
                player->SetSkill(sid, 1, 375, 375);
                ++n;
            }
            Line(log, "gearskills").Num("set", n)
                .Num("of", uint32(sizeof(GEAR_SKILLS) / sizeof(GEAR_SKILLS[0]))).Write();
            return true;
        }
        if (verb == "spawngo")
        {
            uint32 entry = uint32(Int(a, 0));
            GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(entry);
            GameObject* go = nullptr;
            if (info)
            {
                go = player->SummonGameObject(entry, player->Where().X(),
                                              player->Where().Y(),
                                              player->Where().Z(),
                                              player->Where().Facing(), 0);
            }
            Line(log, "spawngo").Num("entry", entry).Bool("ok", go != nullptr)
                .Num("guidlow", go ? go->GetGUIDLow() : 0)
                .Str("error", info ? "" : "no GameObjectInfo").Write();
            return true;
        }
        (void)rest;
        return false;
    }

    // ---------------------------------------------------------------- dp
    bool Dungeons(Player* player, std::string const& verb,
                  std::vector<std::string> const& a, std::string const& rest)
    {
        std::string const& log = g_dungeons;
        if (verb == "ping")
        {
            Line(log, "pong").Str("nonce", rest.c_str())
                .Num("map", player->GetMapId())
                .Flt("x", player->Where().X()).Flt("y", player->Where().Y())
                .Flt("z", player->Where().Z()).Write();
            return true;
        }
        if (verb == "tele" || verb == "goto")
        {
            bool cross = verb == "tele";
            if (a.size() < (cross ? 4u : 3u))
            {
                Line(log, verb.c_str()).Bool("ok", false)
                    .Str("error", ("bad args: " + rest).c_str()).Write();
                return true;
            }
            uint32 map = cross ? uint32(Int(a, 0)) : player->GetMapId();
            size_t o = cross ? 1 : 0;
            float x = Flt(a, o), y = Flt(a, o + 1), z = Flt(a, o + 2);
            // The Lua reported pcall's no-throw flag, so `ok` was true even
            // when TeleportTo refused. This reports the real return, plus the
            // conditions TeleportTo checks, so a refusal names itself instead
            // of surfacing three checks later as a wrong map.
            bool validCoord = MapManager::IsValidMapCoord(map, x, y, z);
            bool gm = player->isGameMaster();
            bool combat = player->IsInCombat();
            bool ok = player->TeleportTo(map, x, y, z, player->Where().Facing());
            Line(log, verb.c_str()).Bool("ok", ok).Num("asked_map", map)
                .Flt("x", x).Flt("y", y).Flt("z", z)
                .Num("landed_map", player->GetMapId())
                .Bool("valid_coord", validCoord).Bool("gm", gm)
                .Bool("in_combat", combat).Write();
            return true;
        }
        if (verb == "scan")
        {
            float range = a.empty() ? 60.0f : Flt(a, 0, 60.0f);
            std::list<Creature*> found;
            MaNGOS::AnyUnitInObjectRangeCheck check(player, range);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(found, check);
            Cell::VisitGridObjects(player, searcher, range);
            uint32 count = 0;
            for (Creature* c : found)
            {
                if (!c || !c->IsInWorld()) { continue; }
                ++count;
                Line(log, "scan_creature").Num("entry", c->GetEntry())
                    .Str("name", c->GetName()).Num("guidlow", c->GetGUIDLow())
                    .Num("rank", c->GetCreatureInfo() ? c->GetCreatureInfo()->Rank : 0)
                    .Bool("in_combat", c->IsInCombat())
                    .Num("hp", c->GetHealth()).Num("maxhp", c->GetMaxHealth())
                    .Flt("dist", player->Where().DistanceTo(c->Where())).Write();
            }
            Line(log, "scan_done").Num("count", count).Flt("range", range).Write();
            return true;
        }
        if (verb == "arm")
        {
            // The Lua bound Eluna creature hooks (9/15/4) to record the
            // encounter. AscProbe already records damage, cast and death for
            // every creature, so arming is the no-op acknowledgement the gate
            // waits on; the events arrive on the probe file either way.
            uint32 entry = uint32(Int(a, 0));
            Line(log, "armed").Num("entry", entry).Bool("ok", true)
                .Str("by", "AscProbe").Write();
            return true;
        }
        if (verb == "engage" || verb == "kill")
        {
            // Echo names and fields are the Lua's: `engage` echoes as
            // "engage" {ok, entry, found, dist, dmg, boss_*}; `kill` echoes as
            // "killblow" {entry, found, dealt, dead, boss_*} -- NOT "kill",
            // which is AscProbe's own credit event on the probe file and what
            // the gate reads for the killer (dungeons_test collects both).
            bool killing = verb == "kill";
            char const* echo = killing ? "killblow" : "engage";
            uint32 entry = uint32(Int(a, 0));
            uint32 dmg = uint32(Int(a, 1, killing ? 100000 : 200));
            Creature* c = NearestOf(player, entry, 200.0f);
            if (!c)
            {
                Line(log, echo).Bool("ok", false).Num("entry", entry).Bool("found", false).Write();
                return true;
            }
            player->SetFacingToObject(c);
            float dist = player->Where().DistanceTo(c->Where());
            if (!killing)
            {
                player->DealDamage(c, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL,
                                   nullptr, false);
                Line(log, echo).Bool("ok", true).Num("entry", entry).Bool("found", true)
                    .Flt("dist", dist).Num("dmg", dmg)
                    .Str("boss_name", c->GetName()).Num("boss_entry", c->GetEntry())
                    .Num("boss_guid", c->GetGUIDLow())
                    .Num("boss_hp", c->IsInWorld() ? c->GetHealth() : 0)
                    .Num("boss_maxhp", c->GetMaxHealth()).Write();
                return true;
            }
            // Bounded chunks so the killing blow is an ordinary swing from the
            // player (credit + loot), never a one-shot overkill that some
            // death paths short-circuit. The probe's own death/kill events
            // record the result independently.
            uint64 dealt = 0;
            bool dead = false;
            for (int i = 0; i < 40; ++i)
            {
                if (!c->IsInWorld() || !c->IsAlive()) { dead = true; break; }
                uint32 hp = c->GetHealth();
                if (hp == 0) { dead = true; break; }
                uint32 step = std::min(dmg, hp);
                player->DealDamage(c, step, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL,
                                   nullptr, false);
                dealt += step;
                if (!c->IsInWorld() || !c->IsAlive() || c->GetHealth() == 0) { dead = true; break; }
            }
            Line(log, echo).Num("entry", entry).Bool("found", true)
                .Num("dealt", (long long)dealt).Bool("dead", dead)
                .Str("boss_name", c->GetName()).Num("boss_entry", c->GetEntry())
                .Num("boss_guid", c->GetGUIDLow())
                .Num("boss_hp", c->IsInWorld() ? c->GetHealth() : 0)
                .Num("boss_maxhp", c->GetMaxHealth()).Write();
            return true;
        }
        return false;
    }
}

namespace AscFixture
{
    void Boot();

    void Init()
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_resources = sConfig.GetStringDefault("Ascension.Fixture.Resources.Path", "");
            g_items     = sConfig.GetStringDefault("Ascension.Fixture.Items.Path", "");
            g_dungeons  = sConfig.GetStringDefault("Ascension.Fixture.Dungeons.Path", "");
        }
        if (Enabled())
        {
            sLog.outString("Ascension fixtures: resources=%s items=%s dungeons=%s",
                           g_resources.empty() ? "off" : g_resources.c_str(),
                           g_items.empty() ? "off" : g_items.c_str(),
                           g_dungeons.empty() ? "off" : g_dungeons.c_str());
            Boot();
        }
    }

    bool Enabled()
    {
        return !g_resources.empty() || !g_items.empty() || !g_dungeons.empty();
    }

    void Boot()
    {
        // The Lua fixtures wrote {"kind":"boot","log":<path>} when Eluna
        // loaded them, and the gates read a FRESH boot line as proof the
        // fixture is armed before they drive it (felsworn_test A,
        // class_resources_test). Core fixtures are armed from world start, so
        // this is written at Init and again on `.reload fixtures`.
        for (std::string const* path : {&g_resources, &g_items, &g_dungeons})
        {
            if (path->empty()) { continue; }
            Line(*path, "boot").Str("log", path->c_str()).Str("by", "AscFixture").Write();
        }
    }

    bool Handle(Player* player, std::string const& msg)
    {
        if (!player || !Enabled()) { return false; }
        // A fixture rearranges the world; only a GM may drive one, and only
        // when the operator configured a file for it.
        if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER) { return false; }

        std::vector<std::string> words = Split(msg);
        if (words.size() < 2) { return false; }
        std::string const& tag = words[0];
        std::string const& verb = words[1];

        std::vector<std::string> args(words.begin() + 2, words.end());
        std::string rest;
        for (size_t i = 2; i < words.size(); ++i)
        {
            if (!rest.empty()) { rest += ' '; }
            rest += words[i];
        }

        if (tag == "rp" && !g_resources.empty()) { return Resources(player, verb, args, rest); }
        if (tag == "ip" && !g_items.empty())     { return Items(player, verb, args, rest); }
        if (tag == "dp" && !g_dungeons.empty())  { return Dungeons(player, verb, args, rest); }
        return false;
    }
}

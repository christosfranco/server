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
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "Common/TimeConstants.h"
#include "Utilities/Errors.h"
#include "Player.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateMask.h"
#include "SkillDiscovery.h"
#include "QuestDef.h"
#include "GossipDef.h"
#include "UpdateData.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "InstanceData.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "ObjectMgr.h"
#include "CorpseManager.h"
#include "CreatureAI.h"
#include "Formulas.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Pet.h"
#include "Util.h"
#include "Transports.h"
#include "Weather.h"
#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"
#include "BattleGround/BattleGroundAV.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "ArenaTeam.h"
#include "Chat.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "AchievementMgr.h"
#include "Mail.h"
#include "SpellAuras.h"
#include "DBCStores.h"
#include "SQLStorages.h"
#include "Vehicle.h"
#include "Calendar.h"
#include "DisableMgr.h"
#ifdef ENABLE_ELUNA
#include "LuaEngine.h"
#endif /* ENABLE_ELUNA */

#include <cmath>
// corpse reclaim times
#define DEATH_EXPIRE_STEP (5*MINUTE)
#define MAX_DEATH_COUNT 3

static const uint32 corpseReclaimDelay[MAX_DEATH_COUNT] = {30, 60, 120};

/* Preconditions:
  - a resurrectable corpse must not be loaded for the player (only bones)
  - the player must be in world
*/
void Player::BuildPlayerRepop()
{
    WorldPacket data(SMSG_PRE_RESURRECT, GetPackGUID().size());
    data << GetPackGUID();
    GetSession()->SendPacket(&data);

    if (getRace() == RACE_NIGHTELF)
    {
        CastSpell(this, 20584, true); // auras SPELL_AURA_INCREASE_SPEED(+speed in wisp form), SPELL_AURA_INCREASE_SWIM_SPEED(+swim speed in wisp form), SPELL_AURA_TRANSFORM (to wisp form)
    }
    CastSpell(this, 8326, true);                            // auras SPELL_AURA_GHOST, SPELL_AURA_INCREASE_SPEED(why?), SPELL_AURA_INCREASE_SWIM_SPEED(why?)

    // there must be SMSG.FORCE_RUN_SPEED_CHANGE, SMSG.FORCE_SWIM_SPEED_CHANGE, SMSG.MOVE_WATER_WALK
    // there must be SMSG.STOP_MIRROR_TIMER
    // there we must send 888 opcode

    // the player can not have a corpse already, only bones which are not returned by GetCorpse
    if (GetCorpse())
    {
        sLog.outError("BuildPlayerRepop: player %s(%d) already has a corpse", GetName(), GetGUIDLow());
        MANGOS_ASSERT(false);
    }

    // NO BODY IS EVER LEFT ON A SHIP. She is a map that sails away, and her grids are pinned
    // for as long as the server runs, so a corpse aboard is one nobody can walk back to and
    // nothing will ever unload. Retail agrees: dying on a transport releases you to the
    // nearest port, alive. RepopAtGraveyard -- which always follows this call -- resurrects
    // him and finds that port from the vessel's own position.
    Corpse* corpse = NULL;

    if (!GetMap()->AsTransport())
    {
        // create a corpse and place it at the player's location
        corpse = CreateCorpse();
        if (!corpse)
        {
            sLog.outError("Error creating corpse for Player %s [%u]", GetName(), GetGUIDLow());
            return;
        }
        GetMap()->Add(corpse);
    }

    // convert player body to ghost
    if (GetDeathState() != GHOULED)
    {
        SetHealth(1);
    }

    SetWaterWalk(true);
    if (!GetSession()->isLogingOut())
    {
        SetRoot(false);
    }

    // BG - remove insignia related
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

    if (GetDeathState() != GHOULED)
    {
        SendCorpseReclaimDelay();
    }

    // to prevent cheating
    if (corpse)
    {
        corpse->ResetGhostTime();
    }

    StopMirrorTimers();                                     // disable timers(bars)

    // set and clear other
    SetByteValue(UNIT_FIELD_BYTES_1, 3, UNIT_BYTE1_FLAG_ALWAYS_STAND);
}

/**
 * @brief Restores the player to life and reapplies post-resurrection effects.
 *
 * @param restore_percent The fraction of health and resources to restore.
 * @param applySickness True to apply resurrection sickness when appropriate.
 */
void Player::ResurrectPlayer(float restore_percent, bool applySickness)
{
    WorldPacket data(SMSG_DEATH_RELEASE_LOC, 4 * 4);        // remove spirit healer position
    data << uint32(-1);
    data << float(0);
    data << float(0);
    data << float(0);
    GetSession()->SendPacket(&data);

    // speed change, land walk

    // remove death flag + set aura
    SetByteValue(UNIT_FIELD_BYTES_1, 3, 0x00);

    SetDeathState(ALIVE);

    if (getRace() == RACE_NIGHTELF)
    {
        RemoveAurasDueToSpell(20584); // speed bonuses
    }
    RemoveAurasDueToSpell(8326);                            // SPELL_AURA_GHOST

    SetWaterWalk(false);
    SetRoot(false);

    // set health/powers (0- will be set in caller)
    if (restore_percent > 0.0f)
    {
        SetHealth(uint32(GetMaxHealth()*restore_percent));
        SetPower(POWER_MANA, uint32(GetMaxPower(POWER_MANA)*restore_percent));
        SetPower(POWER_RAGE, 0);
        SetPower(POWER_ENERGY, uint32(GetMaxPower(POWER_ENERGY)*restore_percent));
    }

    // trigger update zone for alive state zone updates
    uint32 newzone, newarea;
    GetTerrain()->GetZoneAndAreaId(newzone, newarea, Where().X(), Where().Y(), Where().Z());
    UpdateZone(newzone, newarea);

    m_deathTimer = 0;

    // update visibility of world around viewpoint
    m_camera.UpdateVisibilityForOwner();
    // update visibility of player for nearby cameras
    UpdateObjectVisibility();

#ifdef ENABLE_ELUNA
    if (Eluna* e = GetEluna())
    {
        e->OnResurrect(this);
    }
#endif /* ENABLE_ELUNA */

    if (!applySickness)
    {
        return;
    }

    // Characters from level 1-10 are not affected by resurrection sickness.
    // Characters from level 11-19 will suffer from one minute of sickness
    // for each level they are above 10.
    // Characters level 20 and up suffer from ten minutes of sickness.
    int32 startLevel = sWorld.getConfig(CONFIG_INT32_DEATH_SICKNESS_LEVEL);

    if (int32(getLevel()) >= startLevel)
    {
        // set resurrection sickness
        CastSpell(this, SPELL_ID_PASSIVE_RESURRECTION_SICKNESS, true);

        // not full duration
        if (int32(getLevel()) < startLevel + 9)
        {
            int32 delta = (int32(getLevel()) - startLevel + 1) * MINUTE;

            if (SpellAuraHolder* holder = GetSpellAuraHolder(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS))
            {
                holder->SetAuraDuration(delta * IN_MILLISECONDS);
                holder->SendAuraUpdate(false);
            }
        }
    }
}

/**
 * @brief Transitions the player into the corpse state after death.
 */
void Player::KillPlayer()
{
    SetRoot(true);

    StopMirrorTimers();                                     // disable timers(bars)

    SetDeathState(CORPSE);
    // SetFlag( UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_IN_PVP );

    SetUInt32Value(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_NONE);
    ApplyModByteFlag(PLAYER_FIELD_BYTES, 0, PLAYER_FIELD_BYTE_RELEASE_TIMER, !sMapStore.LookupEntry(GetMapId())->Instanceable() && !HasAuraType(SPELL_AURA_PREVENT_RESURRECTION));

    // 6 minutes until repop at graveyard
    m_deathTimer = 6 * MINUTE * IN_MILLISECONDS;

    UpdateCorpseReclaimDelay();                             // dependent at use SetDeathPvP() call before kill

    // don't create corpse at this moment, player might be falling

    // update visibility
    UpdateObjectVisibility();
}

/**
 * @brief Creates a corpse object for the player's current death state.
 *
 * @return The created corpse, or null if creation failed.
 */
Corpse* Player::CreateCorpse()
{
    // prevent existence 2 corpse for player
    SpawnCorpseBones();

    Corpse* corpse = new Corpse((m_ExtraFlags & PLAYER_EXTRA_PVP_DEATH) ? CORPSE_RESURRECTABLE_PVP : CORPSE_RESURRECTABLE_PVE);
    SetPvPDeath(false);

    if (!corpse->Create(sObjectMgr.GenerateCorpseLowGuid(), this))
    {
        delete corpse;
        return NULL;
    }

    uint8 skin       = GetByteValue(PLAYER_BYTES, 0);
    uint8 face       = GetByteValue(PLAYER_BYTES, 1);
    uint8 hairstyle  = GetByteValue(PLAYER_BYTES, 2);
    uint8 haircolor  = GetByteValue(PLAYER_BYTES, 3);
    uint8 facialhair = GetByteValue(PLAYER_BYTES_2, 0);

    corpse->SetByteValue(CORPSE_FIELD_BYTES_1, 1, getRace());
    corpse->SetByteValue(CORPSE_FIELD_BYTES_1, 2, getGender());
    corpse->SetByteValue(CORPSE_FIELD_BYTES_1, 3, skin);

    corpse->SetByteValue(CORPSE_FIELD_BYTES_2, 0, face);
    corpse->SetByteValue(CORPSE_FIELD_BYTES_2, 1, hairstyle);
    corpse->SetByteValue(CORPSE_FIELD_BYTES_2, 2, haircolor);
    corpse->SetByteValue(CORPSE_FIELD_BYTES_2, 3, facialhair);

    uint32 flags = CORPSE_FLAG_UNK2;
    if (HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM))
    {
        flags |= CORPSE_FLAG_HIDE_HELM;
    }
    if (HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK))
    {
        flags |= CORPSE_FLAG_HIDE_CLOAK;
    }
    if (InBattleGround() && !InArena())
    {
        flags |= CORPSE_FLAG_LOOTABLE; // to be able to remove insignia
    }
    corpse->SetUInt32Value(CORPSE_FIELD_FLAGS, flags);

    corpse->SetUInt32Value(CORPSE_FIELD_DISPLAY_ID, GetNativeDisplayId());

    corpse->SetUInt32Value(CORPSE_FIELD_GUILD, GetGuildId());

    uint32 iDisplayID;
    uint32 iIventoryType;
    uint32 _cfi;
    for (int i = 0; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (m_items[i])
        {
            iDisplayID = m_items[i]->GetProto()->DisplayInfoID;
            iIventoryType = m_items[i]->GetProto()->InventoryType;

            _cfi =  iDisplayID | (iIventoryType << 24);
            corpse->SetUInt32Value(CORPSE_FIELD_ITEM + i, _cfi);
        }
    }

    // we not need saved corpses for BG/arenas
    if (!GetMap()->IsBattleGroundOrArena())
    {
        corpse->SaveToDB();
    }

    // register for player, but not show
    sCorpseManager.Add(corpse);
    return corpse;
}

/**
 * @brief Converts an existing corpse into bones and persists the ghost state if needed.
 */
void Player::SpawnCorpseBones()
{
    if (sCorpseManager.ConvertCorpseForPlayer(GetObjectGuid()))
        if (!GetSession()->PlayerLogoutWithSave())          // at logout we will already store the player
        {
            SaveToDB(); // prevent loading as ghost without corpse
        }
}

/**
 * @brief Gets the player's current corpse object.
 *
 * @return The player's corpse, or null if none exists.
 */
Corpse* Player::GetCorpse() const
{
    return sCorpseManager.FindForPlayer(GetObjectGuid());
}


/**
 * @brief Moves the player to the nearest valid graveyard.
 */
void Player::RepopAtGraveyard()
{
    // note: this can be called also when the player is alive
    // for example from WorldSession::HandleMovementOpcodes

    // THE ANCHOR, not the placement. Aboard, this is the map the ship sails and her own
    // waypoint estimate: a hull carries no area table, so asking the map underfoot yields
    // zone 0, and a graveyard is ashore in any case.
    uint32 graveMap;
    float graveX, graveY, graveZ;
    GetWorldAnchor(graveMap, graveX, graveY, graveZ);

    AreaTableEntry const* zone =
        GetAreaEntryByAreaID(AnchorTerrain()->GetAreaId(graveX, graveY, graveZ));

    // Such zones are considered unreachable as a ghost, and so is a ship that has sailed:
    // there is no walking back to a body aboard one, so he is revived on the spot and put
    // ashore at the nearest port below. No corpse was left there -- see BuildPlayerRepop.
    if (!IsAlive() && ((zone && zone->Flags & AREA_FLAG_NEED_FLY) || GetMap()->AsTransport()))
    {
        ResurrectPlayer(0.5f);
        SpawnCorpseBones();
    }

    WorldSafeLocsEntry const* ClosestGrave = NULL;

    // Special handle for battleground maps
    if (BattleGround* bg = GetBattleGround())
    {
        ClosestGrave = bg->GetClosestGraveYard(this);
    }
    else
    {
        ClosestGrave = sObjectMgr.GetClosestGraveYard(graveX, graveY, graveZ, graveMap, GetTeam());
    }

    // stop countdown until repop
    m_deathTimer = 0;

    // No linked graveyard for the anchor zone (Blizzard data covers every
    // stock zone; a third-party or custom map like Ascension's map 900
    // can leave the AreaTable row absent, so GetZoneId returns 0 and
    // GetClosestGraveYard prints "Zone 0 ... does not have a linked
    // graveyard" and returns NULL). Falling through and doing nothing
    // leaves a dead player in a corpse map without a corpse, which the
    // teardown paths (WorldSession::LogoutPlayer, HandleMovementOpcodes)
    // then unravel in unexpected order. Fall back to a) the nearest
    // WorldSafeLocs entry on the same map by 2D distance, b) the home
    // bind, c) refuse to leave things silent. PLAN 14.23.
    if (!ClosestGrave)
    {
        WorldSafeLocsEntry const* fallback = NULL;
        float bestDist2 = 0.0f;
        for (uint32 i = 0; i < sWorldSafeLocsStore.GetNumRows(); ++i)
        {
            WorldSafeLocsEntry const* entry = sWorldSafeLocsStore.LookupEntry(i);
            if (!entry || entry->Continent != graveMap)
            {
                continue;
            }
            float dx = entry->LocX - graveX;
            float dy = entry->LocY - graveY;
            float d2 = dx * dx + dy * dy;
            if (!fallback || d2 < bestDist2)
            {
                fallback = entry;
                bestDist2 = d2;
            }
        }

        if (fallback)
        {
            sLog.outError("RepopAtGraveyard: player %s(%u) died in map %u with no linked graveyard for the anchor zone; falling back to WorldSafeLocs %u on the same map (dist %.1f).",
                          GetName(), GetGUIDLow(), graveMap, fallback->ID, sqrtf(bestDist2));
            ClosestGrave = fallback;
        }
        else if (!GetSession()->PlayerLogout())
        {
            sLog.outError("RepopAtGraveyard: player %s(%u) died in map %u with no linked graveyard for the anchor zone and no WorldSafeLocs entry on that map; teleporting to home bind (map %u).",
                          GetName(), GetGUIDLow(), graveMap, m_homebindMapId);
            TeleportToHomebind();
        }
        // If we are inside LogoutPlayer, the last-resort log line and
        // the deferred home-bind relocation below cover it.
    }

    // A live TeleportTo from inside a WorldSession::LogoutPlayer dead-
    // player branch is unsafe. Player::TeleportTo across maps calls
    // oldmap->Remove(this, false), then HandleMoveWorldportAckOpcode
    // pulls the player synchronously into the new map, and LogoutPlayer
    // then deletes the player through its own map. The player pointer
    // has been observed to remain in the OLD map's client-update set
    // past the delete: PLAN 14.23's third crash, mangosd 2963575,
    // 2026-09-04 17:43:24, a Player at rbp=0x55db356437a0 with
    // m_uint32Values zeroed, sitting in map 900's
    // i_objectsToClientUpdate one tick after the destructor ran. The
    // trigger this session was `.go xyz ... 900` -> fall -> die ->
    // logout, and GetClosestGraveYard resolving map 900's baked ADT
    // area to Dragonblight (zone 65, map 571) -- the linked graveyard
    // that mapEntry->CorpseMapID does not name, so GetClosestGraveYard
    // returned entryFar without ever printing the "no linked graveyard"
    // line. There is no safe TeleportTo from here; write the resolved
    // destination onto the persisted-position fields so the next login
    // lands there, and let LogoutPlayer delete the player from the map
    // he is already on (RemoveFromWorld -> ClearUpdateMask(true) ->
    // RemoveFromClientUpdateList clears the update-set cleanly on the
    // ordinary teardown path).
    //
    // Refinement, PLAN 14.23: honour the resolved graveyard, not the
    // home bind. bb185cc35 wrote RelocateToHomebind unconditionally,
    // which is wrong gameplay: a character who died on a map that DOES
    // have a linked graveyard (e.g. map 900 with WorldSafeLocs 9000
    // now that PLAN 14.24's AreaTable supplement gives Ascension's
    // tiles real zones and game_graveyard_zone rebinds them) should
    // log back in at that graveyard. ClosestGrave above already ran
    // the same resolution chain the non-logout path uses:
    // GetClosestGraveYard (which prefers the same-map candidate over
    // a cross-continent one -- ObjectMgrGraveyard.cpp returns
    // entryNear before entryEntr before entryFar, so a WorldSafeLocs
    // on map 900 always wins over one on map 571 for a death on map
    // 900), then the same-map WorldSafeLocs 2D-distance fallback. If
    // it returned a location, use it; otherwise fall back to the home
    // bind. Either way we do not teleport live. SetLocationMapId +
    // Place().MoveTo update m_mapId and Place; SaveToDB a few
    // statements later picks GetMapId() up under the not-being-
    // teleported branch (see PlayerSave.cpp `savedMap = GetMapId()`).
    // The player stays dead across the write: a dead player logging
    // in at a graveyard with a corpse to run to is stock behaviour
    // (Player::LoadFromDB -> LoadCorpse: dead + corpse present sets
    // the release timer from the corpse's map, dead + no corpse
    // ResurrectPlayer(0.5f)s to prevent an unrecoverable state;
    // SpawnCorpseBones already ran on the death that led here, so
    // the corpse the ghost needs to reclaim is on the map he died
    // on, not at the graveyard he now respawns at).
    if (GetSession()->PlayerLogout())
    {
        if (ClosestGrave)
        {
            sLog.outError("RepopAtGraveyard: player %s(%u) died on map %u; logout in progress -- no live teleport, next login at graveyard %u on map %u (%.1f, %.1f, %.1f)",
                          GetName(), GetGUIDLow(), GetMapId(),
                          ClosestGrave->ID, ClosestGrave->Continent,
                          ClosestGrave->LocX, ClosestGrave->LocY, ClosestGrave->LocZ);
            SetLocationMapId(ClosestGrave->Continent);
            Place().MoveTo(ClosestGrave->LocX, ClosestGrave->LocY, ClosestGrave->LocZ);
        }
        else
        {
            sLog.outError("RepopAtGraveyard: player %s(%u) died on map %u; logout in progress -- no graveyard resolved, next login at home bind (map %u)",
                          GetName(), GetGUIDLow(), GetMapId(), m_homebindMapId);
            RelocateToHomebind();
        }
        return;
    }

    // if no grave found, stay at the current location
    // and don't show spirit healer location
    if (ClosestGrave)
    {
        bool updateVisibility = IsInWorld() && GetMapId() == ClosestGrave->Continent;
        TeleportTo(ClosestGrave->Continent, ClosestGrave->LocX, ClosestGrave->LocY, ClosestGrave->LocZ, Where().Facing());
        if (IsDead())                                       // not send if alive, because it used in TeleportTo()
        {
            WorldPacket data(SMSG_DEATH_RELEASE_LOC, 4 * 4);// show spirit healer position on minimap
            data << ClosestGrave->Continent;
            data << ClosestGrave->LocX;
            data << ClosestGrave->LocY;
            data << ClosestGrave->LocZ;
            GetSession()->SendPacket(&data);
        }
        if (updateVisibility && IsInWorld())
        {
            UpdateVisibilityAndView();
        }
    }
}


/**
 * @brief Gets the current corpse reclaim delay for PvE or PvP death.
 *
 * @param pvp True for PvP death rules; false for PvE death rules.
 * @return The reclaim delay in seconds.
 */
uint32 Player::GetCorpseReclaimDelay(bool pvp) const
{
    if ((pvp && !sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVP)) ||
            (!pvp && !sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVE)))
    {
        return corpseReclaimDelay[0];
    }

    time_t now = time(NULL);
    // 0..2 full period
    uint32 count = (now < m_deathExpireTime) ? uint32((m_deathExpireTime - now) / DEATH_EXPIRE_STEP) : 0;
    return corpseReclaimDelay[count];
}

/**
 * @brief Advances the corpse reclaim delay escalation after death.
 */
void Player::UpdateCorpseReclaimDelay()
{
    bool pvp = m_ExtraFlags & PLAYER_EXTRA_PVP_DEATH;

    if ((pvp && !sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVP)) ||
            (!pvp && !sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVE)))
    {
        return;
    }

    time_t now = time(NULL);
    if (now < m_deathExpireTime)
    {
        // full and partly periods 1..3
        uint32 count = uint32((m_deathExpireTime - now) / DEATH_EXPIRE_STEP + 1);
        if (count < MAX_DEATH_COUNT)
        {
            m_deathExpireTime = now + (count + 1) * DEATH_EXPIRE_STEP;
        }
        else
        {
            m_deathExpireTime = now + MAX_DEATH_COUNT * DEATH_EXPIRE_STEP;
        }
    }
    else
    {
        m_deathExpireTime = now + DEATH_EXPIRE_STEP;
    }
}

/**
 * @brief Sends the current corpse reclaim delay to the client.
 *
 * @param load True when restoring the delay from saved corpse state.
 */
void Player::SendCorpseReclaimDelay(bool load)
{
    Corpse* corpse = GetCorpse();
    if (!corpse)
    {
        return;
    }

    uint32 delay;
    if (load)
    {
        if (corpse->GetGhostTime() > m_deathExpireTime)
        {
            return;
        }

        bool pvp = corpse->GetType() == CORPSE_RESURRECTABLE_PVP;

        uint32 count;
        if ((pvp && sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVP)) ||
                (!pvp && sWorld.getConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVE)))
        {
            count = uint32(m_deathExpireTime - corpse->GetGhostTime()) / DEATH_EXPIRE_STEP;
            if (count >= MAX_DEATH_COUNT)
            {
                count = MAX_DEATH_COUNT - 1;
            }
        }
        else
        {
            count = 0;
        }

        time_t expected_time = corpse->GetGhostTime() + corpseReclaimDelay[count];

        time_t now = time(NULL);
        if (now >= expected_time)
        {
            return;
        }

        delay = uint32(expected_time - now);
    }
    else
    {
        delay = GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP);
    }

    //! corpse reclaim delay 30 * 1000ms or longer at often deaths
    WorldPacket data(SMSG_CORPSE_RECLAIM_DELAY, 4);
    data << uint32(delay * IN_MILLISECONDS);
    GetSession()->SendPacket(&data);
}




// SPDX-License-Identifier: GPL-3.0-or-later
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "SpellAuras.h"
#include "Log.h"
#include "CoaProjection.h"
#include "CoaPowerRequirements.h"
#include "Spell.h"
#include "Creature.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace
{
    // Learned dependencies and lower ranks are spellbook ownership. Triggered
    // effects are aura ownership only; granting them as castable spells is wrong.
    coa::SpellLinks SpellDependencies(uint32 id)
    {
        auto spell = sSpellStore.LookupEntry(id);
        if (!spell)
        {
            throw std::runtime_error("CoA spell dependency missing: " + std::to_string(id));
        }
        coa::SpellLinks links;
        links.previous = sSpellMgr.GetPrevSpellInChain(id);
        auto bounds = sSpellMgr.GetSpellLearnSpellMapBounds(id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            links.learned.insert(it->second.spell);
        }
        for (unsigned effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        {
            uint32 triggered = spell->EffectTriggerSpell[effect];
            if (!triggered) { continue; }
            // Missing triggered targets are not fatal: the catalog compiler
            // may pin an entry whose rank spell references a DBC row this
            // snapshot lacks (class 32 Runemaster entry 4655 rank spell
            // 806711 -> 725391 -> 7015, absent on this staged Spell.dbc).
            // Skip the recursive add; the effect will not fire natively
            // either because Spell::EffectTriggerSpell also LookupEntry's it.
            if (!sSpellStore.LookupEntry(triggered)) { continue; }
            if (spell->Effect[effect] == SPELL_EFFECT_LEARN_SPELL)
            {
                links.learned.insert(triggered);
            }
            links.triggered.insert(triggered);
        }
        return links;
    }

    std::set<uint32> SpellClosure(std::set<uint32> const& seeds, bool auras = false)
    {
        return coa::SpellClosure(seeds, SpellDependencies, auras);
    }
}

uint8 Player::CoaCombatDonor(uint8 pClass)
{
    // Snapshotted from sql/ascension-classes-generated.sql, which the
    // Python generator writes from ideal_donor(). One place, so a stat
    // formula and PlayerItemQuery's relic slot see the same donor. Stock
    // classes 1..11 return themselves.
    switch (pClass)
    {
        case 10: return CLASS_PRIEST;        // Hero
        case 12: return CLASS_ROGUE;         // Barbarian (energy)
        case 13: return CLASS_PRIEST;        // Witch Doctor
        case 14: return CLASS_ROGUE;         // Felsworn (energy)
        case 15: return CLASS_PRIEST;        // Witch Hunter
        case 16: return CLASS_PRIEST;        // Stormbringer
        case 17: return CLASS_WARRIOR;       // Knight of Xoroth (rage)
        case 18: return CLASS_WARRIOR;       // Guardian (rage)
        case 19: return CLASS_ROGUE;         // Templar (energy)
        case 20: return CLASS_WARRIOR;       // Bloodmage (rage)
        case 21: return CLASS_HUNTER;        // Ranger (focus)
        case 22: return CLASS_PRIEST;        // Chronomancer
        case 23: return CLASS_DEATH_KNIGHT;  // Necromancer (runic)
        case 24: return CLASS_PRIEST;        // Pyromancer
        case 25: return CLASS_PALADIN;       // Cultist (mana+plate)
        case 26: return CLASS_ROGUE;         // Starcaller (energy)
        case 27: return CLASS_SHAMAN;        // Sun Cleric (mana+mail)
        case 28: return CLASS_SHAMAN;        // Tinker (mana+mail)
        case 29: return CLASS_PRIEST;        // Venomancer
        case 30: return CLASS_DEATH_KNIGHT;  // Reaper (runic)
        case 31: return CLASS_SHAMAN;        // Primalist (mana+mail)
        case 32: return CLASS_PRIEST;        // Runemaster
        default: return pClass;
    }
}

bool Player::IsCoaManaged() const
{
    auto catalog = sWorld.GetCoaCatalog();
    return GetSession() && GetSession()->GetClientProfile() == proto::ConnectionProfile::AscensionStockAuthCoA &&
        catalog && catalog->HasClass(getClass());
}

bool Player::IsCoaManagedSpell(uint32 spell) const
{
    return IsCoaManaged() && m_coaManagedSpells.count(spell);
}

bool WorldSession::CanUseCharacterClass(uint32 playerClass) const
{
    auto catalog = sWorld.GetCoaCatalog();
    return AdmitCharacterClass(GetClientProfile(), playerClass, catalog && catalog->HasClass(playerClass));
}

uint32 Player::GetProgressionLevelCap() const
{
    return IsCoaManaged() ? 60 : sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
}

coa::StarterPlan const* Player::GetCoaStarter() const
{
    return IsCoaManaged() ? sWorld.GetCoaStarter(getClass(), getRace()) : nullptr;
}

uint32 Player::GetCoaBaseMana(uint32 nativeMana) const
{
    auto plan = GetCoaStarter();
    if (!plan)
    {
        return nativeMana;
    }
    PlayerClassLevelInfo fallback{};
    sObjectMgr.GetPlayerClassLevelInfo(CLASS_MAGE, getLevel(), &fallback);
    uint8 required = m_coaRequiredPowers | coa::PrimaryPowerBit(plan->spell.displayPower) |
        coa::PrimaryPowerBit(plan->spell.power) | coa::PrimaryPowerBit(GetPowerType());
    return coa::RequiredBaseMana(required, nativeMana, fallback.basemana);
}

uint32 Player::GetCoaCreatePower(Powers power) const
{
    auto plan = GetCoaStarter();
    uint8 required = plan ? m_coaRequiredPowers | coa::PrimaryPowerBit(plan->spell.displayPower) |
        coa::PrimaryPowerBit(plan->spell.power) | coa::PrimaryPowerBit(GetPowerType()) : 0;
    return coa::PowerCapacity(required, power, GetCreateMana());
}

void Player::UpdateCoaPowerCaps()
{
    if (!IsCoaManaged() || m_coaFailed)
    {
        return;
    }
    try
    {
        PlayerClassLevelInfo native{};
        sObjectMgr.GetPlayerClassLevelInfo(getClass(), getLevel(), &native);
        SetCreateMana(GetCoaBaseMana(native.basemana));
        for (uint32 slot = 0; slot < MAX_POWERS; ++slot)
        {
            Powers power = Powers(slot);
            uint32 previous = GetMaxPower(power);
            UpdateMaxPower(power);
            // Only initial character creation fills a newly provisioned regen pool.
            // Learning, respec, form changes and relog never refill existing values.
            if (m_coaCreating && !previous && (power == POWER_MANA || power == POWER_FOCUS || power == POWER_ENERGY))
            {
                SetPower(power, GetMaxPower(power));
            }
        }
    }
    catch (std::exception const& error)
    {
        m_coaFailed = true;
        sLog.outError("CoA power capacity failed for guid %u: %s", GetGUIDLow(), error.what());
        GetSession()->KickPlayer();
    }
}

void Player::RefreshCoaPowerRequirements()
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed || m_coaReconciling || m_coaPowerUpdateDepth)
    {
        return;
    }
    try
    {
        std::set<uint32> learned;
        for (auto const& spell : m_spells)
        {
            if (spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled && spell.second.active)
            {
                learned.insert(spell.first);
            }
        }
        auto plan = GetCoaStarter();
        if (!plan)
        {
            throw std::runtime_error("CoA resource context has no starter");
        }
        auto requirements = coa::RequiredPowers(plan->spell.displayPower, plan->spell.power,
            learned, [](uint32 id)
        {
            auto spell = sSpellStore.LookupEntry(id);
            if (!spell)
            {
                throw std::runtime_error("CoA resource spell missing: " + std::to_string(id));
            }
            coa::PowerSpell use;
            use.power = spell->PowerType;
            use.passive = spell->HasAttribute(SPELL_ATTR_PASSIVE);
            use.equipmentRequired = spell->EquippedItemClass >= 0;
            use.casterSourceOnly = IsSpellWithCasterSourceTargetsOnly(spell);
            use.channeled = IsChanneledSpell(spell);
            use.hasCost = spell->ManaCost || spell->ManaCostPct || spell->ManaCostPerLevel ||
                spell->ManaPerSecond || spell->ManaPerSecondPerLevel ||
                spell->HasAttribute(SPELL_ATTR_EX_DRAIN_ALL_POWER);
            for (unsigned effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
            {
                use.effects[effect] = {spell->Effect[effect], spell->ImplicitTargetA[effect],
                    spell->ImplicitTargetB[effect], spell->EffectTriggerSpell[effect], spell->EffectAura[effect],
                    spell->EffectMiscValue[effect]};
                use.selectedTriggerTarget |= spell->ImplicitTargetA[effect] == TARGET_SINGLE_ENEMY;
            }
            return use;
        });
        m_coaRequiredPowers = requirements.powers;
        if (requirements.conditional || requirements.unresolved)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "CoA resource context guid %u: conditional mask %u, unresolved edges %u",
                GetGUIDLow(), uint32(requirements.conditional), uint32(requirements.unresolved));
        }
        UpdateCoaPowerCaps();
    }
    catch (std::exception const& error)
    {
        m_coaFailed = true;
        sLog.outError("CoA power requirements failed for guid %u: %s", GetGUIDLow(), error.what());
        GetSession()->KickPlayer();
    }
}

Player::CoaPowerUpdate::CoaPowerUpdate(Player& player) : m_player(player)
{
    ++m_player.m_coaPowerUpdateDepth;
}

Player::CoaPowerUpdate::~CoaPowerUpdate()
{
    // The initial projection batches loading. Later changes (including equipped
    // grants before AddToWorld) must refresh even while the player is off-map.
    if (!--m_player.m_coaPowerUpdateDepth && m_player.m_coaRequiredPowers)
    {
        m_player.RefreshCoaPowerRequirements();
        m_player.RefreshCoaCombatAuthority();
    }
}

bool Player::PrepareCoaStarterInfrastructure()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    auto plan = GetCoaStarter();
    if (!plan || !m_coaReady || m_coaFailed)
    {
        return false;
    }
    for (auto skill : plan->skills)
    {
        if (!HasSkill(skill) || GetPureSkillValue(skill) < 1)
        {
            auto entry = sSkillLineStore.LookupEntry(skill);
            if (!entry)
            {
                return false;
            }
            SetSkill(skill, 1, entry->CategoryID == SKILL_CATEGORY_WEAPON ? GetMaxSkillValueForLevel() : 1);
        }
    }
    for (auto id : plan->proficiencies)
    {
        RememberCoaIndependentSpell(id);
        learnSpell(id, true);
        if (!HasSpell(id) || m_coaFailed)
        {
            sLog.outError("CoA starter proficiency %u failed for class %u", id, uint32(getClass()));
            return false;
        }
        // Loader admits only pure proficiency/dual-wield effects. Explicit cast
        // handles nonpassive native proficiency rows as well as passive rows.
        CastSpell(this, id, true);
    }
    return !plan->dualWield || CanDualWield();
}

bool Player::EquipCoaStarter()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    auto plan = GetCoaStarter();
    if (!plan)
    {
        return false;
    }
    // Weapon/armour roles map to equipment slots one-to-one. The array is in
    // coa::StarterSlot order (MainHand=0, OffHand=1, Ranged=2, Ammo=3,
    // Chest=4, Legs=5); ammo is handled below because it is not equipped.
    struct { size_t role; uint8 destination; } const equip[] = {
        {0, EQUIPMENT_SLOT_MAINHAND},
        {1, EQUIPMENT_SLOT_OFFHAND},
        {2, EQUIPMENT_SLOT_RANGED},
        {4, EQUIPMENT_SLOT_CHEST},
        {5, EQUIPMENT_SLOT_LEGS},
    };
    for (auto const& pair : equip)
    {
        auto const& item = plan->gear[pair.role];
        if (!item.id)
        {
            continue;
        }
        uint16 destination = 0;
        auto error = CanEquipNewItem(pair.destination, destination, item.id, false);
        if (error != EQUIP_ERR_OK || !EquipNewItem(destination, item.id, true))
        {
            sLog.outError("CoA starter equipment %u role %zu failed for class %u: %u",
                item.id, pair.role, uint32(getClass()), uint32(error));
            return false;
        }
    }
    auto const& ammo = plan->gear[3];
    if (ammo.id)
    {
        if (!StoreNewItemInInventorySlot(ammo.id, std::min(uint32(200), ammo.stack)) || CanUseAmmo(ammo.id) != EQUIP_ERR_OK)
        {
            sLog.outError("CoA starter ammunition %u failed for class %u", ammo.id, uint32(getClass()));
            return false;
        }
        SetAmmo(ammo.id);
    }
    // The supplied offensive ability is discoverable without donor action bars.
    addActionButton(0, 0, plan->spell.id, ACTION_BUTTON_SPELL);
    return true;
}

// 24.2: safe login-time repair. When a CoA-managed character logs in and the
// starter chest or legs slot is empty, grant the planner's chosen item -- but
// only if the character does not already own a copy anywhere (bags, bank,
// equipment or currently-equipped-elsewhere). "Not replacing player-chosen
// equipment" is enforced by GetItemByPos returning null for the destination
// slot; "not duplicating" is enforced by the item-count check across all
// containers. Weapon and ammo slots are deliberately excluded: their absence
// is a player choice (unequipping, banking, disenchant) rather than a shipped
// gap in the character.
bool Player::RepairCoaStarterArmor()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    auto plan = GetCoaStarter();
    if (!plan)
    {
        return false;
    }
    struct { size_t role; uint8 destination; char const* label; } const armour[] = {
        {4, EQUIPMENT_SLOT_CHEST, "chest"},
        {5, EQUIPMENT_SLOT_LEGS,  "legs"},
    };
    for (auto const& pair : armour)
    {
        auto const& item = plan->gear[pair.role];
        if (!item.id)
        {
            continue;   // Plan carries no starter for this slot on this pair.
        }
        if (GetItemByPos(INVENTORY_SLOT_BAG_0, pair.destination))
        {
            continue;   // Slot occupied -- never replace player-chosen gear.
        }
        // GetItemCount(entry, /*inBankAlso*/ true) returns every copy in bags,
        // equipment (other slots) and the bank. A single starter row lives
        // exactly once, so a positive count means the character already got it
        // and moved it elsewhere; do not add another one.
        if (GetItemCount(item.id, true) > 0)
        {
            continue;
        }
        uint16 destination = 0;
        auto error = CanEquipNewItem(pair.destination, destination, item.id, false);
        if (error != EQUIP_ERR_OK)
        {
            sLog.outDetail("CoA starter %s repair skipped for guid %u class %u: cannot equip item %u (%u)",
                pair.label, GetGUIDLow(), uint32(getClass()), item.id, uint32(error));
            continue;
        }
        if (!EquipNewItem(destination, item.id, true))
        {
            sLog.outError("CoA starter %s repair failed for guid %u class %u: EquipNewItem returned false for item %u",
                pair.label, GetGUIDLow(), uint32(getClass()), item.id);
            continue;
        }
        sLog.outString("CoA starter %s repaired for guid %u class %u: granted item %u",
            pair.label, GetGUIDLow(), uint32(getClass()), item.id);
    }
    return true;
}

bool Player::CheckCoaStarterReady()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    auto plan = GetCoaStarter();
    if (!plan || m_coaFailed)
    {
        return false;
    }
    auto spell = sSpellStore.LookupEntry(plan->spell.id);
    if (!spell || !HasItemFitToSpellReqirements(spell))
    {
        return false;
    }
    std::array<coa::StarterItem, 6> equipped{};
    std::set<uint32> skills;
    // Roles: MainHand, OffHand, Ranged, Ammo, Chest, Legs. Ammo (i==3) is a
    // PLAYER_AMMO_ID field, not an equipment slot; the two armour slots come
    // from the ordinary bag-0 equipment layout.
    uint8 const slots[6] = {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED,
        0, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_LEGS};
    for (size_t i = 0; i < equipped.size(); ++i)
    {
        auto item = i == 3 ? nullptr : GetItemByPos(INVENTORY_SLOT_BAG_0, slots[i]);
        auto proto = i == 3 ? ObjectMgr::GetItemPrototype(GetUInt32Value(PLAYER_AMMO_ID))
                            : (item ? item->GetProto() : nullptr);
        if (!proto)
        {
            continue;
        }
        if (i != 3 && (item->IsBroken() || CanUseItem(item, false) != EQUIP_ERR_OK))
        {
            return false;
        }
        equipped[i] = {proto->ItemId, proto->Class, proto->SubClass, proto->InventoryType, Item::GetSkill(proto)};
        if (equipped[i].skill && HasSkill(equipped[i].skill))
        {
            skills.insert(equipped[i].skill);
        }
    }
    uint32 power = plan->spell.power == -2 ? GetHealth() : GetPower(Powers(plan->spell.power));
    uint32 actualCost = Spell::CalculatePowerCost(spell, this);
    bool ready = power >= actualCost && (plan->spell.power != -2 || power > actualCost) &&
        coa::StarterReady(*plan, equipped, skills, GetWeaponProficiency(), GetArmorProficiency(), CanDualWield(),
            HasSpell(plan->spell.id), GetCreateMana(), GetCreateHealth(), power, GetItemCount(equipped[3].id));
    if (!ready)
    {
        sLog.outError("CoA starter readiness failed for class %u spell %u", uint32(getClass()), plan->spell.id);
    }
    return ready;
}

bool Player::IsCoaDefaultSpell(uint32 spell) const
{
    if (spell == 6603) // Attack is universal; donor combat abilities are not.
    {
        return true;
    }
    // [C] 2026-09-12 (ascender-server plan native-coa 23.4): on this realm
    // playercreateinfo_spell for classes 12-32 is the authored starting
    // contract -- tools/make-classes.py's donor BASELINE (weapon and armour
    // skills, Dodge, Detect, Opening, Honorless Target, racials, languages)
    // with the donor's class abilities already stripped -- so every row is a
    // default the character keeps and the save sweep must not manage. Main's
    // category filter below guarded against a stock donor's ability list this
    // realm's generator never writes; it stays for spells that are not rows.
    if (PlayerInfo const* info = sObjectMgr.GetPlayerInfo(getRace(), getClass()))
    {
        for (uint32 row : info->spell)
        {
            if (row == spell)
            {
                return true;
            }
        }
    }
    auto info = sSpellStore.LookupEntry(spell);
    if (!info)
    {
        return false;
    }
    if (IsSpellHaveEffect(info, SPELL_EFFECT_PROFICIENCY) || IsSpellHaveEffect(info, SPELL_EFFECT_DUAL_WIELD))
    {
        auto plan = GetCoaStarter();
        return plan && (plan->proficiencies.count(spell) || plan->nativeProficiencies.count(spell));
    }
    if (IsSpellHaveEffect(info, SPELL_EFFECT_LANGUAGE))
    {
        return true;
    }
    auto bounds = sSpellMgr.GetSkillLineAbilityMapBounds(spell);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        auto skill = sSkillLineStore.LookupEntry(it->second->SkillLine);
        if ((it->second->RaceMask & getRaceMask()) || (skill &&
            (skill->CategoryID == SKILL_CATEGORY_LANGUAGES || skill->CategoryID == SKILL_CATEGORY_WEAPON ||
             skill->CategoryID == SKILL_CATEGORY_ARMOR || skill->CategoryID == SKILL_CATEGORY_PROFESSION ||
             skill->CategoryID == SKILL_CATEGORY_SECONDARY)))
        {
            return true;
        }
    }
    return false;
}

void Player::RememberCoaIndependentSpell(uint32 spell)
{
    if (!IsCoaManaged() || m_coaReconciling || !m_coaReady || !spell)
    {
        return;
    }
    try
    {
        auto shared = SpellClosure({spell});
        m_coaIndependentRoots.insert(spell);
        m_coaAllowedSpells.insert(shared.begin(), shared.end());
    }
    catch (std::exception const& error)
    {
        m_coaFailed = true;
        sLog.outError("CoA independent spell ownership failed for guid %u: %s", GetGUIDLow(), error.what());
        GetSession()->KickPlayer();
    }
}

void Player::LearnCoaQuestSpells(SpellEntry const* reward, bool needsCast)
{
    std::set<uint32> children;
    for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (reward->Effect[i] == SPELL_EFFECT_LEARN_SPELL && reward->EffectTriggerSpell[i])
        {
            children.insert(reward->EffectTriggerSpell[i]);
        }
    }
    if (children.empty() || m_coaFailed)
    {
        return;
    }
    auto previousAllowed = m_coaAllowedSpells;
    try
    {
        if (needsCast)
        {
            auto candidates = SpellClosure(children);
            m_coaAllowedSpells.insert(candidates.begin(), candidates.end());
            CastSpell(this, reward->ID, true);
        }
        m_coaAllowedSpells = previousAllowed;
        for (auto learned : coa::AcceptedQuestGrants(children, [this](uint32 id) { return HasSpell(id); }))
        {
            RememberCoaIndependentSpell(learned);
        }
    }
    catch (std::exception const& error)
    {
        m_coaAllowedSpells = std::move(previousAllowed);
        m_coaFailed = true;
        sLog.outError("CoA quest grant failed for guid %u: %s", GetGUIDLow(), error.what());
        GetSession()->KickPlayer();
    }
}

void Player::RemoveCoaProjectionAuras(uint32 spell)
{
    auto const& holders = GetSpellAuraHolderMap();
    size_t remaining = holders.size();
    for (auto it = holders.begin(); it != holders.end();)
    {
        auto holder = it->second;
        if (holder->GetId() == spell && coa::OwnsProjectionAura(m_coaManagedAuras.count(spell),
            holder->GetCasterGuid() == GetObjectGuid(), !holder->GetCastItemGuid().IsEmpty(),
            holder->IsPassive(), holder->GetAuraMaxDuration()))
        {
            if (!remaining--)
            {
                throw std::runtime_error("CoA aura removal did not converge");
            }
            RemoveSpellAuraHolder(holder);
            it = holders.begin();
        }
        else
        {
            ++it;
        }
    }
}

bool Player::CreateCoaCharacter(uint32 playerClass, std::function<bool()> const& prepare)
{
    if (!GetSession()->CanUseCharacterClass(playerClass) || m_coaState.revision || m_coaCreating)
    {
        return false;
    }
    m_coaCreating = true;
    coa::SqlStore store(CharacterDatabase, {playerClass, 1, 0});
    auto status = store.Create(m_coaState, prepare, [this]
    {
        return m_coaReady && !m_coaFailed && !IsInWorld() && !GetPet() &&
            CheckCoaStarterReady() && QueueCharacterSave(true);
    });
    m_coaCreating = false;
    if (status != coa::CreationStatus::Created)
    {
        m_coaFailed = true;
        if (status == coa::CreationStatus::Failed)
        {
            sLog.outError("CoA character creation commit failed; outcome may be ambiguous");
            GetSession()->KickPlayer();
        }
        return false;
    }
    return true;
}

bool Player::InitializeCoa(bool creating)
{
    if (!GetSession()->CanUseCharacterClass(getClass()))
    {
        return false;
    }
    if (!IsCoaManaged())
    {
        return true;
    }
    try
    {
        auto catalog = sWorld.GetCoaCatalog();
        if (!GetCoaStarter())
        {
            throw std::runtime_error("CoA starter plan missing for character context");
        }
        auto seeds = catalog->AllSpells(getClass());
        // The native starting book (research/ascension-reference, granted by
        // ReconcileCoaSpells on create and at login). In the managed closure
        // so its learned/triggered children resolve the same way catalog
        // spells do; the static set itself is re-unioned at every reconcile,
        // which is what keeps character_spell re-projectable after the
        // managed-spell save sweep.
        for (auto spell : coa::StarterSpells(getClass())) { seeds.insert(spell); }
        // Include class skill spells outside the ordinary purchasable catalog
        // (disabled nodes, old ranks, legacy skill grants). They must not become
        // an alternate authority just because no current ENTRY points at them.
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            auto ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || ability->RaceMask || !(ability->ClassMask & getClassMask()))
            {
                continue;
            }
            auto skill = sSkillLineStore.LookupEntry(ability->SkillLine);
            if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS)
            {
                seeds.insert(ability->Spell);
            }
        }
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            auto talent = sTalentStore.LookupEntry(i);
            auto tab = talent ? sTalentTabStore.LookupEntry(talent->TabID) : nullptr;
            if (tab && (tab->ClassMask & getClassMask()))
            {
                for (auto spell : talent->SpellRank)
                {
                    if (spell)
                    {
                        seeds.insert(spell);
                    }
                }
            }
        }
        if (auto legacy = sObjectMgr.GetClassLevelSpells(getClass()))
        {
            for (auto const& spell : *legacy)
            {
                seeds.insert(spell.second);
            }
        }
        for (auto spell : sWorld.GetCoaTrainingSpells())
        {
            if (IsCoaOrdinaryTrainingSpell(spell))
            {
                seeds.insert(spell);
            }
        }
        auto defaults = sObjectMgr.GetPlayerInfo(getRace(), getClass());
        for (auto spell : defaults->spell)
        {
            if (!IsCoaDefaultSpell(spell))
            {
                seeds.insert(spell);
            }
        }
        auto exists = [](uint32 id) { return sSpellStore.LookupEntry(id) != nullptr; };
        m_coaManagedAuras = coa::RevocationClosure(seeds, SpellDependencies, exists, true);
        auto all = catalog->AllSpells();
        seeds.insert(all.begin(), all.end());
        m_coaManagedSpells = coa::RevocationClosure(seeds, SpellDependencies, exists);
        m_coaState.guid = GetGUIDLow();
        if (creating)
        {
            if (!m_coaCreating)
            {
                throw std::runtime_error("CoA creation requires the checked creation path");
            }
            m_coaState.catalogRevision = catalog->Revision();
            m_coaBuild = catalog->Authorize(getClass(), getLevel(), 0, {});
        }
        else
        {
            coa::SqlStore store(CharacterDatabase, {getClass(), getLevel(), GetUInt32Value(PLAYER_XP)});
            auto status = store.Load(GetGUIDLow(), m_coaState);
            if (status == coa::LoadStatus::Failed)
            {
                throw std::runtime_error("CoA SQL read failed or corrupt header");
            }
            if (status == coa::LoadStatus::Missing)
            {
                std::string error;
                if (coa::Replace(*catalog, store, m_coaState, getClass(), getLevel(), {},
                    std::time(nullptr), m_coaBuild, error) != coa::ApplyStatus::Applied)
                {
                    throw std::runtime_error(error);
                }
            }
            else if (!coa::ValidateLoaded(*catalog, m_coaState, GetGUIDLow(), getClass(), getLevel(), m_coaBuild))
            {
                throw std::runtime_error("CoA durable state failed identity/catalog/build validation");
            }
        }
        m_coaAllowedSpells = SpellClosure(m_coaBuild.spells);
        m_specsCount = 1;
        m_activeSpec = 0;
        m_usedTalentCount = 0;
        m_coaReady = true;
        return true;
    }
    catch (std::exception const& error)
    {
        sLog.outError("Native CoA initialization failed for guid %u: %s", GetGUIDLow(), error.what());
        m_coaFailed = true;
        GetSession()->KickPlayer();
        return false;
    }
}

bool Player::ReconcileCoaSpells()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    if (!m_coaReady || m_coaFailed)
    {
        return false;
    }
    try
    {
        auto desired = m_coaBuild.spells;
        // The native starting book: granted on create and re-granted at login
        // reconcile (make-class-spells.py's playercreateinfo_spell set, now
        // sourced here). Learned below like build roots: managed members are
        // re-projected every login, unmanaged members persist in
        // character_spell.
        for (auto spell : coa::StarterSpells(getClass()))
        {
            if (sSpellStore.LookupEntry(spell)) { desired.insert(spell); }
            else
            {
                sLog.outError("CoA starter spell %u absent from Spell.dbc for class %u", spell, uint32(getClass()));
            }
        }
        // Independent quest/profession/racial roots keep shared dependencies.
        for (auto const& spell : m_spells)
        {
            if (spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled &&
                !m_coaManagedSpells.count(spell.first))
            {
                desired.insert(spell.first);
            }
        }
        desired.insert(m_coaIndependentRoots.begin(), m_coaIndependentRoots.end());
        m_coaAllowedSpells = SpellClosure(desired);
        auto allowedAuras = SpellClosure(desired, true);
        if (m_coaCombatSpells != allowedAuras) { InvalidateCoaCombatRules(); }
        m_coaCombatSpells = allowedAuras;
        m_coaReconciling = true;
        for (auto spell : m_coaManagedSpells)
        {
            if (!m_coaAllowedSpells.count(spell))
            {
                removeSpell(spell, false, false);
            }
        }
        auto const& holders = GetSpellAuraHolderMap();
        size_t remaining = holders.size();
        for (auto it = holders.begin(); it != holders.end();)
        {
            auto holder = it->second;
            if (!allowedAuras.count(holder->GetId()) && coa::OwnsProjectionAura(
                m_coaManagedAuras.count(holder->GetId()), holder->GetCasterGuid() == GetObjectGuid(),
                !holder->GetCastItemGuid().IsEmpty(), holder->IsPassive(), holder->GetAuraMaxDuration()))
            {
                if (!remaining--)
                {
                    throw std::runtime_error("CoA aura reconciliation did not converge");
                }
                RemoveSpellAuraHolder(holder);
                it = holders.begin();
            }
            else
            {
                ++it;
            }
        }
        // Learn roots, not every dependency as active: normal APIs retain passive,
        // auto-learn, rank supersession and triggered-effect semantics.
        for (auto spell : desired)
        {
            if (!HasSpell(spell))
            {
                learnSpell(spell, m_coaManagedSpells.count(spell) != 0);
            }
            if (!HasSpell(spell))
            {
                throw std::runtime_error("CoA authorized spell installation failed: " + std::to_string(spell));
            }
        }
        m_coaReconciling = false;
        RefreshCoaPowerRequirements();
        RefreshCoaCombatAuthority();
        return !m_coaFailed;
    }
    catch (std::exception const& error)
    {
        m_coaReconciling = false;
        m_coaFailed = true;
        sLog.outError("Native CoA spell projection failed for guid %u: %s", GetGUIDLow(), error.what());
        GetSession()->KickPlayer();
        return false;
    }
}

void Player::RefreshCoaCombatAuthority()
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed || m_coaReconciling) { return; }
    std::set<uint32> roots;
    for (auto const& spell : m_spells)
    {
        if (!HasSpell(spell.first)) { continue; }
        if (!m_coaManagedSpells.count(spell.first) || m_coaAllowedSpells.count(spell.first)) { roots.insert(spell.first); }
    }
    auto closure = SpellClosure(roots, true);
    if (closure != m_coaCombatSpells)
    {
        InvalidateCoaCombatRules();
        m_coaCombatSpells = std::move(closure);
    }
    RefreshCoaCombatIntellect();
}

bool Player::SendCoaSnapshot()
{
    if (!IsCoaManaged())
    {
        return true;
    }
    if (!m_coaReady || m_coaFailed || !IsInWorld())
    {
        return false;
    }
    auto descriptor = coa::analytic::BuildActiveStatePacket(0, 1);
    auto entries = coa::analytic::BuildEntriesStatePacket(m_coaState.entries);
    if (descriptor.GetOpcode() != coa::analytic::ActiveStateOpcode || entries.GetOpcode() != coa::analytic::EntriesStateOpcode)
    {
        sLog.outError("CoA snapshot serialization failed for guid %u", GetGUIDLow());
        m_coaFailed = true;
        GetSession()->KickPlayer();
        return false;
    }
    GetSession()->SendPacket(&descriptor);
    GetSession()->SendPacket(&entries);
    return true;
}

bool Player::ChangeCoaLevel(uint32 level)
{
    if (!IsCoaManaged())
    {
        return true;
    }
    if (!m_coaReady || m_coaFailed)
    {
        return false;
    }
    coa::SqlStore store(CharacterDatabase, {getClass(), level, 0});
    std::string error;
    auto status = coa::Replace(*sWorld.GetCoaCatalog(), store, m_coaState, getClass(), level,
        m_coaState.entries, std::time(nullptr), m_coaBuild, error, coa::MutationSource::LevelChange);
    if (status != coa::ApplyStatus::Applied)
    {
        m_coaFailed = true;
        sLog.outError("CoA level refresh failed for guid %u: %s", GetGUIDLow(), error.c_str());
        GetSession()->KickPlayer();
        return false;
    }
    return true;
}

bool Player::ResetCoa(uint32 clearAtLogin)
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed)
    {
        return false;
    }
    coa::SqlStore store(CharacterDatabase, {getClass(), getLevel(), GetUInt32Value(PLAYER_XP), clearAtLogin});
    std::string error;
    if (coa::Replace(*sWorld.GetCoaCatalog(), store, m_coaState, getClass(), getLevel(), {},
        std::time(nullptr), m_coaBuild, error, coa::MutationSource::Reset) != coa::ApplyStatus::Applied)
    {
        m_coaFailed = true;
        sLog.outError("CoA reset failed for guid %u: %s", GetGUIDLow(), error.c_str());
        GetSession()->KickPlayer();
        return false;
    }
    RemoveAtLoginFlag(AtLoginFlags(clearAtLogin), false);
    return ReconcileCoaSpells() && SendCoaSnapshot();
}

bool Player::ApplyCoa(std::vector<coa::analytic::CoaEntry> const& desired)
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed)
    {
        return false;
    }
    coa::SqlStore store(CharacterDatabase, {getClass(), getLevel(), GetUInt32Value(PLAYER_XP)});
    std::string error;
    coa::AuthorizationError refusal;
    auto status = coa::Replace(*sWorld.GetCoaCatalog(), store, m_coaState, getClass(), getLevel(),
        desired, std::time(nullptr), m_coaBuild, error, coa::MutationSource::Request, &refusal);
    if (status == coa::ApplyStatus::Failed)
    {
        m_coaFailed = true;
        sLog.outError("CoA apply failed for guid %u: %s", GetGUIDLow(), error.c_str());
        GetSession()->KickPlayer();
        return false;
    }
    if (status == coa::ApplyStatus::Applied && !ReconcileCoaSpells())
    {
        return false;
    }
    if (!SendCoaSnapshot())
    {
        return false;
    }
    auto token = status == coa::ApplyStatus::Applied ? coa::analytic::ResultToken::UpdateEntriesOk
        : status == coa::ApplyStatus::NoChange ? coa::analytic::ResultToken::NoDiff : coa::analytic::ResultToken::NotTraversible;
    // Refusals carry Replace()'s own reason in the free-form traversal string
    // (the client shows it and never parses it), the same way HandleCoaReplace
    // names its gate. A bare NOT_TRAVERSIBLE cannot say WHICH entry the
    // traversal could not reach, and the client's authorizer and the core's
    // can disagree -- that is exactly what a reader needs to see. The numeric
    // err_entry/err_rank name that entry where the check knows one
    // (not-owned, not-traversable, protected metadata); a whole-set budget
    // refusal has no single offender and stays 0.
    auto result = coa::analytic::BuildUpdateResultPacket(
        token, token == coa::analytic::ResultToken::NotTraversible ? error : std::string(),
        token == coa::analytic::ResultToken::NotTraversible ? refusal.entry : 0,
        token == coa::analytic::ResultToken::NotTraversible ? refusal.rank : 0);
    GetSession()->SendPacket(&result);
    return status == coa::ApplyStatus::Applied;
}

bool WorldSession::AcceptCoaRequest()
{
    if (GetClientProfile() != proto::ConnectionProfile::AscensionStockAuthCoA || !GetPlayer() ||
        PlayerLoading() || isLogingOut() || !GetPlayer()->IsCoaManaged())
    {
        return false;
    }
    auto player = GetPlayer();
    // The second string of 0x72C is free-form (the client shows it as the
    // traversal text); on a refusal it names the gate that refused, so a
    // client trace reads `why=combat` instead of a bare token. The token is
    // unchanged and the string is never parsed by the client.
    if (!m_coaRequestGate.Accept(CoaRequestGate::Clock::now()))
    {
        if (player->SendCoaSnapshot())
        {
            auto result = coa::analytic::BuildUpdateResultPacket(coa::analytic::ResultToken::GameModeNotAllowed, "rate", 0, 0);
            SendPacket(&result);
        }
        return false;
    }
    if (!player->IsInWorld() || !player->IsAlive() || player->IsInCombat() || player->IsBeingTeleported())
    {
        char const* why = !player->IsInWorld() ? "not_in_world"
            : !player->IsAlive() ? "dead"
            : player->IsInCombat() ? "combat"
            : "teleporting";
        auto result = coa::analytic::BuildUpdateResultPacket(coa::analytic::ResultToken::GameModeNotAllowed, why, 0, 0);
        SendPacket(&result);
        return false;
    }
    return true;
}

bool Player::IsCoaTrainer(Creature const* creature)
{
    return creature && creature->GetCreatureInfo() &&
        creature->GetCreatureInfo()->GossipMenuId == CoaTrainerGossipMenu &&
        creature->IsTrainer();
}

bool Player::IsCoaOrdinaryTrainingSpell(uint32 spellId) const
{
    if (!IsCoaManaged() || !sWorld.GetCoaTrainingSpells().count(spellId))
    {
        return false;
    }
    auto spell = sSpellStore.LookupEntry(spellId);
    auto playerClass = sChrClassesStore.LookupEntry(getClass());
    if (!spell || !playerClass || !coa::OrdinaryClassSpell(playerClass->SpellClassSet, spell->SpellClassSet, spell->SpellLevel))
    {
        return false;
    }
    auto bounds = sSpellMgr.GetSkillLineAbilityMapBounds(spellId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        auto ability = it->second;
        auto skill = sSkillLineStore.LookupEntry(ability->SkillLine);
        if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS && ability->MinSkillLineRank <= 1 &&
            (!ability->RaceMask || (ability->RaceMask & getRaceMask())) &&
            (!ability->ClassMask || (ability->ClassMask & getClassMask())) &&
            !(ability->ExcludeRace & getRaceMask()) && !(ability->ExcludeClass & getClassMask()))
        {
            return true;
        }
    }
    return false;
}

bool Player::CanTrainCoaOrdinarySpell(uint32 spellId) const
{
    if (!IsCoaOrdinaryTrainingSpell(spellId))
    {
        return false;
    }
    auto spell = sSpellStore.LookupEntry(spellId);
    uint32 required = spell->SpellLevel;
    if (!IsSpellFitByClassAndRace(spellId, &required))
    {
        return false;
    }
    required = std::max(required, spell->SpellLevel);
    TrainerSpell offer(spellId, 0, 0, 0, required, spellId, true);
    return getLevel() >= required && GetTrainerSpellState(&offer, required) == TRAINER_SPELL_GREEN;
}

bool Player::TrainCoaOrdinarySpell(uint32 spellId)
{
    if (!m_coaReady || m_coaFailed || !CanTrainCoaOrdinarySpell(spellId) || !CharacterDatabase.BeginTransaction())
    {
        return false;
    }
    auto bounds = sSpellMgr.GetSkillLineAbilityMapBounds(spellId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        auto ability = it->second;
        auto skill = sSkillLineStore.LookupEntry(ability->SkillLine);
        if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS && ability->MinSkillLineRank <= 1 &&
            (!ability->RaceMask || (ability->RaceMask & getRaceMask())) &&
            (!ability->ClassMask || (ability->ClassMask & getClassMask())) &&
            !(ability->ExcludeRace & getRaceMask()) && !(ability->ExcludeClass & getClassMask()) &&
            GetPureSkillValue(ability->SkillLine) < 1)
        {
            SetSkill(ability->SkillLine, 1, 1);
        }
    }
    RememberCoaIndependentSpell(spellId);
    learnSpell(spellId, false);
    if (m_coaFailed || !HasSpell(spellId))
    {
        CharacterDatabase.RollbackTransaction();
        m_coaFailed = true;
        GetSession()->KickPlayer();
        return false;
    }
    _SaveSpells();
    _SaveSkills();
    if (!CharacterDatabase.CommitTransactionChecked())
    {
        m_coaFailed = true;
        GetSession()->KickPlayer();
        return false;
    }
    return true;
}

TrainerSpellData Player::GetCoaTrainerSpells() const
{
    TrainerSpellData result;
    result.trainerType = TRAINER_TYPE_CLASS;
    if (!m_coaReady || m_coaFailed || !IsCoaManaged())
    {
        return result;
    }
    // Keep known and future-level services for the native trainer filters.
    // CA entries are not ordinary training: their AE/TE purchases stay in CA UI.
    for (auto id : sWorld.GetCoaTrainingSpells())
    {
        if (!IsCoaOrdinaryTrainingSpell(id))
        {
            continue;
        }
        auto spell = sSpellStore.LookupEntry(id);
        uint32 required = spell->SpellLevel;
        if (IsSpellFitByClassAndRace(id, &required))
        {
            required = std::max(required, spell->SpellLevel);
            result.spellList.emplace(id, TrainerSpell(id, 0, 0, 0, required, id, true));
        }
    }
    return result;
}

void WorldSession::HandleCoaReplace(WorldPacket& packet)
{
    if (!AcceptCoaRequest())
    {
        return;
    }
    auto parsed = coa::analytic::ParseEntriesReplacement(packet);
    if (parsed.status != coa::analytic::ParseStatus::Ok)
    {
        auto result = coa::analytic::BuildUpdateResultPacket(coa::analytic::ResultToken::BadEntry, "", 0, 0);
        SendPacket(&result);
        return;
    }
    GetPlayer()->ApplyCoa(parsed.value.entries);
}

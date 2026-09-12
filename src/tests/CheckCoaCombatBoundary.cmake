# SPDX-License-Identifier: GPL-3.0-or-later
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellCast.cpp" cast)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellChecks.cpp" checks)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellEffectCoa.cpp" adapter)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellEffectDispatch.cpp" dispatch)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerCoaCombat.cpp" player)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerCoa.cpp" progression)
file(READ "${SOURCE_ROOT}/src/game/Object/Unit.cpp" unit)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellHit.cpp" hit)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellAuras.cpp" auras)
file(READ "${SOURCE_ROOT}/src/game/Object/DynamicObject.cpp" area)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellEffectHealPower.cpp" area_effect)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerSave.cpp" save)
file(READ "${SOURCE_ROOT}/src/game/CharacterAdvancement/CoaCombatRules.h" rules_header)
file(READ "${SOURCE_ROOT}/src/game/CharacterAdvancement/CoaCombatRules.cpp" rules)
foreach(token "SendSpellCooldown();" "TakePower();" "TakeReagents();" "TakeAmmo();" "UpdateAchievementCriteria" "e->OnSpellCast")
    string(FIND "${cast}" "castResult = PrepareCoaCombatRules();" prepare)
    string(FIND "${cast}" "${token}" side_effect)
    if(prepare LESS 0 OR side_effect LESS prepare)
        message(FATAL_ERROR "CoA final preparation must precede ${token}")
    endif()
endforeach()
if(NOT checks MATCHES "CheckCast\\(bool strict\\)[ \n]*\\{[ \n]*if \\(auto result = CheckCoaCombatRules" OR
   NOT adapter MATCHES "ResolveTriggerContext" OR NOT adapter MATCHES "CheckCast\\(false\\)" OR
   NOT adapter MATCHES "IsImmuneToSpellEffect" OR NOT adapter MATCHES "CheckRange\\(false\\)")
    message(FATAL_ERROR "CoA admission and native prepared-child target/caster checks disconnected")
endif()
string(FIND "${dispatch}" "eff == 175 || eff == 178 || eff == 183" route)
string(FIND "${dispatch}" "SpellDispatch::IsValidIndex" stock)
if(route LESS 0 OR stock LESS route)
    message(FATAL_ERROR "Custom full-width dispatch must not index the stock table")
endif()
if(player MATCHES "HasSpell\\(4039\\)" OR rules MATCHES "Known\\(c, 4039\\)" OR
   NOT player MATCHES "m_coaBuild.spells.count\\(300755\\)" OR NOT player MATCHES "PolicyResourceEdge")
    message(FATAL_ERROR "Combat spell gates must not mix CA entries with spell IDs or grant all closure resources")
endif()
if(NOT unit MATCHES "AdvanceCombatEpoch\\(\\)" OR NOT unit MATCHES "InvalidateCoaCombatRules" OR
   NOT progression MATCHES "RefreshCoaCombatAuthority" OR NOT hit MATCHES "m_coaTargetLives" OR
   NOT hit MATCHES "m_coaAwaitingImpact" OR NOT player MATCHES "SyncCoaCombatAuras\\(\\)")
    message(FATAL_ERROR "Combat authority, live aura synchronization or weak-identity cancellation is disconnected")
endif()
if(NOT auras MATCHES "spell == 300755 && child == 900755" OR
   NOT auras MATCHES "spell == 500727 && child == 500728" OR NOT save MATCHES "holder->IsCoaControlled\\(\\)")
    message(FATAL_ERROR "Native coalesced timers or transient aura persistence policy disconnected")
endif()
if(rules_header MATCHES "#include [<\"](Unit|Player|World|Database)" OR
   rules MATCHES "#include [<\"](Unit|Player|World|Database)")
    message(FATAL_ERROR "The combat validator must remain value-only")
endif()
if(rules MATCHES "e.vowFulfilled" OR NOT rules MATCHES "c.activeVow == 807749" OR
   NOT adapter MATCHES "e.nativeEventMask = m_procAttacker" OR
   NOT hit MATCHES "uint32 effective = caster->DealSpellDamage" OR
   NOT hit MATCHES "ReportCoaDirectSpell\\(unitTarget, effective")
    message(FATAL_ERROR "Vow bonuses require native owned event classification after effective damage")
endif()
if(NOT adapter MATCHES "m_targets.setDestination\\(location->x, location->y, location->z\\)" OR
   NOT adapter MATCHES "m_coaPreparedAreas\\[i\\] = std::move\\(area\\)" OR
   NOT area_effect MATCHES "location->x, location->y, location->z" OR
   NOT area MATCHES "caster->GetCombatEpoch\\(\\) != m_coaOwnerEpoch")
    message(FATAL_ERROR "Destination execution must use captured local coordinates and owner-epoch cleanup")
endif()

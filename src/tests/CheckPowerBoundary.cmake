# SPDX-License-Identifier: GPL-3.0-or-later
# Connection checks supplement the behavioral production-helper tests.
file(READ "${SOURCE_ROOT}/src/game/Object/Player.cpp" lifecycle)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerRegen.cpp" regen)
file(READ "${SOURCE_ROOT}/src/game/Object/Unit.cpp" unit)
file(READ "${SOURCE_ROOT}/src/game/Object/UnitPower.cpp" power)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerCoa.cpp" coa)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerSpell.cpp" spells)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellPower.cpp" debit)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellChecks.cpp" checks)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellCast.cpp" cast)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/SpellEffectDamageTeleport.cpp" triggers)
file(READ "${SOURCE_ROOT}/src/game/CharacterAdvancement/CoaStarter.h" starter_header)
file(READ "${SOURCE_ROOT}/src/game/CharacterAdvancement/CoaStarter.cpp" starter)

if(NOT lifecycle MATCHES "Unit::SetDeathState\\(s\\);[ \n]+if \\(stateChanged\\)[ \n]+\\{[^}]+ResetPowerRegen\\(\\);" OR
   NOT regen MATCHES "PowerRules::ResetRegeneration\\(m_powerRegenRemainder, m_regenTimer, REGEN_TIME_FULL\\)")
    message(FATAL_ERROR "Real death/revival must reset both fractional carry and the regeneration clock")
endif()
if(NOT checks MATCHES "SpellResourceContext::PowerCost[^\n]*Checks\\(\\)" OR
   NOT debit MATCHES "SpellResourceContext::PowerCost[^\n]*Debits\\(\\)" OR
   NOT cast MATCHES "SpellCastResult castResult = CheckPower\\(\\);" OR
   NOT triggers MATCHES "SpellResourceContext::Trigger::Direct, m_caster, unitTarget" OR
   NOT triggers MATCHES "SpellResourceContext::Trigger::Force, m_caster, unitTarget")
    message(FATAL_ERROR "Resource graph and actual cost/trigger dispatch must share their context rules")
endif()
if(NOT unit MATCHES "PowerRules::ApplyDelta\\(current, maximum, dVal\\)" OR
   NOT unit MATCHES "ApplyPowerMod\\(powertype, Damage, true\\)" OR
   NOT power MATCHES "new_powertype != POWER_MANA && !nativePlayer" OR
   NOT power MATCHES "SetUInt32Value\\(UNIT_FIELD_POWER1 \\+ power, val\\)")
    message(FATAL_ERROR "Power changes must clamp wide values and CoA display switches must not refill")
endif()
foreach(method "bool Player::addSpell" "void Player::removeSpell")
    if(NOT spells MATCHES "${method}[^\n]*[\n]+\\{[\n ]+CoaPowerUpdate powerUpdate\\(\\*this\\);")
        message(FATAL_ERROR "${method}: spellbook mutation must refresh cached resource requirements")
    endif()
endforeach()
if(NOT coa MATCHES "m_coaReconciling = false;[\n ]+RefreshCoaPowerRequirements\\(\\);" OR
   NOT coa MATCHES "auto requirements = coa::RequiredPowers" OR
   NOT coa MATCHES "m_coaRequiredPowers = requirements.powers" OR
   NOT coa MATCHES "spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled && spell.second.active")
    message(FATAL_ERROR "Committed projection and active learned spells must drive the resource cache")
endif()
if(starter MATCHES "RegenerateStarterFocus" OR starter_header MATCHES "RegenerateStarterFocus" OR
   debit MATCHES "-\\(int32\\)m_powerCost" OR
   NOT debit MATCHES "ApplyPowerMod\\(powerType, m_powerCost, false\\)")
    message(FATAL_ERROR "Retired regen and pre-clamp signed cost narrowing must not return")
endif()

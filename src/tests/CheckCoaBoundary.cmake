# SPDX-License-Identifier: GPL-3.0-or-later
# Integration guardrails complementary to pure catalog/store/projection tests.
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerLearn.cpp" learn)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerSpell.cpp" spell)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerLoad.cpp" load)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerSave.cpp" save)
file(READ "${SOURCE_ROOT}/src/game/Object/PlayerCoa.cpp" player)
file(READ "${SOURCE_ROOT}/src/game/Object/Player.cpp" lifecycle)
file(READ "${SOURCE_ROOT}/src/game/Object/Player.h" player_header)
file(READ "${SOURCE_ROOT}/src/game/Object/ItemPersistence.cpp" item_save)
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/AchievementMgr.cpp" achievements)
file(READ "${SOURCE_ROOT}/src/game/Object/Guild.cpp" guild)
file(READ "${SOURCE_ROOT}/src/game/Object/GuildBank.cpp" bank)
file(READ "${SOURCE_ROOT}/src/game/Object/InventoryTransaction.h" inventory_transaction)
if(NOT guild MATCHES "bool Guild::Disband\\(\\)[ \n]*\\{[ \n]*if \\(!CanDisband\\(\\)\\)" OR
   NOT guild MATCHES "GuildBankPersistence::CanCleanup\\(m_bankSaveBlocked, true\\)" OR
   NOT guild MATCHES "GuildBankPersistence::DeleteItems\\(m_TabListMap, m_bankSaveBlocked, alsoInDB\\)" OR
   NOT guild MATCHES "DeleteGuildBankItems\\(false\\)" OR guild MATCHES "pItem->DeleteFromDB\\(\\)")
    message(FATAL_ERROR "Guild disband and direct durable cache destruction must honor bank quarantine; memory-only destruction remains allowed")
endif()
if(NOT bank MATCHES "Guild::_StoreItem[^\n]*[\n]*\\{[\n ]*if \\(m_bankSaveBlocked" OR
   NOT bank MATCHES "Guild::RemoveItem[^\n]*[\n]*\\{[\n ]*if \\(m_bankSaveBlocked" OR
   NOT bank MATCHES "Guild::LoadGuildBankFromDB[^\n]*[\n]*\\{[\n ]*if \\(m_bankSaveBlocked")
    message(FATAL_ERROR "Stale bank cache mutation/deletion/reload must be refused below UI entry points")
endif()
string(FIND "${inventory_transaction}" "m_database.IsTransactionActive()" queue_gate)
string(FIND "${inventory_transaction}" "m_database.BeginTransaction()" queue_begin)
if(queue_gate LESS 0 OR queue_begin LESS queue_gate)
    message(FATAL_ERROR "Inventory ownership must check this database's thread-local queue before any nested Begin, including empty/disjoint owners")
endif()
foreach(caller Object/Player.cpp WorldHandlers/GuildHandler.cpp WorldHandlers/GuildMgr.cpp
               ChatCommands/GuildCommands.cpp WorldHandlers/CharacterHandler.cpp
               WorldHandlers/AccountMgr.cpp ChatCommands/PlayerCommands.cpp)
    file(READ "${SOURCE_ROOT}/src/game/${caller}" deletion_caller)
    if(deletion_caller MATCHES "\n[ \t]*[A-Za-z_]+->Disband\\(\\);" OR
       deletion_caller MATCHES "\n[ \t]*Player::DeleteFromDB\\([^\n]*\\);")
        message(FATAL_ERROR "${caller}: deletion refusal must propagate before deleting guild state or acknowledging success")
    endif()
endforeach()
if(NOT save MATCHES "bool Player::SaveInventoryAndGoldToDB\\(\\)[ \n]*\\{[ \n]*return WithInventorySavePreflight" OR
   NOT player_header MATCHES "IsSaveBlocked\\(\\) const \\{ return m_saveBlocked \\|\\| m_coaFailed;" OR
   NOT item_save MATCHES "owner->IsSaveBlocked\\(\\)")
    message(FATAL_ERROR "Inventory failures must propagate and poison ordinary as well as native/item saves")
endif()
if(NOT achievements MATCHES "transaction->OnCommit\\(\\[this, achievement\\]" OR
   NOT save MATCHES "m_inventoryTransaction->OnCommit\\(\\[this\\] \\{ SaveToDB\\(\\);")
    message(FATAL_ERROR "Nested achievement reward mail and script full saves must wait for the inventory commit")
endif()
foreach(owner WorldHandlers/TradeHandler.cpp WorldHandlers/MailHandler.cpp
              WorldHandlers/AuctionHouseHandler.cpp WorldHandlers/GuildHandler.cpp
              Object/AuctionHouseMgr.cpp Object/GuildBank.cpp)
    file(READ "${SOURCE_ROOT}/src/game/${owner}" transaction_owner)
    if(NOT transaction_owner MATCHES "transaction.Begin\\(\\)" OR
       NOT transaction_owner MATCHES "transaction.Commit\\(\\)" OR
       transaction_owner MATCHES "->SaveInventoryAndGoldToDB\\(\\)")
        message(FATAL_ERROR "${owner}: inventory owners must use the shared checked participant transaction")
    endif()
endforeach()
if(NOT save MATCHES "WithInventorySavePreflight\\(\\[this, createOnly\\].*QueueValidatedCharacterSave\\(createOnly\\)" OR
   NOT save MATCHES "void Player::_SaveInventory\\(\\)" OR
   save MATCHES "if \\(!_SaveInventory\\(\\)\\)")
    message(FATAL_ERROR "All character saves must preflight inventory before consuming dirty state; inventory saving must not reject late")
endif()
foreach(file Player.cpp PlayerQuest.cpp PlayerZone.cpp PlayerSave.cpp)
    file(READ "${SOURCE_ROOT}/src/game/Object/${file}" level_source)
    if(level_source MATCHES "getConfig\\(CONFIG_UINT32_MAX_PLAYER_LEVEL\\)")
        message(FATAL_ERROR "${file}: player progression must use the per-player level cap")
    endif()
endforeach()
file(READ "${SOURCE_ROOT}/src/game/WorldHandlers/CharacterHandler.cpp" login)
file(READ "${SOURCE_ROOT}/src/game/Server/WorldGateway.cpp" auth)
file(READ "${SOURCE_ROOT}/src/game/Server/WorldSession.cpp" session)
file(READ "${SOURCE_ROOT}/src/game/Server/OpcodeTable.cpp" opcodes)
if(NOT session MATCHES "void WorldSession::ExecuteOpcode[^\n]*[\n]*\\{[\n ]*if \\(_player && _player->IsSaveBlocked\\(\\)\\)" OR
   NOT session MATCHES "void WorldSession::LogoutPlayer[^\n]*[\n]*\\{[\n ]*if \\(_player && _player->IsSaveBlocked\\(\\)\\)")
    message(FATAL_ERROR "Failed inventory participants must not process further input or save during logout")
endif()

if(NOT learn MATCHES "void Player::LearnClassLevelSpells\\(\\)[ \n]*\\{[ \n]*if \\(IsCoaManaged\\(\\)\\)" OR
   NOT learn MATCHES "if \\(!IsCoaDefaultSpell\\(tspell\\)\\)")
    message(FATAL_ERROR "CoA must bypass legacy class-level and donor combat default grants")
endif()
if(NOT spell MATCHES "talentPos = IsCoaManagedSpell\\(spell_id\\) \\? nullptr : nativeTalentPos" OR
   NOT spell MATCHES "if \\(nativeTalentPos && IsSpellHaveEffect")
    message(FATAL_ERROR "CoA Talent.dbc overlap needs ownership-based bookkeeping exclusion AND native trigger mechanics")
endif()
if(NOT load MATCHES "if \\(IsCoaManagedSpell\\(spell_id\\)\\)" OR
   load MATCHES "DELETE FROM `character_spell` WHERE `spell`" OR
   NOT save MATCHES "if \\(IsCoaManagedSpell\\(itr->first\\)\\)")
    message(FATAL_ERROR "CA projection must not load/save as stock spells or globally delete another player's rows")
endif()
if(NOT lifecycle MATCHES "SendItemDurations\\(\\);[^;]*SendCoaSnapshot\\(\\);" OR
   NOT player MATCHES "m_coaFailed \\|\\| !IsInWorld\\(\\)" OR
   NOT player MATCHES "SendPacket\\(&descriptor\\);[ \n]*GetSession\\(\\)->SendPacket\\(&entries\\);")
    message(FATAL_ERROR "CA snapshot must follow player identity creation and its loadout descriptor")
endif()
if(NOT lifecycle MATCHES "CanUseCharacterClass\\(class_\\)" OR
   NOT load MATCHES "CanUseCharacterClass\\(fields\\[4\\]" OR
   NOT lifecycle MATCHES "if \\(!ChangeCoaLevel\\(level\\)\\)" OR
   NOT save MATCHES "if \\(IsSaveBlocked\\(\\) \\|\\| m_coaCreating" OR
   learn MATCHES "RememberCoaIndependentSpell\\(spell_id\\)" OR
   NOT player MATCHES "RemoveAtLoginFlag\\(AtLoginFlags\\(clearAtLogin\\), false\\)")
    message(FATAL_ERROR "CoA lifecycle must gate class/profile, commit level before mutation, preserve quest cast-only roots and consume durable resets")
endif()
string(FIND "${load}" "CanUseCharacterClass(fields[4]" class_gate)
string(FIND "${load}" "ObjectMgr::CheckPlayerName" name_gate)
if(class_gate LESS 0 OR name_gate LESS class_gate OR
   NOT login MATCHES "CreateCoaCharacter\\(class_, prepare\\)" OR
   NOT player MATCHES "CheckCoaStarterReady\\(\\) && QueueCharacterSave\\(true\\)" OR
   NOT save MATCHES "if \\(!createOnly\\)")
    message(FATAL_ERROR "CoA creation must use checked insert-only persistence; login profile checks must precede rename writes")
endif()
file(READ "${SOURCE_ROOT}/src/game/ChatCommands/PlayerCommands.cpp" commands)
file(READ "${SOURCE_ROOT}/src/game/ChatCommands/PlayerStateCommands.cpp" levelup)
if(NOT commands MATCHES "AdmitCharacterClass\\(proto::ConnectionProfile::Stock, playerClass, false\\)" OR
   NOT commands MATCHES "AND `class` BETWEEN 1 AND 11" OR
   NOT commands MATCHES "if \\(!HandleCharacterLevel" OR
   NOT levelup MATCHES "if \\(!HandleCharacterLevel")
    message(FATAL_ERROR "Offline native leveling must fail without a write or a success notification")
endif()
if(NOT auth MATCHES "row->profile = result.profile;" OR
   NOT auth MATCHES "std::move\\(admissionContext\\), row->profile" OR
   NOT session MATCHES "packet->GetOpcode\\(\\) >= SessionOpcodeCount" OR
   NOT opcodes MATCHES "0x727, [^\n]*STATUS_LOGGEDIN, PROCESS_THREADUNSAFE")
    message(FATAL_ERROR "CA requires verified profile snapshot, bounds-checked sends and world-thread logged-in dispatch")
endif()

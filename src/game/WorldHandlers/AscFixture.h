/**
 * AscFixture -- the tester-owned world fixtures, in the core.
 *
 * This is the C++ successor of the three Eluna fixture scripts
 * `resources_probe.lua`, `items_probe.lua` and `dungeons_probe.lua`: the same
 * SAY command words, the same JSON line shapes, the same per-fixture files, so
 * the gates that drive them (`class_resources_test`, `felsworn_test`,
 * `ascension_class_bar_test`, `items_test`, `dungeons_test`,
 * `remote_parity_test`) read this without change. It exists so the realm can
 * be built with `-DSCRIPT_LIB_ELUNA=OFF` (PLAN 23.6): those three were the only
 * Lua left after the production set was retired (23.5) and the world-event
 * probe became `AscProbe` (23.6).
 *
 * A FIXTURE, NOT A FEATURE. Every verb here exists to put the world into a
 * state a gate can measure -- spawn a passive target, grant a weapon, fill a
 * power bar, teleport to a clean spot, arm a boss. None of it is reachable by
 * a player: `Handle` refuses unless the caller is a GM (SEC_GAMEMASTER) AND
 * the fixture file is configured, which the shipped `mangosd.conf` does not do.
 * Off unless `Ascension.Fixture.<name>.Path` names a file.
 *
 * The command surface is the Lua's, verbatim, so no gate changes:
 *   rp ping|tele|spawn|despawn|fillpower|give|auras     (resources)
 *   ip additem|removeitem|gearskills|spawngo            (items)
 *   dp ping|tele|goto|scan|arm|engage|kill              (dungeons)
 */
#ifndef MANGOS_H_ASC_FIXTURE
#define MANGOS_H_ASC_FIXTURE

#include "Platform/Define.h"
#include <string>

class Player;

namespace AscFixture
{
    /// Read the `Ascension.Fixture.*.Path` keys from the loaded config.
    void Init();
    bool Enabled();

    /// Write the `boot` line the gates read as proof a fixture is armed.
    void Boot();

    /**
     * Offer one SAY line to the fixtures. Returns true when the line was a
     * fixture command and has been handled -- the caller must then swallow it
     * (not broadcast it), exactly as the Eluna handler's `false` return did.
     */
    bool Handle(Player* player, std::string const& msg);
}

#endif

// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaCombatRules.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace coa::combat
{
    namespace
    {
        // Small authored policy bindings. Full native tuples/provenance stay private.
        constexpr uint32_t InnerDemonDurationMs = 30000;
        constexpr Resource Resources[] = {
            {500149, 27, 20, 804584, false}, {500706, 25, 100, 0, false},
            {500906, 17, 6, 0, false}, {557325, 21, 20, 0, false},
            {560414, 30, 1, 0, false}, {561136, 13, 5, 0, false},
            {570131, 23, 10, 0, false}, {680441, 31, 15, 0, true},
            {680687, 20, 10, 0, false}, {680854, 29, 15, 0, false}, {706613, 20, 10, 0, false},
            {707133, 23, 15, 0, false}, {800058, 14, 6, 0, false},
            {801324, 30, 8, 0, false}, {801816, 28, 100, 0, false},
            {803102, 16, 100, 0, false}, {804068, 15, 8, 0, false},
            {804301, 24, 5, 0, true}, {804329, 21, 5, 0, false},
            {804378, 26, 4, 0, false}, {804581, 32, 3, 0, false},
            {804583, 32, 3, 0, false}, {804670, 25, 5, 0, false},
            {804711, 25, 5, 0, false}, {804735, 26, 5, 0, false},
            {805077, 30, 5, 0, false}, {805095, 29, 10, 0, false},
            {806068, 31, 5, 0, false}, {806269, 22, 5, 0, false},
            {806554, 31, 5, 0, false}, {807389, 24, 150, 300755, false},
            {807533, 24, 5, 300755, false}, {500363, 30, 3, 0, false}
        };
        constexpr Binding Bindings[] = {
            {500363, 2, 175, 30, Operation::ConvertSoul, 805077, 805077, -5, 7},
            {500728, 0, 175, 25, Operation::Delta, 500706, 500706, -2, 2},
            {501421, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501422, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501423, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501424, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501425, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501426, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501427, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501428, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501429, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501430, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501431, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {501432, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {505184, 0, 175, 13, Operation::Delta, 561136, 561136, -1, 0},
            {520318, 0, 175, 20, Operation::Delta, 680687, 680687, 10, 0},
            {520501, 0, 175, 30, Operation::Delta, 560414, 560414, 1, 0},
            {520773, 1, 175, 25, Operation::Delta, 500706, 500706, 3, 1},
            {520791, 1, 175, 21, Operation::Delta, 804329, 804329, 5, 1},
            {524905, 0, 175, 20, Operation::Delta, 680687, 680687, 2, 0},
            {524912, 0, 175, 17, Operation::Delta, 500906, 500906, 1, 0},
            {524955, 0, 175, 17, Operation::Delta, 500906, 500906, 2, 0},
            {557334, 0, 175, 21, Operation::Delta, 557325, 557325, 1, 0},
            {560142, 1, 175, 31, Operation::Delta, 680441, 680441, -1, 0},
            {561113, 0, 175, 30, Operation::Delta, 560414, 560414, -1, 0},
            {567555, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {570132, 0, 175, 23, Operation::Delta, 570131, 570131, 1, 0},
            {572381, 0, 175, 24, Operation::Delta, 804301, 804301, 6, 2},
            {572806, 0, 175, 24, Operation::Delta, 807533, 807533, 1, 0},
            {572861, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572862, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572863, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572864, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572865, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572866, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {572867, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {573215, 0, 175, 31, Operation::Delta, 806068, 806068, 2, 0},
            {681072, 0, 175, 31, Operation::Delta, 680441, 680441, 2, 0},
            {681103, 0, 175, 25, Operation::Delta, 500706, 500706, 1, 0},
            {705672, 2, 175, 16, Operation::Delta, 803102, 803102, -20, 0},
            {706535, 0, 175, 28, Operation::Delta, 801816, 801816, 3, 0},
            {707474, 0, 175, 28, Operation::Delta, 801816, 801816, -5, 0},
            {707579, 0, 175, 28, Operation::Delta, 801816, 801816, 5, 0},
            {707596, 0, 175, 28, Operation::Delta, 801816, 801816, 20, 0},
            {707737, 0, 175, 23, Operation::Delta, 707133, 707133, -1, 0},
            {800060, 0, 175, 14, Operation::Delta, 800058, 800058, 1, 0},
            {800171, 0, 175, 30, Operation::Delta, 801324, 801324, 1, 0},
            {800764, 2, 175, 27, Operation::Delta, 500149, 500149, 10, 2},
            {801024, 0, 175, 14, Operation::Delta, 800058, 800058, 2, 0},
            {801035, 0, 175, 16, Operation::Delta, 803102, 803102, 5, 0},
            {801844, 2, 175, 16, Operation::Delta, 804084, 803102, 20, 0},
            {801951, 2, 175, 21, Operation::Delta, 804329, 804329, 5, 0},
            {803207, 0, 175, 29, Operation::Delta, 680854, 680854, 1, 0},
            {804044, 1, 175, 16, Operation::Delta, 803102, 803102, 5, 1},
            {804069, 0, 175, 15, Operation::Delta, 804068, 804068, 2, 0},
            {804084, 1, 175, 16, Operation::Delta, 803102, 803102, 10, 1},
            {804086, 1, 175, 16, Operation::Delta, 803102, 803102, 20, 1},
            {804098, 1, 175, 27, Operation::Delta, 500149, 500149, 1, 1},
            {804209, 0, 175, 25, Operation::Delta, 500706, 500706, 5, 0},
            {804210, 0, 175, 25, Operation::Delta, 500706, 500706, 10, 0},
            {804566, 1, 175, 32, Operation::Delta, 804581, 804581, -1, 0},
            {804567, 1, 175, 32, Operation::Delta, 804583, 804583, -1, 0},
            {804585, 1, 175, 27, Operation::Delta, 500149, 500149, -1, 1},
            {804736, 1, 175, 26, Operation::Delta, 804735, 804735, -1, 0},
            {804826, 1, 175, 16, Operation::Delta, 803102, 803102, 100, 0},
            {804995, 2, 175, 26, Operation::Delta, 804378, 804378, -1, 0},
            {805102, 0, 175, 29, Operation::Delta, 805095, 805095, 15, 0},
            {805239, 1, 175, 14, Operation::Delta, 800058, 800058, 6, 4},
            {805669, 0, 175, 17, Operation::Delta, 500906, 500906, 6, 0},
            {806270, 0, 175, 22, Operation::Delta, 806269, 806269, 1, 0},
            {806589, 0, 175, 31, Operation::Delta, 806554, 806554, 1, 0},
            {807278, 1, 175, 25, Operation::Delta, 500706, 500706, 2, 1},
            {807390, 0, 175, 24, Operation::Delta, 807389, 807389, 30, 0},
            {807391, 0, 175, 24, Operation::Delta, 807389, 807389, 20, 0},
            {807392, 0, 175, 24, Operation::Delta, 807389, 807389, 5, 0},
            {807394, 0, 175, 24, Operation::Delta, 807389, 807389, 10, 0},
            {807536, 0, 175, 24, Operation::Delta, 807533, 807533, -1, 0},
            {807650, 0, 175, 16, Operation::Delta, 803102, 803102, 2, 0},
            {807693, 0, 175, 25, Operation::Cap, 804670, 804670, 1, 0},
            {807693, 1, 175, 25, Operation::Cap, 804711, 804711, 1, 0},

            {505158, 1, 178, 13, Operation::Duration, 505211, 0, 0, 0},
            {520533, 0, 178, 30, Operation::Duration, 570097, 0, 0, 0},
            {520868, 0, 178, 24, Operation::Duration, 1604, 0, 0, 0},
            {538442, 0, 178, 24, Operation::Duration, 800103, 0, 0, 2},
            {560036, 1, 178, 32, Operation::Duration, 808089, 0, 0, 0},
            {560233, 1, 178, 32, Operation::Duration, 520417, 0, 0, 1},
            {573451, 0, 178, 16, Operation::SummonDuration, 573438, 0, 0, 0},
            {574160, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {574161, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {574162, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681119, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681223, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681224, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681225, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681226, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {681227, 1, 178, 31, Operation::Duration, 573070, 0, 0, 0},
            {704235, 2, 178, 29, Operation::Duration, 706000, 0, 0, 0},
            {707191, 0, 178, 29, Operation::Duration, 800914, 0, 0, 0},
            {712446, 0, 178, 31, Operation::Duration, 680421, 0, 0, 0},
            {712448, 0, 178, 27, Operation::Duration, 804751, 0, 4000, 0},
            {803557, 0, 178, 23, Operation::Duration, 807653, 0, 0, 0},
            {806039, 1, 178, 25, Operation::DurationAtDestination, 520418, 0, 0, 0},
            {807673, 1, 178, 26, Operation::Duration, 801975, 0, 0, 1},

            {500241, 0, 183, 28, Operation::Delay, 760384, 0, 0, 0},
            {500483, 1, 183, 30, Operation::Delay, 500481, 0, 0, 0},
            {500483, 2, 183, 30, Operation::Delay, 500481, 0, 0, 0},
            {500712, 2, 183, 25, Operation::Delay, 354191, 0, 0, 0},
            {502951, 1, 183, 29, Operation::Delay, 803539, 0, 0, 0},
            {502952, 1, 183, 29, Operation::Delay, 803540, 0, 0, 0},
            {502953, 1, 183, 29, Operation::Delay, 803541, 0, 0, 0},
            {502954, 1, 183, 29, Operation::Delay, 803542, 0, 0, 0},
            {502955, 1, 183, 29, Operation::Delay, 803543, 0, 0, 0},
            {502956, 1, 183, 29, Operation::Delay, 803544, 0, 0, 0},
            {502957, 1, 183, 29, Operation::Delay, 803545, 0, 0, 0},
            {520024, 0, 183, 27, Operation::Delay, 520026, 0, 0, 0},
            {520026, 2, 183, 27, Operation::Delay, 520028, 0, 0, 0},
            {520186, 1, 183, 22, Operation::DelayAtDestination, 520205, 0, 0, 0},
            {520402, 1, 183, 24, Operation::Delay, 520404, 0, 0, 0},
            {520452, 0, 183, 17, Operation::DelayResource, 500906, 500906, 1, 0},
            {520504, 0, 183, 19, Operation::Delay, 520505, 0, 0, 0},
            {524739, 1, 183, 19, Operation::Delay, 524740, 0, 0, 0},
            {524739, 2, 183, 19, Operation::Delay, 524738, 0, 0, 0},
            {524994, 0, 183, 24, Operation::Delay, 800790, 0, 0, 0},
            {525371, 0, 183, 13, Operation::Delay, 525399, 0, 0, 0},
            {525617, 0, 183, 14, Operation::DelayResource, 800058, 800058, 1, 0},
            {547525, 0, 183, 32, Operation::Delay, 502827, 0, 0, 0},
            {560155, 0, 183, 31, Operation::Delay, 680448, 0, 0, 0},
            {560248, 1, 183, 29, Operation::DelayAtDestination, 520663, 0, 0, 0},
            {560518, 2, 183, 12, Operation::Delay, 800327, 0, 0, 0},
            {561010, 2, 183, 12, Operation::Delay, 800327, 0, 0, 0},
            {561011, 2, 183, 12, Operation::Delay, 800327, 0, 0, 0},
            {561012, 2, 183, 12, Operation::Delay, 800327, 0, 0, 0},
            {561013, 2, 183, 12, Operation::Delay, 800327, 0, 0, 0},
            {561294, 0, 183, 30, Operation::Delay, 561290, 0, 0, 0},
            {567577, 0, 183, 16, Operation::Delay, 570039, 0, 0, 0},
            {572368, 0, 183, 26, Operation::Delay, 572418, 0, 0, 0},
            {573038, 1, 183, 30, Operation::Delay, 800797, 0, 0, 0},
            {573215, 1, 183, 31, Operation::Delay, 680472, 0, 0, 0},
            {575614, 0, 183, 30, Operation::DelayResource, 500363, 500363, 1, 0},
            {575614, 1, 183, 30, Operation::Delay, 524732, 0, 0, 0},
            {587253, 0, 183, 18, Operation::Delay, 572612, 0, 0, 0},
            {680338, 0, 183, 30, Operation::DelayResource, 500363, 500363, 1, 0},
            {680842, 2, 183, 24, Operation::Delay, 681265, 0, 0, 0},
            {681472, 1, 183, 27, Operation::Delay, 681531, 0, 0, 0},
            {704554, 0, 183, 30, Operation::DelayResource, 500363, 500363, 1, 0},
            {704679, 1, 183, 23, Operation::Delay, 705103, 0, 0, 0},
            {707068, 0, 183, 15, Operation::Delay, 707581, 0, 0, 0},
            {707422, 0, 183, 13, Operation::Delay, 806288, 0, 0, 0},
            {800764, 1, 183, 27, Operation::SolarMarkers, 704396, 500149, 0, 0},
            {800899, 1, 183, 29, Operation::Delay, 803538, 0, 0, 0},
            {801277, 0, 183, 22, Operation::Delay, 802600, 0, 0, 0},
            {801327, 1, 183, 30, Operation::Delay, 504564, 0, 0, 0},
            {802631, 0, 183, 32, Operation::DelayAtDestination, 803745, 0, 0, 0},
            {802870, 0, 183, 18, Operation::Delay, 802871, 0, 0, 0},
            {803030, 0, 183, 30, Operation::Delay, 570097, 0, 0, 0},
            {804470, 0, 183, 22, Operation::Delay, 804490, 0, 0, 0},
            {804826, 0, 183, 16, Operation::Delay, 803101, 0, 0, 0},
            {804924, 1, 183, 19, Operation::Delay, 704576, 0, 0, 0},
            {805077, 1, 183, 30, Operation::Fragments, 805078, 805077, 0, 0},
            {806109, 2, 183, 14, Operation::Delay, 807235, 0, 0, 0},
            {806415, 0, 183, 31, Operation::DelayAtDestination, 807859, 0, 0, 0},
            {806415, 2, 183, 31, Operation::DelayAtDestination, 807120, 0, 0, 0},
            {807425, 0, 183, 14, Operation::Delta, 800058, 800058, -2, 0},
            {807761, 0, 183, 29, Operation::Delay, 807631, 0, 0, 0}
        };

        bool Known(Context const& c, uint32_t spell)
        {
            return c.known && c.known(c.data, spell);
        }
        bool SelfAura(Context const& c, uint32_t spell)
        {
            return c.selfAura && c.selfAura(c.data, spell);
        }
        bool Authorized(Context const& c, uint32_t spell, uint32_t resource = 0)
        {
            return c.authorized && c.authorized(c.data, spell, resource);
        }
        NativeSpell Native(Context const& c, uint32_t spell)
        {
            return c.nativeSpell ? c.nativeSpell(c.data, spell) : NativeSpell{};
        }
        bool ResourceAllowed(Context const& c, Resource const& r)
        {
            return c.playerClass == r.playerClass && (!r.knownMarker || Known(c, r.knownMarker));
        }
        bool TargetValid(Context const& c, Identity const& target, Target policy)
        {
            if (!target.guid || !target.generation || target.instance != c.actor.instance ||
                target.map != c.actor.map || target.frame != c.actor.frame || target.deck != c.actor.deck)
            {
                return false;
            }
            if (policy == Target::Self)
            {
                return target == c.actor && c.alive && c.inWorld;
            }
            if ((policy != Target::Friendly && policy != Target::Enemy) || !c.resolve)
            {
                return false;
            }
            auto r = c.resolve(c.data, target);
            return r.identity == target && r.exists && r.alive && r.inWorld && r.inRange &&
                (policy == Target::Friendly ? r.friendly : !r.friendly);
        }
        bool StillHere(Context const& c, Identity const& target)
        {
            if (target == c.actor) { return true; }
            if (!c.resolve) { return false; }
            auto r = c.resolve(c.data, target);
            return r.identity == target && r.exists && r.alive && r.inWorld;
        }
        bool DestinationValid(Context const& c, Destination const& d)
        {
            if (d.owner != c.actor || !d.phase || d.recipients.size > 20 ||
                !std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) ||
                std::abs(d.x) > 32768 || std::abs(d.y) > 32768 || std::abs(d.z) > 32768 ||
                (d.mode != DestinationMode::Recipients && d.mode != DestinationMode::PersistentArea) ||
                (d.mode == DestinationMode::PersistentArea && d.recipients.size)) { return false; }
            for (size_t i = 0; i < d.recipients.size; ++i)
            {
                auto const& id = d.recipients.values[i];
                if (!id.guid || !id.generation || id.map != c.actor.map || id.instance != c.actor.instance ||
                    id.frame != c.actor.frame || id.deck != c.actor.deck) { return false; }
                for (size_t j = 0; j < i; ++j)
                {
                    if (id == d.recipients.values[j]) { return false; }
                }
            }
            return c.destinationValid && c.destinationValid(c.data, d);
        }
        bool PendingValid(Context const& c, CastIntent const& cast)
        {
            if (cast.source != c.actor || cast.originalCaster != c.actor ||
                !Authorized(c, cast.parent) || !Native(c, cast.spell).exists) { return false; }
            if (auto* location = std::get_if<Destination>(&cast.location))
            {
                if (!DestinationValid(c, *location)) { return false; }
                if (location->mode == DestinationMode::PersistentArea || !location->recipients.size) { return true; }
                for (size_t i = 0; i < location->recipients.size; ++i)
                {
                    if (StillHere(c, location->recipients.values[i])) { return true; }
                }
                return false;
            }
            if (!StillHere(c, cast.target)) { return false; }
            if (cast.kind != IntentKind::ResourceGain) { return true; }
            auto* resource = FindResource(cast.resource);
            return resource && ResourceAllowed(c, *resource) && Authorized(c, cast.parent, cast.resource);
        }
        Aura* FindAura(State& s, uint32_t spell, Identity const& target)
        {
            for (std::size_t i = 0; i < s.auras.size; ++i)
            {
                auto& a = s.auras.values[i];
                if (a.spell == spell && a.target == target) { return &a; }
            }
            return nullptr;
        }
        Aura& Cell(State& s, uint32_t spell, Identity const& target)
        {
            if (auto* a = FindAura(s, spell, target)) { return *a; }
            if (!s.auras.Push({spell, target, s.actor})) { throw Status::Limit; }
            return s.auras.values[s.auras.size - 1];
        }
        uint64_t Expiry(Context const& c, uint32_t spell, bool required = false)
        {
            auto info = Native(c, spell);
            if (!info.exists) { throw Status::Failed; }
            if (info.durationMs < -1 || info.durationMs > int32_t(MaxAuraDurationMs) ||
                (required && info.durationMs <= 0)) { throw Status::Invalid; }
            return info.durationMs > 0 ? c.nowMs + uint32_t(info.durationMs) : 0;
        }
        bool IsCurrency(uint32_t spell)
        {
            return spell == 500149 || spell == 500706 || spell == 807389 ||
                spell == 807533 || spell == 805077 || spell == 500363;
        }
        void SetMarker(Context const& c, State& s, uint32_t spell, bool active)
        {
            if (!active && !FindAura(s, spell, c.actor)) { return; }
            if (active && !Native(c, spell).exists) { throw Status::Failed; }
            auto& a = Cell(s, spell, c.actor);
            a.stacks = active ? 1 : 0;
            a.expiresMs = 0;
        }
        void Delta(Context const& c, State& s, uint32_t source, uint32_t spell,
            Identity const& target, int32_t amount, bool spend, bool decay = false)
        {
            auto* r = FindResource(spell);
            if (!r || !ResourceAllowed(c, *r) || !Authorized(c, source, spell))
            {
                throw Status::Unauthorized;
            }
            if (!Native(c, spell).exists) { throw Status::Failed; }
            if (amount < -10000 || amount > 10000) { throw Status::Invalid; }
            if (!IsTargetAuraResource(spell) && target != c.actor) { throw Status::Invalid; }
            if (spell == 500149 && amount > 0 && Count(s, 807440, c.actor)) { return; }
            if (spell == 500706 && amount < 0 && Count(s, 803061, c.actor) && !decay) { return; }
            auto& a = Cell(s, spell, target);
            int64_t total = int64_t(a.stacks) + amount;
            if (total < 0 && spend) { throw Status::Insufficient; }
            if (spell == 807389 && total >= 100)
            {
                int32_t embers = int32_t(total / 100);
                total %= 100; // Whole conversions at Ember cap are discarded, remainder retained.
                Delta(c, s, source, 807533, c.actor, embers, false);
            }
            if (spell == 805077 && total >= 5)
            {
                int32_t souls = int32_t(total / 5);
                total %= 5;
                Delta(c, s, source, 500363, c.actor, souls, false);
            }
            auto cap = spell == 807389 ? 99 : int32_t(r->cap) + a.capBonus;
            if (amount > 0 && (!a.stacks || !r->nonRefresh) && !IsCurrency(spell))
            {
                a.expiresMs = Expiry(c, spell, r->nonRefresh);
            }
            a.stacks = int32_t(std::clamp<int64_t>(total, 0, cap));
            if (!a.stacks) { a.expiresMs = 0; }
        }
        void Markers(Context const& c, State& s, bool felswornCapped)
        {
            if (c.playerClass == 27)
            {
                int32_t solar = Count(s, 500149, c.actor);
                SetMarker(c, s, 802938, solar >= 5);
                SetMarker(c, s, 802939, solar >= 10);
                SetMarker(c, s, 804586, solar >= 20);
                SetMarker(c, s, 704396, solar >= 20);
            }
            if (c.playerClass == 30)
            {
                SetMarker(c, s, 803031, Count(s, 500363, c.actor) >= 3);
            }
            // THE DEMON WITHIN (class_resources.lua [14].at_cap, PLAN 22.4c).
            // 800222 is known but never cast; at 6 Felfury the orb is
            // consumed and 804216 Inner Demon applied. AddAura semantics, not
            // a cast: 804216 carries CasterAuraSpell=800058, so casting it
            // after consuming the orb would die on CheckCast (the Sun
            // Cleric Dawn ordering trap). Idempotent: below cap, requires
            // unknown, or buff already up is a no-op; while the buff is up a
            // further capping is left alone.
            //
            // Deferred one event: the transform fires only if the cap was
            // already reached BEFORE this event, so the capping grant
            // publishes the visible 6 first (the gate journals the wire max)
            // and the consume lands on the next event (the Refresh every
            // cast closes with, or the next cast). A same-tick consume would
            // erase the transient 6 before any wire state carried it.
            if (felswornCapped && c.playerClass == 14 && Count(s, 800058, c.actor) >= 6 &&
                Known(c, 800222) && !Count(s, 804216, c.actor))
            {
                if (!Native(c, 804216).exists) { throw Status::Failed; }
                Delta(c, s, 800058, 800058, c.actor, -Count(s, 800058, c.actor), true);
                auto& demon = Cell(s, 804216, c.actor);
                demon.stacks = 1;
                // [C] stand-in, 22.10: 804216 DurationIndex is 0
                // (permanent); Ascension resolves $804216d server-side. The
                // stock Metamorphosis precedent (warlock 47241,
                // DurationIndex 9 = 30,000 ms) stands in until the live
                // capture measures Ascension's number.
                demon.expiresMs = c.nowMs + InnerDemonDurationMs;
            }
            if (c.playerClass == 25 && Count(s, 500706, c.actor) >= 100 &&
                !Known(c, 805120) && !Count(s, 803061, c.actor))
            {
                auto& madness = Cell(s, 803061, c.actor);
                madness.stacks = 1;
                madness.expiresMs = Expiry(c, 803061, true);
                s.decayRemainderMs = 0;
            }
        }
        bool AuraEqual(Aura const& a, Aura const& b)
        {
            return a.spell == b.spell && a.target == b.target && a.caster == b.caster &&
                a.stacks == b.stacks && a.charges == b.charges && a.capBonus == b.capBonus &&
                a.expiresMs == b.expiresMs;
        }
        bool FirstCast(CastHistory& history, uint64_t cast)
        {
            if (cast > history.newest)
            {
                uint64_t distance = cast - history.newest;
                history.seen = distance >= 64 ? 1 : (history.seen << distance) | 1;
                history.newest = cast;
                return true;
            }
            uint64_t distance = history.newest - cast;
            if (distance >= 64 || (history.seen & (uint64_t(1) << distance))) { return false; }
            history.seen |= uint64_t(1) << distance;
            return true;
        }
        bool ValidState(State const& s)
        {
            if (s.auras.size > MaxAuras || s.pending.size > MaxPending ||
                s.decayRemainderMs >= 10000 || s.intellect < 0 || !s.nextOrder) { return false; }
            for (std::size_t i = 0; i < s.auras.size; ++i)
            {
                auto const& a = s.auras.values[i];
                auto* r = FindResource(a.spell);
                bool marker = a.spell == 802938 || a.spell == 802939 || a.spell == 804586 ||
                    a.spell == 704396 || a.spell == 803031 || a.spell == 803061 || a.spell == 807440 ||
                    a.spell == 804216;
                if ((!r && !marker) || a.caster != s.actor || !a.target.guid ||
                    !a.target.generation || a.target.instance != s.actor.instance ||
                    a.stacks < 0 || a.charges < 0 || a.charges > 10 ||
                    (a.charges && a.spell != 807440) ||
                    (a.capBonus && (a.capBonus != 1 || (a.spell != 804670 && a.spell != 804711))) ||
                    a.stacks > (r ? int32_t(r->cap) + a.capBonus : 1) ||
                    (!IsTargetAuraResource(a.spell) && a.target != s.actor) ||
                    (a.spell == 807440 && bool(a.stacks) != bool(a.charges))) { return false; }
                for (std::size_t j = 0; j < i; ++j)
                {
                    auto const& b = s.auras.values[j];
                    if (a.spell == b.spell && a.target == b.target) { return false; }
                }
            }
            for (std::size_t i = 0; i < s.pending.size; ++i)
            {
                auto const& cast = s.pending.values[i];
                auto* b = FindBinding(cast.parent, cast.slot, 183);
                if (!b || (b->operation != Operation::Delay && b->operation != Operation::DelayResource &&
                    b->operation != Operation::DelayAtDestination) ||
                    cast.spell != b->child || cast.source != s.actor || cast.originalCaster != s.actor ||
                    !cast.sequence || cast.sequence >= s.nextOrder || !cast.ruleDepth ||
                    cast.ruleDepth > MaxRuleDepth || cast.ancestors[cast.ruleDepth - 1] != cast.parent ||
                    cast.dueMs <= s.clockMs || cast.dueMs - s.clockMs > MaxDelayMs ||
                    (b->operation == Operation::DelayResource &&
                        (cast.kind != IntentKind::ResourceGain || cast.resource != b->resource || cast.amount != b->amount)) ||
                    (b->operation == Operation::Delay && cast.kind != IntentKind::Cast)) { return false; }
                for (std::size_t j = 0; j < i; ++j)
                {
                    if (cast.sequence == s.pending.values[j].sequence) { return false; }
                }
            }
            return true;
        }
        bool CastEqual(CastIntent const& a, CastIntent const& b)
        {
            if (a.location.index() != b.location.index()) { return false; }
            if (auto* first = std::get_if<Destination>(&a.location))
            {
                auto const& second = std::get<Destination>(b.location);
                if (first->owner != second.owner || first->phase != second.phase || first->mode != second.mode ||
                    first->x != second.x || first->y != second.y || first->z != second.z ||
                    first->recipients.size != second.recipients.size) { return false; }
                for (size_t i = 0; i < first->recipients.size; ++i)
                {
                    if (first->recipients.values[i] != second.recipients.values[i]) { return false; }
                }
            }
            return a.kind == b.kind && a.parent == b.parent && a.spell == b.spell && a.slot == b.slot &&
                a.source == b.source && a.originalCaster == b.originalCaster && a.target == b.target &&
                a.targetPolicy == b.targetPolicy && a.dueMs == b.dueMs && a.sequence == b.sequence &&
                a.durationMs == b.durationMs && a.resource == b.resource && a.amount == b.amount &&
                a.ruleDepth == b.ruleDepth && a.ancestors == b.ancestors &&
                a.payload.basePoints == b.payload.basePoints && a.payload.familyMask == b.payload.familyMask &&
                a.payload.effectMask == b.payload.effectMask && a.payload.procMask == b.payload.procMask &&
                a.payload.nativeFlags == b.payload.nativeFlags;
        }
    }

    bool operator==(Identity const& a, Identity const& b)
    {
        return a.guid == b.guid && a.instance == b.instance && a.generation == b.generation &&
            a.map == b.map && a.frame == b.frame && a.deck == b.deck;
    }
    bool operator!=(Identity const& a, Identity const& b) { return !(a == b); }

    bool IsTargetAuraResource(uint32_t spell)
    {
        return spell == 557325 || spell == 570131 || spell == 680854 ||
            spell == 804068 || spell == 804735 || spell == 804378 ||
            spell == 806269 || spell == 806554 || spell == 804670 || spell == 804711;
    }
    Generator const* FindGenerator(uint32_t spell)
    {
        // class_resources.lua CLASSES generators, same (spell, class,
        // resource, amount, gate). Felsworn 801901 is deliberately absent:
        // its third trigger effect fires 800058 itself and the core applies
        // that trigger natively, so a rule here would double every cast.
        // 804097/804098 are the Vow periodic covers (+2/+1 Solar Power).
        // Flash is 500144 plus ranks 502352-502358 (+1 each).
        static constexpr Generator Generators[] = {
            {804097, 27, 500149, 2, GeneratorGate::None, 0},
            {500144, 27, 500149, 1, GeneratorGate::None, 0},
            {502352, 27, 500149, 1, GeneratorGate::None, 0},
            {502353, 27, 500149, 1, GeneratorGate::None, 0},
            {502354, 27, 500149, 1, GeneratorGate::None, 0},
            {502355, 27, 500149, 1, GeneratorGate::None, 0},
            {502356, 27, 500149, 1, GeneratorGate::None, 0},
            {502357, 27, 500149, 1, GeneratorGate::None, 0},
            {502358, 27, 500149, 1, GeneratorGate::None, 0},
            {800232, 27, 500149, 2, GeneratorGate::None, 0},
            {800611, 27, 500149, 1, GeneratorGate::SelfAura, 680313},
            {804020, 16, 803102, 20, GeneratorGate::Known, 500040},
            {500125, 20, 706613, 1, GeneratorGate::KnownOrSelfAura, 500107},
            {800790, 24, 807389, 20, GeneratorGate::None, 0},
            {801016, 17, 500906, 2, GeneratorGate::None, 0},
            {500074, 21, 804329, 1, GeneratorGate::None, 0},
            {500720, 25, 500706, 20, GeneratorGate::None, 0},
            {500357, 30, 805077, 1, GeneratorGate::None, 0},
        };
        for (auto const& g : Generators)
        {
            if (g.spell == spell) { return &g; }
        }
        return nullptr;
    }
    Binding const* FindBinding(uint32_t spell, uint32_t slot, uint32_t effect)
    {
        for (auto const& b : Bindings)
        {
            if (b.spell == spell && b.slot == slot && b.effect == effect) { return &b; }
        }
        return nullptr;
    }
    Resource const* FindResource(uint32_t spell)
    {
        for (auto const& r : Resources)
        {
            if (r.spell == spell) { return &r; }
        }
        return nullptr;
    }
    bool HasBindings(uint32_t spell)
    {
        for (auto const& b : Bindings)
        {
            if (b.spell == spell) { return true; }
        }
        return false;
    }
    bool ControlledAura(uint32_t spell)
    {
        return FindResource(spell) || spell == 802938 || spell == 802939 || spell == 804586 ||
            spell == 704396 || spell == 807440 || spell == 803061 || spell == 803031 || spell == 804216;
    }
    bool PolicyResourceEdge(uint32_t source, uint32_t resource)
    {
        if (source == resource && FindResource(resource)) { return true; }
        if (auto* g = FindGenerator(source)) { return g->resource == resource; }
        if (source == 804584 && resource == 500149) { return true; }
        if (source == 300755 && (resource == 807389 || resource == 807533)) { return true; }
        if (source == 807389 && resource == 807533) { return true; }
        if (source == 805077 && resource == 500363) { return true; }
        for (auto const& b : Bindings)
        {
            if (b.spell != source) { continue; }
            if (b.resource == resource || (b.resource == 807389 && resource == 807533) ||
                (b.resource == 805077 && resource == 500363)) { return true; }
        }
        return false;
    }
    int32_t Count(State const& s, uint32_t spell, Identity const& target)
    {
        if (s.auras.size > MaxAuras) { return 0; }
        for (std::size_t i = 0; i < s.auras.size; ++i)
        {
            auto const& a = s.auras.values[i];
            if (a.spell == spell && a.target == target) { return a.stacks; }
        }
        return 0;
    }

    Plan Evaluate(Context const& c, State const& previous, Event const& e)
    {
        Plan p;
        p.m_revision = previous.revision;
        p.m_playerClass = c.playerClass;
        p.m_nowMs = c.nowMs;
        p.m_event = e;
        try
        {
            if (!ValidState(previous) || !c.actor.guid || !c.actor.generation ||
                c.playerClass < 12 || c.playerClass > 32 || c.nowMs < previous.clockMs ||
                c.nowMs > std::numeric_limits<uint64_t>::max() - MaxAuraDurationMs ||
                previous.revision == std::numeric_limits<uint64_t>::max() ||
                e.effects.size > 3 || !e.sequence || e.sequence <= previous.lastEvent)
            {
                throw Status::Invalid;
            }
            auto& s = p.m_next;
            s = previous;
            bool reset = e.kind == EventKind::Invalidate || !c.alive || !c.inWorld ||
                (previous.actor.guid && previous.actor != c.actor);
            bool maintenance = e.kind == EventKind::Tick || e.kind == EventKind::Refresh ||
                e.kind == EventKind::Invalidate;
            if (reset && !maintenance) { throw Status::Stale; }
            if (reset)
            {
                s = State{};
                s.nextOrder = previous.nextOrder;
            }
            s.actor = c.actor;
            s.revision = previous.revision + 1;
            s.lastEvent = e.sequence;
            s.clockMs = c.nowMs;
            s.inCombat = c.inCombat;
            if (!reset)
            {
                if (e.kind == EventKind::RemoveAura &&
                    (e.originalCaster != c.actor || !e.source.guid || !e.source.generation ||
                        e.source.instance != c.actor.instance || e.source.map != c.actor.map ||
                        e.source.frame != c.actor.frame || e.source.deck != c.actor.deck))
                {
                    throw Status::Unauthorized;
                }
                // Retire cancelled jobs before capacity admission, without firing
                // unrelated due work during this sweep. Dispatch remains below.
                std::size_t pending = 0;
                for (std::size_t i = 0; i < s.pending.size; ++i)
                {
                    auto const cast = s.pending.values[i];
                    if (PendingValid(c, cast)) { s.pending.values[pending++] = cast; }
                }
                s.pending.size = pending;
                bool endMadness = false;
                for (std::size_t i = 0; i < s.auras.size; ++i)
                {
                    auto& a = s.auras.values[i];
                    auto* r = FindResource(a.spell);
                    bool lost = r && !ResourceAllowed(c, *r);
                    lost = lost || (a.spell == 807440 && !Known(c, 804584));
                    bool remove = e.kind == EventKind::RemoveAura && e.resource == a.spell &&
                        e.target == a.target && e.originalCaster == a.caster;
                    bool retired = !StillHere(c, a.target);
                    if (lost || remove || retired || (a.expiresMs && c.nowMs >= a.expiresMs))
                    {
                        endMadness = endMadness || (a.spell == 803061 && a.stacks);
                        a.stacks = a.charges = 0;
                        a.expiresMs = 0;
                        if (lost || retired) { a.capBonus = 0; }
                    }
                    if (a.capBonus && !Known(c, 807693))
                    {
                        a.capBonus = 0;
                        a.stacks = std::min(a.stacks, int32_t(r->cap));
                    }
                }
                if (endMadness)
                {
                    if (auto* a = FindAura(s, 500706, c.actor)) { a->stacks = 0; }
                    s.decayRemainderMs = 0;
                }
                std::size_t live = 0;
                for (std::size_t i = 0; i < s.auras.size; ++i)
                {
                    auto const aura = s.auras.values[i];
                    if (aura.stacks || aura.charges || aura.capBonus) { s.auras.values[live++] = aura; }
                }
                s.auras.size = live;
                if (maintenance)
                {
                    if (Count(s, 807389, c.actor) >= 100) { Delta(c, s, 807389, 807389, c.actor, 0, false); }
                    if (Count(s, 805077, c.actor) >= 5) { Delta(c, s, 805077, 805077, c.actor, 0, false); }
                }

                if (!maintenance && e.kind != EventKind::RemoveAura)
                {
                    if (!e.successful) { throw Status::Failed; }
                    if (e.source != c.actor || e.originalCaster != c.actor || !Authorized(c, e.spell))
                    {
                        throw Status::Unauthorized;
                    }
                    if (e.ruleDepth >= MaxRuleDepth ||
                        std::find(e.ancestors.begin(), e.ancestors.begin() + e.ruleDepth, e.spell) !=
                            e.ancestors.begin() + e.ruleDepth) { throw Status::Recursive; }
                }

                // Decay belongs to the elapsed OLD interval, never to a just-earned gain.
                if (c.playerClass == 25)
                {
                    if (previous.inCombat || endMadness || Count(s, 803061, c.actor))
                    {
                        s.decayRemainderMs = 0;
                    }
                    else
                    {
                        uint64_t elapsed = c.nowMs - previous.clockMs;
                        uint64_t ticks = elapsed / 10000;
                        uint64_t fraction = elapsed % 10000 + s.decayRemainderMs;
                        ticks += fraction / 10000;
                        s.decayRemainderMs = uint32_t(fraction % 10000);
                        if (ticks && Count(s, 500706, c.actor))
                        {
                            Delta(c, s, 500728, 500706, c.actor, -int32_t(std::min<uint64_t>(ticks, 50) * 2), false, true);
                        }
                    }
                    if (c.inCombat || previous.inCombat != c.inCombat) { s.decayRemainderMs = 0; }
                }

                if (e.kind == EventKind::Cast && e.spell == 804584)
                {
                    if (c.playerClass != 27 || !Known(c, 804584)) { throw Status::Unauthorized; }
                    if (e.effects.size || e.target != c.actor || Count(s, 807440, c.actor)) { throw Status::Invalid; }
                    if (Count(s, 500149, c.actor) != 20) { throw Status::Insufficient; }
                    // Preflight the real aura with its prerequisite still present; publish together.
                    uint64_t expires = Expiry(c, 807440, true);
                    Delta(c, s, e.spell, 500149, c.actor, -20, true);
                    auto& dawn = Cell(s, 807440, c.actor);
                    dawn.stacks = 1;
                    dawn.charges = 10;
                    dawn.expiresMs = expires;
                    s.dawnReadyMs = c.nowMs;
                    p.m_suppressSlots = 7;
                }
                else if (e.kind == EventKind::Cast && FindGenerator(e.spell))
                {
                    // Tooltip-only generators (class_resources.lua): the cast
                    // carries no bound 175/178/183 slot, so the adapter emits
                    // the Cast with empty effects and every native slot
                    // executes normally (suppression came from collection).
                    // Same numbers the Lua asserted, including the Dawn block
                    // (Delta refuses 500149 gains while 807440 is up) and the
                    // Heat soft cap (whole-hundreds convert to Embers).
                    auto const* g = FindGenerator(e.spell);
                    if (g->playerClass != c.playerClass) { throw Status::Unauthorized; }
                    if (e.effects.size || e.target != c.actor) { throw Status::Invalid; }
                    bool gated = false;
                    switch (g->gate)
                    {
                        case GeneratorGate::None: gated = true; break;
                        case GeneratorGate::Known: gated = Known(c, g->gateSpell); break;
                        case GeneratorGate::SelfAura: gated = SelfAura(c, g->gateSpell); break;
                        case GeneratorGate::KnownOrSelfAura:
                            gated = Known(c, g->gateSpell) || SelfAura(c, g->gateSpell);
                            break;
                    }
                    if (!gated) { throw Status::Unauthorized; }
                    Delta(c, s, e.spell, g->resource, c.actor, g->amount, false);
                }
                else if (e.kind == EventKind::Cast || e.kind == EventKind::CapModifier)
                {
                    if (!e.effects.size) { throw Status::Unsupported; }
                    uint32_t seen = 0;
                    // Slot order is native order, independent of adapter list order.
                    auto effects = e.effects;
                    std::sort(effects.values.begin(), effects.values.begin() + effects.size,
                        [](Effect const& a, Effect const& b) { return a.slot < b.slot; });
                    for (std::size_t i = 0; i < effects.size; ++i)
                    {
                        auto const& f = effects.values[i];
                        auto* b = FindBinding(e.spell, f.slot, f.type);
                        if (!b || b->operation == Operation::Unsupported) { throw Status::Unsupported; }
                        if (f.slot >= 3 || (seen & (1u << f.slot)) || f.child != b->child)
                        {
                            throw Status::Invalid;
                        }
                        seen |= 1u << f.slot;
                        if (b->playerClass != c.playerClass) { throw Status::Unauthorized; }
                        bool destination = b->operation == Operation::DurationAtDestination || b->operation == Operation::DelayAtDestination;
                        auto* location = std::get_if<Destination>(&f.location);
                        if (destination != bool(location) || (location && f.target != Identity{}) || (destination ? !DestinationValid(c, *location) :
                            !TargetValid(c, f.target, f.targetPolicy))) { throw Status::Invalid; }
                        if (location && ((e.spell == 802631 || e.spell == 806415) !=
                            (location->mode == DestinationMode::PersistentArea))) { throw Status::Invalid; }
                        if (!Native(c, b->child).exists) { throw Status::Failed; }
                        if ((b->operation == Operation::Cap) != (e.kind == EventKind::CapModifier))
                        {
                            throw Status::Invalid;
                        }
                        p.m_suppressSlots |= (1u << f.slot) | b->suppressSlots;
                        auto target = f.target;
                        // Electrocute's enemy effect credits the actor, not the enemy's Static.
                        if (b->child == 804084 || IsCurrency(b->resource)) { target = c.actor; }
                        switch (b->operation)
                        {
                            case Operation::Delta:
                                if (e.spell != 500728)
                                {
                                    Delta(c, s, e.spell, b->resource, target, b->amount,
                                        b->amount < 0 && e.spell != 804585);
                                }
                                break;
                            case Operation::ConvertSoul:
                                Delta(c, s, e.spell, 805077, c.actor, -5, true);
                                Delta(c, s, e.spell, 500363, c.actor, 1, false);
                                break;
                            case Operation::Cap:
                            {
                                if (e.apply && !Known(c, 807693)) { throw Status::Unauthorized; }
                                if (!Authorized(c, e.spell, b->resource)) { throw Status::Unauthorized; }
                                auto& a = Cell(s, b->resource, target);
                                a.capBonus = e.apply ? 1 : 0;
                                a.stacks = std::min(a.stacks, 5 + a.capBonus);
                                break;
                            }
                            case Operation::SolarMarkers:
                                if (!Known(c, 804584)) { throw Status::Unauthorized; }
                                break;
                            case Operation::Fragments:
                                Delta(c, s, e.spell, 805077, c.actor, 0, false);
                                break;
                            default:
                            {
                                if (f.calculatedValue <= 0 || f.calculatedValue > int32_t(MaxDelayMs))
                                {
                                    throw Status::Invalid;
                                }
                                int32_t time = b->amount && b->operation == Operation::Duration ?
                                    b->amount : f.calculatedValue;
                                if (s.nextOrder == std::numeric_limits<uint64_t>::max()) { throw Status::Limit; }
                                CastIntent cast;
                                cast.parent = e.spell;
                                cast.spell = b->child;
                                cast.slot = f.slot;
                                cast.source = e.source;
                                cast.originalCaster = e.originalCaster;
                                cast.target = f.target;
                                cast.targetPolicy = f.targetPolicy;
                                cast.sequence = s.nextOrder++;
                                cast.payload = f.payload;
                                cast.location = f.location;
                                cast.ruleDepth = e.ruleDepth + 1;
                                cast.ancestors = e.ancestors;
                                cast.ancestors[e.ruleDepth] = e.spell;
                                if (std::find(cast.ancestors.begin(), cast.ancestors.begin() + cast.ruleDepth, cast.spell) !=
                                    cast.ancestors.begin() + cast.ruleDepth) { throw Status::Recursive; }
                                cast.dueMs = c.nowMs;
                                if (b->operation == Operation::Duration || b->operation == Operation::SummonDuration ||
                                    b->operation == Operation::DurationAtDestination)
                                {
                                    cast.kind = b->operation == Operation::SummonDuration ?
                                        IntentKind::SummonDuration : IntentKind::DurationCast;
                                    cast.durationMs = uint32_t(time);
                                    if (e.spell == 560233 &&
                                        (!cast.payload.basePoints[0] || !cast.payload.basePoints[1] || !cast.payload.basePoints[2]))
                                    {
                                        throw Status::Invalid;
                                    }
                                    if (!p.m_casts.Push(cast)) { throw Status::Limit; }
                                }
                                else
                                {
                                    cast.dueMs += uint32_t(time);
                                    if (b->operation == Operation::DelayResource)
                                    {
                                        auto* resource = FindResource(b->resource);
                                        if (!resource || !ResourceAllowed(c, *resource) ||
                                            !Authorized(c, e.spell, b->resource)) { throw Status::Unauthorized; }
                                        if (cast.target != c.actor) { throw Status::Invalid; }
                                        cast.kind = IntentKind::ResourceGain;
                                        cast.resource = b->resource;
                                        cast.amount = b->amount;
                                    }
                                    if (!s.pending.Push(cast)) { throw Status::Limit; }
                                }
                                break;
                            }
                        }
                    }
                }
                else if (e.kind == EventKind::ResourceGain || e.kind == EventKind::ResourceSpend)
                {
                    if (e.amount <= 0 || !TargetValid(c, e.target, e.targetPolicy)) { throw Status::Invalid; }
                    Delta(c, s, e.spell, e.resource, e.target,
                        e.kind == EventKind::ResourceSpend ? -e.amount : e.amount,
                        e.kind == EventKind::ResourceSpend);
                }
                else if (e.kind == EventKind::SetResource)
                {
                    if (e.amount < 0 || e.amount > 10000 || !TargetValid(c, e.target, e.targetPolicy)) { throw Status::Invalid; }
                    Delta(c, s, e.spell, e.resource, e.target, e.amount - Count(s, e.resource, e.target), false, true);
                }
                else if (e.kind == EventKind::DirectSpell)
                {
                    if (!e.castId) { throw Status::Invalid; }
                    bool recordedTarget = e.resolvedHit && e.target.guid && e.target.generation &&
                        e.target.instance == c.actor.instance && e.target.map == c.actor.map &&
                        e.target.frame == c.actor.frame && e.target.deck == c.actor.deck;
                    bool qualifies = e.direct && !e.triggered && !e.ruleDepth && Known(c, e.spell) &&
                        e.effectiveAmount && (e.damage != e.healing) && (!e.damage || e.target != c.actor) &&
                        (e.damage ? e.targetPolicy == Target::Enemy :
                            (e.targetPolicy == Target::Self || e.targetPolicy == Target::Friendly)) &&
                        (recordedTarget || TargetValid(c, e.target, e.targetPolicy));
                    if (qualifies && e.damage && e.target != c.actor && e.critical &&
                        c.playerClass == 24 && Known(c, 300755) && FirstCast(s.heatCasts, e.castId))
                    {
                        Delta(c, s, 300755, 807389, c.actor, 20, false);
                    }
                    auto* dawn = FindAura(s, 807440, c.actor);
                    if (qualifies && e.target != Identity{} && c.playerClass == 27 && dawn && dawn->charges &&
                        c.nowMs >= s.dawnReadyMs && FirstCast(s.dawnCasts, e.castId))
                    {
                        if (c.weaponMask > 3) { throw Status::Invalid; }
                        // Native807749 declares melee ABILITY damage -> one auto
                        // attack with each equipped weapon. 0x10 is the native
                        // successful melee-spell event, not an arbitrary proc mask.
                        if (c.activeVow == 807749 && Known(c, 807749) && Authorized(c, 807749) &&
                            e.damage && (e.nativeEventMask & 0x10) &&
                            TargetValid(c, e.target, Target::Enemy))
                        {
                            for (uint32_t hand = 0; hand < 2; ++hand)
                            {
                                if (!(c.weaponMask & (1u << hand))) { continue; }
                                uint32_t child = hand ? 506823 : 506824;
                                if (!Native(c, child).exists) { throw Status::Failed; }
                                if (s.nextOrder == std::numeric_limits<uint64_t>::max()) { throw Status::Limit; }
                                CastIntent cast;
                                cast.kind = IntentKind::Fulfillment;
                                cast.parent = 807749;
                                cast.spell = child;
                                cast.slot = hand;
                                cast.source = c.actor;
                                cast.originalCaster = e.originalCaster;
                                cast.target = e.target;
                                cast.targetPolicy = Target::Enemy;
                                cast.dueMs = c.nowMs;
                                cast.sequence = s.nextOrder++;
                                cast.ruleDepth = 2;
                                cast.ancestors[0] = 807440;
                                cast.ancestors[1] = 807749;
                                cast.payload.basePoints[0] = 100; // Full auto attack, not raw1% weapon helper.
                                cast.payload.procMask = e.nativeEventMask;
                                if (!p.m_casts.Push(cast)) { throw Status::Limit; }
                            }
                        }
                        --dawn->charges;
                        if (!dawn->charges) { dawn->stacks = 0; dawn->expiresMs = 0; }
                        s.dawnReadyMs = c.nowMs + 1000;
                    }
                }
                else if (!maintenance && e.kind != EventKind::RemoveAura) { throw Status::Unsupported; }

                bool intellect = c.playerClass == 24 && Known(c, 300755);
                if (!intellect)
                {
                    s.intellect = 0;
                    s.intellectAtMs = 0;
                }
                else if (e.kind == EventKind::Refresh || c.nowMs >= s.intellectAtMs)
                {
                    if (c.criticalStrikeRating < 0) { throw Status::Invalid; }
                    if (!Native(c, 900755).exists) { throw Status::Failed; }
                    s.intellect = c.criticalStrikeRating;
                    s.intellectAtMs = c.nowMs + 5000;
                }

                std::sort(s.pending.values.begin(), s.pending.values.begin() + s.pending.size,
                    [](CastIntent const& a, CastIntent const& b)
                    {
                        return a.dueMs != b.dueMs ? a.dueMs < b.dueMs : a.sequence < b.sequence;
                    });
                std::size_t keep = 0;
                for (std::size_t i = 0; i < s.pending.size; ++i)
                {
                    auto const cast = s.pending.values[i];
                    if (!PendingValid(c, cast)) { continue; }
                    if (cast.dueMs <= c.nowMs)
                    {
                        if (!std::holds_alternative<Destination>(cast.location) &&
                            !TargetValid(c, cast.target, cast.targetPolicy)) { continue; }
                        if (cast.kind == IntentKind::ResourceGain)
                        {
                            Delta(c, s, cast.parent, cast.resource, cast.target, cast.amount, false);
                        }
                        else if (!p.m_casts.Push(cast)) { throw Status::Limit; }
                    }
                    else { s.pending.values[keep++] = cast; }
                }
                s.pending.size = keep;
                Markers(c, s, Count(previous, 800058, c.actor) >= 6);
            }

            // Net deltas, not intermediate threshold/cost states, cross the commit boundary.
            auto append = [&](Aura const& before, Aura const& after)
            {
                if (!AuraEqual(before, after) && !p.m_deltas.Push({before, after})) { throw Status::Limit; }
            };
            for (std::size_t i = 0; i < previous.auras.size; ++i)
            {
                auto const& before = previous.auras.values[i];
                auto* after = FindAura(s, before.spell, before.target);
                Aura empty = before;
                empty.stacks = empty.charges = empty.capBonus = 0;
                empty.expiresMs = 0;
                append(before, after ? *after : empty);
            }
            for (std::size_t i = 0; i < s.auras.size; ++i)
            {
                auto const& after = s.auras.values[i];
                bool exists = false;
                for (std::size_t j = 0; j < previous.auras.size; ++j)
                {
                    auto const& before = previous.auras.values[j];
                    exists = exists || (before.spell == after.spell && before.target == after.target);
                }
                if (!exists)
                {
                    Aura empty = after;
                    empty.stacks = empty.charges = empty.capBonus = 0;
                    empty.expiresMs = 0;
                    append(empty, after);
                }
            }
            std::size_t keep = 0;
            for (std::size_t i = 0; i < s.auras.size; ++i)
            {
                auto const a = s.auras.values[i];
                if (a.stacks || a.charges || a.capBonus) { s.auras.values[keep++] = a; }
            }
            s.auras.size = keep;
            p.m_intellect = {900755, previous.intellect, s.intellect};
            p.m_status = Status::Ready;
        }
        catch (Status error)
        {
            p.m_status = error;
            p.m_next = previous;
            p.m_deltas = {};
            p.m_casts = {};
            p.m_suppressSlots = 0;
            p.m_intellect = {900755, previous.intellect, previous.intellect};
        }
        return p;
    }

    Status Commit(Context const& c, Plan const& p, bool effectsReady, State& s)
    {
        if (p.m_status != Status::Ready) { return p.m_status; }
        if (!effectsReady) { return Status::Failed; }
        if (s.revision != p.m_revision || c.nowMs != p.m_nowMs || c.playerClass != p.m_playerClass ||
            c.actor != p.m_next.actor) { return Status::Stale; }
        // Callbacks are value-only observers. A lost gate/target must invalidate preflight.
        auto fresh = Evaluate(c, s, p.m_event);
        if (fresh.Result() != Status::Ready) { return fresh.Result(); }
        if (fresh.m_deltas.size != p.m_deltas.size || fresh.m_casts.size != p.m_casts.size ||
            fresh.m_next.pending.size != p.m_next.pending.size ||
            fresh.m_next.auras.size != p.m_next.auras.size ||
            fresh.m_next.nextOrder != p.m_next.nextOrder ||
            fresh.m_next.heatCasts.newest != p.m_next.heatCasts.newest ||
            fresh.m_next.heatCasts.seen != p.m_next.heatCasts.seen ||
            fresh.m_next.dawnCasts.newest != p.m_next.dawnCasts.newest ||
            fresh.m_next.dawnCasts.seen != p.m_next.dawnCasts.seen ||
            fresh.m_next.dawnReadyMs != p.m_next.dawnReadyMs ||
            fresh.m_next.intellectAtMs != p.m_next.intellectAtMs ||
            fresh.m_next.decayRemainderMs != p.m_next.decayRemainderMs ||
            fresh.m_next.inCombat != p.m_next.inCombat ||
            fresh.m_intellect.replacement != p.m_intellect.replacement) { return Status::Stale; }
        for (std::size_t i = 0; i < p.m_deltas.size; ++i)
        {
            if (!AuraEqual(fresh.m_deltas.values[i].after, p.m_deltas.values[i].after)) { return Status::Stale; }
        }
        for (std::size_t i = 0; i < p.m_next.auras.size; ++i)
        {
            if (!AuraEqual(fresh.m_next.auras.values[i], p.m_next.auras.values[i])) { return Status::Stale; }
        }
        for (std::size_t i = 0; i < p.m_casts.size; ++i)
        {
            if (!CastEqual(fresh.m_casts.values[i], p.m_casts.values[i])) { return Status::Stale; }
        }
        for (std::size_t i = 0; i < p.m_next.pending.size; ++i)
        {
            if (!CastEqual(fresh.m_next.pending.values[i], p.m_next.pending.values[i])) { return Status::Stale; }
        }
        s = fresh.m_next;
        return Status::Ready;
    }
}

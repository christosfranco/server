// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaState.h"
#include "CoaProjection.h"
#include "SessionMailbox.h"
#include "SessionProtocolPolicy.h"
#include "DataIntegrity/Sha256.h"
#include "DBCFileLoader.h"
#include <cstdlib>
#include <functional>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>

namespace
{
    std::string Hash(std::string const& text)
    {
        unsigned char bytes[32];
        size_t count = 32;
        if (EVP_Q_digest(nullptr, "SHA256", nullptr, text.data(), text.size(), bytes, &count) != 1)
        {
            throw std::runtime_error("test digest failed");
        }
        return testing::BytesToHex(bytes, count);
    }
    bool Rejected(std::function<void()> const& action)
    {
        try { action(); }
        catch (std::invalid_argument const&) { return true; }
        return false;
    }

    // A refusal is more than a throw: the message the server writes is what
    // the wire and the DLL both key on. `RejectedWith` asserts the throw is
    // there AND its .what() is the exact string the specification names.
    // Introduced for PLAN character-foundations 24.7 (owner correction,
    // ascender commit 8031c64): the retired `CoA essence budget exceeded`
    // string must not appear where the specification names
    // `CoA point budget exceeded`, and both facts have to be observed to
    // prove the refusal is the new authoritative one.
    bool RejectedWith(std::function<void()> const& action, std::string const& expected)
    {
        try { action(); }
        catch (std::invalid_argument const& e) { return expected == e.what(); }
        return false;
    }
    std::string RejectedMessage(std::function<void()> const& action)
    {
        try { action(); }
        catch (std::invalid_argument const& e) { return e.what(); }
        return "<no throw>";
    }
    std::string Fixture(std::string const& extra = "")
    {
        std::ostringstream s;
        s << "ASCENDER_COA_AUTHORIZATION\t1\t60\t10\nCLASS\t12\t14\nCLASS\t13\t15\n";
        for (unsigned c : {12, 13})
        {
            for (unsigned l = 1; l <= 60; ++l)
            {
                s << "BUDGET\t" << c << '\t' << l << '\t' << (l < 10 ? 0 : (l - 10) / 2 + 1)
                    << '\t' << (l < 10 ? 0 : (l - 9) / 2) << '\n';
            }
            s << "BASESPELL\t" << c << '\t' << 900 + c << "\t1\t1\n";
        }
        s << "BASESPELL\t12\t999\t2\t60\nSPEC\t1\t12\t100\t101\nSPEC\t2\t12\t102\t101\n"
             "SPEC\t3\t13\t103\t104\n";
        auto entry = [&](unsigned id, unsigned cls, unsigned ae, unsigned te, unsigned rank,
            unsigned level = 10, unsigned flags = 0, unsigned tabAe = 0)
        {
            s << "ENTRY\t" << id << '\t' << cls << "\t1\t" << flags << '\t' << level << '\t'
                << ae << '\t' << te << '\t' << rank << "\t0\t0\t0\t0\t0\t0\t" << tabAe << "\t0\n";
            for (unsigned r = 1; r <= rank; ++r)
            {
                s << "SPELL\t" << id << '\t' << r << '\t' << 10000 + id * 10 + r << "\t1\t" << level << '\n';
            }
        };
        entry(100, 12, 0, 0, 1); entry(101, 12, 1, 0, 1);
        entry(102, 12, 0, 0, 1); entry(103, 13, 0, 0, 1); entry(104, 13, 1, 0, 1);
        entry(105, 12, 0, 0, 1); // Automatic trait unlocks paid descendant.
        entry(106, 12, 1, 0, 2); entry(107, 12, 1, 0, 1); // Foreign branch.
        entry(108, 12, 1, 0, 1); entry(109, 12, 1, 0, 1); // Disconnected cycle.
        entry(110, 12, 1, 0, 1, 10, 0, 3); // Requires prior investment, not final total.
        entry(111, 12, 0, 1, 1); entry(112, 12, 0, 0, 1, 60);
        entry(113, 12, 0, 0, 1, 1); // Class-shared free node is NOT a starting kit.
        s << "OWNER\t100\t1\nOWNER\t101\t1\nOWNER\t101\t2\nOWNER\t102\t2\n"
             "OWNER\t103\t3\nOWNER\t104\t3\nOWNER\t105\t1\nOWNER\t106\t1\nOWNER\t107\t2\n"
             "OWNER\t112\t1\nCONNECT\t105\t101\nCONNECT\t106\t105\n"
             "CONNECT\t108\t109\nCONNECT\t109\t108\nREQUIRE\t111\t106\t2\n"
             "SPELL\t106\t2\t88888\t2\t60\n" << extra;
        return s.str();
    }
    std::shared_ptr<coa::Catalog const> Catalog(std::string const& extra = "")
    {
        auto text = Fixture(extra);
        return coa::Catalog::Parse(text, Hash(text));
    }
    struct FakeStore : coa::Store
    {
        bool failRead = false, failCommit = false;
        unsigned commits = 0;
        coa::State durable;
        std::function<void()> beforeCommit;
        coa::LoadStatus Load(uint32_t guid, coa::State& state) override
        {
            if (failRead) { return coa::LoadStatus::Failed; }
            if (!durable.revision || durable.guid != guid) { return coa::LoadStatus::Missing; }
            state = durable;
            return coa::LoadStatus::Found;
        }
        bool Commit(coa::State const& previous, coa::State const& next) override
        {
            ++commits;
            if (beforeCommit) { beforeCommit(); }
            if (failCommit || previous.revision != durable.revision) { return false; }
            durable = next;
            return true;
        }
    };
}

TEST(Coa_catalog_pin_schema_arity_duplicates_and_bounds)
{
    auto text = Fixture();
    CHECK(Rejected([&] { coa::Catalog::Parse(text, ""); }));
    CHECK(Rejected([&] { coa::Catalog::Parse(text, std::string(64, '0')); }));
    for (auto suffix : {"UNKNOWN\t1\n", "OWNER\t100\t1\n", "CLASS\t12\t14\n", "START\t12\t999999\n",
        "OWNER\t101\t3\n", "GROUP\t113\t1\t2\n", "GROUP\t113\t4294967296\n", "GROUP\t113\t01\n",
        "CONNECT\t113\t113\n", "REQUIRE\t113\t106\t3\n", "\n"})
    {
        auto bad = text + suffix;
        CHECK(Rejected([&] { coa::Catalog::Parse(bad, Hash(bad)); }));
    }
    auto oversized = std::string(16 * 1024 * 1024 + 1, '\n');
    CHECK(Rejected([&] { coa::Catalog::Parse(oversized, Hash(oversized)); }));
}

TEST(Coa_levels_1_9_10_60_voluntary_spec_and_base_rank_upgrade)
{
    auto c = Catalog();
    for (unsigned level : {1, 9, 10, 60})
    {
        auto b = c->Authorize(12, level, 0, {});
        CHECK(b.entries.empty());
        CHECK(b.spells == std::set<uint32_t>{level == 60 ? 999u : 912u});
        CHECK(Rejected([&] { c->Authorize(12, level, 0, {{113, 1}}); }));
    }
    CHECK(Rejected([&] { c->Authorize(12, 9, 1, {}); }));
    auto b = c->Authorize(12, 10, 1, {});
    CHECK((b.entries == coa::Ranks{{100, 1}, {101, 1}, {105, 1}}));
    CHECK(!b.entries.count(112));
    CHECK(c->Authorize(12, 60, 1, {}).entries.count(112));
}

TEST(Coa_wrong_class_branch_multiple_markers_rank_budget_and_cycles)
{
    // [C] 2026-09-17 (plans/character-foundations.md 24.5): the last
    // assertion in this test used to require Rejected for
    // c->Authorize(12, 10, 1, {{106, 1}}).
    //   Old:  at level 10 the spec-1 package (marker 100 + root 101, ae 1)
    //         plus the requested 106 (ae 1) totals ae 2 vs budget ae 1;
    //         Authorize refused "CoA essence budget exceeded" and the
    //         Rejected() lambda observed the throw.
    //   New:  essence does not participate in authorization (CoaCatalog.cpp
    //         Authorize no longer applies the whole-set budget refusal);
    //         the same build is otherwise valid -- ownership OK (106 owned
    //         by spec 1), rank 1 <= maxRank 2, prerequisites empty,
    //         traversal reaches 106 through the auto-added free trait 105
    //         (spec-1 owned, 0/0, connections={101}) which itself reaches
    //         the auto-added root 101. So Authorize now returns a Build
    //         with {100, 101, 105, 106} at rank 1 and does not throw.
    //   Why:  deliberate specification change requested by the owner and
    //         landed in the same series of commits as this rewrite; the
    //         old-behaviour assertion encoded a refusal that no longer
    //         fires. The four preceding legs of this TEST (wrong class,
    //         branch/marker/rank/cycle refusals) still fire on their own
    //         reasons and are unchanged.
    //
    // [C] 2026-09-18 (plans/character-foundations.md 24.7; ascender commit
    // 8031c64). Owner correction supersedes the 24.5 rewrite above.
    //   Old (24.5 shape, above): Authorize(12, 10, 1, {{106, 1}}) returned
    //         a Build with {100, 101, 105, 106} at rank 1 -- essence did
    //         not participate in authorization at all.
    //   New (24.7): the catalog's per-class per-level ae/te and each
    //         entry's ae_cost/te_cost are the authoritative class-tree /
    //         spec-tree talent-point budgets, and the first unaffordable
    //         pick is refused server-side over the complete desired build.
    //         At level 10 the spec-1 package (marker 100 + auto-added
    //         root 101, ae 1) exactly fills the class-tree budget (ae 1);
    //         the requested 106 (ae 1) puts the desired build one class
    //         point over budget. Authorize must throw invalid_argument
    //         with the EXACT what() = "CoA point budget exceeded" and
    //         must not throw the retired "CoA essence budget exceeded"
    //         (asserted in Coa_point_budget_authoritative_class_spec
    //         below via RejectedWith's what() equality; the same expected
    //         string is enforced here).
    //   Why:  owner correction 2026-09-18 (PLAN character-foundations
    //         24.7, ascender commit 8031c64). The user report is a
    //         level-10 Barbarian with both counters at zero yet further
    //         picks accepted server-side. The generic evaluator's
    //         `points` / `required_class_points` fields are zero for
    //         every entry, so the catalog's ae/te values are the only
    //         authoritative talent-point budgets. The four preceding
    //         legs of this TEST (wrong class, branch/marker/rank/cycle
    //         refusals) still fire on their own reasons and remain
    //         unchanged.
    auto c = Catalog();
    CHECK(Rejected([&] { c->Authorize(13, 60, 1, {}); }));
    for (coa::Ranks r : {coa::Ranks{{104, 1}}, {{107, 1}}, {{102, 1}}, {{106, 0}}, {{106, 3}}, {{108, 1}, {109, 1}}})
    {
        CHECK(Rejected([&] { c->Authorize(12, 60, 1, r); }));
    }
    CHECK(Rejected([&] { c->SelectedSpec({{100, 1}, {102, 1}}); }));
    // 24.7: the whole-set point-budget refusal is authoritative. Level
    // 10 fits ae 1 / te 0 exactly for marker+root; adding 106 (ae 1)
    // puts the build one class-tree point over.
    CHECK(RejectedWith([&] { c->Authorize(12, 10, 1, {{106, 1}}); },
        "CoA point budget exceeded"));
    // And the retired essence string must not be what the server threw.
    CHECK(RejectedMessage([&] { c->Authorize(12, 10, 1, {{106, 1}}); })
        != "CoA essence budget exceeded");
}

TEST(Coa_free_trait_closure_prerequisite_investment_and_shared_root)
{
    auto c = Catalog();
    CHECK(c->Authorize(12, 12, 1, {{106, 1}}).entries.count(105));
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{111, 1}, {106, 1}}); }));
    CHECK(c->Authorize(12, 60, 1, {{111, 1}, {106, 2}}).entries.count(111));
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{110, 1}, {106, 1}}); }));
    auto b = c->Authorize(12, 60, 1, {{110, 1}, {106, 2}});
    CHECK(b.entries.count(110));
    CHECK(b.spells.count(88888));
    CHECK(!c->Authorize(12, 59, 1, {{106, 2}}).spells.count(88888));
    CHECK(c->Authorize(12, 10, 2, {}).entries.count(101));
    CHECK(!c->Authorize(12, 10, 2, {}).entries.count(105));
}

TEST(Coa_shared_spec_roots_load_and_authorize_across_specs)
{
    // 23.13 (owner decision 2026-09-15): Class-tab spec roots are
    // class-shared, so a root with empty owners is a valid spec package;
    // a root owned only by another spec still is not.
    auto text = Fixture(
        "ENTRY\t120\t12\t1\t0\t10\t1\t0\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
        "SPELL\t120\t1\t11200\t1\t10\n");
    auto repoint = std::string("SPEC\t2\t12\t102\t101\n");
    auto at = text.find(repoint);
    REQUIRE(at != std::string::npos);
    text.replace(at, repoint.size(), "SPEC\t2\t12\t102\t120\n");
    for (auto row : {std::string("OWNER\t101\t1\n"), std::string("OWNER\t101\t2\n")})
    {
        at = text.find(row);
        REQUIRE(at != std::string::npos);
        text.erase(at, row.size());
    }
    auto c = coa::Catalog::Parse(text, Hash(text));
    auto b = c->Authorize(12, 10, 1, {{100, 1}});
    CHECK(b.entries.count(100)); CHECK(b.entries.count(101));
    auto shared = c->Authorize(12, 12, 1, {{100, 1}, {120, 1}});
    CHECK(shared.entries.count(101)); CHECK(shared.entries.count(120));
    CHECK(c->Authorize(12, 10, 2, {}).entries.count(120));
    auto foreign = Fixture();
    auto owned = std::string("OWNER\t101\t1\n");
    at = foreign.find(owned);
    REQUIRE(at != std::string::npos);
    foreign.erase(at, owned.size());
    CHECK(Rejected([&] { coa::Catalog::Parse(foreign, Hash(foreign)); }));
}

TEST(Coa_mutual_exclusion_group_and_explicit_start)
{
    auto c = Catalog("EXCLUDE\t106\t110\n");
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{110, 1}, {106, 2}}); }));
    c = Catalog("GROUP\t106\t9\nGROUP\t111\t9\n");
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{111, 1}, {106, 2}}); }));
    CHECK(Rejected([&] { Catalog("START\t12\t113\n"); }));
}

TEST(Coa_investment_any_flag_and_candidate_points_excluded)
{
    auto text = Fixture("ENTRY\t114\t12\t1\t0\t10\t0\t1\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
                        "SPELL\t114\t1\t99914\t1\t10\n");
    auto position = text.find("ENTRY\t110\t12\t1\t0\t10\t1\t0\t1\t0\t0\t0\t0\t0\t0\t3\t0\n");
    REQUIRE(position != std::string::npos);
    auto end = text.find('\n', position);
    text.replace(position, end - position,
        "ENTRY\t110\t12\t1\t0\t10\t1\t0\t1\t0\t0\t0\t0\t0\t0\t3\t1");
    auto c = coa::Catalog::Parse(text, Hash(text));
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{110, 1}, {114, 1}}); }));
    end = text.find('\n', position);
    text.replace(position, end - position,
        "ENTRY\t110\t12\t1\t16384\t10\t1\t0\t1\t0\t0\t0\t0\t0\t0\t3\t1");
    c = coa::Catalog::Parse(text, Hash(text));
    CHECK(c->Authorize(12, 60, 1, {{110, 1}, {114, 1}}).entries.count(110));

    text = Fixture();
    position = text.find("ENTRY\t110\t");
    end = text.find('\n', position);
    text.replace(position, end - position,
        "ENTRY\t110\t12\t1\t0\t10\t1\t0\t1\t2\t2\t0\t0\t0\t0\t0\t0");
    position = text.find("ENTRY\t106\t");
    end = text.find('\n', position);
    text.replace(position, end - position,
        "ENTRY\t106\t12\t1\t0\t10\t1\t0\t2\t1\t0\t0\t0\t0\t0\t0\t0");
    c = coa::Catalog::Parse(text, Hash(text));
    CHECK(Rejected([&] { c->Authorize(12, 60, 1, {{110, 1}, {106, 1}}); }));
    CHECK(c->Authorize(12, 60, 1, {{110, 1}, {106, 2}}).entries.count(110));
}

TEST(Coa_store_commit_failure_cas_and_no_mutation_before_commit)
{
    auto c = Catalog();
    FakeStore store;
    coa::State current; current.guid = 7;
    coa::Build installed;
    std::string error;
    store.beforeCommit = [&] { CHECK_EQ(current.revision, 0u); CHECK(installed.spells.empty()); };
    store.failCommit = true;
    CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}}, 100, installed, error) == coa::ApplyStatus::Failed);
    CHECK_EQ(current.revision, 0u);
    CHECK(installed.spells.empty());
    store.failCommit = false;
    CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}}, 100, installed, error) == coa::ApplyStatus::Applied);
    CHECK_EQ(current.revision, 1u);
    CHECK_EQ(current.entries.size(), 3u);
    store.beforeCommit = {};
    auto stale = current;
    CHECK(coa::Replace(*c, store, current, 12, 10, {}, 200, installed, error) == coa::ApplyStatus::Applied);
    CHECK(installed.entries.empty());
    CHECK(coa::Replace(*c, store, stale, 12, 10, {}, 200, installed, error) == coa::ApplyStatus::Failed);
}

TEST(Coa_store_reload_identity_and_protected_metadata)
{
    auto c = Catalog(); FakeStore store;
    coa::State current; current.guid = 7;
    coa::Build installed; std::string error;
    REQUIRE(coa::Replace(*c, store, current, 12, 10, {{100, 1, 1, false, 999999}}, 100, installed, error) == coa::ApplyStatus::Applied);
    CHECK_EQ(current.entries[0].learnedTime, 100);
    coa::State loaded;
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Found);
    CHECK(coa::ValidateLoaded(*c, loaded, 7, 12, 10, installed));
    CHECK(!coa::ValidateLoaded(*c, loaded, 8, 12, 10, installed));
    CHECK(!coa::ValidateLoaded(*c, loaded, 7, 13, 10, installed));
    loaded.catalogRevision = "wrong";
    CHECK(!coa::ValidateLoaded(*c, loaded, 7, 12, 10, installed));
    store.failRead = true;
    CHECK(store.Load(7, loaded) == coa::LoadStatus::Failed);
    auto desired = current.entries;
    desired[0].learnedTime = 101;
    CHECK(coa::Replace(*c, store, current, 12, 10, desired, 200, installed, error) == coa::ApplyStatus::Rejected);
    desired = current.entries; desired[0].locked = true;
    CHECK(coa::Replace(*c, store, current, 12, 10, desired, 200, installed, error) == coa::ApplyStatus::Rejected);
    current.entries[0].locked = true;
    CHECK(coa::Replace(*c, store, current, 12, 10, {}, 200, installed, error) == coa::ApplyStatus::Rejected);
    CHECK_EQ(store.commits, 1u);
}

TEST(Coa_level_refresh_spec_replacement_reset_and_reload)
{
    auto c = Catalog(); FakeStore store;
    coa::State current; current.guid = 7;
    coa::Build installed; std::string error;
    REQUIRE(coa::Replace(*c, store, current, 12, 10, {{100, 1}}, 100, installed, error) == coa::ApplyStatus::Applied);
    REQUIRE(coa::Replace(*c, store, current, 12, 60, current.entries, 200, installed, error) == coa::ApplyStatus::Applied);
    CHECK(installed.entries.count(112)); CHECK(installed.spells.count(999));
    CHECK(coa::ValidateLoaded(*c, store.durable, 7, 12, 60, installed));
    REQUIRE(coa::Replace(*c, store, current, 12, 60, {{102, 1}}, 300, installed, error) == coa::ApplyStatus::Applied);
    CHECK((installed.entries == coa::Ranks{{101, 1}, {102, 1}}));
    CHECK_EQ(current.entries[0].learnedTime, 100);
    REQUIRE(coa::Replace(*c, store, current, 12, 60, {}, 400, installed, error) == coa::ApplyStatus::Applied);
    CHECK(installed.entries.empty()); CHECK((installed.spells == std::set<uint32_t>{999}));
    coa::State loaded;
    REQUIRE(store.Load(7, loaded) == coa::LoadStatus::Found);
    CHECK_EQ(loaded.revision, 4u);
    CHECK(coa::ValidateLoaded(*c, loaded, 7, 12, 60, installed));
}

TEST(Coa_positive_rank_limits_survive_real_wire_store_and_level_refresh)
{
    auto c = Catalog(); FakeStore store;
    coa::State current; current.guid = 7;
    coa::Build installed; std::string error;
    auto wire = coa::analytic::BuildEntriesReplacementPacket({{100,1,0xffffffff,false,0}, {106,2,0xffffffff,false,0}});
    auto parsed = coa::analytic::ParseEntriesReplacement(wire);
    REQUIRE(parsed.status == coa::analytic::ParseStatus::Ok);
    REQUIRE(coa::Replace(*c, store, current, 12, 20, parsed.value.entries, 100, installed, error) == coa::ApplyStatus::Applied);
    auto packet = coa::analytic::BuildEntriesStatePacket(current.entries);
    REQUIRE(packet.GetOpcode() == coa::analytic::EntriesStateOpcode);
    auto snapshot = coa::analytic::ParseEntriesState(packet);
    REQUIRE(snapshot.status == coa::analytic::ParseStatus::Ok);
    for (auto const& e : snapshot.value.entries)
    {
        CHECK_EQ(e.spellRankLimit, 1u);
    }
    REQUIRE(coa::Replace(*c, store, current, 12, 60, current.entries, 200, installed, error) == coa::ApplyStatus::Applied);
    for (auto const& e : current.entries)
    {
        CHECK_EQ(e.spellRankLimit, e.entryId == 106 ? 2u : 1u);
    }
    CHECK(coa::ValidateLoaded(*c, current, 7, 12, 60, installed));
    current.entries[0].spellRankLimit = 0;
    CHECK(!coa::ValidateLoaded(*c, current, 7, 12, 60, installed));
    CHECK(coa::analytic::BuildEntriesStatePacket(current.entries).GetOpcode() != coa::analytic::EntriesStateOpcode);
}

TEST(Coa_no_diff_requests_do_not_write_and_internal_level_reset_changes_commit)
{
    auto c = Catalog(); FakeStore store;
    coa::State current; current.guid = 7;
    coa::Build installed; std::string error;
    REQUIRE(coa::Replace(*c, store, current, 12, 1, {}, 100, installed, error) == coa::ApplyStatus::Applied);
    for (int request = 0; request < 100; ++request)
    {
        CHECK(coa::Replace(*c, store, current, 12, 1, {}, 200, installed, error) == coa::ApplyStatus::NoChange);
    }
    CHECK_EQ(store.commits, 1u);
    CHECK_EQ(current.revision, 1u);
    CHECK(coa::Replace(*c, store, current, 12, 9, {}, 200, installed, error, coa::MutationSource::LevelChange) == coa::ApplyStatus::Applied);
    CHECK_EQ(store.commits, 2u);
    CHECK(coa::Replace(*c, store, current, 12, 9, {}, 300, installed, error, coa::MutationSource::Reset) == coa::ApplyStatus::Applied);
    CHECK_EQ(store.commits, 3u);
}

TEST(Coa_refusal_names_offending_entry_and_budget_names_none)
{
    // 23.17(c): the 0x72C err_entry/err_rank come out of Replace
    // structurally. A refusal that blames one entry names it; a wire-shape
    // refusal stays 0. Error strings are byte-identical: the client shows
    // them, it never parses them.
    //
    // [C] 2026-09-17 (plans/character-foundations.md 24.5): the third
    // block of this test used to require Rejected with error "CoA essence
    // budget exceeded" and offender {0, 0} for
    // Replace(12, 10, {{100, 1}, {106, 1}}) -- the whole-set-budget
    // refusal whose no-single-offender property was the very reason
    // 23.17(c) named this test "..._budget_names_none".
    //   Old:  at level 10 the spec-1 package (marker 100 + root 101, ae 1)
    //         plus 106 (ae 1) totalled ae 2 vs budget ae 1; Authorize
    //         raised "CoA essence budget exceeded" as a whole-set refusal
    //         with no offender, so refusal.entry/rank stayed 0 and Replace
    //         mapped it to Rejected.
    //   New (24.5): essence does not participate in authorization. The
    //         same build satisfies ownership, ranks, prerequisites,
    //         traversal (through the auto-added free trait 105) and the
    //         ordinary point budget, so Replace applies it and commits.
    //   Why (24.5): deliberate specification change; essence budgets stay
    //         in the catalog only as reference/display data.
    //
    // [C] 2026-09-18 (plans/character-foundations.md 24.7; ascender commit
    // 8031c64). Owner correction supersedes the 24.5 rewrite of this
    // block.
    //   Old (24.5 shape, immediately above): the same
    //         Replace(12, 10, {{100, 1}, {106, 1}}) call returned
    //         ApplyStatus::Applied, the store committed once, error
    //         stayed empty, and current.entries carried
    //         {100, 101, 105, 106}.
    //   New (24.7): the catalog's per-class per-level ae/te and each
    //         entry's ae_cost/te_cost are the authoritative talent-point
    //         budgets. At level 10 marker+root exactly fills ae 1 / te 0;
    //         adding 106 (ae 1) is one class-tree point over budget so
    //         Authorize throws "CoA point budget exceeded", Replace maps
    //         that to ApplyStatus::Rejected, error carries the exact
    //         string, and the store commits zero times. The whole-set-
    //         budget refusal is authoritative again; the retired string
    //         "CoA essence budget exceeded" must not be written anywhere.
    //         The "budget_names_none" property this test's name captures
    //         holds again: a whole-set budget refusal has no
    //         single-entry offender, so refusal.entry / refusal.rank
    //         stay 0 (the throw comes off the aggregate cost check, not
    //         off any single blame() call in the per-entry loop above).
    //   Why:  owner correction 2026-09-18, PLAN character-foundations
    //         24.7, ascender commit 8031c64. Server source audit shows
    //         no other aggregate authority (generic `points` /
    //         `required_class_points` are zero for every entry), so the
    //         catalog's ae/te values are the authoritative class-tree /
    //         spec-tree talent-point budgets.
    //
    // The three preserved blocks (not-owned/107, not-traversable/110,
    // protected-metadata) are unchanged: this rewrite only replaces the
    // budget leg, not the surrounding structural refusals.
    auto c = Catalog();
    coa::Build installed;
    std::string error;

    // Not owned: 107 belongs to spec 2, requested under spec 1's marker.
    {
        FakeStore store;
        coa::State current; current.guid = 7;
        coa::AuthorizationError refusal;
        CHECK(coa::Replace(*c, store, current, 12, 60, {{100, 1}, {107, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA entry not owned"));
        CHECK_EQ(refusal.entry, 107u);
        CHECK_EQ(refusal.rank, 1u);
    }

    // Not traversable: 110 needs 3 prior tab AE; 101 + 106 fund only 2.
    {
        FakeStore store;
        coa::State current; current.guid = 7;
        coa::AuthorizationError refusal;
        CHECK(coa::Replace(*c, store, current, 12, 60, {{100, 1}, {106, 1}, {110, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA build not traversable"));
        CHECK_EQ(refusal.entry, 110u);
        CHECK_EQ(refusal.rank, 1u);
    }

    // 24.7 point-budget refusal (was 24.5 Applied, was 23.17(c)
    // essence-budget Rejected). At level 10 the package + 106 totals
    // ae 2 vs budget ae 1 -- one class-tree point over. Replace maps
    // Authorize's throw to Rejected, error carries "CoA point budget
    // exceeded" (never the retired essence string), the store commits
    // zero times, and current.entries stays empty because nothing was
    // loaded on this fresh guid.
    {
        FakeStore store;
        coa::State current; current.guid = 7;
        coa::AuthorizationError refusal;
        error.clear();
        CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}, {106, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA point budget exceeded"));
        CHECK(error != std::string("CoA essence budget exceeded"));
        CHECK_EQ(store.commits, 0u);
        CHECK(current.entries.empty());
    }

    // Protected metadata: the tampered entry is the offender.
    {
        FakeStore store;
        coa::State current; current.guid = 7;
        coa::AuthorizationError refusal;
        REQUIRE(coa::Replace(*c, store, current, 12, 10, {{100, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Applied);
        auto desired = current.entries;
        REQUIRE(!desired.empty());
        desired[0].learnedTime += 1;
        CHECK(coa::Replace(*c, store, current, 12, 10, desired, 200,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA protected metadata changed"));
        CHECK_EQ(refusal.entry, desired[0].entryId);
        CHECK_EQ(refusal.rank, desired[0].rank);
    }
}

TEST(Coa_profile_class_gate_rate_work_bound_and_mailbox_coalescing)
{
    using P = proto::ConnectionProfile;
    for (uint32_t cls = 12; cls <= 32; ++cls)
    {
        CHECK(!AdmitCharacterClass(P::Stock, cls, true));
        CHECK(!AdmitCharacterClass(P::AscensionClearHeaders, cls, true));
        CHECK(!AdmitCharacterClass(P::AscensionStockAuthCoA, cls, false));
        CHECK(AdmitCharacterClass(P::AscensionStockAuthCoA, cls, true));
    }
    CHECK(AdmitCharacterClass(P::Stock, 8, false));
    CHECK(!AdmitCharacterClass(P::Stock, 0, false));
    CoaRequestGate gate;
    auto now = CoaRequestGate::Clock::time_point{};
    CHECK(gate.Accept(now));
    CHECK(!gate.Accept(now + std::chrono::milliseconds(499)));
    CHECK(gate.Accept(now + std::chrono::milliseconds(500)));
    SessionMailbox mailbox(P::AscensionStockAuthCoA);
    CHECK(mailbox.Enqueue(std::make_unique<WorldPacket>(0x727, 4)));
    CHECK(!mailbox.Enqueue(std::make_unique<WorldPacket>(0x727, 4)));
    WorldPacket* packet = nullptr;
    REQUIRE(mailbox.Next(packet)); delete packet;
    CHECK(mailbox.Enqueue(std::make_unique<WorldPacket>(0x727, 4)));
}

// plans/spell-progression.md 26.2: the trainer-buy CMSG cadence is not the
// analytic 0x727 cadence, and coupling them silently drops half the buys.
// The fix keeps CoaRequestGate at 500ms for 0x727 (the test just above)
// while the trainer-buy handler uses a state-only gate that does not
// consume that slot. This test pins the pre-fix pathology so the regression
// can be observed: a 100ms cadence -- the shape SendTrainerList's response
// tail plus a next CMSG produces on loopback (world log 2026-09-19
// 13:41:37-13:42:31, guid 1971: every "bought" line is 0.1s from the
// previous, every dropped buy is silently ignored) -- puts 10 of 11 buys
// inside the 500ms slot and would refuse them.
TEST(Coa_trainer_buys_cannot_share_the_500ms_analytic_slot)
{
    CoaRequestGate gate;
    auto t = CoaRequestGate::Clock::time_point{};
    unsigned accepted = 0;
    for (unsigned i = 0; i < 11; ++i)
    {
        if (gate.Accept(t + std::chrono::milliseconds(i * 100))) { ++accepted; }
    }
    // On a 100ms cadence the analytic gate accepts exactly one buy per
    // 500ms window -- 3 of 11 in a 1-second run. That is precisely the
    // shape the live world log recorded for every class 12..32 buy loop
    // before this fix: `bought` in the log iff Accept returned true, no
    // response at all otherwise. AcceptCoaTrainerRequest deliberately does
    // NOT call gate.Accept so the trainer-buy handler never sees this.
    CHECK_EQ(accepted, 3u);
}

TEST(Coa_quest_cast_wrappers_and_external_or_timed_auras_are_not_projection)
{
    // Wrapper100 teaches200/201. Only actually known child200 is durable ownership.
    auto grants = coa::AcceptedQuestGrants({200,201}, [](uint32_t id) { return id == 100 || id == 200; });
    CHECK((grants == std::set<uint32_t>{200}));
    CHECK(coa::AcceptedQuestGrants({}, [](uint32_t) { return true; }).empty());
    CHECK(coa::OwnsProjectionAura(true, true, false, true, -1));
    CHECK(!coa::OwnsProjectionAura(true, false, false, true, -1));
    CHECK(!coa::OwnsProjectionAura(true, true, true, true, -1));
    CHECK(!coa::OwnsProjectionAura(true, true, false, false, 30000));
    CHECK(!coa::OwnsProjectionAura(false, true, false, true, -1));
}

TEST(Coa_profile_opcode_bounds_and_mailbox)
{
    using P = proto::ConnectionProfile;
    for (uint32_t op = NUM_MSG_TYPES; op <= 0xFFFF; ++op)
    {
        CHECK(!AdmitSessionOpcode(P::Stock, op, 4));
        CHECK(!AdmitSessionOpcode(P::AscensionClearHeaders, op, 4));
        CHECK(AdmitSessionOpcode(P::AscensionStockAuthCoA, op, 4) == (op == 0x727));
        CHECK(!AdmitSessionSend(P::Stock, op));
    }
    CHECK(!AdmitSessionOpcode(P::AscensionStockAuthCoA, 0x727, 4 + 1024 * 21 + 1));
    SessionMailbox stock, coa(P::AscensionStockAuthCoA);
    CHECK(!stock.Enqueue(std::make_unique<WorldPacket>(0x727, 4)));
    CHECK(coa.Enqueue(std::make_unique<WorldPacket>(0x727, 4)));
}

TEST(Coa_projection_shared_spells_rank_dependency_cycles_and_trigger_isolation)
{
    std::map<uint32_t, coa::SpellLinks> graph = {
        {100, {99, {50}, {10}}}, {200, {0, {50}, {20}}},
        {50, {0, {51}, {}}}, {51, {0, {50}, {}}},
        {300755, {0, {75}, {76}}}, {400, {0, {75}, {}}}};
    auto resolve = [&](uint32_t id) { return graph[id]; };
    auto managed = coa::SpellClosure({100, 200, 300755}, resolve);
    CHECK((managed == std::set<uint32_t>{50, 51, 75, 99, 100, 200, 300755}));
    CHECK(!managed.count(76));
    auto selected = coa::SpellClosure({200, 400}, resolve);
    CHECK(selected.count(50)); CHECK(selected.count(51)); CHECK(selected.count(75));
    CHECK(!selected.count(99)); CHECK(!selected.count(100)); CHECK(!selected.count(300755));
    auto auras = coa::SpellClosure({200, 400}, resolve, true);
    CHECK(auras.count(20)); CHECK(!auras.count(10)); CHECK(!auras.count(76));
    CHECK(coa::SpellClosure({300755}, resolve, true).count(76));
}

TEST(Coa_revocation_keeps_missing_legacy_ids_without_authorizing_missing_grants)
{
    std::map<uint32_t, coa::SpellLinks> graph = {
        {100, {0, {200}, {804255}}}, {200, {0, {}, {}}}};
    auto exists = [&](uint32_t id) { return graph.count(id) != 0; };
    auto resolve = [&](uint32_t id)
    {
        if (!exists(id))
        {
            throw std::invalid_argument("missing spell");
        }
        return graph.at(id);
    };
    CHECK((coa::RevocationClosure({100, 804255}, resolve, exists, true) ==
        std::set<uint32_t>{100, 200, 804255}));
    CHECK((coa::SpellClosure({100}, resolve) == std::set<uint32_t>{100, 200}));
    CHECK(Rejected([&] { coa::SpellClosure({804255}, resolve); }));
    CHECK(Rejected([&] { coa::SpellClosure({100}, resolve, true); }));
}

TEST(Coa_ordinary_class_spell_family_and_level_gate)
{
    for (uint32_t family = 18; family <= 38; ++family)
    {
        for (uint32_t level : {1, 4, 9, 10, 12, 60})
        {
            CHECK(coa::OrdinaryClassSpell(family, family, level));
            CHECK(!coa::OrdinaryClassSpell(family, family + 1, level));
        }
        CHECK(!coa::OrdinaryClassSpell(family, family, 0));
        CHECK(!coa::OrdinaryClassSpell(family, family, 61));
    }
    CHECK(!coa::OrdinaryClassSpell(0, 0, 1));
}

TEST(Coa_ordinary_training_native_rank_prerequisites)
{
    std::set<uint32_t> candidates{100, 101, 102, 200, 805651};
    std::vector<coa::TrainingRank> rows{{1,100,100,1}, {2,100,101,2}, {3,100,102,3},
        {4,805351,805351,1}, {5,805351,805651,2}, {6,900,901,2}};
    auto exists = [&](uint32_t id) { return candidates.count(id) != 0; };
    auto mapping = coa::TrainingPreviousSpells(candidates, rows, exists);
    CHECK_EQ(mapping.at(100), 0u);
    CHECK_EQ(mapping.at(101), 100u);
    CHECK_EQ(mapping.at(102), 101u);
    CHECK_EQ(mapping.at(200), 0u);
    CHECK(!mapping.count(805651));
    CHECK(!mapping.count(900)); CHECK(!mapping.count(901));
    CHECK(!coa::TrainingRankKnown(mapping, 101, [](uint32_t) { return false; }));
    CHECK(coa::TrainingRankKnown(mapping, 101, [](uint32_t id) { return id == 100; }));
    CHECK(!coa::TrainingRankKnown(mapping, 102, [](uint32_t id) { return id == 100; }));
    CHECK(!coa::TrainingRankKnown(mapping, 805651, [](uint32_t) { return true; }));
    CHECK(coa::TrainingRankKnown(mapping, 200, [](uint32_t) { return false; }));
}

TEST(Coa_ordinary_training_malformed_rank_sequences_fail_closed)
{
    auto exists = [](uint32_t) { return true; };
    std::vector<coa::TrainingRank> valid{{1,100,100,1}, {2,100,101,2}};
    for (auto bad : std::vector<coa::TrainingRank>{{3,100,102,2}, {3,200,101,2},
        {2,200,200,1}, {3,100,102,0}, {3,100,102,1}, {0,100,102,3}, {3,100,0,3}})
    {
        auto rows = valid;
        rows.push_back(bad);
        auto mapping = coa::TrainingPreviousSpells({100,101,300}, rows, exists);
        CHECK(!mapping.count(100)); CHECK(!mapping.count(101)); CHECK(mapping.count(300));
    }
    CHECK(coa::TrainingPreviousSpells({101}, {{2,100,101,2}}, exists).empty());
    CHECK(coa::TrainingPreviousSpells({102}, {{1,100,100,1}, {3,100,102,3}}, exists).empty());
    CHECK(coa::TrainingPreviousSpells({100}, {{2,100,101,2}}, exists).empty());
}

TEST(Coa_ordinary_training_native_baked_rank_fixture)
{
    auto path = std::getenv("ASCENDER_TRAINING_DATA");
    auto catalogPath = std::getenv("ASCENDER_COA_TSV");
    auto pin = std::getenv("ASCENDER_COA_SHA256");
    if (!path || !catalogPath || !pin)
    {
        std::printf("  SKIP native training artifact: set ASCENDER_TRAINING_DATA/ASCENDER_COA_TSV/ASCENDER_COA_SHA256\n");
        return;
    }
    auto advancement = coa::Catalog::Load(catalogPath, pin)->AllSpells();
    DBCFileLoader spells, trainers, ranks;
    std::string base = std::string(path) + "/dbc/";
    // Approved server projection, not the client's wider Spell.dbc layout.
    REQUIRE(spells.Load((base + "Spell.dbc").c_str(), std::string(234, 'i').c_str()));
    REQUIRE(trainers.Load((base + "NPCTrainer.dbc").c_str(), "iiii"));
    REQUIRE(ranks.Load((base + "SpellRank.dbc").c_str(), "iiii"));
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> definitions;
    for (uint32_t i = 0; i < spells.GetNumRows(); ++i)
    {
        auto row = spells.getRecord(i);
        definitions.emplace(row.getUInt(0), std::make_pair(row.getUInt(39), row.getUInt(208)));
    }
    std::set<uint32_t> candidates;
    for (uint32_t i = 0; i < trainers.GetNumRows(); ++i)
    {
        auto id = trainers.getRecord(i).getUInt(1);
        auto found = definitions.find(id);
        if (found != definitions.end() && found->second.first >= 1 && found->second.first <= 60 &&
            !advancement.count(id))
        {
            candidates.insert(id);
        }
    }
    std::vector<coa::TrainingRank> rows;
    for (uint32_t i = 0; i < ranks.GetNumRows(); ++i)
    {
        auto row = ranks.getRecord(i);
        rows.push_back({row.getUInt(0), row.getUInt(1), row.getUInt(2), row.getUInt(3)});
    }
    auto mapping = coa::TrainingPreviousSpells(candidates, rows,
        [&](uint32_t id) { return definitions.count(id) != 0; });
    std::set<uint32_t> families;
    uint32_t later = 0;
    for (auto const& entry : mapping)
    {
        CHECK(!advancement.count(entry.first));
        families.insert(definitions.at(entry.first).second);
        later += definitions.at(entry.first).first >= 10;
    }
    for (uint32_t family = 18; family <= 38; ++family) CHECK(families.count(family));
    CHECK(later > 0);
    for (uint32_t id : {560750,801718,653130,653234,653236,653242,801707,500232,706742})
    {
        CHECK(candidates.count(id)); CHECK(mapping.count(id));
        CHECK(definitions.at(id).first <= 4);
        CHECK_EQ(definitions.at(id).second, 34u);
    }
    CHECK_EQ(definitions.at(502573).first, 8u);
    CHECK(mapping.count(502573)); CHECK(mapping.at(502573) != 0);
    CHECK(!coa::TrainingRankKnown(mapping, 502573, [](uint32_t) { return false; }));
    CHECK(coa::TrainingRankKnown(mapping, 502573,
        [&](uint32_t id) { return id == mapping.at(502573); }));
    CHECK(candidates.count(805651)); CHECK(!candidates.count(805351)); CHECK(!advancement.count(805351));
    CHECK_EQ(mapping.at(805651), 805351u);
    CHECK(!coa::TrainingRankKnown(mapping, 805651, [](uint32_t) { return false; }));
    std::printf("  native rank check: %zu candidates, %zu valid, %u level10-60, Repair previous %u\n",
        candidates.size(), mapping.size(), later, mapping.at(502573));
}

TEST(Coa_native_all_classes_specs_levels_and_300755_marker)
{
    auto path = std::getenv("ASCENDER_COA_TSV");
    auto pin = std::getenv("ASCENDER_COA_SHA256");
    if (!path || !pin)
    {
        std::printf("  SKIP native CoA artifact: set explicit ASCENDER_COA_TSV/ASCENDER_COA_SHA256\n");
        return;
    }
    auto c = coa::Catalog::Load(path, pin);
    CHECK_EQ(c->Entries().size(), 3616u);
    CHECK_EQ(c->Specs().size(), 70u);
    CHECK(c->AllSpells().count(300755));
    std::array<uint32_t,21> const starters = {{801787,807037,801901,804179,804020,500904,800311,
        805406,500125,500074,804418,801722,800790,500720,801972,800231,500549,800869,500357,500402,707141}};
    for (unsigned cls = 12; cls <= 32; ++cls)
    {
        CHECK_EQ(c->BaseSpell(cls, 1).spell, starters[cls - 12]);
        for (unsigned level = 1; level <= 60; ++level)
        {
            auto b = c->Authorize(cls, level, 0, {});
            CHECK(b.entries.empty()); CHECK_EQ(b.spells.size(), 1u);
        }
    }
    for (auto const& spec : c->Specs())
    {
        for (unsigned level = 10; level <= 60; ++level)
        {
            auto b = c->Authorize(spec.second.playerClass, level, spec.first, {});
            CHECK_EQ(b.entries.at(spec.second.marker), 1u);
            CHECK_EQ(b.entries.at(spec.second.root), 1u);
            CHECK_EQ(c->SelectedSpec(b.entries), spec.first);
            std::vector<coa::analytic::CoaEntry> entries;
            for (auto const& e : b.entries)
            {
                entries.push_back({e.first,e.second,c->EntrySpell(e.first,e.second,level).rank,false,1});
            }
            auto packet = coa::analytic::BuildEntriesStatePacket(entries);
            CHECK(packet.GetOpcode() == coa::analytic::EntriesStateOpcode);
            CHECK(coa::analytic::ParseEntriesState(packet).status == coa::analytic::ParseStatus::Ok);
        }
    }
}

TEST(Coa_taking_vows_unlocks_the_seven_vow_buffs)
{
    // Sun Cleric node 34025 "Spiteful" grants Taking Vows 300331; the build
    // then carries the seven Vow self-buffs, which have no other grant path
    // (ca_talents.lua VOW_SPELLS, same ids).
    std::ostringstream extra;
    extra << "CLASS\t27\t16\nBASESPELL\t27\t90027\t1\t1\n";
    for (unsigned l = 1; l <= 60; ++l) { extra << "BUDGET\t27\t" << l << "\t5\t0\n"; }
    extra << "SPEC\t70\t27\t34026\t34027\n"
             "ENTRY\t34026\t27\t1\t0\t10\t0\t0\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
             "SPELL\t34026\t1\t340261\t1\t10\nOWNER\t34026\t70\n"
             "ENTRY\t34027\t27\t1\t0\t10\t1\t0\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
             "SPELL\t34027\t1\t340271\t1\t10\nOWNER\t34027\t70\n"
             "ENTRY\t34025\t27\t1\t0\t1\t1\t0\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
             "SPELL\t34025\t1\t300331\t1\t1\n";
    auto c = Catalog(extra.str());
    auto plain = c->Authorize(27, 10, 70, {});
    CHECK(!plain.spells.count(300331));
    for (uint32_t vow : {803489u, 803491u, 803494u, 803719u, 807435u, 807547u, 807749u})
    {
        CHECK(!plain.spells.count(vow));
    }
    auto vowed = c->Authorize(27, 10, 70, {{34025, 1}});
    CHECK(vowed.spells.count(300331));
    for (uint32_t vow : {803489u, 803491u, 803494u, 803719u, 807435u, 807547u, 807749u})
    {
        CHECK(vowed.spells.count(vow));
    }
}

TEST(Coa_point_budget_authoritative_class_spec)
{
    // PLAN character-foundations 24.7 -- class/spec talent-point budgets
    // are authoritative (owner correction, 2026-09-18, ascender commit
    // 8031c64).
    //
    // The wire gate against a live realm is
    // ascender-server/test/talent_point_budget_test.py (class 12
    // Barbarian, spec 1 Tactics, class pick 34291 / spell 705169, spec
    // pick 30163 / spell 804138, L10 budget 1/0, L12 budget 2/1).
    //
    // This core test uses an extended Fixture that mirrors that wire
    // shape exactly by adding a spec-owned TE entry (id 121) with no
    // prerequisites, so a single-pick L10 spec-tree overflow reaches
    // the point-budget check rather than the base fixture's
    // 111-requires-106-rank-2 traversal refusal. Everything else --
    // marker 100, root 101, class pick 106 -- comes from the base
    // Fixture.
    //
    //   marker    100       ae 0 / te 0, owners {1}
    //   root      101       ae 1 / te 0, owners {1}  (auto-added by
    //                       Authorize once specId is nonzero)
    //   free 105  ae 0 / te 0, owners {1}, connects to root 101
    //   class pk  106       ae 1 / te 0, owners {1}, connects to 105,
    //                       maxRank 2   -- mirrors ascender 34291
    //   spec pk   121       ae 0 / te 1, owners {1}, no requires
    //                       -- mirrors ascender 30163 (spec pick
    //                       whose only prerequisites are the
    //                       marker/root the spec auto-adds)
    //   budgets   L10 -> ae 1 / te 0   (exactly marker+root)
    //             L12 -> ae 2 / te 1   (exactly marker+root+106+121)
    //
    // Assertions here name exactly the properties the plan's *Check:*
    // line requires: the marker-only save at L10 is the legal control;
    // adding EITHER one reachable class pick OR one spec pick must be
    // refused with the exact string "CoA point budget exceeded" and
    // must NOT be refused with the retired "CoA essence budget
    // exceeded"; the L12 marker+class+spec exactly fits and authorizes;
    // ownership and traversal refusals remain unchanged. If any of
    // these facts moves, this test fails and the reason names which
    // fact moved.
    auto c = Catalog(
        "ENTRY\t121\t12\t1\t0\t10\t0\t1\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
        "SPELL\t121\t1\t99121\t1\t10\n"
        "OWNER\t121\t1\n");

    // 1. Marker-only at L10 is the legal control: authorizes and the
    // build is exactly {marker, root, free trait}.
    auto markerOnly = c->Authorize(12, 10, 1, {});
    CHECK(markerOnly.entries.count(100));
    CHECK(markerOnly.entries.count(101));
    CHECK(markerOnly.entries.count(105));
    CHECK(!markerOnly.entries.count(106));
    CHECK(!markerOnly.entries.count(121));

    // 2. L10 + one class pick (one AE over) must refuse with the exact
    // 24.7 traversal string. The retired 24.5 acceptance (returned a
    // valid Build) must be gone; the retired 23.17(c) essence string
    // must be gone too.
    CHECK(RejectedWith([&] { c->Authorize(12, 10, 1, {{106, 1}}); },
        "CoA point budget exceeded"));
    CHECK(RejectedMessage([&] { c->Authorize(12, 10, 1, {{106, 1}}); })
        != "CoA essence budget exceeded");

    // 3. L10 + one spec pick (one TE over) must refuse the same way.
    // 121 costs 0 AE / 1 TE; L10 budget is te 0 so this is one
    // spec-tree point over.
    CHECK(RejectedWith([&] { c->Authorize(12, 10, 1, {{121, 1}}); },
        "CoA point budget exceeded"));
    CHECK(RejectedMessage([&] { c->Authorize(12, 10, 1, {{121, 1}}); })
        != "CoA essence budget exceeded");

    // 4. L12 marker + class pick + spec pick exactly fits ae 2 / te 1
    // and must authorize -- this proves the refusal at L10 is
    // budget-specific rather than an unrelated ownership / traversal /
    // prerequisite fault or a moved fixture. On pre-fix core this leg
    // also passes today (the whole set is legal both under 24.5 and
    // 24.7); it fails only if the fixture drifts.
    auto fits = c->Authorize(12, 12, 1, {{106, 1}, {121, 1}});
    CHECK(fits.entries.count(100));
    CHECK(fits.entries.count(101));
    CHECK(fits.entries.count(106) && fits.entries.at(106) == 1u);
    CHECK(fits.entries.count(121) && fits.entries.at(121) == 1u);

    // 5. Ownership refusal is unchanged: 107 is owned by spec 2, so
    // requesting it under spec 1's marker still refuses "CoA entry not
    // owned" -- not "CoA point budget exceeded". The 24.7 change does
    // not loosen unrelated checks. Level 60 keeps the whole-set point
    // budget well above the cost so this leg reaches the ownership
    // check rather than the budget one.
    CHECK(RejectedWith([&] { c->Authorize(12, 60, 1, {{107, 1}}); },
        "CoA entry not owned"));

    // 6. Replace maps the point-budget throw to Rejected, propagates
    // the exact string as `error`, and does not commit. This is the
    // wire path a live client sees on 0x72C: result NOT_TRAVERSIBLE,
    // traversal "CoA point budget exceeded", store untouched.
    {
        FakeStore store;
        coa::State current; current.guid = 42;
        coa::Build installed;
        coa::AuthorizationError refusal;
        std::string error;
        CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}, {106, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal)
            == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA point budget exceeded"));
        CHECK(error != std::string("CoA essence budget exceeded"));
        CHECK_EQ(store.commits, 0u);
        CHECK(current.entries.empty());
    }

    // 7. And the opposite corner (spec-tree overflow) on the wire path.
    {
        FakeStore store;
        coa::State current; current.guid = 43;
        coa::Build installed;
        coa::AuthorizationError refusal;
        std::string error;
        CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}, {121, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal)
            == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA point budget exceeded"));
        CHECK_EQ(store.commits, 0u);
    }

    // 8. The L12 marker + class + spec save on the wire path applies
    // and commits, and current.entries holds every requested pick.
    // This is the exact positive control the ascender wire gate
    // (test/talent_point_budget_test.py, phase 6) reads back from the
    // characters DB.
    {
        FakeStore store;
        coa::State current; current.guid = 44;
        coa::Build installed;
        coa::AuthorizationError refusal;
        std::string error;
        CHECK(coa::Replace(*c, store, current, 12, 12, {{100, 1}, {106, 1}, {121, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal)
            == coa::ApplyStatus::Applied);
        CHECK(error.empty());
        CHECK_EQ(store.commits, 1u);
        coa::Ranks got;
        for (auto const& e : current.entries) { got.emplace(e.entryId, e.rank); }
        CHECK(got.count(100) && got.count(101) && got.at(106) == 1u
            && got.at(121) == 1u);
    }
}

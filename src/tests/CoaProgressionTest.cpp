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
    auto c = Catalog();
    CHECK(Rejected([&] { c->Authorize(13, 60, 1, {}); }));
    for (coa::Ranks r : {coa::Ranks{{104, 1}}, {{107, 1}}, {{102, 1}}, {{106, 0}}, {{106, 3}}, {{108, 1}, {109, 1}}})
    {
        CHECK(Rejected([&] { c->Authorize(12, 60, 1, r); }));
    }
    CHECK(Rejected([&] { c->SelectedSpec({{100, 1}, {102, 1}}); }));
    CHECK(Rejected([&] { c->Authorize(12, 10, 1, {{106, 1}}); }));
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
    // structurally. A refusal that blames one entry names it; a whole-set
    // budget refusal (and wire-shape refusals) stay 0. Error strings are
    // byte-identical: the client shows them, it never parses them.
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

    // Budget: at 10 the package already spends the 1 AE, so +106 exceeds
    // the whole set -- no single offender, 0 is correct.
    {
        FakeStore store;
        coa::State current; current.guid = 7;
        coa::AuthorizationError refusal;
        CHECK(coa::Replace(*c, store, current, 12, 10, {{100, 1}, {106, 1}}, 100,
            installed, error, coa::MutationSource::Request, &refusal) == coa::ApplyStatus::Rejected);
        CHECK_STR(error, std::string("CoA essence budget exceeded"));
        CHECK_EQ(refusal.entry, 0u);
        CHECK_EQ(refusal.rank, 0u);
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

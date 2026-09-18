#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "server/bap/internal.h"
#include "server/bap/encrypted/bap_connection_publication.h"
#include "server/bap/encrypted/queuez/queuez_state_validation.h"
#include "server/bap/encrypted/queuez/staging/queuez_family_staging.h"
#include "middleware/bap/family_unsubscription.h"
#include "middleware/datagen/definitions.h"
#include "core/logging/log.h"
#include "core/settings/settings.h"

// Exercise the production request parser, subscription lifecycle, and acquisition staging.
namespace dawn::core::log { void write(Channel, Level, std::string_view) noexcept {} }
// The connection-publication unit also contains activity code. These unused dependencies must
// never run in this account-subscription test.
namespace dawn::core::settings { const Settings& get() noexcept { std::abort(); } }
namespace dawn::state::activity::events { void reload() noexcept { std::abort(); } }
namespace dawn::server::bap {
void retire_bap_activity_lease_locked(state::activity::ActivityInstanceKey) noexcept { std::abort(); }
void update_hud_anchor_after_binding_replacement_locked(const ActivityBindingState&,
                                                       const ActivityBindingState&) noexcept { std::abort(); }
}
namespace dawn::server::bap::encrypted::push::activity {
bool seed_identity(state::activity::ActivityInstanceKey, std::uint64_t, std::uint64_t) noexcept { std::abort(); }
bool seed_transition_token(state::activity::ActivityInstanceKey) noexcept { std::abort(); }
}
namespace q = dawn::server::bap::encrypted::queuez;
namespace dg = dawn::middleware::datagen;
namespace unsub = dawn::middleware::bap::family_unsubscription;
constexpr std::uint64_t root=0x9EAA300100100100ULL, character=root+2;
constexpr std::uint64_t newItem=0x4000000000001013ULL;
int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)

q::SessionState seed() {
    q::SessionState s{};
    s.family4Active=true; s.family4RootSoid=root; s.family4Version=7;
    s.family4ResidentCount=3;
    s.family4Residents[0]={root,dg::kAccountObjectId};
    s.family4Residents[1]={character,dg::kCharacterObjectId};
    s.family4Residents[2]={newItem-1,dg::kItemInstanceObjectId};
    s.family3Active=true; s.family3RootSoid=root; s.family3Version=4;
    s.family0Active=true; s.family0Character=character; s.family0Version=3;
    s.family0RootSoid=root;
    return s;
}

q::SessionState unsubscribe(const q::SessionState& before, std::uint8_t family, std::uint64_t key) {
    q::SessionState after{};
    q::stage_unsubscription(before,family,key,after);
    return after;
}

bool pull(const q::SessionState& s, q::ItemAcquisition& acquisition) {
    return q::stage_item_acquisition(s,root,character,newItem,true,acquisition);
}

int main() {
    const auto before=seed(); q::ItemAcquisition acquisition{};
    CHECK(q::valid(before)); CHECK(pull(before,acquisition));
    for (const auto family: std::array<std::uint8_t,5>{0,2,3,5,255}) {
        // Same decoded request, differing only in its family selector.
        std::array<std::byte,9> wire{}; wire[0]=static_cast<std::byte>(family);
        for (unsigned i=0;i<8;++i) wire[i+1]=static_cast<std::byte>(root>>(56-8*i));
        unsub::Request request{}; CHECK(unsub::parse(wire,request));
        CHECK(request.familyType==family); CHECK(request.familyRootSoid==root);
        auto after=unsubscribe(before,request.familyType,request.familyRootSoid);
        CHECK(q::valid(after)); CHECK(pull(after,acquisition));
        CHECK(acquisition.after.family4Version==8);
        CHECK(acquisition.after.family4ResidentCount==4);
        CHECK(acquisition.after.family4Residents[3].objectSoid==newItem);
        CHECK(after.family4Residents[2].objectSoid==newItem-1);
        q::ProfileItemAcquisition profile{};
        CHECK(q::stage_profile_item_acquisition(after,root,newItem,true,true,profile));
        CHECK(profile.after.family4Version==8);
        CHECK(profile.after.family4ResidentCount==4);
        CHECK(q::stage_profile_item_acquisition(after,root,0,false,false,profile));
        CHECK(profile.after.family4ResidentCount==3);
        if (family==0) { CHECK(!after.family0Active); CHECK(after.family3Active); }
        if (family==3) { CHECK(!after.family3Active); CHECK(after.family0Active); }
        std::printf("PASS family %u unsubscribe: preserves Collections acquisition staging\n",family);
    }
    for (const auto family: std::array<std::uint8_t,6>{0,2,3,4,5,255}) {
        for (const auto key: {std::uint64_t{0},root+100}) {
            auto after=unsubscribe(before,family,key);
            CHECK(q::staging::same_state(before,after));
            CHECK(after.family0RootSoid==before.family0RootSoid);
        }
    }
    auto released=unsubscribe(before,4,root);
    CHECK(q::valid(released)); CHECK(!pull(released,acquisition));
    q::ProfileItemAcquisition profile{};
    CHECK(!q::stage_profile_item_acquisition(released,root,newItem,true,true,profile));
    CHECK(released.family0Active); CHECK(released.family3Active);
    auto twice=unsubscribe(released,4,root);
    CHECK(q::staging::same_state(released,twice));
    for (const auto phase: {q::Family3Phase::publishOnce,q::Family3Phase::responseOnly}) {
        auto pending=before; pending.family3Phase=phase;
        for (const auto family: std::array<std::uint8_t,2>{3,4}) CHECK(q::valid(unsubscribe(pending,family,root)));
    }
    auto pending=before; pending.pendingBannerRoot=root;
    CHECK(unsubscribe(pending,0,root).pendingBannerRoot==0);
    CHECK(unsubscribe(pending,2,root).pendingBannerRoot==root);
    // A fresh subscription after a real Family-4 release restores its version-zero manifest.
    std::array<dawn::middleware::queuez::Object,3> objects{};
    for(std::size_t i=0;i<objects.size();++i) {
        objects[i].id=before.family4Residents[i].definitionId;
        objects[i].version=before.family4Residents[i].objectSoid;
    }
    dawn::middleware::queuez::Family snapshot{};
    snapshot.type=4; snapshot.rootSoid=root; snapshot.version=0;
    snapshot.flags=dawn::middleware::queuez::kFullSnapshotFlag; snapshot.objects=objects;
    q::SessionState restored{};
    CHECK(q::stage_family4_snapshot(released,snapshot,restored));
    CHECK(pull(restored,acquisition)); CHECK(acquisition.after.family4Version==1);
    // Banner unsubscribe/resubscribe owns its own version-zero ladder and exact root.
    auto bannerReleased=unsubscribe(before,0,root);
    bool publish=false, incremental=false;
    CHECK(q::stage_family0_subscription(bannerReleased,root,character,publish,incremental,restored));
    CHECK(publish && !incremental && restored.family0Version==0);
    CHECK(restored.family0RootSoid==root && restored.family4Version==7);
    CHECK(!q::stage_family0_subscription(before,root+99,character,publish,incremental,restored));
    auto detached=unsubscribe(before,4,root);
    detached=unsubscribe(detached,3,root);
    CHECK(q::valid(detached) && detached.family0Active);
    CHECK(!unsubscribe(detached,0,root).family0Active);
    CHECK(unsubscribe(detached,0,root+99).family0Active);
    auto changed=before; changed.family0RootSoid=root+99;
    CHECK(!q::staging::same_state(before,changed));
    changed=before; changed.pendingBannerRoot=root;
    CHECK(!q::staging::same_state(before,changed));
    changed=before; changed.family0RootSoid=0; CHECK(!q::valid(changed));

    // Deferred publication cancellation runs only after a committed reply and matches roots.
    auto peer=std::make_unique<dawn::server::bap::Session>();
    auto arm=[&] {
        peer->family4RepushArmed=true; peer->family4RepushRoot=root; peer->family4RepushDueTick=123;
        peer->bannerRepushArmed=true; peer->bannerRepushRoot=root; peer->bannerRepushDueTick=456;
        peer->accountResyncArmed=true; peer->accountResyncGeneration=789;
    };
    q::StagedPublication publication{};
    arm(); publication.hasState=true; publication.cancelBannerRepushRoot=root;
    dawn::server::bap::encrypted::arm_repushes(*peer,publication);
    CHECK(!peer->bannerRepushArmed && peer->bannerRepushRoot==0 && peer->bannerRepushDueTick==0);
    CHECK(peer->family4RepushArmed && peer->family4RepushDueTick==123 && peer->accountResyncArmed);
    arm(); publication.cancelBannerRepushRoot=root+99;
    dawn::server::bap::encrypted::arm_repushes(*peer,publication);
    CHECK(peer->bannerRepushArmed && peer->family4RepushArmed);
    publication={}; publication.hasState=true; publication.cancelFamily4RepushRoot=root;
    publication.releasedFamily4=true;
    dawn::server::bap::encrypted::arm_repushes(*peer,publication);
    CHECK(!peer->family4RepushArmed && peer->family4RepushRoot==0 && peer->family4RepushDueTick==0);
    CHECK(peer->bannerRepushArmed && peer->bannerRepushDueTick==456);
    CHECK(!peer->accountResyncArmed && peer->accountResyncGeneration==0);
    arm(); publication.hasState=false;
    dawn::server::bap::encrypted::arm_repushes(*peer,publication);
    CHECK(peer->family4RepushArmed && peer->bannerRepushArmed && peer->accountResyncArmed);

    std::printf("PASS %d checks. Production Collections unsubscription regression checks.\n",checks);
}

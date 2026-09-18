#pragma once
#include "omega_roster_readiness.h"
#include "omega_monitor_edges.h"
#include "../../../../state/activity/coo/omega_opening.h"

namespace dawn::server::bap::encrypted::activity_message::omega_opening_intake {
namespace opening = state::activity::coo::omega::opening;
namespace sense = middleware::bap::activity_message::sense_update;
struct Context final {
    std::uint64_t binding{}, packet{};
    bool parsed{}, handleBound{}, epochBound{}, destinationBound{};
};
struct Result final { bool queued{}, overflow{}; };

// Called under the existing session lock. Copy only authenticated native facts;
// all ordering decisions, leases and authority changes belong to Run::update.
[[nodiscard]] inline Result capture(opening::Run& run, const Context& context,
                                    const sense::SenseUpdate& update) noexcept {
    Result result;
    if (!context.parsed || !context.handleBound || !context.epochBound
        || update.objectCount > update.objects.size()) { return result; }
    const bool ready = context.destinationBound
        && omega_roster_readiness::exact_omega_initial_report(update);
    const auto put = [&](opening::Receipt receipt) {
        result.queued = true;
        result.overflow |= !run.enqueue(receipt);
    };
    const auto edge = [&](opening::Kind kind) { put({context.binding, context.packet, kind}); };
    if (!context.destinationBound || (update.hasRosterAcknowledgement && !ready)) {
        edge(opening::Kind::reset);
    }
    if (!context.destinationBound) { return result; }
    if (ready) { edge(opening::Kind::roster); }
    if (omega_monitor_edges::entered(update, 20)) { edge(opening::Kind::approach); }
    for (std::size_t i = 0; i < update.objectCount; ++i) {
        opening::lattice::Scene scene{};
        if (opening::lattice::extract_scene(update.objects[i], scene)) {
            put(opening::scene_receipt(context.binding, context.packet, scene));
        }
    }
    if (omega_monitor_edges::entered(update, 24)) {
        edge(opening::Kind::entrance);
    }
    return result;
}
} // namespace dawn::server::bap::encrypted::activity_message::omega_opening_intake

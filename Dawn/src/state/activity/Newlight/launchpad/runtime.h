#pragma once
#include "controller.h"
namespace dawn::state::activity::newlight::launchpad {
bool prepare(std::uint64_t,bool) noexcept;
Frame snapshot(std::uint64_t,std::uint64_t,bool) noexcept;
Request request() noexcept;
coo::Generation native_owner() noexcept;
bool opening_mask(std::uint64_t) noexcept;
void observe_fly_in_complete() noexcept;
std::uint64_t native_run() noexcept;
bool publication_due(std::uint64_t) noexcept;
void observe_arrival(coo::Generation,std::uint8_t) noexcept;
void observe_region(coo::Generation,std::int32_t) noexcept;
bool observe_cinematic(const cinematics::Incident&) noexcept;
bool observe_retirement(coo::Generation) noexcept;
void observe_position(float,float,float) noexcept;
void observe_submission(std::uint64_t,std::uint32_t,std::int64_t,std::uint32_t,std::uint8_t,std::uint32_t) noexcept;
void observe_prepared(coo::Generation,coo::Asset) noexcept;
void observe_object(const coo::ObjectReceipt&) noexcept;
void observe_lights(coo::Generation,const coo::ObjectReceipt&) noexcept;
void observe_device(std::uint32_t,std::uint8_t,std::uint16_t,const middleware::bap::activity_message::device_sense::Output&) noexcept;
void observe_actor(std::uint32_t,std::uint8_t,std::uint16_t,const middleware::bap::activity_message::combatant_sense::Output&) noexcept;
void observe_ghost(coo::Generation,const EnemyReceipt&,ghost::Sample) noexcept;
void observe_passenger(std::uint32_t,std::uint8_t,std::uint16_t,const middleware::bap::activity_message::native_sense::Passenger&) noexcept;
void observe_source(std::uint32_t,std::uint8_t,std::uint16_t,const middleware::bap::activity_message::squad_sense::Output&) noexcept;
void observe_use(std::uint32_t,std::uint8_t,std::uint16_t,const middleware::bap::activity_message::object_sense::Output&) noexcept;
void observe_cache_looted(coo::Generation,const coo::ObjectReceipt&) noexcept;
bool observe_admission(const EnemyReceipt&) noexcept;
void observe_entrance(coo::Generation,const EnemyReceipt&) noexcept;
bool observe_death(const EnemyReceipt&) noexcept;
void observe_readiness(const EnemyReceipt&,coo::EnemyReadiness) noexcept;
GrantRequest grant_request() noexcept;
bool observe_granted(const GrantRequest&,std::uint64_t) noexcept;
// Holds the accepted pickup binding across inventory commit; stale runs cannot consume a grant.
bool commit_grant(const GrantRequest&,std::uint64_t,bool,void*,bool(*)(void*) noexcept) noexcept;
void observe_native_object(void*) noexcept;
void observe_native_shutter(coo::Generation,std::uint32_t,bool placedAtDoor) noexcept;
void observe_native_shutter_gate(void*) noexcept;
void poll_native_objects() noexcept;
void finish_handoff(coo::Generation) noexcept;
}

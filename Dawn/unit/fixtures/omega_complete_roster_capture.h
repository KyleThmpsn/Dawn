#pragma once
#include "middleware/bap/activity_message/sense_update.h"

namespace complete_opening_capture {
// Decoded from the live 2026-09-18 Omega startup: tick 82968, packet 1,
// hash D7FA4C7E7B650F0F, 358 bytes / 2863 bits. Roster and every native
// object body were logged independently; the raw hex log stops at 320 bytes.
// No account, session, or player identity is retained.
inline void fill(dawn::middleware::bap::activity_message::sense_update::SenseUpdate& update) {
    update = {};
    update.epoch = {UINT64_MAX, UINT64_MAX};
    update.hasRosterAcknowledgement = true;
    update.rosterEntryCount = 16; update.topLevelRosterCount = 3;
    update.bubbleBlockCount = 5; update.groupCount = 2; update.objectCount = 6;
    update.paddingBits = 1;
    update.rosterEntries[0] = {0x4786C0E0U, -1, 0x83, true};
    update.rosterEntries[1] = {0x29D7B029U, -1, 0x83, true};
    update.rosterEntries[2] = {0x82FB58B7U, -1, 0x83, true};
    update.rosterEntries[3] = {0x2763EC97U, 11, 0x83, true};
    update.rosterEntries[4] = {0x0A7A8608U, 12, 0x83, true};
    update.rosterEntries[5] = {0x34D23982U, 13, 0x83, true};
    update.rosterEntries[6] = {0xBA5F26EFU, 15, 0x83, true};
    update.rosterEntries[7] = {0xD00142CFU, 15, 0x83, true};
    update.rosterEntries[8] = {0xF7A6CE7FU, 15, 0x83, true};
    update.rosterEntries[9] = {0xF4D0E0B2U, 14, 0x83, true};
    update.rosterEntries[10] = {0x95FB2E01U, 14, 0x83, true};
    update.rosterEntries[11] = {0x0040BF06U, 14, 0x83, true};
    update.rosterEntries[12] = {0x0040BF05U, 14, 0x83, true};
    update.rosterEntries[13] = {0x0040BF03U, 14, 0x83, true};
    update.rosterEntries[14] = {0x99BD2FEBU, 14, 0x83, true};
    update.rosterEntries[15] = {0x0040BF04U, 14, 0x83, true};
    update.groups[0] = {0xBA5F26EFU, 334, 0, 2};
    update.groups[1] = {0xD00142CFU, 606, 2, 4};
    auto& object0 = update.objects[0];
    object0.registryKey = 0xBA5F26EFU;
    object0.groupOrdinal = 0; object0.objectOrdinal = 0;
    object0.slotType = 23; object0.slotIndex = 1;
    object0.bodyBits = 167; object0.revision = 1; object0.hasRootDelta = true;
    object0.bodyFirst = 0xAFFFFFFFF3F80000ULL;
    object0.bodySecond = 0x0BFFFFFFFAFFFFFFULL;
    object0.bodyThird = 0x0000007F00000001ULL;
    auto& object1 = update.objects[1];
    object1.registryKey = 0xBA5F26EFU;
    object1.groupOrdinal = 0; object1.objectOrdinal = 1;
    object1.slotType = 70; object1.slotIndex = 2;
    object1.bodyBits = 54; object1.revision = 1; object1.hasRootDelta = true;
    object1.bodyFirst = 0x0020800100000001ULL;
    object1.bodySecond = 0x0000000000000000ULL;
    object1.bodyThird = 0x0000000000000000ULL;
    auto& object2 = update.objects[2];
    object2.registryKey = 0xD00142CFU;
    object2.groupOrdinal = 1; object2.objectOrdinal = 0;
    object2.slotType = 1; object2.slotIndex = 0;
    object2.bodyBits = 85; object2.revision = 1; object2.hasRootDelta = true;
    object2.bodyFirst = 0x8093180000000000ULL;
    object2.bodySecond = 0x0000000000000001ULL;
    object2.bodyThird = 0x0000000000000000ULL;
    auto& object3 = update.objects[3];
    object3.registryKey = 0xD00142CFU;
    object3.groupOrdinal = 1; object3.objectOrdinal = 1;
    object3.slotType = 43; object3.slotIndex = 1;
    object3.bodyBits = 75; object3.revision = 1; object3.hasRootDelta = true;
    object3.bodyFirst = 0xC000000048000000ULL;
    object3.bodySecond = 0x0000000000000001ULL;
    object3.bodyThird = 0x0000000000000000ULL;
    auto& object4 = update.objects[4];
    object4.registryKey = 0xD00142CFU;
    object4.groupOrdinal = 1; object4.objectOrdinal = 2;
    object4.slotType = 23; object4.slotIndex = 16;
    object4.bodyBits = 167; object4.revision = 1; object4.hasRootDelta = true;
    object4.bodyFirst = 0xAFFFFFFFF3F80000ULL;
    object4.bodySecond = 0x0BFFFFFFFAFFFFFFULL;
    object4.bodyThird = 0x0000007F00000001ULL;
    auto& object5 = update.objects[5];
    object5.registryKey = 0xD00142CFU;
    object5.groupOrdinal = 1; object5.objectOrdinal = 3;
    object5.slotType = 70; object5.slotIndex = 17;
    object5.bodyBits = 54; object5.revision = 1; object5.hasRootDelta = true;
    object5.bodyFirst = 0x0020800100000001ULL;
    object5.bodySecond = 0x0000000000000000ULL;
    object5.bodyThird = 0x0000000000000000ULL;
}
} // namespace complete_opening_capture

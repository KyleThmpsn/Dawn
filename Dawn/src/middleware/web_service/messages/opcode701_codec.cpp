#include "opcode701.h"

#include <array>
#include <cstring>

#include "../../encoding/bit_reader.h"
#include "../../datagen/family4/account/preferences/preferences_encoder.h"
#include "../../datagen/family4/account/preferences/native_key_binding_map.h"

namespace dawn::middleware::web_service::messages::opcode701 {
namespace {
namespace preferences = datagen::family4::account::preferences;

struct Field {
    std::uint16_t offset;
    std::uint16_t count;
    std::uint16_t stride;
    std::uint8_t width;
    std::uint8_t bytes;
    bool optional;
    std::uint32_t bias;
    std::span<const Field> children;
};

#include "opcode701_schema.inl"

// The update starts at native account + 0x748. Only these two subrecords are writable here.
constexpr std::size_t kPreferences = 0x44C;
constexpr std::size_t kBindings = 0x6F8;

struct Records {
    preferences::Record preferences{};
    preferences::BindingsRecord bindings{};
    bool touched{};
    bool voiceMirrorTouched{};
};

void overlay(Records& records, std::size_t offset, std::uint8_t size,
             std::uint64_t value) noexcept {
    std::byte* target = nullptr;
    if (offset >= kPreferences && offset + size <= kPreferences + sizeof(records.preferences)) {
        target = reinterpret_cast<std::byte*>(&records.preferences) + offset - kPreferences;
    } else if (offset >= kBindings && offset + size <= kBindings + sizeof(records.bindings)) {
        target = reinterpret_cast<std::byte*>(&records.bindings) + offset - kBindings;
    }
    if (target != nullptr) {
        records.touched = true;
        if (offset == kBindings + offsetof(preferences::BindingsRecord, voiceChatMirror))
            records.voiceMirrorTouched = true;
        for (std::uint8_t i = 0; i < size; ++i) {
            target[i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
        }
    }
}

bool read(encoding::bits::Reader& reader, std::span<const Field> fields,
          std::size_t base, Records& records) noexcept {
    for (const auto& field : fields) {
        for (std::size_t i = 0; i < field.count; ++i) {
            std::uint64_t value = 0;
            if (field.optional) {
                if (!reader.read(1, value)) return false;
                if (value == 0) continue;
            }
            const auto offset = base + field.offset + i * field.stride;
            if (!field.children.empty()) {
                if (!read(reader, field.children, offset, records)) return false;
            } else {
                if (!reader.read(field.width, value)) return false;
                overlay(records, offset, field.bytes, value - field.bias);
            }
        }
    }
    return true;
}

} // namespace

bool parse_settings(const Message& message,
    const state::account::settings::AccountSettings& before,
    state::account::settings::AccountSettings& output) noexcept {
    if (message.opcode != kOpcode) return false;
    Records records{};
    if (!preferences::encode(before, records.preferences, records.bindings)) return false;
    encoding::bits::Reader reader(message.payload);
    if (!read(reader, schema80807603, 0, records)) return false;
    // Two absent envelope extensions, followed by zero alignment bits; no extra bytes accepted.
    const auto remaining = reader.remaining_bits();
    std::uint64_t trailer = 0;
    if (remaining < 2 || remaining > 9
        || !reader.read(static_cast<std::uint8_t>(remaining), trailer) || trailer != 0) return false;
    if (!records.touched) {
        output = before;
        return true;
    }
    auto candidate = before;
    const auto& record = records.preferences;
    const auto& bindings = records.bindings;
    // Reverse the semantic-to-native account mapping only after the whole packet is validated.
    candidate.controls.buttonLayout = record.buttonLayout;
    candidate.controls.movementMode = record.movementMode;
    candidate.controls.controllerLookSensitivity = record.controllerLookSensitivity;
    candidate.controls.doublePressDelay = record.doublePressDelay;
    candidate.controls.mouseLookSensitivity = record.mouseLookSensitivity;
    candidate.controls.adsSensitivityModifier = record.adsSensitivityModifier;
    candidate.controls.controllerInvertVertical = record.controllerInvertVertical != 0;
    candidate.controls.controllerInvertHorizontal = record.controllerInvertHorizontal != 0;
    candidate.controls.mouseInvertVertical = record.mouseInvertVertical != 0;
    candidate.controls.mouseInvertHorizontal = record.mouseInvertHorizontal != 0;
    candidate.controls.controllerAutoLookCentering = record.controllerAutoLookCentering != 0;
    candidate.controls.controllerVibration = record.controllerVibration != 0;
    candidate.controls.unidentifiedToggle = record.unidentifiedToggle != 0;
    candidate.controls.mouseAimSmoothing = record.mouseAimSmoothing != 0;
    candidate.controls.controllerSwapShoulders = record.controllerSwapShoulders != 0;
    candidate.audio.voiceOutputMode = record.voiceOutputMode;
    candidate.audio.teamVoiceChannel = record.teamVoiceChannel;
    candidate.audio.reservedMode = record.reservedAudioMode;
    candidate.audio.migrationVersion = record.audioMigrationVersion;
    candidate.audio.chatVolume = record.chatVolume;
    candidate.audio.muteWhenUnfocused = record.muteWhenUnfocused != 0;
    candidate.audio.soundEffectsVolume = record.soundEffectsVolume;
    candidate.audio.dialogueVolume = record.dialogueVolume;
    candidate.audio.musicVolume = record.musicVolume;
    candidate.display.brightness = record.brightness;
    candidate.display.showFps = record.showFps != 0;
    candidate.display.hdrMode = record.hdrMode;
    candidate.display.calibrationPrimary = record.calibrationPrimary;
    candidate.display.calibrationAlpha = record.calibrationAlpha;
    candidate.display.motionBlur = record.motionBlurMirror != 0;
    candidate.display.filmGrain = record.filmGrainMirror != 0;
    candidate.display.chromaticAberration = record.chromaticAberrationMirror != 0;
    candidate.interface.subtitlesMode = record.subtitlesMode;
    candidate.interface.colorblindMode = record.colorblindMode;
    candidate.interface.helmetMode = record.helmetMode;
    candidate.interface.hudOpacity = record.hudOpacity;
    candidate.interface.displayHints = record.displayHints != 0;
    candidate.interface.backgroundOpacity = record.backgroundOpacity;
    candidate.interface.reticleLocation = record.reticleLocation;
    candidate.interface.reticleColor = record.reticleColor;
    candidate.interface.textSize = record.textSize;
    candidate.interface.textColor = record.textColor;
    candidate.interface.textBackgroundStyle = record.textBackgroundStyle;
    candidate.interface.textBackgroundOpacity = record.textBackgroundOpacity;
    candidate.interface.reservedTextMode = record.reservedTextMode;
    candidate.interface.subtitleOptionsEntry = record.subtitleOptionsEntry;
    candidate.social.preferGoodConnection = record.preferGoodConnection != 0;
    candidate.social.textChatMode = record.textChatMode;
    candidate.social.showRealNames = record.showRealNames != 0;
    candidate.social.clanInviteNotifications = record.clanInviteNotifications != 0;
    candidate.social.profanityFilter = record.profanityFilter != 0;
    candidate.social.voiceChatEnabled = record.voiceChatEnabled != 0;
    candidate.social.whisperChatMode = record.whisperChatMode;
    candidate.social.teamChatJoinMode = record.teamChatJoinMode;
    candidate.social.localChatJoinMode = record.localChatJoinMode;
    candidate.social.clanChatJoinMode = record.clanChatJoinMode;
    candidate.social.chatAutoHideMode = record.chatAutoHideMode;
    candidate.pc.seedVersion = bindings.accountSeedVersion;
    if (records.voiceMirrorTouched)
        candidate.pc.voiceChatEnabled = bindings.voiceChatMirror != 0;
    candidate.pc.verticalSyncMode = static_cast<std::int8_t>(bindings.verticalSyncMirror);
    candidate.pc.fieldOfViewAdjustment = bindings.fieldOfViewAdjustment;
    candidate.pc.useLocalKeyBindings = bindings.sourceSelector != 0;
    for (std::size_t slot = 0; slot < preferences::kActionsBySlot.size(); ++slot) {
        auto& binding = candidate.keyBindings.values[
            static_cast<std::size_t>(preferences::kActionsBySlot[slot])];
        const auto primary = static_cast<std::uint16_t>(bindings.keyBindings[slot]);
        const auto secondary = static_cast<std::uint16_t>(bindings.keyBindings[slot] >> 16U);
        binding.primary = primary == 0x74U ? std::nullopt : std::optional{primary};
        binding.secondary = secondary == 0x74U ? std::nullopt : std::optional{secondary};
    }
    if (!state::account::settings::valid(candidate)) return false;
    output = candidate;
    return true;
}

} // namespace dawn::middleware::web_service::messages::opcode701

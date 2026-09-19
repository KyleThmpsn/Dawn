// SPDX-License-Identifier: GPL-3.0-only
// Icon layer layouts adapted from Sundial by KyleThmpsn; see vendor/sundial/NOTICE.md.
#include "preview.h"
#include <Windows.h>
#include "client/content/items/packages/internal.h"
#include "state/editor/localized_strings.h"
#include <d3d11.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace dawn::core::ui::modules::loadout::preview {
namespace {
namespace packages = client::content::items::packages;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace strings = state::editor::strings;
struct Layer { unsigned width{}, height{}, format{}, pitch{}; std::vector<std::byte> data; };
struct Result { std::uint32_t tag{}; std::vector<Layer> layers; bool primary{}; };
struct Texture { std::array<ID3D11ShaderResourceView*, 4> views{}; std::size_t count{}; int lastFrame{}; bool pending{}, failed{}; };
ID3D11Device* g_device{};
std::unordered_map<std::uint32_t, Texture> g_textures;
std::mutex g_lock;
std::condition_variable g_wake;
std::deque<std::uint32_t> g_requests;
std::deque<Result> g_results;
std::thread g_worker;
bool g_stop{};
bool valid_tag(std::uint32_t tag) { return tables::package_of(tag) != tables::kAbsentPackageId; }
bool layer(const reader::Source& source, reader::Scratch& scratch, std::span<const std::byte> container, std::size_t at, Layer& out) {
    std::uint32_t tag{}, textureTag{}, dataTag{}; std::size_t resource{};
    tables::Array lanes{}, textures{};
    std::vector<std::byte> definition, header;
    if (!strings::read(container, at, tag) || !valid_tag(tag)
        || !reader::read_tag(source, scratch, tag, definition)
        || !strings::relative(definition, 0x10, resource)
        || !tables::find_array_at(definition, resource, lanes) || lanes.count > 32
        || !tables::find_array_at(definition, lanes.dataOffset, textures) || textures.count > 32
        || !strings::read(std::span<const std::byte>(definition), textures.dataOffset, textureTag)
        || !reader::read_tag(source, scratch, textureTag, header, dataTag) || !valid_tag(dataTag)
        || !reader::read_tag(source, scratch, dataTag, out.data)) return false;
    std::uint16_t width{}, height{};
    if (!strings::read(std::span<const std::byte>(header), 4, out.format)
        || !strings::read(std::span<const std::byte>(header), 0x0E, width)
        || !strings::read(std::span<const std::byte>(header), 0x10, height)
        || !width || !height || width > 2048 || height > 2048) return false;
    out.width = width; out.height = height;
    unsigned rows = height;
    switch (out.format) {
    case 28: case 29: case 87: case 91: out.pitch = width * 4U; break;
    case 71: case 72: out.pitch = ((width + 3U) / 4U) * 8U; rows = (height + 3U) / 4U; break;
    case 74: case 75: case 77: case 78: case 98: case 99:
        out.pitch = ((width + 3U) / 4U) * 16U; rows = (height + 3U) / 4U; break;
    default: return false;
    }
    const auto length = static_cast<std::size_t>(out.pitch) * rows;
    if (out.data.size() < length) return false;
    out.data.resize(length);
    return true;
}
Result read_icon(std::uint32_t tag, reader::Scratch& scratch) {
    Result result; result.tag = tag;
    reader::BlockKeys keys{};
    struct CleanKeys { reader::BlockKeys& keys; ~CleanKeys() { SecureZeroMemory(&keys, sizeof keys); } } clean{keys};
    core::path::Buffer path{};
    if (!packages::package_directory(path) || !packages::collect_keys(keys)) return result;
    reader::Source source{path.chars.data(), &keys};
    std::vector<std::byte> container;
    if (!reader::read_tag(source, scratch, tag, container)) return result;
    for (const auto at : {0x1CU, 0x14U, 0x20U, 0x24U}) {
        Layer decoded;
        if (layer(source, scratch, container, at, decoded)) {
            if (at == 0x14U) result.primary = true;
            result.layers.push_back(std::move(decoded));
        }
    }
    return result;
}
void run() noexcept {
    try {
        auto scratch = std::make_unique<reader::Scratch>();
        struct CloseFiles { reader::Scratch& scratch; ~CloseFiles() { reader::close_files(scratch); } } close{*scratch};
        for (;;) {
            std::uint32_t tag{};
            { std::unique_lock lock(g_lock);
                g_wake.wait(lock, [] { return g_stop || !g_requests.empty(); });
                if (g_stop) break;
                tag = g_requests.front(); g_requests.pop_front();
            }
            Result result; result.tag = tag;
            try { result = read_icon(tag, *scratch); } catch (...) { /* Return a failed preview for this item. */ }
            { std::unique_lock lock(g_lock);
                g_wake.wait(lock, [] { return g_stop || g_results.size() < 48; });
                if (g_stop) break;
                g_results.push_back(std::move(result));
            }
        }
    } catch (...) {
        std::lock_guard lock(g_lock); g_stop = true; g_requests.clear();
    }
}
void free(Texture& texture) { for (auto*& view : texture.views) if (view) { view->Release(); view = nullptr; } }
void drain() {
    std::deque<Result> results;
    { std::lock_guard lock(g_lock); results.swap(g_results); }
    g_wake.notify_all();
    for (auto& result : results) {
        auto it = g_textures.find(result.tag);
        if (it == g_textures.end() || !g_device) continue;
        auto& texture = it->second; texture.pending = false;
        if (!result.primary) { texture.failed = true; continue; }
        // A device reset can drop an in-flight read and let the same tag be requested again, so a
        // second result for one tag must replace the first rather than append past the array.
        free(texture); texture.count = 0;
        for (const auto& data : result.layers) {
            if (texture.count >= texture.views.size()) break;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = data.width; desc.Height = data.height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>(data.format); desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA pixels{data.data.data(), data.pitch, 0};
            ID3D11Texture2D* image{}; ID3D11ShaderResourceView* view{};
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, &pixels, &image))) {
                const auto created = g_device->CreateShaderResourceView(image, nullptr, &view);
                image->Release();
                if (SUCCEEDED(created)) texture.views[texture.count++] = view;
            }
        }
        texture.failed = texture.count == 0;
    }
}
}
void attach(ID3D11Device* device) noexcept { release(); g_device = device; }
bool unavailable(std::uint32_t tag) noexcept {
    const auto it = g_textures.find(tag);
    if (it != g_textures.end() && it->second.failed) return true;
    std::lock_guard lock(g_lock); return g_stop;
}
void release() noexcept {
    for (auto& [tag, texture] : g_textures) { (void)tag; free(texture); }
    g_textures.clear(); g_device = nullptr;
    std::lock_guard lock(g_lock); g_requests.clear(); g_results.clear();
    g_wake.notify_all();
}
void shutdown() noexcept {
    { std::lock_guard lock(g_lock); g_stop = true; }
    g_wake.notify_all();
    if (g_worker.joinable()) g_worker.join();
    { std::lock_guard lock(g_lock); g_requests.clear(); g_results.clear(); }
}
bool draw(std::uint32_t tag, ImVec2 position, float size, ImU32 tint) noexcept {
    if (!g_device || !valid_tag(tag)) return false;
    try {
        drain();
        if (!g_worker.joinable()) { { std::lock_guard lock(g_lock); g_stop = false; } g_worker = std::thread(run); }
        const int frame = ImGui::GetFrameCount();
        if (!g_textures.contains(tag) && g_textures.size() >= 256) {
            auto oldest = g_textures.end();
            for (auto it = g_textures.begin(); it != g_textures.end(); ++it)
                if (!it->second.pending && it->second.lastFrame < frame - 1
                    && (oldest == g_textures.end() || it->second.lastFrame < oldest->second.lastFrame)) oldest = it;
            if (oldest == g_textures.end()) return false;
            free(oldest->second); g_textures.erase(oldest);
        }
        auto [it, added] = g_textures.try_emplace(tag);
        auto& texture = it->second; texture.lastFrame = frame;
        if (added) {
            std::lock_guard lock(g_lock);
            if (!g_stop && g_requests.size() < 128) { g_requests.push_back(tag); texture.pending = true; g_wake.notify_one(); }
            else { g_textures.erase(it); return false; }
        }
        for (std::size_t i = 0; i < texture.count; ++i)
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture.views[i]), position, {position.x + size, position.y + size}, {0.0F, 0.0F}, {1.0F, 1.0F}, tint);
        return texture.count != 0;
    } catch (...) { return false; }
}
}

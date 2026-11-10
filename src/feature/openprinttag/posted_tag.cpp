#include "posted_tag.hpp"

#include <mutex>

#include <feature/openprinttag/data_utils.hpp>
#include <timing.h>

namespace buddy::openprinttag {

void PostedTags::post(VirtualToolIndex tool, const FilamentParametersInfo &info) {
    std::lock_guard lock(mutex_);
    entries_[tool] = Entry {
        .data = {
            .parameters = info.parameters,
            .data_safe_to_use = info.data_safe_to_use,
        },
        .posted_at_ms = ticks_ms(),
    };
}

void PostedTags::clear(VirtualToolIndex tool) {
    std::lock_guard lock(mutex_);
    entries_[tool] = std::nullopt;
}

std::optional<PostedTags::Data> PostedTags::take(VirtualToolIndex tool) {
    std::lock_guard lock(mutex_);
    auto &entry = fresh_entry(tool);
    if (!entry) {
        return std::nullopt;
    }
    const Data result = entry->data;
    entry = std::nullopt;
    return result;
}

std::optional<PostedTags::Data> PostedTags::peek(VirtualToolIndex tool) {
    std::lock_guard lock(mutex_);
    const auto &entry = fresh_entry(tool);
    if (!entry) {
        return std::nullopt;
    }
    return entry->data;
}

std::optional<PostedTags::Entry> &PostedTags::fresh_entry(VirtualToolIndex tool) {
    auto &entry = entries_[tool];
    if (entry && ticks_diff(ticks_ms(), entry->posted_at_ms) > static_cast<int32_t>(expiration_ms)) {
        entry = std::nullopt;
    }
    return entry;
}

PostedTags &posted_tags() {
    static PostedTags instance;
    return instance;
}

} // namespace buddy::openprinttag

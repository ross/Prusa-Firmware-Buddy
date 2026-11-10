/// @file
#pragma once

#include <optional>

#include <filament.hpp>
#include <freertos/mutex.hpp>
#include <tool_index.hpp>
#include <utils/storage/strong_index_array.hpp>

namespace buddy::openprinttag {

struct FilamentParametersInfo;

/// Filament data from a tag read by something other than the printer's own
/// reader (for example a third-party reader posting it over PrusaLink).
///
/// The data is held per tool until the next filament load into that tool
/// consumes it, it is replaced or cleared, or it expires. The load then uses
/// it as if the tag was read by the printer, except that the tag is not
/// assigned to the tool, so filament usage is not written back to it.
///
/// Thread-safe.
class PostedTags {

public:
    /// How long the data stays available for a load
    static constexpr uint32_t expiration_ms = 5 * 60 * 1000;

    struct Data {
        FilamentTypeParameters parameters;

        /// See @p FilamentParametersInfo::data_safe_to_use
        bool data_safe_to_use;
    };

    /// Stores @p info for the next load into @p tool, replacing any previous data
    void post(VirtualToolIndex tool, const FilamentParametersInfo &info);

    void clear(VirtualToolIndex tool);

    /// @returns the data for @p tool and clears it, or nullopt if there is none or it has expired
    std::optional<Data> take(VirtualToolIndex tool);

    /// @returns the data for @p tool without clearing it, or nullopt if there is none or it has expired
    std::optional<Data> peek(VirtualToolIndex tool);

private:
    struct Entry {
        Data data;
        uint32_t posted_at_ms;
    };

    /// Must be called with the mutex locked. Drops the entry if it has expired.
    std::optional<Entry> &fresh_entry(VirtualToolIndex tool);

    freertos::Mutex mutex_;
    StrongIndexArray<std::optional<Entry>, VirtualToolIndex::count, VirtualToolIndex, VirtualToolIndex::to_raw_static> entries_;
};

PostedTags &posted_tags();

} // namespace buddy::openprinttag

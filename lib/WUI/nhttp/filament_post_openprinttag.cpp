#include "filament_post.h"

#include <feature/openprinttag/data_utils.hpp>
#include <feature/openprinttag/posted_tag.hpp>
#include <tool_index.hpp>

#include <optional>

/*
 * The FilamentPost methods that touch the printer, separate so tests can
 * replace them.
 */

namespace nhttp::printer {

using buddy::openprinttag::FilamentParametersInfo;
using FieldValues = FilamentParametersInfo::FieldValues;

namespace {

    // Static rather than on the network thread's stack
    std::optional<FieldValues> field_values;

} // namespace

void FilamentPost::reset_fields() {
    field_values.emplace();
}

std::optional<FilamentPost::FieldError> FilamentPost::set_field(std::string_view key, std::string_view value) {
    const auto result = field_values->set(key, value);
    if (result) {
        return std::nullopt;
    }
    switch (result.error()) {
    case FieldValues::SetError::unknown_field:
        return FieldError::unknown_field;
    case FieldValues::SetError::invalid_value:
        return FieldError::invalid_value;
    case FieldValues::SetError::too_many_items:
        return FieldError::too_many_items;
    }
    return FieldError::invalid_value;
}

void FilamentPost::post_fields(uint8_t tool) {
    buddy::openprinttag::posted_tags().post(VirtualToolIndex::from_raw(tool), FilamentParametersInfo { *field_values });
}

} // namespace nhttp::printer

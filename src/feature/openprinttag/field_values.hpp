/// @file
#pragma once

#include <array>
#include <charconv>
#include <expected>
#include <string_view>
#include <tuple>

#include <feature/openprinttag/requests_read.hpp>
#include <utils/meta/value_pack.hpp>

namespace buddy::openprinttag {

/// Values of OpenPrintTag fields held in memory instead of read from a tag,
/// for example when they are sent over the network by a third-party reader.
///
/// Provides the same `result()` / `are_results_valid()` interface as
/// `MultiReadFieldRequestRef`, so data utilities can process either.
///
/// Values are set from their textual representation, keyed by the field name
/// from the OpenPrintTag specification:
/// - integers as decimal numbers,
/// - strings as they are (must fit the field's maximum length),
/// - enums by the item name (`"PETG"`),
/// - enum arrays by calling @p set once per item,
/// - colors as `#RRGGBB` or `#RRGGBBAA` (too transparent colors count as not present, as on a tag),
/// - UUIDs in the canonical 8-4-4-4-12 form.
template <CField auto... fields_>
class FieldValuesImpl {

public:
    static constexpr auto fields = ValuePack<fields_...> {};

    enum class SetError : uint8_t {
        unknown_field,
        invalid_value,
        too_many_items,
    };

    /// Sets field @p key to @p value. For enum arrays, appends an item.
    std::expected<void, SetError> set(std::string_view key, std::string_view value) {
        std::expected<void, SetError> result = std::unexpected(SetError::unknown_field);
        // Short-circuits on the first field with a matching name
        (void)((::openprinttag::enum_item_name(fields_) == key ? (result = std::get<Slot<fields_>>(slots).set(value), true) : false) || ...);
        return result;
    }

    template <CField auto field>
    ReadFieldRequest<field>::Result result() const {
        return std::get<Slot<field>>(slots).result();
    }

    /// Values in memory are never a result of a failed read
    [[nodiscard]] bool are_results_valid() const {
        return true;
    }

private:
    using Error = Request::Error;

    template <CField auto field>
    struct Slot {
        using Traits = FieldTraits<field>;
        using Result = ReadFieldRequest<field>::Result;
        static constexpr FieldType field_type = Traits::field_type;

        std::expected<void, SetError> set(std::string_view value) {
            if constexpr (field_type == FieldType::int_) {
                int64_t v;
                if (!parse_int(value, v)) {
                    return std::unexpected(SetError::invalid_value);
                }
                data = v;
                present = true;

            } else if constexpr (field_type == FieldType::string) {
                if (value.size() > Traits::max_length) {
                    return std::unexpected(SetError::invalid_value);
                }
                length = value.copy(data.data(), data.size());
                present = true;

            } else if constexpr (field_type == FieldType::enum_) {
                const auto item = parse_enum<typename Traits::Enum>(value);
                if (!item) {
                    return std::unexpected(SetError::invalid_value);
                }
                data = *item;
                present = true;

            } else if constexpr (field_type == FieldType::enum_array) {
                const auto item = parse_enum<typename Traits::Enum>(value);
                if (!item) {
                    return std::unexpected(SetError::invalid_value);
                }
                if (length >= data.size()) {
                    return std::unexpected(SetError::too_many_items);
                }
                data[length++] = *item;
                present = true;

            } else if constexpr (field_type == FieldType::color_rgba) {
                // #RRGGBB or #RRGGBBAA
                if (!value.starts_with('#') || (value.size() != 7 && value.size() != 9)) {
                    return std::unexpected(SetError::invalid_value);
                }
                uint32_t rgb, alpha = 0xff;
                if (!parse_int(value.substr(1, 6), rgb, 16) || (value.size() == 9 && !parse_int(value.substr(7), alpha, 16))) {
                    return std::unexpected(SetError::invalid_value);
                }
                // Same treatment as tag reads: Color has no transparency
                present = (alpha >= 16);
                data = Color::from_raw(rgb);

            } else if constexpr (field_type == FieldType::uuid) {
                const auto uuid = UUID::from_string(value);
                if (!uuid) {
                    return std::unexpected(SetError::invalid_value);
                }
                data = *uuid;
                present = true;

            } else {
                static_assert(false, "Unsupported field type");
            }

            return {};
        }

        Result result() const {
            if (!present) {
                return std::unexpected(Error::field_not_present);
            }
            if constexpr (field_type == FieldType::string) {
                return std::string_view { data.data(), length };
            } else if constexpr (field_type == FieldType::enum_array) {
                return std::span<const typename Traits::Enum> { data.data(), length };
            } else {
                return data;
            }
        }

        /// Float fields would need std::from_chars for floats, which is too big for the firmware
        static_assert(field_type != FieldType::number, "Unsupported field type");

        /// Whole @p str must be a number
        template <typename T>
        static bool parse_int(std::string_view str, T &value, int base = 10) {
            const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value, base);
            return ec == std::errc {} && ptr == str.data() + str.size();
        }

        template <typename Enum>
        static std::optional<Enum> parse_enum(std::string_view name) {
            for (size_t i = 0; i < ::openprinttag::enum_capacity<Enum>(); i++) {
                const auto item = static_cast<Enum>(i);
                if (!name.empty() && ::openprinttag::enum_item_name(item) == name) {
                    return item;
                }
            }
            return std::nullopt;
        }

        static auto storage_type() {
            if constexpr (field_type == FieldType::string) {
                return std::array<char, Traits::max_length> {};
            } else if constexpr (field_type == FieldType::enum_array) {
                return std::array<typename Traits::Enum, Traits::max_length> {};
            } else {
                return typename Result::value_type {};
            }
        }

        decltype(storage_type()) data {};
        uint8_t length = 0;
        bool present = false;
    };

    std::tuple<Slot<fields_>...> slots;
};

} // namespace buddy::openprinttag

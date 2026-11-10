/// @file
#pragma once

#include <bitset>
#include <cmath>
#include <cstdint>

#include "field_values.hpp"
#include "requests_read_multi.hpp"

#include <filament.hpp>
#include <filament_meta.hpp>
#include <option/has_chamber_api.h>
#include <utils/compact_optional.hpp>
#include <utils/string_builder.hpp>
#include <uuid/uuid.hpp>

namespace buddy::openprinttag {

/// Utility for determining remaining and full material amounts
struct AmountsInfo {

    using Requirements = ValuePack<
        MainField::nominal_netto_full_weight,
        MainField::actual_netto_full_weight,
        MainField::nominal_full_length,
        MainField::actual_full_length,
        MainField::density,
        MainField::filament_diameter,
        AuxField::consumed_weight>;

    using RequestRef = Requirements::ApplyOn<MultiReadFieldRequestRef>;

    enum class ValueSource : uint8_t {
        /// The value is not known
        no_value,

        /// The value was taken from the "nominal_" field on the tag
        nominal,

        /// The value was taken from the "actual_" field on the tag
        actual,

        /// The value was determined by other means, various fallbacks, guesses and necromancy
        heuristic,
    };

    explicit AmountsInfo(const RequestRef &req);

    /// Remaining amount of material, in grams
    CompactOptional<float, NAN> remaining_weight_g;

    /// Netto weight of the full spool, in grams
    CompactOptional<float, NAN> full_weight_g;

    /// Remaining amount of material, in millimetres
    CompactOptional<float, NAN> remaining_length_mm;

    /// Netto length of the full spool, in millimetres
    CompactOptional<float, NAN> full_length_mm;

    /// How the full_weight was determined
    ValueSource full_weight_source : 2 = ValueSource::no_value;

    /// How the full_length was determined
    ValueSource full_length_source : 2 = ValueSource::no_value;

    using WeightStrBuffer = std::array<char, 32>;
    using LengthStrBuffer = std::array<char, 32>;

    /// Builds "remaining/full g" string
    void build_weight_str(StringBuilder &sb) const;

    /// Builds "remaining/full m" string
    void build_length_str(StringBuilder &sb) const;
};

/// Utilify for determining material type and its abbreviation
struct AbbreviationInfo {

    using Requirements = ValuePack<
        MainField::material_type,
        MainField::material_abbreviation>;

    using RequestRef = Requirements::ApplyOn<MultiReadFieldRequestRef>;

    explicit AbbreviationInfo(const RequestRef &req);

    /// Constructs from in-memory values of a superset of the requirements
    template <CField auto... fields>
    explicit AbbreviationInfo(const FieldValuesImpl<fields...> &values) {
        init(values);
    }

    /// Abbreviation of the material
    /// subset of the @p abbreviation_buffer
    std::string_view abbreviation;

    /// Buffer used for storing @p abbreviation
    FieldTraits<MainField::material_abbreviation>::Buffer abbreviation_buffer;

private:
    template <typename Source>
    void init(const Source &src);
};

/// Utility for determining UUIDs
struct UUIDInfo {
    using Requirements = ValuePack<
        MainField::brand_uuid,
        MainField::instance_uuid,
        MainField::package_uuid,
        MainField::material_uuid,
        MainField::brand_name,
        MainField::material_name,
        MainField::gtin>;

    using RequestRef = Requirements::ApplyOn<MultiReadFieldRequestRef>;

    explicit UUIDInfo(const RequestRef &req, const TagUID &tag_uid);

    /// Constructs from in-memory values of a superset of the requirements.
    /// Without a tag UID, @p instance_uuid is known only if present in @p values.
    template <CField auto... fields>
    explicit UUIDInfo(const FieldValuesImpl<fields...> &values) {
        init(values, nullptr);
    }

    using OptionalUUID = CompactOptional<UUID, UUID::nil()>;

    OptionalUUID brand_uuid;
    OptionalUUID material_uuid;
    OptionalUUID package_uuid;
    OptionalUUID instance_uuid;

private:
    template <typename Source>
    void init(const Source &src, const TagUID *tag_uid);
};

/// Utility for determining filament parameters
struct FilamentParametersInfo {

    using BaseRequirements = ValuePack<
        MainField::min_print_temperature,
        MainField::max_print_temperature,
        MainField::preheat_temperature,
        MainField::max_bed_temperature,
        MainField::min_bed_temperature,
#if HAS_CHAMBER_API()
        MainField::min_chamber_temperature,
        MainField::max_chamber_temperature,
        MainField::chamber_temperature,
#endif
        MainField::hardness_shore_a,
        MainField::hardness_shore_d,
        MainField::primary_color,
        MainField::tags,
        MainField::material_type>;

    using Requirements = BaseRequirements //
        ::Concatenate<AbbreviationInfo::Requirements> //
        ::Concatenate<UUIDInfo::Requirements> //
        ::Unique<>;

    using RequestRef = Requirements::ApplyOn<MultiReadFieldRequestRef>;

    using MissingParameters = std::bitset<filament_type_parameter_count>;

    using FieldValues = Requirements::ApplyOn<FieldValuesImpl>;

    explicit FilamentParametersInfo(const RequestRef &req, const TagUID &tag_uid);

    /// Constructs from values that did not come from the printer's reader.
    /// The parameters are not associated with any tag (no @p FilamentTypeParameters::openprinttag_uid_hash).
    explicit FilamentParametersInfo(const FieldValues &values);

    /// @returns whether the information is missing the specified parameter
    template <auto FilamentTypeParameters::*mem_ptr>
    inline bool is_missing() const {
        return missing_parameters.test(filament_type_parameter_index<mem_ptr>);
    }

    FilamentTypeParameters parameters;

    /// Bitset of parameters that were not present on the OPT data
    /// This does not necessarily mean failure though - the params could have been "suggested" from the base type instead
    /// For a full failure, rather check @p deduction_failed
    /// Indexes correspond with @p filament_type_parameter_index of each member
    MissingParameters missing_parameters;

    /// Denotes whether the data is complete/reliable enough to be used for operations
    bool data_safe_to_use : 1 = false;

private:
    template <typename Source>
    void init(const Source &src, const TagUID *tag_uid);
};

} // namespace buddy::openprinttag

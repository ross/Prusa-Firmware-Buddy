#pragma once

#include "status_page.h"

#include <http/types.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace nhttp::printer {

/// Handler of `POST /api/v1/filament/{tool}`: OpenPrintTag fields read by an
/// external tag reader, staged for the next filament load into the tool.
///
/// The body is a JSON object keyed by OpenPrintTag field names:
/// - scalar fields take a string or a number,
/// - enum array fields (`tags`) take an array of strings,
/// - `null` means the field is not present.
///
/// Fields the printer doesn't use are ignored, so a reader can post
/// everything it decoded. Invalid values of the used fields, nested objects
/// and nested arrays are rejected with 400, naming the offending field.
///
/// The body is gathered in a buffer shared by all connections, so only one
/// post can be in progress at a time. Construct through @p start.
class FilamentPost {
public:
    static constexpr size_t max_body_size = 1024;

    /// Maximum number of JSON tokens in the body: one for the object, two per
    /// field, one per array item.
    static constexpr size_t max_tokens = 96;

    using StartResult = std::variant<handler::StatusPage, FilamentPost>;

    /// @returns the handler, or an error page if the body has no length, is
    /// too large, or another post is in progress.
    static StartResult start(std::optional<size_t> content_length, uint8_t tool, bool can_keep_alive, bool json_errors);

    FilamentPost(FilamentPost &&other);
    FilamentPost &operator=(FilamentPost &&other);
    FilamentPost(const FilamentPost &) = delete;
    FilamentPost &operator=(const FilamentPost &) = delete;
    ~FilamentPost();

    bool want_read() const { return phase == Phase::reading; }
    bool want_write() const { return phase == Phase::responding; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *output, size_t output_size, handler::Step &out);

private:
    enum class Phase : uint8_t {
        reading,
        responding,
    };

    enum class FieldError : uint8_t {
        unknown_field,
        invalid_value,
        too_many_items,
    };

    FilamentPost(size_t content_length, uint8_t tool, bool can_keep_alive, bool json_errors);

    void release_buffer();

    /// Parses the gathered body and posts the fields.
    /// @returns the response status, setting @p response_message on error
    http::Status process();

    http::Status error(http::Status status, const char *message);
    http::Status field_error(const char *what, std::string_view key);

    /*
     * These touch the printer, so they live in a separate file and are
     * replaced in tests. They work with a single set of fields, which is fine
     * because the server runs in a single thread and the fields are only used
     * within one process() call.
     */
    static void reset_fields();
    static std::optional<FieldError> set_field(std::string_view key, std::string_view value);
    static void post_fields(uint8_t tool);

    size_t content_length;
    size_t body_used = 0;

    /// Message of the error response; StatusPage only points to it
    std::array<char, 64> response_message {};
    http::Status response_status = http::Status::NoContent;

    uint8_t tool;
    Phase phase = Phase::reading;
    bool owns_buffer = true;
    bool can_keep_alive;
    bool json_errors;
};

} // namespace nhttp::printer

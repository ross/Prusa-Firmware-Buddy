#include "filament_post.h"
#include "handler.h"

#include <json_encode.h>

#define JSMN_HEADER
#include <jsmn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nhttp::printer {

using handler::Continue;
using handler::StatusPage;
using handler::Step;
using http::Status;
using std::nullopt;
using std::string_view;

namespace {

    // Too big to be a member of every connection's handler, so all
    // connections share one. The server runs in a single thread, so a flag is
    // enough to guard it.
    struct Scratch {
        std::array<char, FilamentPost::max_body_size> body;
        std::array<jsmntok_t, FilamentPost::max_tokens> tokens;
    };

    Scratch scratch;
    bool scratch_in_use = false;

    string_view token_text(const jsmntok_t &token) {
        return string_view(scratch.body.data() + token.start, token.end - token.start);
    }

    /// De-escapes the string in place
    string_view token_value(const jsmntok_t &token) {
        char *start = scratch.body.data() + token.start;
        return string_view(start, unescape_json_i(start, token.end - token.start));
    }

    bool is_scalar(const jsmntok_t &token) {
        return token.type == JSMN_STRING || token.type == JSMN_PRIMITIVE;
    }

} // namespace

FilamentPost::StartResult FilamentPost::start(std::optional<size_t> content_length, uint8_t tool, bool can_keep_alive, bool json_errors) {
    // The body is not read in any of the error cases, so the connection has to be dropped.
    if (!content_length.has_value()) {
        return StatusPage(Status::LengthRequired, StatusPage::CloseHandling::ErrorClose, json_errors);
    }
    if (*content_length > max_body_size) {
        return StatusPage(Status::PayloadTooLarge, StatusPage::CloseHandling::ErrorClose, json_errors);
    }
    if (*content_length == 0) {
        return StatusPage(Status::BadRequest, StatusPage::CloseHandling::ErrorClose, json_errors, nullopt, "Empty body");
    }
    if (scratch_in_use) {
        return StatusPage(Status::Conflict, StatusPage::CloseHandling::ErrorClose, json_errors, nullopt, "Another filament post in progress");
    }

    scratch_in_use = true;
    return FilamentPost(*content_length, tool, can_keep_alive, json_errors);
}

FilamentPost::FilamentPost(size_t content_length, uint8_t tool, bool can_keep_alive, bool json_errors)
    : content_length(content_length)
    , tool(tool)
    , can_keep_alive(can_keep_alive)
    , json_errors(json_errors) {}

FilamentPost::FilamentPost(FilamentPost &&other)
    : content_length(other.content_length)
    , body_used(other.body_used)
    , response_message(other.response_message)
    , response_status(other.response_status)
    , tool(other.tool)
    , phase(other.phase)
    , owns_buffer(other.owns_buffer)
    , can_keep_alive(other.can_keep_alive)
    , json_errors(other.json_errors) {
    other.owns_buffer = false;
}

FilamentPost &FilamentPost::operator=(FilamentPost &&other) {
    if (this != &other) {
        release_buffer();
        content_length = other.content_length;
        body_used = other.body_used;
        response_message = other.response_message;
        response_status = other.response_status;
        tool = other.tool;
        phase = other.phase;
        owns_buffer = other.owns_buffer;
        can_keep_alive = other.can_keep_alive;
        json_errors = other.json_errors;
        other.owns_buffer = false;
    }
    return *this;
}

FilamentPost::~FilamentPost() {
    release_buffer();
}

void FilamentPost::release_buffer() {
    if (owns_buffer) {
        scratch_in_use = false;
        owns_buffer = false;
    }
}

void FilamentPost::step(string_view input, bool terminated_by_client, uint8_t *output, size_t output_size, Step &out) {
    if (phase == Phase::responding) {
        // Rendered from here rather than handed over as the next handler,
        // because the message lives in this handler.
        const auto close_handling = can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close;
        StatusPage(response_status, close_handling, json_errors, nullopt, response_message.data()).step(input, terminated_by_client, output, output_size, out);
        return;
    }

    // FIXME: Chunked transfer encoding is not supported.
    const size_t to_read = std::min(input.size(), content_length - body_used);
    memcpy(scratch.body.data() + body_used, input.data(), to_read);
    body_used += to_read;

    if (body_used < content_length) {
        if (terminated_by_client) {
            out = Step { to_read, 0, StatusPage(Status::BadRequest, StatusPage::CloseHandling::ErrorClose, json_errors, nullopt, "Truncated request") };
        } else {
            out = Step { to_read, 0, Continue() };
        }
        return;
    }

    response_status = process();
    release_buffer();
    phase = Phase::responding;
    out = Step { to_read, 0, Continue() };
}

Status FilamentPost::error(Status status, const char *message) {
    snprintf(response_message.data(), response_message.size(), "%s", message);
    return status;
}

Status FilamentPost::field_error(const char *what, string_view key) {
    snprintf(response_message.data(), response_message.size(), "%s %.*s", what, static_cast<int>(key.size()), key.data());
    return Status::BadRequest;
}

Status FilamentPost::process() {
    auto &tokens = scratch.tokens;
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, scratch.body.data(), body_used, tokens.data(), tokens.size());
    if (count == JSMN_ERROR_NOMEM) {
        return error(Status::PayloadTooLarge, "Too many JSON tokens");
    }
    if (count < 1 || tokens[0].type != JSMN_OBJECT) {
        return error(Status::BadRequest, "Couldn't parse JSON");
    }

    const auto set = [&](string_view key, const jsmntok_t &token) -> std::optional<Status> {
        switch (set_field(key, token_value(token)).value_or(FieldError::unknown_field)) {
        case FieldError::unknown_field:
            // Also covers success; fields the printer doesn't use are ignored
            return nullopt;
        case FieldError::invalid_value:
            return field_error("Invalid value of", key);
        case FieldError::too_many_items:
            return field_error("Too many items in", key);
        }
        return nullopt;
    };

    // The shape is flat, so walk the tokens directly instead of recursing,
    // which would let a deeply nested body eat the network thread's stack.
    reset_fields();
    int pos = 1;
    for (int field = 0; field < tokens[0].size; field++) {
        if (pos + 1 >= count || tokens[pos].type != JSMN_STRING || tokens[pos].size != 1) {
            return error(Status::BadRequest, "Couldn't parse JSON");
        }
        const string_view key = token_text(tokens[pos++]);
        const jsmntok_t &value = tokens[pos++];

        if (value.type == JSMN_PRIMITIVE && token_text(value) == "null") {
            continue;

        } else if (is_scalar(value)) {
            if (const auto status = set(key, value)) {
                return *status;
            }

        } else if (value.type == JSMN_ARRAY) {
            for (int item = 0; item < value.size; item++) {
                if (pos >= count || !is_scalar(tokens[pos])) {
                    return field_error("Unsupported value of", key);
                }
                if (const auto status = set(key, tokens[pos++])) {
                    return *status;
                }
            }

        } else {
            return field_error("Unsupported value of", key);
        }
    }
    if (pos != count) {
        return error(Status::BadRequest, "Couldn't parse JSON");
    }

    post_fields(tool);
    return Status::NoContent;
}

} // namespace nhttp::printer

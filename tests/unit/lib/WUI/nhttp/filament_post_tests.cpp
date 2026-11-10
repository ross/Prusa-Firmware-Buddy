#include <nhttp/filament_post.h>
#include <nhttp/handler.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using nhttp::handler::Continue;
using nhttp::handler::StatusPage;
using nhttp::handler::Step;
using nhttp::handler::Terminating;
using nhttp::printer::FilamentPost;
using std::string;
using std::string_view;

namespace {

std::vector<std::pair<string, string>> fields_set;
std::optional<uint8_t> posted_tool;

/// Feeds @p body in chunks of @p chunk and renders the response.
/// @returns the response text
string run(string_view body, size_t chunk = 1024, uint8_t tool = 1) {
    fields_set.clear();
    posted_tool.reset();

    auto start = FilamentPost::start(body.size(), tool, true, false);
    auto *post = std::get_if<FilamentPost>(&start);
    REQUIRE(post != nullptr);

    Step step { 0, 0, Continue() };
    while (post->want_read()) {
        const auto part = body.substr(0, chunk);
        post->step(part, false, nullptr, 0, step);
        REQUIRE(step.read == part.size());
        REQUIRE(std::holds_alternative<Continue>(step.next));
        body.remove_prefix(part.size());
    }
    REQUIRE(body.empty());
    REQUIRE(post->want_write());

    uint8_t output[1024];
    post->step({}, false, output, sizeof output, step);
    return string(reinterpret_cast<char *>(output), step.written);
}

bool has_status(const string &response, string_view status) {
    return response.starts_with("HTTP/1.1 " + string(status));
}

} // namespace

namespace nhttp::printer {

void FilamentPost::reset_fields() {}

std::optional<FilamentPost::FieldError> FilamentPost::set_field(std::string_view key, std::string_view value) {
    if (key == "unknown") {
        return FieldError::unknown_field;
    }
    if (value == "bad") {
        return FieldError::invalid_value;
    }
    fields_set.emplace_back(key, value);
    return std::nullopt;
}

void FilamentPost::post_fields(uint8_t tool) {
    posted_tool = tool;
}

} // namespace nhttp::printer

TEST_CASE("Filament post success") {
    const string response = run(R"({"material_type": "PETG", "min_print_temperature": 230, "unknown": 1, "gtin": null, "tags": ["abrasive", "glitter\"y"]})", 7);
    CHECK(has_status(response, "204"));
    CHECK(posted_tool == 1);
    const decltype(fields_set) expected {
        { "material_type", "PETG" },
        { "min_print_temperature", "230" },
        { "tags", "abrasive" },
        { "tags", "glitter\"y" },
    };
    CHECK(fields_set == expected);
}

TEST_CASE("Filament post invalid") {
    string response;
    SECTION("invalid value") {
        response = run(R"({"material_type": "bad"})");
        CHECK(response.find("Invalid value of material_type") != string::npos);
    }
    SECTION("invalid array item") {
        response = run(R"({"tags": ["abrasive", "bad"]})");
        CHECK(response.find("Invalid value of tags") != string::npos);
    }
    SECTION("nested object") {
        response = run(R"({"material_type": {"a": 1}})");
        CHECK(response.find("Unsupported value of material_type") != string::npos);
    }
    SECTION("nested array") {
        response = run(R"({"tags": [["a"]]})");
        CHECK(response.find("Unsupported value of tags") != string::npos);
    }
    SECTION("malformed") {
        response = run(R"({"material_type": )");
        CHECK(response.find("Couldn't parse JSON") != string::npos);
    }
    SECTION("not an object") {
        response = run(R"(["PETG"])");
        CHECK(response.find("Couldn't parse JSON") != string::npos);
    }
    SECTION("trailing value") {
        response = run(R"({} {})");
        CHECK(response.find("Couldn't parse JSON") != string::npos);
    }
    CHECK(has_status(response, "400"));
    CHECK_FALSE(posted_tool.has_value());
}

TEST_CASE("Filament post too many tokens") {
    string body = R"({"tags": [)";
    for (size_t i = 0; i < FilamentPost::max_tokens; i++) {
        body += i ? ",1" : "1";
    }
    body += "]}";
    const string response = run(body);
    CHECK(has_status(response, "413"));
    CHECK_FALSE(posted_tool.has_value());
}

TEST_CASE("Filament post refused upfront") {
    SECTION("too large") {
        auto start = FilamentPost::start(FilamentPost::max_body_size + 1, 0, true, false);
        CHECK(std::holds_alternative<StatusPage>(start));
    }
    SECTION("no length") {
        auto start = FilamentPost::start(std::nullopt, 0, true, false);
        CHECK(std::holds_alternative<StatusPage>(start));
    }
    SECTION("empty") {
        auto start = FilamentPost::start(0, 0, true, false);
        CHECK(std::holds_alternative<StatusPage>(start));
    }
}

TEST_CASE("Filament post one at a time") {
    std::optional<FilamentPost::StartResult> first = FilamentPost::start(2, 0, true, false);
    REQUIRE(std::holds_alternative<FilamentPost>(*first));
    CHECK(std::holds_alternative<StatusPage>(FilamentPost::start(2, 0, true, false)));

    // Moving keeps the buffer taken
    FilamentPost moved = std::move(std::get<FilamentPost>(*first));
    first.reset();
    CHECK(std::holds_alternative<StatusPage>(FilamentPost::start(2, 0, true, false)));

    SECTION("released when the body is processed") {
        Step step { 0, 0, Continue() };
        moved.step("{}", false, nullptr, 0, step);
        CHECK(std::holds_alternative<FilamentPost>(FilamentPost::start(2, 0, true, false)));
    }
    SECTION("truncated body releases on destruction") {
        Step step { 0, 0, Continue() };
        moved.step("{", true, nullptr, 0, step);
        auto *state = std::get_if<nhttp::handler::ConnectionState>(&step.next);
        REQUIRE(state != nullptr);
        CHECK(std::holds_alternative<StatusPage>(*state));
        {
            FilamentPost gone = std::move(moved);
        }
        CHECK(std::holds_alternative<FilamentPost>(FilamentPost::start(2, 0, true, false)));
    }
}

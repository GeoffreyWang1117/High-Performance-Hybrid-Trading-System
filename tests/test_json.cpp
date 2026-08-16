/**
 * @file test_json.cpp
 * @brief Tests for the JSON parser/serializer used by LLM backends
 */

#include "titans/core/json.hpp"
#include <iostream>
#include <cmath>

using namespace titans::json;

namespace {

bool test_parse_primitives() {
    if (!parse("null").is_null()) return false;
    if (parse("true").as_bool() != true) return false;
    if (parse("false").as_bool() != false) return false;
    if (parse("42").as_int() != 42) return false;
    if (parse("-3.5").as_number() != -3.5) return false;
    if (parse("1e3").as_number() != 1000.0) return false;
    if (parse("\"hello\"").as_string() != "hello") return false;
    return true;
}

bool test_parse_structures() {
    auto v = parse(R"({"name":"titans","count":3,"tags":["a","b"],"nested":{"ok":true}})");

    if (!v.is_object()) return false;
    if (v["name"].as_string() != "titans") return false;
    if (v["count"].as_int() != 3) return false;
    if (v["tags"].size() != 2) return false;
    if (v["tags"][1].as_string() != "b") return false;
    if (!v["nested"]["ok"].as_bool()) return false;
    if (!v.contains("name") || v.contains("missing")) return false;

    // Missing keys return null, not crash
    if (!v["missing"]["deep"].is_null()) return false;

    return true;
}

bool test_string_escapes() {
    auto v = parse(R"("line1\nline2\t\"quoted\" \\ A")");
    const std::string& s = v.as_string();

    if (s.find('\n') == std::string::npos) return false;
    if (s.find('\t') == std::string::npos) return false;
    if (s.find("\"quoted\"") == std::string::npos) return false;
    if (s.find('\\') == std::string::npos) return false;
    if (s.find('A') == std::string::npos) return false;  // A

    return true;
}

bool test_roundtrip() {
    Value original = object({
        {"model", "llama3.1:8b"},
        {"temperature", 0.7},
        {"stream", false},
        {"messages", array({
            object({{"role", "user"}, {"content", "hi \"there\"\n"}})
        })}
    });

    std::string encoded = stringify(original);
    auto decoded = parse(encoded);

    if (decoded["model"].as_string() != "llama3.1:8b") return false;
    if (std::abs(decoded["temperature"].as_number() - 0.7) > 1e-9) return false;
    if (decoded["stream"].as_bool() != false) return false;
    if (decoded["messages"][0]["content"].as_string() != "hi \"there\"\n") return false;

    return true;
}

bool test_try_parse_failure() {
    if (try_parse("{invalid").has_value()) return false;
    if (try_parse("").has_value()) return false;
    if (try_parse("{\"a\":}").has_value()) return false;
    if (!try_parse("{}").has_value()) return false;
    if (!try_parse("[]").has_value()) return false;
    return true;
}

bool test_ollama_response_shape() {
    // Shape of a real Ollama /api/chat response
    auto v = parse(R"({
        "model": "llama3.1:8b",
        "message": {"role": "assistant", "content": "{\"classification\": \"normal\"}"},
        "done": true,
        "prompt_eval_count": 26,
        "eval_count": 298
    })");

    if (v["message"]["content"].as_string().empty()) return false;
    if (!v["done"].as_bool()) return false;
    if (v["prompt_eval_count"].as_int() != 26) return false;
    if (v["eval_count"].as_int() != 298) return false;

    // The content itself is nested JSON — parse it too
    auto inner = try_parse(v["message"]["content"].as_string());
    if (!inner || (*inner)["classification"].as_string() != "normal") return false;

    return true;
}

}  // namespace

bool run_json_tests() {
    struct { const char* name; bool (*fn)(); } cases[] = {
        {"parse_primitives", test_parse_primitives},
        {"parse_structures", test_parse_structures},
        {"string_escapes", test_string_escapes},
        {"roundtrip", test_roundtrip},
        {"try_parse_failure", test_try_parse_failure},
        {"ollama_response_shape", test_ollama_response_shape},
    };

    for (const auto& c : cases) {
        std::cout << "  test_" << c.name << "... ";
        if (!c.fn()) return false;
        std::cout << "OK\n";
    }
    return true;
}

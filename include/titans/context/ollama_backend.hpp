/**
 * @file ollama_backend.hpp
 * @brief Production-Ready Ollama LLM Backend Implementation
 *
 * Full implementation of Ollama API integration using the
 * Titans HTTP client and JSON libraries.
 */

#pragma once

#include "llm_interface.hpp"
#include "../core/http_client.hpp"
#include "../core/json.hpp"
#include <thread>
#include <queue>
#include <condition_variable>

namespace titans {
namespace context {

// ============================================================================
// Ollama API Implementation
// ============================================================================

class OllamaBackendImpl : public LLMBackend {
public:
    explicit OllamaBackendImpl(const OllamaConfig& config = {})
        : config_(config),
          base_url_("http://" + config.host + ":" + std::to_string(config.port)) {}

    std::string name() const override { return "Ollama"; }

    bool is_available() const override {
        http::HttpClient client;
        auto resp = client.get(base_url_ + "/api/tags", 5000);
        return resp.success;
    }

    std::vector<std::string> available_models() const override {
        http::HttpClient client;
        auto resp = client.get(base_url_ + "/api/tags", 10000);

        std::vector<std::string> models;
        if (resp.success) {
            auto data = json::try_parse(resp.body);
            if (data && data->contains("models")) {
                for (const auto& model : (*data)["models"].as_array()) {
                    models.push_back(model["name"].as_string());
                }
            }
        }
        return models;
    }

    LLMResponse complete(const LLMRequest& request) override {
        auto start = std::chrono::high_resolution_clock::now();

        LLMResponse response;
        response.request_id = request.request_id;
        response.model = request.model.empty() ? config_.default_model : request.model;

        // Build Ollama API request
        json::Value req_body = json::object({
            {"model", response.model},
            {"messages", build_messages(request.messages)},
            {"stream", false},
            {"options", json::object({
                {"temperature", request.temperature},
                {"num_predict", request.max_tokens}
            })}
        });

        if (request.response_format && *request.response_format == "json") {
            req_body["format"] = "json";
        }

        http::HttpClient client;
        auto http_resp = client.post(
            base_url_ + "/api/chat",
            json::stringify(req_body),
            "application/json",
            config_.timeout_ms
        );

        auto end = std::chrono::high_resolution_clock::now();
        response.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (!http_resp.success) {
            response.success = false;
            response.error_message = "HTTP error: " + http_resp.error +
                                    " (status: " + std::to_string(http_resp.status_code) + ")";
            return response;
        }

        // Parse response
        auto resp_json = json::try_parse(http_resp.body);
        if (!resp_json) {
            response.success = false;
            response.error_message = "Failed to parse JSON response";
            return response;
        }

        response.success = true;
        response.content = (*resp_json)["message"]["content"].as_string();

        // Extract token usage if available
        if (resp_json->contains("prompt_eval_count")) {
            response.prompt_tokens = (*resp_json)["prompt_eval_count"].as_int();
        }
        if (resp_json->contains("eval_count")) {
            response.completion_tokens = (*resp_json)["eval_count"].as_int();
        }
        response.total_tokens = response.prompt_tokens + response.completion_tokens;

        // Calculate time to first token from eval_duration if available
        if (resp_json->contains("eval_duration")) {
            double eval_ns = (*resp_json)["eval_duration"].as_number();
            double total_ns = (*resp_json)["total_duration"].as_number();
            response.time_to_first_token_ms = (total_ns - eval_ns) / 1e6;
        }

        return response;
    }

    std::future<LLMResponse> complete_async(const LLMRequest& request) override {
        return std::async(std::launch::async, [this, request]() {
            return complete(request);
        });
    }

    std::vector<LLMResponse> complete_batch(
        const std::vector<LLMRequest>& requests,
        int max_concurrent
    ) override {
        std::vector<LLMResponse> responses(requests.size());
        std::vector<std::future<LLMResponse>> futures;

        size_t i = 0;
        while (i < requests.size()) {
            // Launch batch of concurrent requests
            size_t batch_end = std::min(i + max_concurrent, requests.size());
            for (size_t j = i; j < batch_end; ++j) {
                futures.push_back(complete_async(requests[j]));
            }

            // Collect results
            for (size_t j = i; j < batch_end; ++j) {
                responses[j] = futures[j - i].get();
            }

            futures.clear();
            i = batch_end;
        }

        return responses;
    }

    // Pull model if not available locally
    bool pull_model(const std::string& model_name) {
        json::Value req_body = json::object({
            {"name", model_name},
            {"stream", false}
        });

        http::HttpClient client;
        auto resp = client.post(
            base_url_ + "/api/pull",
            json::stringify(req_body),
            "application/json",
            600000  // 10 minute timeout for large models
        );

        return resp.success;
    }

    // Get model info
    json::Value get_model_info(const std::string& model_name) {
        json::Value req_body = json::object({
            {"name", model_name}
        });

        http::HttpClient client;
        auto resp = client.post(
            base_url_ + "/api/show",
            json::stringify(req_body),
            "application/json",
            10000
        );

        if (resp.success) {
            auto parsed = json::try_parse(resp.body);
            if (parsed) return *parsed;
        }
        return json::Value(nullptr);
    }

private:
    json::Value build_messages(const std::vector<LLMMessage>& messages) {
        json::Array arr;
        for (const auto& msg : messages) {
            std::string role;
            switch (msg.role) {
                case LLMMessage::Role::System: role = "system"; break;
                case LLMMessage::Role::User: role = "user"; break;
                case LLMMessage::Role::Assistant: role = "assistant"; break;
            }
            arr.push_back(json::object({
                {"role", role},
                {"content", msg.content}
            }));
        }
        return json::Value(std::move(arr));
    }

    OllamaConfig config_;
    std::string base_url_;
};

// ============================================================================
// vLLM Backend Implementation (OpenAI-compatible API)
// ============================================================================

class VLLMBackendImpl : public LLMBackend {
public:
    explicit VLLMBackendImpl(const VLLMConfig& config = {})
        : config_(config),
          base_url_("http://" + config.host + ":" + std::to_string(config.port)) {}

    std::string name() const override { return "vLLM"; }

    bool is_available() const override {
        http::HttpClient client;
        auto resp = client.get(base_url_ + "/v1/models", 5000);
        return resp.success;
    }

    std::vector<std::string> available_models() const override {
        http::HttpClient client;
        auto resp = client.get(base_url_ + "/v1/models", 10000);

        std::vector<std::string> models;
        if (resp.success) {
            auto data = json::try_parse(resp.body);
            if (data && data->contains("data")) {
                for (const auto& model : (*data)["data"].as_array()) {
                    models.push_back(model["id"].as_string());
                }
            }
        }
        return models;
    }

    LLMResponse complete(const LLMRequest& request) override {
        auto start = std::chrono::high_resolution_clock::now();

        LLMResponse response;
        response.request_id = request.request_id;
        response.model = request.model;

        // Build OpenAI-compatible request
        json::Array messages;
        for (const auto& msg : request.messages) {
            std::string role;
            switch (msg.role) {
                case LLMMessage::Role::System: role = "system"; break;
                case LLMMessage::Role::User: role = "user"; break;
                case LLMMessage::Role::Assistant: role = "assistant"; break;
            }
            messages.push_back(json::object({
                {"role", role},
                {"content", msg.content}
            }));
        }

        json::Value req_body = json::object({
            {"model", request.model},
            {"messages", messages},
            {"temperature", request.temperature},
            {"max_tokens", request.max_tokens},
            {"stream", false}
        });

        if (request.response_format && *request.response_format == "json") {
            req_body["response_format"] = json::object({{"type", "json_object"}});
        }

        http::HttpClient client;
        auto http_resp = client.post(
            base_url_ + "/v1/chat/completions",
            json::stringify(req_body),
            "application/json",
            config_.timeout_ms
        );

        auto end = std::chrono::high_resolution_clock::now();
        response.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (!http_resp.success) {
            response.success = false;
            response.error_message = "HTTP error: " + http_resp.error;
            return response;
        }

        auto resp_json = json::try_parse(http_resp.body);
        if (!resp_json) {
            response.success = false;
            response.error_message = "Failed to parse JSON response";
            return response;
        }

        response.success = true;
        response.content = (*resp_json)["choices"][0]["message"]["content"].as_string();

        // Token usage
        if (resp_json->contains("usage")) {
            const auto& usage = (*resp_json)["usage"];
            response.prompt_tokens = usage["prompt_tokens"].as_int();
            response.completion_tokens = usage["completion_tokens"].as_int();
            response.total_tokens = usage["total_tokens"].as_int();
        }

        return response;
    }

    std::future<LLMResponse> complete_async(const LLMRequest& request) override {
        return std::async(std::launch::async, [this, request]() {
            return complete(request);
        });
    }

private:
    VLLMConfig config_;
    std::string base_url_;
};

// ============================================================================
// Streaming Support (for real-time TTFT measurement)
// ============================================================================

class StreamingCallback {
public:
    virtual ~StreamingCallback() = default;
    virtual void on_token(const std::string& token) = 0;
    virtual void on_complete(const LLMResponse& response) = 0;
    virtual void on_error(const std::string& error) = 0;
};

class OllamaStreamingBackend {
public:
    explicit OllamaStreamingBackend(const OllamaConfig& config = {})
        : config_(config),
          base_url_("http://" + config.host + ":" + std::to_string(config.port)) {}

    void stream_complete(const LLMRequest& request, StreamingCallback& callback) {
        auto start = std::chrono::high_resolution_clock::now();
        bool first_token = true;
        double ttft_ms = 0;

        json::Value req_body = json::object({
            {"model", request.model.empty() ? config_.default_model : request.model},
            {"messages", build_messages(request.messages)},
            {"stream", true},
            {"options", json::object({
                {"temperature", request.temperature},
                {"num_predict", request.max_tokens}
            })}
        });

        // For streaming, we need to handle chunked responses
        http::SocketConnection conn;
        if (!conn.connect(config_.host, config_.port)) {
            callback.on_error("Connection failed: " + conn.last_error());
            return;
        }

        std::string http_req = "POST /api/chat HTTP/1.1\r\n"
                              "Host: " + config_.host + "\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(json::stringify(req_body).size()) + "\r\n"
                              "\r\n" + json::stringify(req_body);

        if (!conn.send(http_req)) {
            callback.on_error("Send failed: " + conn.last_error());
            return;
        }

        // Read and process streaming response
        std::string buffer;
        std::string accumulated_content;
        int prompt_tokens = 0, completion_tokens = 0;

        char recv_buf[4096];
        bool headers_done = false;

        while (true) {
            // Simple blocking read (production would use async I/O)
            ssize_t received = recv(conn.is_open() ? 3 : -1, recv_buf, sizeof(recv_buf) - 1, 0);
            if (received <= 0) break;

            recv_buf[received] = '\0';
            buffer += recv_buf;

            // Skip HTTP headers
            if (!headers_done) {
                size_t header_end = buffer.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    buffer = buffer.substr(header_end + 4);
                    headers_done = true;
                } else {
                    continue;
                }
            }

            // Process NDJSON lines
            size_t line_end;
            while ((line_end = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, line_end);
                buffer = buffer.substr(line_end + 1);

                if (line.empty()) continue;

                auto chunk = json::try_parse(line);
                if (!chunk) continue;

                if (chunk->contains("message")) {
                    std::string token = (*chunk)["message"]["content"].as_string();
                    if (!token.empty()) {
                        if (first_token) {
                            auto now = std::chrono::high_resolution_clock::now();
                            ttft_ms = std::chrono::duration<double, std::milli>(now - start).count();
                            first_token = false;
                        }
                        callback.on_token(token);
                        accumulated_content += token;
                    }
                }

                if (chunk->contains("done") && (*chunk)["done"].as_bool()) {
                    if (chunk->contains("prompt_eval_count")) {
                        prompt_tokens = (*chunk)["prompt_eval_count"].as_int();
                    }
                    if (chunk->contains("eval_count")) {
                        completion_tokens = (*chunk)["eval_count"].as_int();
                    }

                    auto end = std::chrono::high_resolution_clock::now();

                    LLMResponse response;
                    response.request_id = request.request_id;
                    response.model = request.model;
                    response.content = accumulated_content;
                    response.success = true;
                    response.prompt_tokens = prompt_tokens;
                    response.completion_tokens = completion_tokens;
                    response.total_tokens = prompt_tokens + completion_tokens;
                    response.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
                    response.time_to_first_token_ms = ttft_ms;

                    callback.on_complete(response);
                    return;
                }
            }
        }

        callback.on_error("Stream ended unexpectedly");
    }

private:
    json::Value build_messages(const std::vector<LLMMessage>& messages) {
        json::Array arr;
        for (const auto& msg : messages) {
            std::string role;
            switch (msg.role) {
                case LLMMessage::Role::System: role = "system"; break;
                case LLMMessage::Role::User: role = "user"; break;
                case LLMMessage::Role::Assistant: role = "assistant"; break;
            }
            arr.push_back(json::object({
                {"role", role},
                {"content", msg.content}
            }));
        }
        return json::Value(std::move(arr));
    }

    OllamaConfig config_;
    std::string base_url_;
};

// ============================================================================
// Backend Factory
// ============================================================================

class LLMBackendFactory {
public:
    static std::shared_ptr<LLMBackend> create_ollama(const OllamaConfig& config = {}) {
        return std::make_shared<OllamaBackendImpl>(config);
    }

    static std::shared_ptr<LLMBackend> create_vllm(const VLLMConfig& config = {}) {
        return std::make_shared<VLLMBackendImpl>(config);
    }

    static std::shared_ptr<LLMBackend> auto_detect() {
        // Try Ollama first (default port 11434)
        OllamaConfig ollama_config;
        auto ollama = create_ollama(ollama_config);
        if (ollama->is_available()) {
            return ollama;
        }

        // Try vLLM (default port 8000)
        VLLMConfig vllm_config;
        auto vllm = create_vllm(vllm_config);
        if (vllm->is_available()) {
            return vllm;
        }

        return nullptr;
    }
};

// ============================================================================
// Health Check and Diagnostics
// ============================================================================

struct LLMDiagnostics {
    bool ollama_available = false;
    bool vllm_available = false;
    std::vector<std::string> ollama_models;
    std::vector<std::string> vllm_models;
    double ollama_ping_ms = 0;
    double vllm_ping_ms = 0;

    void print() const {
        printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                    LLM BACKEND DIAGNOSTICS                    ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ Ollama: %s (%.1fms)                                        ║\n",
               ollama_available ? "✓ Available" : "✗ Not found",
               ollama_ping_ms);
        if (ollama_available && !ollama_models.empty()) {
            printf("║   Models: ");
            for (size_t i = 0; i < std::min(ollama_models.size(), size_t(3)); ++i) {
                printf("%s%s", ollama_models[i].c_str(),
                       i < std::min(ollama_models.size(), size_t(3)) - 1 ? ", " : "");
            }
            if (ollama_models.size() > 3) printf(" (+%zu more)", ollama_models.size() - 3);
            printf("\n");
        }
        printf("╠───────────────────────────────────────────────────────────────╣\n");
        printf("║ vLLM:   %s (%.1fms)                                        ║\n",
               vllm_available ? "✓ Available" : "✗ Not found",
               vllm_ping_ms);
        if (vllm_available && !vllm_models.empty()) {
            printf("║   Models: ");
            for (size_t i = 0; i < vllm_models.size(); ++i) {
                printf("%s%s", vllm_models[i].c_str(),
                       i < vllm_models.size() - 1 ? ", " : "");
            }
            printf("\n");
        }
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
    }
};

inline LLMDiagnostics run_diagnostics() {
    LLMDiagnostics diag;

    // Check Ollama
    {
        auto start = std::chrono::high_resolution_clock::now();
        auto ollama = LLMBackendFactory::create_ollama();
        diag.ollama_available = ollama->is_available();
        auto end = std::chrono::high_resolution_clock::now();
        diag.ollama_ping_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (diag.ollama_available) {
            diag.ollama_models = ollama->available_models();
        }
    }

    // Check vLLM
    {
        auto start = std::chrono::high_resolution_clock::now();
        auto vllm = LLMBackendFactory::create_vllm();
        diag.vllm_available = vllm->is_available();
        auto end = std::chrono::high_resolution_clock::now();
        diag.vllm_ping_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (diag.vllm_available) {
            diag.vllm_models = vllm->available_models();
        }
    }

    return diag;
}

}  // namespace context
}  // namespace titans

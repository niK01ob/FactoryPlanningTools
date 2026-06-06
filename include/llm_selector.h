#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "solver.h"
#include "task_profile.h"

class LLMSelector {
public:
    struct Config {
        std::string mode = "off";  // off | mock | real
        std::string api_key;
        std::string model = "gpt-4o-mini";
        int timeout_ms = 15000;
        std::string endpoint = "https://api.openai.com/v1/chat/completions";
        Solver::HeuristicType fallback = Solver::HeuristicType::Directive;
    };

    struct Decision {
        Solver::HeuristicType selected = Solver::HeuristicType::Directive;
        std::string selected_label = "DIRECTIVE";
        std::string raw_response;
        bool used_fallback = false;
        long long llm_latency_ms = 0;
    };

    static Config ConfigFromEnv() {
        Config cfg;
        if (const char* m = std::getenv("LLM_SELECTOR_MODE")) {
            cfg.mode = ToLower(m);
        }
        if (const char* k = std::getenv("LLM_API_KEY")) {
            cfg.api_key = k;
        }
        if (const char* model = std::getenv("LLM_MODEL")) {
            cfg.model = model;
        }
        if (const char* t = std::getenv("LLM_TIMEOUT_MS")) {
            try {
                cfg.timeout_ms = std::stoi(t);
            } catch (...) {
            }
        }
        if (const char* e = std::getenv("LLM_ENDPOINT")) {
            cfg.endpoint = e;
        }
        return cfg;
    }

    static bool IsEnabled(const Config& cfg) { return cfg.mode != "off"; }

    static std::string ToLabel(Solver::HeuristicType h) {
        switch (h) {
            case Solver::HeuristicType::Dummy:
                return "DUMMY";
            case Solver::HeuristicType::Directive:
                return "DIRECTIVE";
            case Solver::HeuristicType::Fine:
                return "FINE";
            case Solver::HeuristicType::RoundRobin:
                return "ROUND_ROBIN";
            case Solver::HeuristicType::Dependent:
                return "DEPENDENT";
            case Solver::HeuristicType::ShortestProcessingTime:
                return "SPT";
            case Solver::HeuristicType::LongestProcessingTime:
                return "LPT";
            case Solver::HeuristicType::LeastFlexible:
                return "LEAST_FLEXIBLE";
            case Solver::HeuristicType::SlackBased:
                return "SLACK_BASED";
        }
        return "DIRECTIVE";
    }

    static Decision Select(const ProblemData* data, const Config& cfg,
                           uint64_t seed) {
        const auto t1 = std::chrono::steady_clock::now();
        Decision d;

        if (!IsEnabled(cfg)) {
            d.selected = cfg.fallback;
            d.selected_label = ToLabel(d.selected);
            d.raw_response = "selector_disabled";
            d.used_fallback = true;
            return d;
        }

        const TaskProfile profile = TaskProfileBuilder::Build(data);
        const std::string profile_text =
            TaskProfileBuilder::ToTaskProfileV1Text(profile);

        std::string raw;
        if (cfg.mode == "mock") {
            raw = MockLLMResponse(profile, seed);
        } else if (cfg.mode == "real") {
            raw = RealLLMResponse(cfg, profile_text, seed);
        } else {
            raw = "UNSUPPORTED_MODE";
        }

        d.raw_response = raw;
        if (!TryParseLabel(raw, d.selected)) {
            d.selected = cfg.fallback;
            d.selected_label = ToLabel(d.selected);
            d.used_fallback = true;
        } else {
            d.selected_label = ToLabel(d.selected);
        }

        const auto t2 = std::chrono::steady_clock::now();
        d.llm_latency_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1)
                .count();
        return d;
    }

private:
    static std::string ToLower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return out;
    }

    static std::string ToUpper(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::toupper(c));
                       });
        return out;
    }

    static bool TryParseLabel(const std::string& raw,
                              Solver::HeuristicType& out) {
        const std::string u = ToUpper(raw);
        if (u.find("ROUND_ROBIN") != std::string::npos) {
            out = Solver::HeuristicType::RoundRobin;
            return true;
        }
        if (u.find("LEAST_FLEXIBLE") != std::string::npos) {
            out = Solver::HeuristicType::LeastFlexible;
            return true;
        }
        if (u.find("SLACK_BASED") != std::string::npos ||
            u.find("SLACK") != std::string::npos) {
            out = Solver::HeuristicType::SlackBased;
            return true;
        }
        if (u.find("DEPENDENT") != std::string::npos) {
            out = Solver::HeuristicType::Dependent;
            return true;
        }
        if (u.find("SPT") != std::string::npos ||
            u.find("SHORTEST_PROCESSING_TIME") != std::string::npos) {
            out = Solver::HeuristicType::ShortestProcessingTime;
            return true;
        }
        if (u.find("LPT") != std::string::npos ||
            u.find("LONGEST_PROCESSING_TIME") != std::string::npos) {
            out = Solver::HeuristicType::LongestProcessingTime;
            return true;
        }
        if (u.find("DIRECTIVE") != std::string::npos) {
            out = Solver::HeuristicType::Directive;
            return true;
        }
        if (u.find("DUMMY") != std::string::npos) {
            out = Solver::HeuristicType::Dummy;
            return true;
        }
        if (u.find("FINE") != std::string::npos) {
            out = Solver::HeuristicType::Fine;
            return true;
        }
        return false;
    }

    static std::string EscapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 64);
        for (char c : s) {
            switch (c) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out.push_back(c);
            }
        }
        return out;
    }

    static std::string ReadFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in.is_open()) {
            return "";
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    static std::string ExtractContentField(const std::string& response) {
        // Minimal parser for chat.completions payload
        // looks for first "content":"..."
        const std::string key = "\"content\"";
        auto pos = response.find(key);
        if (pos == std::string::npos) {
            return response;
        }
        pos = response.find(':', pos + key.size());
        if (pos == std::string::npos) {
            return response;
        }
        pos = response.find('"', pos);
        if (pos == std::string::npos) {
            return response;
        }
        ++pos;
        std::string out;
        bool esc = false;
        for (; pos < response.size(); ++pos) {
            const char c = response[pos];
            if (esc) {
                switch (c) {
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    default:
                        out.push_back(c);
                        break;
                }
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                break;
            } else {
                out.push_back(c);
            }
        }
        return out.empty() ? response : out;
    }

    static std::string RealLLMResponse(const Config& cfg,
                                       const std::string& profile_text,
                                       uint64_t seed) {
        if (cfg.api_key.empty()) {
            return "REAL_MODE_NO_API_KEY";
        }

        const std::filesystem::path tmp_dir =
            std::filesystem::temp_directory_path();
        const std::string suffix =
            std::to_string(seed) + "_" +
            std::to_string(static_cast<long long>(
                std::chrono::steady_clock::now().time_since_epoch().count()));

        const std::filesystem::path req = tmp_dir / ("llm_req_" + suffix + ".json");
        const std::filesystem::path resp =
            tmp_dir / ("llm_resp_" + suffix + ".json");

        const std::string system_prompt =
            "You choose one front-sorting heuristic for a production scheduling "
            "solver. Available heuristics: "
            "DUMMY keeps the original front order; it can be strong when the "
            "task is easy, sparse, or has enough slack. "
            "DIRECTIVE prioritizes works with earlier deadlines; it is often "
            "strong when deadlines are tight or slack is small. "
            "FINE prioritizes works with larger lateness penalty; use it when "
            "penalty coefficients are highly uneven, not merely because fines "
            "exist. "
            "ROUND_ROBIN rotates priority between works; use it when many works "
            "compete and the graph/resource structure is dense. "
            "DEPENDENT prioritizes operations that unlock many immediate child "
            "operations in the dependency graph. "
            "SPT prioritizes shorter operations by minimum processing time. "
            "LPT prioritizes longer operations by minimum processing time. "
            "LEAST_FLEXIBLE prioritizes operations with fewer possible tools. "
            "SLACK_BASED prioritizes works with the smallest remaining time "
            "reserve: deadline minus estimated remaining work. "
            "Before answering, internally score each heuristic from 0 to 3 for "
            "expected final score on this task: 3 means likely best, 0 means "
            "likely worst. Consider slack, matrix density, graph edges, fine "
            "spread, operation count, and work count. Choose the heuristic with "
            "the highest internal score. Break ties by expected lower final "
            "penalty, not by name order. Return exactly one token from: DUMMY, "
            "DIRECTIVE, FINE, ROUND_ROBIN, DEPENDENT, SPT, LPT, "
            "LEAST_FLEXIBLE, SLACK_BASED. Do not output the scores. No "
            "explanations.";

        std::ofstream out(req);
        if (!out.is_open()) {
            return "REAL_MODE_REQ_WRITE_FAILED";
        }

        out << "{"
            << "\"model\":\"" << EscapeJson(cfg.model) << "\","
            << "\"temperature\":0,"
            << "\"messages\":["
            << "{\"role\":\"system\",\"content\":\""
            << EscapeJson(system_prompt) << "\"},"
            << "{\"role\":\"user\",\"content\":\""
            << EscapeJson(profile_text) << "\"}"
            << "]"
            << "}";
        out.close();

        const int timeout_sec = std::max(1, cfg.timeout_ms / 1000);

        std::ostringstream cmd;
        cmd << "curl -sS --max-time " << timeout_sec << " -X POST "
            << "\"" << cfg.endpoint << "\" "
            << "-H \"Authorization: Bearer " << cfg.api_key << "\" "
            << "-H \"Content-Type: application/json\" "
            << "--data @\"" << req.string() << "\" "
            << "> \"" << resp.string() << "\"";

        const int rc = std::system(cmd.str().c_str());
        std::string resp_body = ReadFile(resp);

        std::error_code ec;
        std::filesystem::remove(req, ec);
        std::filesystem::remove(resp, ec);

        if (rc != 0) {
            return "REAL_MODE_CURL_EXIT_" + std::to_string(rc) +
                   (resp_body.empty() ? "" : (";" + resp_body));
        }
        if (resp_body.empty()) {
            return "REAL_MODE_EMPTY_RESPONSE";
        }

        return ExtractContentField(resp_body);
    }

    static std::string MockLLMResponse(const TaskProfile& p, uint64_t seed) {
        // deterministic mock policy based on profile and seed parity
        if (p.mean_slack < 0.0) {
            return p.max_fine > p.mean_fine * 1.3 ? "FINE" : "SLACK_BASED";
        }
        if (p.matrix_density > 0.35 && p.n_edges > p.n_operations) {
            return "DEPENDENT";
        }
        if (p.single_tool_operation_ratio > 0.50) {
            return "LEAST_FLEXIBLE";
        }
        if (p.mean_time < 80.0 && p.n_operations > 50) {
            return "SPT";
        }
        if ((seed % 17) == 0) {
            return "DUMMY";
        }
        return "DIRECTIVE";
    }
};

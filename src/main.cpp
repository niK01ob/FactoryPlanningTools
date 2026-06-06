#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "generator_data_v2.h"
#include "llm_selector.h"
#include "scorer.h"
#include "solution_checker.h"
#include "solver.h"

namespace {
const char* ToName(Solver::HeuristicType h) {
    switch (h) {
        case Solver::HeuristicType::Dummy:
            return "dummy";
        case Solver::HeuristicType::Directive:
            return "directive";
        case Solver::HeuristicType::Fine:
            return "fine";
        case Solver::HeuristicType::RoundRobin:
            return "round_robin";
        case Solver::HeuristicType::Dependent:
            return "dependent";
        case Solver::HeuristicType::ShortestProcessingTime:
            return "spt";
        case Solver::HeuristicType::LongestProcessingTime:
            return "lpt";
        case Solver::HeuristicType::LeastFlexible:
            return "least_flexible";
        case Solver::HeuristicType::SlackBased:
            return "slack_based";
    }
    return "unknown";
}

const char* DifficultyToName(GeneratorDataV2::DifficultyPreset d) {
    switch (d) {
        case GeneratorDataV2::DifficultyPreset::Easy:
            return "easy";
        case GeneratorDataV2::DifficultyPreset::Medium:
            return "medium";
        case GeneratorDataV2::DifficultyPreset::Hard:
            return "hard";
    }
    return "unknown";
}

struct Metric {
    uint64_t success = 0;
    uint64_t fail = 0;
    double min_score = std::numeric_limits<double>::max();
    double max_score = 0.0;
};

struct RunResult {
    bool valid = false;
    double score = 0.0;
    long long runtime_ms = 0;
    std::string selected_heuristic;
    long long llm_latency_ms = 0;
    int llm_used_fallback = 0;
    std::string llm_raw_response;
};

struct MethodSpec {
    std::string name;
    bool is_llm = false;
    Solver::HeuristicType heuristic = Solver::HeuristicType::Dummy;
};

class MethodThreadPool {
public:
    explicit MethodThreadPool(size_t thread_count) {
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~MethodThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    MethodThreadPool(const MethodThreadPool&) = delete;
    MethodThreadPool& operator=(const MethodThreadPool&) = delete;

    std::future<RunResult> Submit(std::function<RunResult()> job) {
        auto task =
            std::make_shared<std::packaged_task<RunResult()>>(std::move(job));
        std::future<RunResult> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push([task]() { (*task)(); });
        }
        cv_.notify_one();

        return future;
    }

private:
    void WorkerLoop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !jobs_.empty(); });

                if (stop_ && jobs_.empty()) {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

std::string CsvQuote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

RunResult RunMethod(const ProblemData& source_data, const MethodSpec& method,
                    const LLMSelector::Config& llm_cfg, uint64_t seed) {
    RunResult result;
    result.selected_heuristic = method.name;
    result.llm_raw_response = "";

    try {
        ProblemData test_data(source_data);
        Solver::HeuristicType heuristic = method.heuristic;

        if (method.is_llm) {
            const auto decision = LLMSelector::Select(&source_data, llm_cfg, seed);
            heuristic = decision.selected;
            result.selected_heuristic = decision.selected_label;
            result.llm_latency_ms = decision.llm_latency_ms;
            result.llm_used_fallback = decision.used_fallback ? 1 : 0;
            result.llm_raw_response = decision.raw_response;
        }

        const auto t1 = std::chrono::steady_clock::now();
        Solver::Solve(&test_data, heuristic, seed);
        const auto t2 = std::chrono::steady_clock::now();
        result.runtime_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1)
                .count();

        SolutionChecker::Check(&test_data);
        result.valid = true;
        result.score = Scorer::CalculateScore(&test_data);
    } catch (const std::exception& e) {
        result.valid = false;
        if (result.llm_raw_response.empty()) {
            result.llm_raw_response = std::string("ERROR: ") + e.what();
        }
    } catch (...) {
        result.valid = false;
        if (result.llm_raw_response.empty()) {
            result.llm_raw_response = "ERROR: unknown exception";
        }
    }

    return result;
}

std::vector<RunResult> RunMethodsForTask(
    const ProblemData& source_data, const std::vector<MethodSpec>& methods,
    const LLMSelector::Config& llm_cfg, uint64_t seed, size_t method_threads,
    MethodThreadPool* method_pool) {
    std::vector<RunResult> results(methods.size());

    if (method_threads <= 1 || methods.size() <= 1 || method_pool == nullptr) {
        for (size_t i = 0; i < methods.size(); ++i) {
            results[i] = RunMethod(source_data, methods[i], llm_cfg, seed);
        }
        return results;
    }

    std::vector<std::future<RunResult>> futures;
    futures.reserve(methods.size());

    for (size_t i = 0; i < methods.size(); ++i) {
        futures.push_back(method_pool->Submit(
            [&source_data, &methods, &llm_cfg, seed, i]() {
                return RunMethod(source_data, methods[i], llm_cfg, seed);
            }));
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        results[i] = futures[i].get();
    }

    return results;
}
}  // namespace

int main(int argc, char** argv) {
    size_t task_count = 1000;
    size_t method_threads = 1;
    GeneratorDataV2::DifficultyPreset difficulty =
        GeneratorDataV2::DifficultyPreset::Easy;

    LLMSelector::Config llm_cfg = LLMSelector::ConfigFromEnv();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--llm=", 0) == 0) {
            llm_cfg.mode = a.substr(6);
        } else if (a.rfind("--tasks=", 0) == 0) {
            task_count = static_cast<size_t>(std::stoull(a.substr(8)));
        } else if (a.rfind("--difficulty=", 0) == 0) {
            const std::string d = a.substr(13);
            if (d == "easy") {
                difficulty = GeneratorDataV2::DifficultyPreset::Easy;
            } else if (d == "medium") {
                difficulty = GeneratorDataV2::DifficultyPreset::Medium;
            } else if (d == "hard") {
                difficulty = GeneratorDataV2::DifficultyPreset::Hard;
            }
        } else if (a.rfind("--method-threads=", 0) == 0) {
            method_threads = static_cast<size_t>(std::stoull(a.substr(17)));
            if (method_threads == 0) {
                method_threads = 1;
            }
        }
    }

    const std::string kLongCsvPath = "baseline_results_long.csv";
    const std::string kWideCsvPath = "baseline_results_wide.csv";
    const std::string kConfigPath = "experiment_config.yaml";

    const std::array<Solver::HeuristicType, 9> kHeuristics{
        Solver::HeuristicType::Dummy, Solver::HeuristicType::Directive,
        Solver::HeuristicType::Fine, Solver::HeuristicType::RoundRobin,
        Solver::HeuristicType::Dependent,
        Solver::HeuristicType::ShortestProcessingTime,
        Solver::HeuristicType::LongestProcessingTime,
        Solver::HeuristicType::LeastFlexible,
        Solver::HeuristicType::SlackBased};

    std::vector<MethodSpec> methods;
    for (auto h : kHeuristics) {
        methods.push_back(MethodSpec{ToName(h), false, h});
    }
    if (LLMSelector::IsEnabled(llm_cfg)) {
        methods.push_back(MethodSpec{"llm", true, Solver::HeuristicType::Directive});
    }

    method_threads = std::min(method_threads, methods.size());
    std::unique_ptr<MethodThreadPool> method_pool;
    if (method_threads > 1) {
        method_pool = std::make_unique<MethodThreadPool>(method_threads);
    }

    GeneratorDataV2 generator(difficulty);
    std::map<std::string, Metric> metrics;

    std::ofstream csv_long(kLongCsvPath);
    if (!csv_long.is_open()) {
        std::cerr << "Cannot open output file: " << kLongCsvPath << std::endl;
        return 1;
    }
    csv_long << "task_id,seed,method,valid,score,runtime_ms,selected_heuristic,llm_latency_ms,llm_used_fallback,llm_raw_response\n";

    std::ofstream csv_wide(kWideCsvPath);
    if (!csv_wide.is_open()) {
        std::cerr << "Cannot open output file: " << kWideCsvPath << std::endl;
        return 1;
    }
    csv_wide << "task_id,seed";
    for (const auto& m : methods) {
        csv_wide << "," << m.name << "_valid"
                 << "," << m.name << "_score"
                 << "," << m.name << "_runtime_ms"
                 << "," << m.name << "_selected_heuristic"
                 << "," << m.name << "_llm_latency_ms"
                 << "," << m.name << "_llm_used_fallback";
    }
    csv_wide << "\n";

    const uint64_t base_seed = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    std::ofstream yaml(kConfigPath);
    if (yaml.is_open()) {
        yaml << "experiment:\n";
        yaml << "  task_count: " << task_count << "\n";
        yaml << "  difficulty: " << DifficultyToName(difficulty) << "\n";
        yaml << "  base_seed: " << base_seed << "\n";
        yaml << "  llm_mode: " << llm_cfg.mode << "\n";
        yaml << "  llm_model: " << llm_cfg.model << "\n";
        yaml << "  llm_timeout_ms: " << llm_cfg.timeout_ms << "\n";
        yaml << "  llm_endpoint: " << llm_cfg.endpoint << "\n";
        yaml << "  method_threads: " << method_threads << "\n";
        yaml << "  long_results: " << kLongCsvPath << "\n";
        yaml << "  wide_results: " << kWideCsvPath << "\n";
        yaml << "  methods:\n";
        for (const auto& m : methods) {
            yaml << "    - " << m.name << "\n";
        }
        yaml.close();
    }

    for (size_t task_id = 0; task_id < task_count; ++task_id) {
        const uint64_t seed = base_seed + task_id * 7919;
        generator.Generate(seed);
        ProblemData source_data((*generator.GetData()));

        const auto task_results =
            RunMethodsForTask(source_data, methods, llm_cfg, seed, method_threads,
                              method_pool.get());

        for (size_t i = 0; i < methods.size(); ++i) {
            const auto& method = methods[i];
            const RunResult& result = task_results[i];

            if (result.valid) {
                metrics[method.name].success++;
                metrics[method.name].min_score =
                    std::min(metrics[method.name].min_score, result.score);
                metrics[method.name].max_score =
                    std::max(metrics[method.name].max_score, result.score);
            } else {
                metrics[method.name].fail++;
            }

            csv_long << task_id << "," << seed << "," << method.name << ","
                     << (result.valid ? 1 : 0) << "," << result.score << ","
                     << result.runtime_ms << "," << result.selected_heuristic
                     << "," << result.llm_latency_ms << ","
                     << result.llm_used_fallback << ","
                     << CsvQuote(result.llm_raw_response) << "\n";
        }

        csv_wide << task_id << "," << seed;
        for (size_t i = 0; i < methods.size(); ++i) {
            const RunResult& r = task_results[i];
            csv_wide << "," << (r.valid ? 1 : 0) << "," << r.score << ","
                     << r.runtime_ms << "," << r.selected_heuristic << ","
                     << r.llm_latency_ms << "," << r.llm_used_fallback;
        }
        csv_wide << "\n";
    }

    csv_long.close();
    csv_wide.close();

    std::cout << "Saved long baseline results to: " << kLongCsvPath << "\n";
    std::cout << "Saved wide baseline results to: " << kWideCsvPath << "\n";
    std::cout << "Saved experiment config to: " << kConfigPath << "\n";

    for (const auto& m : methods) {
        const Metric& mm = metrics[m.name];
        std::cout << "[" << m.name << "] success=" << mm.success
                  << " fail=" << mm.fail;
        if (mm.success > 0) {
            std::cout << " min_score=" << mm.min_score
                      << " max_score=" << mm.max_score;
        }
        std::cout << "\n";
    }

    return 0;
}

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
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
        case GeneratorDataV2::DifficultyPreset::SmallEasy:
            return "small_easy";
        case GeneratorDataV2::DifficultyPreset::SmallMedium:
            return "small_medium";
        case GeneratorDataV2::DifficultyPreset::SmallHard:
            return "small_hard";
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

std::string JsonQuote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
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
                break;
        }
    }
    out += "\"";
    return out;
}

std::string FormatTaskId(size_t task_id) {
    std::ostringstream out;
    out << "task_" << std::setw(6) << std::setfill('0') << task_id;
    return out.str();
}

void WriteTaskDataV1(const ProblemData& data, std::ostream& out) {
    out << "TASK_DATA_V1\n";
    out << "TOOLS " << data.tools.size() << "\n";
    for (size_t tool_id = 0; tool_id < data.tools.size(); ++tool_id) {
        const auto& schedule = data.tools[tool_id].GetSchedule();
        out << tool_id << " " << schedule.size();
        for (const auto& interval : schedule) {
            out << " " << interval.start << " " << interval.end;
        }
        out << "\n";
    }

    out << "OPERATIONS " << data.operations.size() << "\n";
    for (size_t op_id = 0; op_id < data.operations.size(); ++op_id) {
        const auto& op = data.operations[op_id];
        out << op_id << " " << (op.stoppable ? 1 : 0) << " "
            << op.previous_op_id.size();
        for (size_t parent_id : op.previous_op_id) {
            out << " " << parent_id;
        }
        out << " " << op.possible_tools.size();
        for (size_t tool_id : op.possible_tools) {
            out << " " << tool_id;
        }
        out << "\n";
    }

    out << "TIMES " << data.times_matrix.size() << " "
        << (data.times_matrix.empty() ? 0 : data.times_matrix.front().size())
        << "\n";
    for (const auto& row : data.times_matrix) {
        for (size_t tool_id = 0; tool_id < row.size(); ++tool_id) {
            if (tool_id > 0) {
                out << " ";
            }
            out << row[tool_id];
        }
        out << "\n";
    }

    out << "WORKS " << data.works.size() << "\n";
    for (size_t work_id = 0; work_id < data.works.size(); ++work_id) {
        const auto& work = data.works[work_id];
        out << work_id << " " << work.start_time << " " << work.directive
            << " " << work.fine_coef << " " << work.operation_ids.size();
        for (size_t op_id : work.operation_ids) {
            out << " " << op_id;
        }
        out << "\n";
    }
    out << "END\n";
}

std::string TaskProfileCsvHeader() {
    return "task_id,seed,n_tools,n_operations,n_works,stoppable_ratio,"
           "matrix_density,mean_time,max_time,time_cv,time_range_ratio,"
           "avg_tools_per_operation,min_tools_per_operation,"
           "single_tool_operation_ratio,resource_load_ratio,n_edges,"
           "graph_density,avg_in_degree,max_in_degree,avg_out_degree,"
           "max_out_degree,avg_ops_per_work,max_ops_per_work,ops_per_work_std,"
           "mean_slack,min_slack,max_slack,std_slack,tight_work_ratio,"
           "min_fine,mean_fine,max_fine,std_fine,fine_cv,fine_range_ratio\n";
}

void WriteTaskProfileCsvRow(size_t task_id, uint64_t seed, const TaskProfile& p,
                            std::ostream& out) {
    out << task_id << "," << seed << "," << p.n_tools << ","
        << p.n_operations << "," << p.n_works << "," << p.stoppable_ratio
        << "," << p.matrix_density << "," << p.mean_time << ","
        << p.max_time << "," << p.time_cv << "," << p.time_range_ratio
        << "," << p.avg_tools_per_operation << ","
        << p.min_tools_per_operation << "," << p.single_tool_operation_ratio
        << "," << p.resource_load_ratio << "," << p.n_edges << ","
        << p.graph_density << "," << p.avg_in_degree << ","
        << p.max_in_degree << "," << p.avg_out_degree << ","
        << p.max_out_degree << "," << p.avg_ops_per_work << ","
        << p.max_ops_per_work << "," << p.ops_per_work_std << ","
        << p.mean_slack << "," << p.min_slack << "," << p.max_slack << ","
        << p.std_slack << "," << p.tight_work_ratio << "," << p.min_fine
        << "," << p.mean_fine << "," << p.max_fine << "," << p.std_fine
        << "," << p.fine_cv << "," << p.fine_range_ratio << "\n";
}

int GenerateTaskDataset(size_t task_count,
                        GeneratorDataV2::DifficultyPreset difficulty,
                        uint64_t base_seed,
                        const std::filesystem::path& out_dir) {
    std::filesystem::create_directories(out_dir / "tasks");
    std::filesystem::create_directories(out_dir / "profiles");
    std::filesystem::create_directories(out_dir / "prompts");

    std::ofstream profiles_csv(out_dir / "task_profiles.csv");
    if (!profiles_csv.is_open()) {
        std::cerr << "Cannot open task_profiles.csv in " << out_dir << "\n";
        return 1;
    }
    profiles_csv << TaskProfileCsvHeader();

    std::ofstream prompts_jsonl(out_dir / "prompts.jsonl");
    if (!prompts_jsonl.is_open()) {
        std::cerr << "Cannot open prompts.jsonl in " << out_dir << "\n";
        return 1;
    }

    std::ofstream manifest(out_dir / "dataset_config.yaml");
    if (!manifest.is_open()) {
        std::cerr << "Cannot open dataset_config.yaml in " << out_dir << "\n";
        return 1;
    }

    manifest << "dataset:\n";
    manifest << "  format: TASK_DATA_V1\n";
    manifest << "  task_count: " << task_count << "\n";
    manifest << "  difficulty: " << DifficultyToName(difficulty) << "\n";
    manifest << "  base_seed: " << base_seed << "\n";
    manifest << "  tasks_dir: tasks\n";
    manifest << "  profiles_dir: profiles\n";
    manifest << "  prompts_dir: prompts\n";
    manifest << "  profiles_csv: task_profiles.csv\n";
    manifest << "  prompts_jsonl: prompts.jsonl\n";

    GeneratorDataV2 generator(difficulty);
    const std::string system_prompt = LLMSelector::SystemPrompt();

    for (size_t task_id = 0; task_id < task_count; ++task_id) {
        const uint64_t seed = base_seed + task_id * 7919;
        generator.Generate(seed);
        const ProblemData source_data((*generator.GetData()));

        const TaskProfile profile = TaskProfileBuilder::Build(&source_data);
        const std::string profile_text =
            TaskProfileBuilder::ToTaskProfileV1Text(profile);
        const std::string task_stem = FormatTaskId(task_id);

        std::ofstream task_file(out_dir / "tasks" / (task_stem + ".task"));
        if (!task_file.is_open()) {
            std::cerr << "Cannot write task file for task_id=" << task_id << "\n";
            return 1;
        }
        WriteTaskDataV1(source_data, task_file);

        std::ofstream profile_file(out_dir / "profiles" / (task_stem + ".txt"));
        if (!profile_file.is_open()) {
            std::cerr << "Cannot write profile file for task_id=" << task_id
                      << "\n";
            return 1;
        }
        profile_file << profile_text;

        std::ofstream prompt_file(out_dir / "prompts" / (task_stem + ".json"));
        if (!prompt_file.is_open()) {
            std::cerr << "Cannot write prompt file for task_id=" << task_id
                      << "\n";
            return 1;
        }
        prompt_file << "{\n"
                    << "  \"task_id\": " << task_id << ",\n"
                    << "  \"seed\": " << seed << ",\n"
                    << "  \"messages\": [\n"
                    << "    {\"role\": \"system\", \"content\": "
                    << JsonQuote(system_prompt) << "},\n"
                    << "    {\"role\": \"user\", \"content\": "
                    << JsonQuote(profile_text) << "}\n"
                    << "  ]\n"
                    << "}\n";

        prompts_jsonl << "{\"task_id\":" << task_id << ",\"seed\":" << seed
                      << ",\"messages\":[{\"role\":\"system\",\"content\":"
                      << JsonQuote(system_prompt)
                      << "},{\"role\":\"user\",\"content\":"
                      << JsonQuote(profile_text) << "}]}\n";

        WriteTaskProfileCsvRow(task_id, seed, profile, profiles_csv);
    }

    std::cout << "Generated task dataset in: " << out_dir << "\n";
    std::cout << "Saved tasks to: " << (out_dir / "tasks") << "\n";
    std::cout << "Saved profiles to: " << (out_dir / "profiles") << "\n";
    std::cout << "Saved prompts to: " << (out_dir / "prompts") << "\n";
    std::cout << "Saved prompts JSONL to: " << (out_dir / "prompts.jsonl")
              << "\n";
    std::cout << "Saved profile CSV to: " << (out_dir / "task_profiles.csv")
              << "\n";
    std::cout << "Saved dataset config to: " << (out_dir / "dataset_config.yaml")
              << "\n";
    return 0;
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
    bool generate_only = false;
    uint64_t base_seed = 0;
    std::filesystem::path out_dir = "generated_tasks";
    GeneratorDataV2::DifficultyPreset difficulty =
        GeneratorDataV2::DifficultyPreset::SmallEasy;

    LLMSelector::Config llm_cfg = LLMSelector::ConfigFromEnv();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--llm=", 0) == 0) {
            llm_cfg.mode = a.substr(6);
        } else if (a == "--generate-only") {
            generate_only = true;
        } else if (a.rfind("--tasks=", 0) == 0) {
            task_count = static_cast<size_t>(std::stoull(a.substr(8)));
        } else if (a.rfind("--base-seed=", 0) == 0) {
            base_seed = static_cast<uint64_t>(std::stoull(a.substr(12)));
        } else if (a.rfind("--out-dir=", 0) == 0) {
            out_dir = a.substr(10);
        } else if (a.rfind("--difficulty=", 0) == 0) {
            const std::string d = a.substr(13);
            if (d == "small_easy") {
                difficulty = GeneratorDataV2::DifficultyPreset::SmallEasy;
            } else if (d == "small_medium") {
                difficulty = GeneratorDataV2::DifficultyPreset::SmallMedium;
            } else if (d == "small_hard") {
                difficulty = GeneratorDataV2::DifficultyPreset::SmallHard;
            } else if (d == "easy") {
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

    if (base_seed == 0) {
        base_seed = static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
    }

    if (generate_only) {
        return GenerateTaskDataset(task_count, difficulty, base_seed, out_dir);
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

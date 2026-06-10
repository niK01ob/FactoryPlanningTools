#include <array>
#include <algorithm>
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
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "generator_data.h"
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

const char* DifficultyToName(GeneratorData::DifficultyPreset d) {
    switch (d) {
        case GeneratorData::DifficultyPreset::SmallEasy:
            return "small_easy";
        case GeneratorData::DifficultyPreset::SmallMedium:
            return "small_medium";
        case GeneratorData::DifficultyPreset::SmallHard:
            return "small_hard";
        case GeneratorData::DifficultyPreset::Easy:
            return "easy";
        case GeneratorData::DifficultyPreset::Medium:
            return "medium";
        case GeneratorData::DifficultyPreset::Hard:
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
    std::shared_ptr<ProblemData> solution;
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

std::string SafeFileName(const std::string& value) {
    std::string out;
    for (char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

std::vector<size_t> BuildOperationToWorkMap(const ProblemData& data) {
    std::vector<size_t> op_to_work(
        data.operations.size(), std::numeric_limits<size_t>::max());
    for (size_t work_id = 0; work_id < data.works.size(); ++work_id) {
        for (size_t op_id : data.works[work_id].operation_ids) {
            if (op_id < op_to_work.size()) {
                op_to_work[op_id] = work_id;
            }
        }
    }
    return op_to_work;
}

uint64_t WorkCompletionTime(const ProblemData& data, const Work& work) {
    uint64_t completion = 0;
    for (size_t op_id : work.operation_ids) {
        if (op_id < data.operations.size()) {
            completion = std::max(completion, data.operations[op_id].end_time);
        }
    }
    return completion;
}

void WriteIdArray(std::ostream& out, const std::set<size_t>& ids) {
    out << "[";
    bool first = true;
    for (size_t id : ids) {
        if (!first) {
            out << ", ";
        }
        out << id;
        first = false;
    }
    out << "]";
}

void WriteSolutionJson(const ProblemData& data,
                       const RunResult& result,
                       size_t task_id,
                       uint64_t seed,
                       const std::string& method_name,
                       const std::string& task_source,
                       std::ostream& out) {
    const auto op_to_work = BuildOperationToWorkMap(data);

    out << "{\n";
    out << "  \"format\": \"SOLUTION_VISUALIZATION_V1\",\n";
    out << "  \"task_id\": " << task_id << ",\n";
    out << "  \"seed\": " << seed << ",\n";
    if (!task_source.empty()) {
        out << "  \"task_source\": " << JsonQuote(task_source) << ",\n";
    }
    out << "  \"method\": " << JsonQuote(method_name) << ",\n";
    out << "  \"selected_heuristic\": "
        << JsonQuote(result.selected_heuristic) << ",\n";
    out << "  \"valid\": " << (result.valid ? "true" : "false") << ",\n";
    out << "  \"score\": " << result.score << ",\n";
    out << "  \"runtime_ms\": " << result.runtime_ms << ",\n";
    out << "  \"llm_latency_ms\": " << result.llm_latency_ms << ",\n";
    out << "  \"llm_used_fallback\": " << result.llm_used_fallback << ",\n";

    out << "  \"tools\": [\n";
    for (size_t tool_id = 0; tool_id < data.tools.size(); ++tool_id) {
        const auto& tool = data.tools[tool_id];
        uint64_t busy_time = 0;
        for (const auto& interval : tool.GetWorkProcess()) {
            busy_time += interval.GetTimeSpan();
        }
        const uint64_t available_time = tool.GetTotalAvailableTime();
        const double utilization =
            available_time == 0
                ? 0.0
                : static_cast<double>(busy_time) /
                      static_cast<double>(available_time);

        out << "    {\n";
        out << "      \"tool_id\": " << tool_id << ",\n";
        out << "      \"available_time\": " << available_time << ",\n";
        out << "      \"busy_time\": " << busy_time << ",\n";
        out << "      \"utilization\": " << utilization << ",\n";
        out << "      \"schedule\": [";
        bool first_interval = true;
        for (const auto& interval : tool.GetSchedule()) {
            if (!first_interval) {
                out << ", ";
            }
            out << "[" << interval.start << ", " << interval.end << "]";
            first_interval = false;
        }
        out << "]\n";
        out << "    }" << (tool_id + 1 == data.tools.size() ? "\n" : ",\n");
    }
    out << "  ],\n";

    out << "  \"works\": [\n";
    for (size_t work_id = 0; work_id < data.works.size(); ++work_id) {
        const auto& work = data.works[work_id];
        const uint64_t completion = WorkCompletionTime(data, work);
        const uint64_t tardiness =
            completion > work.directive ? completion - work.directive : 0;
        const double penalty =
            static_cast<double>(tardiness) * work.fine_coef;

        out << "    {\n";
        out << "      \"work_id\": " << work_id << ",\n";
        out << "      \"start\": " << work.start_time << ",\n";
        out << "      \"directive\": " << work.directive << ",\n";
        out << "      \"fine\": " << work.fine_coef << ",\n";
        out << "      \"completion\": " << completion << ",\n";
        out << "      \"tardiness\": " << tardiness << ",\n";
        out << "      \"penalty\": " << penalty << ",\n";
        out << "      \"operations\": ";
        WriteIdArray(out, work.operation_ids);
        out << "\n";
        out << "    }" << (work_id + 1 == data.works.size() ? "\n" : ",\n");
    }
    out << "  ],\n";

    out << "  \"operations\": [\n";
    for (size_t op_id = 0; op_id < data.operations.size(); ++op_id) {
        const auto& op = data.operations[op_id];
        out << "    {\n";
        out << "      \"operation_id\": " << op_id << ",\n";
        out << "      \"work_id\": ";
        if (op_to_work[op_id] == std::numeric_limits<size_t>::max()) {
            out << "null";
        } else {
            out << op_to_work[op_id];
        }
        out << ",\n";
        out << "      \"start\": " << op.start_time << ",\n";
        out << "      \"end\": " << op.end_time << ",\n";
        out << "      \"stoppable\": "
            << (op.stoppable ? "true" : "false") << ",\n";
        out << "      \"parents\": ";
        WriteIdArray(out, op.previous_op_id);
        out << ",\n";
        out << "      \"possible_tools\": ";
        WriteIdArray(out, op.possible_tools);
        out << "\n";
        out << "    }" << (op_id + 1 == data.operations.size() ? "\n" : ",\n");
    }
    out << "  ],\n";

    out << "  \"assignments\": [\n";
    bool first_assignment = true;
    for (size_t tool_id = 0; tool_id < data.tools.size(); ++tool_id) {
        for (const auto& interval : data.tools[tool_id].GetWorkProcess()) {
            const size_t op_id = interval.operation;
            if (!first_assignment) {
                out << ",\n";
            }
            out << "    {\n";
            out << "      \"tool_id\": " << tool_id << ",\n";
            out << "      \"operation_id\": " << op_id << ",\n";
            out << "      \"work_id\": ";
            if (op_id >= op_to_work.size() ||
                op_to_work[op_id] == std::numeric_limits<size_t>::max()) {
                out << "null";
            } else {
                out << op_to_work[op_id];
            }
            out << ",\n";
            out << "      \"start\": " << interval.start << ",\n";
            out << "      \"end\": " << interval.end << "\n";
            out << "    }";
            first_assignment = false;
        }
    }
    out << "\n  ]\n";
    out << "}\n";
}

bool WriteSolutionJsonFile(const std::filesystem::path& out_dir,
                           const ProblemData& data,
                           const RunResult& result,
                           size_t task_id,
                           uint64_t seed,
                           const std::string& method_name,
                           const std::string& task_source) {
    std::filesystem::create_directories(out_dir);
    const std::filesystem::path path =
        out_dir / (FormatTaskId(task_id) + "_" + SafeFileName(method_name) +
                   ".json");
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "Cannot write solution JSON: " << path << "\n";
        return false;
    }
    WriteSolutionJson(data, result, task_id, seed, method_name, task_source,
                      out);
    return true;
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

void ExpectToken(std::istream& in, const std::string& expected) {
    std::string actual;
    if (!(in >> actual) || actual != expected) {
        throw std::runtime_error("Expected token '" + expected + "'");
    }
}

ProblemData ReadTaskDataV1(std::istream& in) {
    ExpectToken(in, "TASK_DATA_V1");

    ProblemData data;

    ExpectToken(in, "TOOLS");
    size_t tool_count = 0;
    in >> tool_count;
    if (!in) {
        throw std::runtime_error("Invalid TOOLS section");
    }
    data.tools.reserve(tool_count);
    for (size_t i = 0; i < tool_count; ++i) {
        size_t tool_id = 0;
        size_t interval_count = 0;
        in >> tool_id >> interval_count;
        if (!in || tool_id != i) {
            throw std::runtime_error("Invalid tool row");
        }
        std::set<Tool::TimeInterval> schedule;
        for (size_t j = 0; j < interval_count; ++j) {
            uint64_t start = 0;
            uint64_t end = 0;
            in >> start >> end;
            if (!in || end < start) {
                throw std::runtime_error("Invalid tool interval");
            }
            schedule.insert(Tool::TimeInterval(start, end));
        }
        data.tools.push_back(Tool(schedule));
    }

    ExpectToken(in, "OPERATIONS");
    size_t operation_count = 0;
    in >> operation_count;
    if (!in) {
        throw std::runtime_error("Invalid OPERATIONS section");
    }
    data.operations.reserve(operation_count);
    for (size_t i = 0; i < operation_count; ++i) {
        size_t op_id = 0;
        int stoppable = 0;
        size_t parent_count = 0;
        in >> op_id >> stoppable >> parent_count;
        if (!in || op_id != i) {
            throw std::runtime_error("Invalid operation row");
        }
        std::set<size_t> parents;
        for (size_t j = 0; j < parent_count; ++j) {
            size_t parent_id = 0;
            in >> parent_id;
            if (!in || parent_id >= operation_count) {
                throw std::runtime_error("Invalid operation parent id");
            }
            parents.insert(parent_id);
        }
        size_t possible_tool_count = 0;
        in >> possible_tool_count;
        if (!in || possible_tool_count == 0) {
            throw std::runtime_error("Invalid operation tool count");
        }
        std::set<size_t> possible_tools;
        for (size_t j = 0; j < possible_tool_count; ++j) {
            size_t tool_id = 0;
            in >> tool_id;
            if (!in || tool_id >= tool_count) {
                throw std::runtime_error("Invalid operation tool id");
            }
            possible_tools.insert(tool_id);
        }
        data.operations.push_back(
            Operation(stoppable != 0, parents, possible_tools));
    }

    ExpectToken(in, "TIMES");
    size_t time_rows = 0;
    size_t time_cols = 0;
    in >> time_rows >> time_cols;
    if (!in || time_rows != operation_count || time_cols != tool_count) {
        throw std::runtime_error("Invalid TIMES dimensions");
    }
    data.times_matrix.assign(time_rows, std::vector<uint64_t>(time_cols, 0));
    for (size_t row = 0; row < time_rows; ++row) {
        for (size_t col = 0; col < time_cols; ++col) {
            in >> data.times_matrix[row][col];
            if (!in) {
                throw std::runtime_error("Invalid TIMES value");
            }
        }
    }

    ExpectToken(in, "WORKS");
    size_t work_count = 0;
    in >> work_count;
    if (!in) {
        throw std::runtime_error("Invalid WORKS section");
    }
    data.works.reserve(work_count);
    for (size_t i = 0; i < work_count; ++i) {
        size_t work_id = 0;
        uint64_t start_time = 0;
        uint64_t directive = 0;
        double fine = 0.0;
        size_t op_count = 0;
        in >> work_id >> start_time >> directive >> fine >> op_count;
        if (!in || work_id != i || op_count == 0) {
            throw std::runtime_error("Invalid work row");
        }
        std::set<size_t> operation_ids;
        for (size_t j = 0; j < op_count; ++j) {
            size_t op_id = 0;
            in >> op_id;
            if (!in || op_id >= operation_count) {
                throw std::runtime_error("Invalid work operation id");
            }
            operation_ids.insert(op_id);
        }
        data.works.push_back(Work(start_time, directive, fine, operation_ids));
    }

    ExpectToken(in, "END");
    return data;
}

ProblemData ReadTaskDataV1File(const std::filesystem::path& task_file) {
    std::ifstream in(task_file);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open task file: " + task_file.string());
    }
    return ReadTaskDataV1(in);
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
                        GeneratorData::DifficultyPreset difficulty,
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

    GeneratorData generator(difficulty);
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
                    const LLMSelector::Config& llm_cfg, uint64_t seed,
                    bool export_solution_json) {
    RunResult result;
    result.selected_heuristic = method.name;
    result.llm_raw_response = "";
    ProblemData test_data(source_data);

    try {
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

        result.score = Scorer::CalculateScore(&test_data);
        SolutionChecker::Check(&test_data);
        result.valid = true;
    } catch (const std::exception& e) {
        result.valid = false;
        result.score = Scorer::CalculateScore(&test_data);
        if (result.llm_raw_response.empty()) {
            result.llm_raw_response = std::string("ERROR: ") + e.what();
        }
    } catch (...) {
        result.valid = false;
        result.score = Scorer::CalculateScore(&test_data);
        if (result.llm_raw_response.empty()) {
            result.llm_raw_response = "ERROR: unknown exception";
        }
    }

    if (export_solution_json) {
        result.solution = std::make_shared<ProblemData>(test_data);
    }

    return result;
}

RunResult RunLlmOnly(const ProblemData& source_data,
                     const LLMSelector::Config& llm_cfg, uint64_t seed) {
    RunResult result;
    result.valid = true;

    try {
        const auto decision = LLMSelector::Select(&source_data, llm_cfg, seed);
        result.selected_heuristic = decision.selected_label;
        result.llm_latency_ms = decision.llm_latency_ms;
        result.llm_used_fallback = decision.used_fallback ? 1 : 0;
        result.llm_raw_response = decision.raw_response;
    } catch (const std::exception& e) {
        result.valid = false;
        result.llm_used_fallback = 1;
        result.llm_raw_response = std::string("ERROR: ") + e.what();
    } catch (...) {
        result.valid = false;
        result.llm_used_fallback = 1;
        result.llm_raw_response = "ERROR: unknown exception";
    }

    return result;
}

std::vector<RunResult> RunMethodsForTask(
    const ProblemData& source_data, const std::vector<MethodSpec>& methods,
    const LLMSelector::Config& llm_cfg, uint64_t seed, size_t method_threads,
    MethodThreadPool* method_pool, bool export_solution_json) {
    std::vector<RunResult> results(methods.size());

    if (method_threads <= 1 || methods.size() <= 1 || method_pool == nullptr) {
        for (size_t i = 0; i < methods.size(); ++i) {
            results[i] = RunMethod(source_data, methods[i], llm_cfg, seed,
                                   export_solution_json);
        }
        return results;
    }

    std::vector<std::future<RunResult>> futures;
    futures.reserve(methods.size());

    for (size_t i = 0; i < methods.size(); ++i) {
        futures.push_back(method_pool->Submit(
            [&source_data, &methods, &llm_cfg, seed, i,
             export_solution_json]() {
                return RunMethod(source_data, methods[i], llm_cfg, seed,
                                 export_solution_json);
            }));
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        results[i] = futures[i].get();
    }

    return results;
}

std::vector<std::filesystem::path> CollectTaskFiles(
    const std::filesystem::path& task_file,
    const std::filesystem::path& tasks_dir,
    size_t max_task_count) {
    std::vector<std::filesystem::path> files;
    if (!task_file.empty()) {
        files.push_back(task_file);
        return files;
    }

    if (tasks_dir.empty()) {
        return files;
    }

    if (!std::filesystem::exists(tasks_dir) ||
        !std::filesystem::is_directory(tasks_dir)) {
        throw std::runtime_error("Tasks directory does not exist: " +
                                 tasks_dir.string());
    }

    for (const auto& entry : std::filesystem::directory_iterator(tasks_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".task") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (max_task_count > 0 && files.size() > max_task_count) {
        files.resize(max_task_count);
    }
    return files;
}

void WriteExperimentConfig(const std::string& config_path,
                           size_t task_count,
                           const std::string& source,
                           const std::string& source_path,
                           const std::string& difficulty,
                           uint64_t base_seed,
                           const LLMSelector::Config& llm_cfg,
                           size_t method_threads,
                           const std::string& long_results,
                           const std::string& wide_results,
                           bool export_solution_json,
                           const std::string& solution_json_dir,
                           const std::vector<MethodSpec>& methods) {
    std::ofstream yaml(config_path);
    if (!yaml.is_open()) {
        return;
    }
    yaml << "experiment:\n";
    yaml << "  task_count: " << task_count << "\n";
    yaml << "  source: " << source << "\n";
    if (!source_path.empty()) {
        yaml << "  source_path: " << source_path << "\n";
    }
    if (!difficulty.empty()) {
        yaml << "  difficulty: " << difficulty << "\n";
    }
    yaml << "  base_seed: " << base_seed << "\n";
    yaml << "  llm_mode: " << llm_cfg.mode << "\n";
    yaml << "  llm_model: " << llm_cfg.model << "\n";
    yaml << "  llm_timeout_ms: " << llm_cfg.timeout_ms << "\n";
    yaml << "  llm_endpoint: " << llm_cfg.endpoint << "\n";
    yaml << "  method_threads: " << method_threads << "\n";
    yaml << "  long_results: " << long_results << "\n";
    yaml << "  wide_results: " << wide_results << "\n";
    yaml << "  export_solution_json: "
         << (export_solution_json ? "true" : "false") << "\n";
    if (export_solution_json) {
        yaml << "  solution_json_dir: " << solution_json_dir << "\n";
    }
    yaml << "  methods:\n";
    for (const auto& m : methods) {
        yaml << "    - " << m.name << "\n";
    }
}

void WriteLlmOnlyConfig(const std::string& config_path,
                        size_t task_count,
                        const std::string& source,
                        const std::string& source_path,
                        const std::string& difficulty,
                        uint64_t base_seed,
                        const LLMSelector::Config& llm_cfg,
                        const std::string& output_csv) {
    std::ofstream yaml(config_path);
    if (!yaml.is_open()) {
        return;
    }
    yaml << "experiment:\n";
    yaml << "  mode: llm_only\n";
    yaml << "  task_count: " << task_count << "\n";
    yaml << "  source: " << source << "\n";
    if (!source_path.empty()) {
        yaml << "  source_path: " << source_path << "\n";
    }
    if (!difficulty.empty()) {
        yaml << "  difficulty: " << difficulty << "\n";
    }
    yaml << "  base_seed: " << base_seed << "\n";
    yaml << "  llm_mode: " << llm_cfg.mode << "\n";
    yaml << "  llm_model: " << llm_cfg.model << "\n";
    yaml << "  llm_timeout_ms: " << llm_cfg.timeout_ms << "\n";
    yaml << "  llm_endpoint: " << llm_cfg.endpoint << "\n";
    yaml << "  output_csv: " << output_csv << "\n";
    yaml << "  solver_enabled: false\n";
    yaml << "  solution_json_enabled: false\n";
}

int RunLlmOnlyTaskFiles(const std::vector<std::filesystem::path>& task_files,
                        const LLMSelector::Config& llm_cfg,
                        uint64_t base_seed,
                        const std::string& output_csv_path,
                        const std::string& config_path,
                        const std::string& source,
                        const std::string& source_path) {
    if (task_files.empty()) {
        std::cerr << "No .task files found for LLM-only run\n";
        return 1;
    }

    std::ofstream csv(output_csv_path);
    if (!csv.is_open()) {
        std::cerr << "Cannot open output file: " << output_csv_path
                  << std::endl;
        return 1;
    }

    csv << "task_id,seed,task_source,valid,selected_heuristic,"
        << "llm_latency_ms,llm_used_fallback,llm_raw_response\n";

    size_t success = 0;
    size_t fail = 0;
    size_t fallback = 0;

    WriteLlmOnlyConfig(config_path, task_files.size(), source, source_path, "",
                       base_seed, llm_cfg, output_csv_path);

    for (size_t task_id = 0; task_id < task_files.size(); ++task_id) {
        const uint64_t seed = base_seed + task_id * 7919;
        const auto& path = task_files[task_id];
        ProblemData source_data = ReadTaskDataV1File(path);
        const RunResult result = RunLlmOnly(source_data, llm_cfg, seed);

        success += result.valid ? 1 : 0;
        fail += result.valid ? 0 : 1;
        fallback += result.llm_used_fallback ? 1 : 0;

        csv << task_id << "," << seed << "," << CsvQuote(path.string())
            << "," << (result.valid ? 1 : 0) << ","
            << result.selected_heuristic << "," << result.llm_latency_ms
            << "," << result.llm_used_fallback << ","
            << CsvQuote(result.llm_raw_response) << "\n";
    }

    std::cout << "Loaded task files: " << task_files.size() << "\n";
    std::cout << "Saved LLM-only responses to: " << output_csv_path << "\n";
    std::cout << "Saved experiment config to: " << config_path << "\n";
    std::cout << "[llm_only] success=" << success << " fail=" << fail
              << " fallback=" << fallback << "\n";
    return 0;
}

int RunLlmOnlyGeneratedTasks(size_t task_count,
                             GeneratorData::DifficultyPreset difficulty,
                             uint64_t base_seed,
                             const LLMSelector::Config& llm_cfg,
                             const std::string& output_csv_path,
                             const std::string& config_path) {
    std::ofstream csv(output_csv_path);
    if (!csv.is_open()) {
        std::cerr << "Cannot open output file: " << output_csv_path
                  << std::endl;
        return 1;
    }

    csv << "task_id,seed,task_source,valid,selected_heuristic,"
        << "llm_latency_ms,llm_used_fallback,llm_raw_response\n";

    size_t success = 0;
    size_t fail = 0;
    size_t fallback = 0;
    GeneratorData generator(difficulty);

    WriteLlmOnlyConfig(config_path, task_count, "generated", "",
                       DifficultyToName(difficulty), base_seed, llm_cfg,
                       output_csv_path);

    for (size_t task_id = 0; task_id < task_count; ++task_id) {
        const uint64_t seed = base_seed + task_id * 7919;
        generator.Generate(seed);
        ProblemData source_data((*generator.GetData()));
        const RunResult result = RunLlmOnly(source_data, llm_cfg, seed);

        success += result.valid ? 1 : 0;
        fail += result.valid ? 0 : 1;
        fallback += result.llm_used_fallback ? 1 : 0;

        csv << task_id << "," << seed << ",generated,"
            << (result.valid ? 1 : 0) << "," << result.selected_heuristic
            << "," << result.llm_latency_ms << ","
            << result.llm_used_fallback << ","
            << CsvQuote(result.llm_raw_response) << "\n";
    }

    std::cout << "Saved LLM-only responses to: " << output_csv_path << "\n";
    std::cout << "Saved experiment config to: " << config_path << "\n";
    std::cout << "[llm_only] success=" << success << " fail=" << fail
              << " fallback=" << fallback << "\n";
    return 0;
}

int RunTaskFiles(const std::vector<std::filesystem::path>& task_files,
                 const std::vector<MethodSpec>& methods,
                 const LLMSelector::Config& llm_cfg,
                 uint64_t base_seed,
                 size_t method_threads,
                 MethodThreadPool* method_pool,
                 const std::string& long_csv_path,
                 const std::string& wide_csv_path,
                 const std::string& config_path,
                 const std::string& source,
                 const std::string& source_path,
                 bool export_solution_json,
                 const std::filesystem::path& solution_json_dir) {
    if (task_files.empty()) {
        std::cerr << "No .task files found for dataset run\n";
        return 1;
    }

    std::map<std::string, Metric> metrics;
    std::ofstream csv_long(long_csv_path);
    if (!csv_long.is_open()) {
        std::cerr << "Cannot open output file: " << long_csv_path << std::endl;
        return 1;
    }
    csv_long << "task_id,seed,task_source,method,valid,score,runtime_ms,"
             << "selected_heuristic,llm_latency_ms,llm_used_fallback,"
             << "llm_raw_response\n";

    std::ofstream csv_wide(wide_csv_path);
    if (!csv_wide.is_open()) {
        std::cerr << "Cannot open output file: " << wide_csv_path << std::endl;
        return 1;
    }
    csv_wide << "task_id,seed,task_source";
    for (const auto& m : methods) {
        csv_wide << "," << m.name << "_valid"
                 << "," << m.name << "_score"
                 << "," << m.name << "_runtime_ms"
                 << "," << m.name << "_selected_heuristic"
                 << "," << m.name << "_llm_latency_ms"
                 << "," << m.name << "_llm_used_fallback";
    }
    csv_wide << "\n";

    WriteExperimentConfig(config_path, task_files.size(), source, source_path,
                          "", base_seed, llm_cfg, method_threads,
                          long_csv_path, wide_csv_path, export_solution_json,
                          solution_json_dir.string(), methods);

    for (size_t task_id = 0; task_id < task_files.size(); ++task_id) {
        const uint64_t seed = base_seed + task_id * 7919;
        const auto& path = task_files[task_id];
        ProblemData source_data = ReadTaskDataV1File(path);

        const auto task_results =
            RunMethodsForTask(source_data, methods, llm_cfg, seed,
                              method_threads, method_pool,
                              export_solution_json);

        const std::string task_source = path.string();
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

            csv_long << task_id << "," << seed << ","
                     << CsvQuote(task_source) << "," << method.name << ","
                     << (result.valid ? 1 : 0) << "," << result.score << ","
                     << result.runtime_ms << "," << result.selected_heuristic
                     << "," << result.llm_latency_ms << ","
                     << result.llm_used_fallback << ","
                     << CsvQuote(result.llm_raw_response) << "\n";

            if (export_solution_json && result.solution) {
                WriteSolutionJsonFile(solution_json_dir, *result.solution,
                                      result, task_id, seed, method.name,
                                      task_source);
            }
        }

        csv_wide << task_id << "," << seed << "," << CsvQuote(task_source);
        for (size_t i = 0; i < methods.size(); ++i) {
            const RunResult& r = task_results[i];
            csv_wide << "," << (r.valid ? 1 : 0) << "," << r.score << ","
                     << r.runtime_ms << "," << r.selected_heuristic << ","
                     << r.llm_latency_ms << "," << r.llm_used_fallback;
        }
        csv_wide << "\n";
    }

    std::cout << "Loaded task files: " << task_files.size() << "\n";
    std::cout << "Saved long baseline results to: " << long_csv_path << "\n";
    std::cout << "Saved wide baseline results to: " << wide_csv_path << "\n";
    std::cout << "Saved experiment config to: " << config_path << "\n";
    if (export_solution_json) {
        std::cout << "Saved solution JSON files to: " << solution_json_dir
                  << "\n";
    }

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
}  // namespace

int main(int argc, char** argv) {
    size_t task_count = 1000;
    size_t method_threads = 1;
    bool generate_only = false;
    bool llm_only = false;
    bool export_solution_json = false;
    bool task_count_explicit = false;
    uint64_t base_seed = 0;
    std::filesystem::path out_dir = "generated_tasks";
    std::filesystem::path solution_json_dir = "solution_json";
    std::filesystem::path task_file;
    std::filesystem::path tasks_dir;
    GeneratorData::DifficultyPreset difficulty =
        GeneratorData::DifficultyPreset::SmallEasy;

    LLMSelector::Config llm_cfg = LLMSelector::ConfigFromEnv();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--llm=", 0) == 0) {
            llm_cfg.mode = a.substr(6);
        } else if (a == "--generate-only") {
            generate_only = true;
        } else if (a == "--llm-only") {
            llm_only = true;
        } else if (a == "--export-solution-json") {
            export_solution_json = true;
        } else if (a.rfind("--solution-json-dir=", 0) == 0) {
            solution_json_dir = a.substr(20);
        } else if (a.rfind("--tasks=", 0) == 0) {
            task_count = static_cast<size_t>(std::stoull(a.substr(8)));
            task_count_explicit = true;
        } else if (a.rfind("--base-seed=", 0) == 0) {
            base_seed = static_cast<uint64_t>(std::stoull(a.substr(12)));
        } else if (a.rfind("--out-dir=", 0) == 0) {
            out_dir = a.substr(10);
        } else if (a.rfind("--task-file=", 0) == 0) {
            task_file = a.substr(12);
        } else if (a.rfind("--tasks-dir=", 0) == 0) {
            tasks_dir = a.substr(12);
        } else if (a.rfind("--difficulty=", 0) == 0) {
            const std::string d = a.substr(13);
            if (d == "small_easy") {
                difficulty = GeneratorData::DifficultyPreset::SmallEasy;
            } else if (d == "small_medium") {
                difficulty = GeneratorData::DifficultyPreset::SmallMedium;
            } else if (d == "small_hard") {
                difficulty = GeneratorData::DifficultyPreset::SmallHard;
            } else if (d == "easy") {
                difficulty = GeneratorData::DifficultyPreset::Easy;
            } else if (d == "medium") {
                difficulty = GeneratorData::DifficultyPreset::Medium;
            } else if (d == "hard") {
                difficulty = GeneratorData::DifficultyPreset::Hard;
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
    const std::string kLlmOnlyCsvPath = "llm_responses.csv";
    const std::string kConfigPath = "experiment_config.yaml";

    if (llm_only) {
        if (!LLMSelector::IsEnabled(llm_cfg)) {
            std::cerr << "--llm-only requires enabled LLM selector "
                      << "(use --llm=real, --llm=mock, or "
                      << "LLM_SELECTOR_MODE)\n";
            return 1;
        }
        if (export_solution_json) {
            std::cerr << "--export-solution-json is ignored in --llm-only "
                      << "mode because solver is not run\n";
        }

        if (!task_file.empty() || !tasks_dir.empty()) {
            try {
                const size_t max_files =
                    (!tasks_dir.empty() && task_count_explicit) ? task_count : 0;
                const auto task_files =
                    CollectTaskFiles(task_file, tasks_dir, max_files);
                const std::string source = !task_file.empty() ? "task_file"
                                                              : "tasks_dir";
                const std::string source_path =
                    (!task_file.empty() ? task_file : tasks_dir).string();
                return RunLlmOnlyTaskFiles(task_files, llm_cfg, base_seed,
                                           kLlmOnlyCsvPath, kConfigPath,
                                           source, source_path);
            } catch (const std::exception& e) {
                std::cerr << "LLM-only run failed: " << e.what() << "\n";
                return 1;
            }
        }

        return RunLlmOnlyGeneratedTasks(task_count, difficulty, base_seed,
                                        llm_cfg, kLlmOnlyCsvPath,
                                        kConfigPath);
    }

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

    if (!task_file.empty() || !tasks_dir.empty()) {
        try {
            const size_t max_files =
                (!tasks_dir.empty() && task_count_explicit) ? task_count : 0;
            const auto task_files =
                CollectTaskFiles(task_file, tasks_dir, max_files);
            const std::string source = !task_file.empty() ? "task_file"
                                                          : "tasks_dir";
            const std::string source_path =
                (!task_file.empty() ? task_file : tasks_dir).string();
            return RunTaskFiles(task_files, methods, llm_cfg, base_seed,
                                method_threads, method_pool.get(),
                                kLongCsvPath, kWideCsvPath, kConfigPath,
                                source, source_path, export_solution_json,
                                solution_json_dir);
        } catch (const std::exception& e) {
            std::cerr << "Dataset run failed: " << e.what() << "\n";
            return 1;
        }
    }

    GeneratorData generator(difficulty);
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
        yaml << "  export_solution_json: "
             << (export_solution_json ? "true" : "false") << "\n";
        if (export_solution_json) {
            yaml << "  solution_json_dir: " << solution_json_dir.string()
                 << "\n";
        }
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
                              method_pool.get(), export_solution_json);

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

            if (export_solution_json && result.solution) {
                WriteSolutionJsonFile(solution_json_dir, *result.solution,
                                      result, task_id, seed, method.name, "");
            }
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
    if (export_solution_json) {
        std::cout << "Saved solution JSON files to: " << solution_json_dir
                  << "\n";
    }

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

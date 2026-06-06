#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "problem_data.h"

struct TaskProfile {
    size_t n_tools = 0;
    size_t n_operations = 0;
    size_t n_works = 0;
    double stoppable_ratio = 0.0;
    double matrix_density = 0.0;
    double mean_time = 0.0;
    double max_time = 0.0;
    size_t n_edges = 0;
    double graph_density = 0.0;
    double avg_in_degree = 0.0;
    size_t max_in_degree = 0;
    double mean_fine = 0.0;
    double min_fine = 0.0;
    double max_fine = 0.0;
    double std_fine = 0.0;
    double fine_cv = 0.0;
    double fine_range_ratio = 0.0;
    double mean_slack = 0.0;
    double min_slack = 0.0;
    double max_slack = 0.0;
    double std_slack = 0.0;
    double tight_work_ratio = 0.0;
    double avg_ops_per_work = 0.0;
    size_t max_ops_per_work = 0;
    double ops_per_work_std = 0.0;
    double avg_tools_per_operation = 0.0;
    size_t min_tools_per_operation = 0;
    double single_tool_operation_ratio = 0.0;
};

class TaskProfileBuilder {
public:
    static TaskProfile Build(const ProblemData* data) {
        TaskProfile p;
        p.n_tools = data->tools.size();
        p.n_operations = data->operations.size();
        p.n_works = data->works.size();

        if (p.n_operations == 0 || p.n_tools == 0) {
            return p;
        }

        size_t stoppable = 0;
        for (const auto& op : data->operations) {
            if (op.stoppable) {
                ++stoppable;
            }
            p.n_edges += op.previous_op_id.size();
            p.max_in_degree = std::max(p.max_in_degree, op.previous_op_id.size());
        }
        p.stoppable_ratio = static_cast<double>(stoppable) / p.n_operations;
        p.avg_in_degree = static_cast<double>(p.n_edges) / p.n_operations;
        p.graph_density = p.n_operations > 1
                              ? static_cast<double>(p.n_edges) /
                                    (static_cast<double>(p.n_operations) *
                                     static_cast<double>(p.n_operations - 1))
                              : 0.0;

        size_t non_zero = 0;
        double sum_time = 0.0;
        for (const auto& row : data->times_matrix) {
            for (auto t : row) {
                if (t > 0) {
                    ++non_zero;
                    sum_time += static_cast<double>(t);
                    p.max_time = std::max(p.max_time, static_cast<double>(t));
                }
            }
        }
        const double total_cells = static_cast<double>(p.n_operations) * p.n_tools;
        p.matrix_density = total_cells > 0.0 ? static_cast<double>(non_zero) / total_cells : 0.0;
        p.mean_time = non_zero > 0 ? sum_time / static_cast<double>(non_zero) : 0.0;
        p.avg_tools_per_operation =
            static_cast<double>(non_zero) / static_cast<double>(p.n_operations);
        p.min_tools_per_operation = p.n_tools;
        size_t single_tool_operations = 0;
        for (const auto& op : data->operations) {
            const size_t tools_count = op.possible_tools.size();
            p.min_tools_per_operation = std::min(p.min_tools_per_operation, tools_count);
            if (tools_count == 1) {
                ++single_tool_operations;
            }
        }
        p.single_tool_operation_ratio =
            static_cast<double>(single_tool_operations) / p.n_operations;

        if (!data->works.empty()) {
            std::vector<double> fines;
            std::vector<double> slacks;
            std::vector<double> ops_per_work;
            fines.reserve(data->works.size());
            slacks.reserve(data->works.size());
            ops_per_work.reserve(data->works.size());
            for (const auto& w : data->works) {
                fines.push_back(w.fine_coef);
                ops_per_work.push_back(static_cast<double>(w.operation_ids.size()));
                p.max_ops_per_work =
                    std::max(p.max_ops_per_work, w.operation_ids.size());

                uint64_t min_possible = 0;
                for (auto op_id : w.operation_ids) {
                    uint64_t best = 0;
                    bool first = true;
                    for (auto tool_id : data->operations[op_id].possible_tools) {
                        const uint64_t t = data->times_matrix[op_id][tool_id];
                        if (t == 0) {
                            continue;
                        }
                        if (first || t < best) {
                            best = t;
                            first = false;
                        }
                    }
                    min_possible += best;
                }
                const double slack = static_cast<double>(w.directive) -
                                     static_cast<double>(w.start_time + min_possible);
                slacks.push_back(slack);
            }

            FillStats(fines, p.min_fine, p.mean_fine, p.max_fine, p.std_fine);
            p.fine_cv = p.mean_fine != 0.0 ? p.std_fine / p.mean_fine : 0.0;
            p.fine_range_ratio =
                p.min_fine > 0.0 ? p.max_fine / p.min_fine : 0.0;

            FillStats(slacks, p.min_slack, p.mean_slack, p.max_slack,
                      p.std_slack);
            size_t tight_count = 0;
            for (double slack : slacks) {
                if (slack <= 0.0) {
                    ++tight_count;
                }
            }
            p.tight_work_ratio =
                static_cast<double>(tight_count) / static_cast<double>(slacks.size());

            double min_ops = 0.0;
            double max_ops = 0.0;
            FillStats(ops_per_work, min_ops, p.avg_ops_per_work, max_ops,
                      p.ops_per_work_std);
        }

        return p;
    }

    static std::string ToTaskProfileV1Text(const TaskProfile& p) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(4);
        out << "TASK_PROFILE_V1\n";
        out << "SIZE: tools=" << p.n_tools << ", operations=" << p.n_operations
            << ", works=" << p.n_works << "\n";
        out << "OPS: stoppable_ratio=" << p.stoppable_ratio << "\n";
        out << "TIMES: density=" << p.matrix_density << ", mean=" << p.mean_time
            << ", max=" << p.max_time << "\n";
        out << "RESOURCES: avg_tools_per_operation=" << p.avg_tools_per_operation
            << ", min_tools_per_operation=" << p.min_tools_per_operation
            << ", single_tool_operation_ratio="
            << p.single_tool_operation_ratio << "\n";
        out << "GRAPH: edges=" << p.n_edges
            << ", density=" << p.graph_density
            << ", avg_in_degree=" << p.avg_in_degree
            << ", max_in_degree=" << p.max_in_degree << "\n";
        out << "WORKS: avg_ops_per_work=" << p.avg_ops_per_work
            << ", max_ops_per_work=" << p.max_ops_per_work
            << ", ops_per_work_std=" << p.ops_per_work_std << "\n";
        out << "DEADLINES: mean_slack=" << p.mean_slack
            << ", min_slack=" << p.min_slack
            << ", max_slack=" << p.max_slack
            << ", std_slack=" << p.std_slack
            << ", tight_work_ratio=" << p.tight_work_ratio << "\n";
        out << "FINES: min=" << p.min_fine
            << ", mean=" << p.mean_fine
            << ", max=" << p.max_fine
            << ", std=" << p.std_fine
            << ", cv=" << p.fine_cv
            << ", range_ratio=" << p.fine_range_ratio << "\n";
        return out.str();
    }

private:
    static void FillStats(const std::vector<double>& values, double& min_value,
                          double& mean_value, double& max_value,
                          double& std_value) {
        if (values.empty()) {
            min_value = 0.0;
            mean_value = 0.0;
            max_value = 0.0;
            std_value = 0.0;
            return;
        }

        min_value = *std::min_element(values.begin(), values.end());
        max_value = *std::max_element(values.begin(), values.end());
        mean_value = std::accumulate(values.begin(), values.end(), 0.0) /
                     static_cast<double>(values.size());

        double variance = 0.0;
        for (double v : values) {
            const double d = v - mean_value;
            variance += d * d;
        }
        variance /= static_cast<double>(values.size());
        std_value = std::sqrt(variance);
    }
};

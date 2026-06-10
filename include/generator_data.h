#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "problem_data.h"

class GeneratorData {
public:
    enum class DifficultyPreset {
        SmallEasy,
        SmallMedium,
        SmallHard,
        Easy,
        Medium,
        Hard
    };

    struct Params {
        size_t min_tools = 6;
        size_t max_tools = 14;
        size_t min_intervals_per_tool = 14;
        size_t max_intervals_per_tool = 30;
        uint64_t min_gap = 20;
        uint64_t max_gap = 200;
        uint64_t min_interval = 80;
        uint64_t max_interval = 420;

        size_t min_works = 3;
        size_t max_works = 10;
        size_t min_ops_per_work = 4;
        size_t max_ops_per_work = 14;

        double edge_prob = 0.14;
        double stoppable_prob = 0.70;
        double extra_tool_prob = 0.10;
        double tight_deadline_share = 0.30;
        double medium_deadline_share = 0.45;
    };

    GeneratorData() : params_(Params()) {}
    explicit GeneratorData(Params params) : params_(params) {}
    explicit GeneratorData(DifficultyPreset preset) : params_(Params()) {
        ApplyPreset(preset);
    }

    void ApplyPreset(DifficultyPreset preset) {
        params_ = Params();
        if (preset == DifficultyPreset::SmallEasy) {
            params_.min_tools = 8;
            params_.max_tools = 16;
            params_.min_intervals_per_tool = 18;
            params_.max_intervals_per_tool = 34;
            params_.min_works = 3;
            params_.max_works = 8;
            params_.min_ops_per_work = 4;
            params_.max_ops_per_work = 12;
            params_.edge_prob = 0.10;
            params_.stoppable_prob = 0.78;
            params_.extra_tool_prob = 0.06;
            params_.tight_deadline_share = 0.15;
            params_.medium_deadline_share = 0.55;
            return;
        }

        if (preset == DifficultyPreset::SmallMedium) {
            params_.min_tools = 6;
            params_.max_tools = 14;
            params_.min_intervals_per_tool = 14;
            params_.max_intervals_per_tool = 30;
            params_.min_works = 3;
            params_.max_works = 10;
            params_.min_ops_per_work = 4;
            params_.max_ops_per_work = 14;
            params_.edge_prob = 0.14;
            params_.stoppable_prob = 0.70;
            params_.extra_tool_prob = 0.10;
            params_.tight_deadline_share = 0.30;
            params_.medium_deadline_share = 0.45;
            return;
        }

        if (preset == DifficultyPreset::SmallHard) {
            params_.min_tools = 7;
            params_.max_tools = 14;
            params_.min_intervals_per_tool = 16;
            params_.max_intervals_per_tool = 30;
            params_.min_works = 4;
            params_.max_works = 10;
            params_.min_ops_per_work = 5;
            params_.max_ops_per_work = 14;
            params_.edge_prob = 0.155;
            params_.stoppable_prob = 0.68;
            params_.extra_tool_prob = 0.12;
            params_.tight_deadline_share = 0.34;
            params_.medium_deadline_share = 0.44;
            return;
        }

        if (preset == DifficultyPreset::Easy) {
            params_.min_tools = 32;
            params_.max_tools = 45;
            params_.min_intervals_per_tool = 75;
            params_.max_intervals_per_tool = 115;
            params_.min_gap = 40;
            params_.max_gap = 160;
            params_.min_interval = 180;
            params_.max_interval = 620;
            params_.min_works = 32;
            params_.max_works = 52;
            params_.min_ops_per_work = 16;
            params_.max_ops_per_work = 30;
            params_.edge_prob = 0.10;
            params_.stoppable_prob = 0.90;
            params_.extra_tool_prob = 0.30;
            params_.tight_deadline_share = 0.14;
            params_.medium_deadline_share = 0.52;
            return;
        }

        if (preset == DifficultyPreset::Medium) {
            params_.min_tools = 46;
            params_.max_tools = 62;
            params_.min_intervals_per_tool = 105;
            params_.max_intervals_per_tool = 160;
            params_.min_gap = 35;
            params_.max_gap = 150;
            params_.min_interval = 200;
            params_.max_interval = 720;
            params_.min_works = 58;
            params_.max_works = 88;
            params_.min_ops_per_work = 24;
            params_.max_ops_per_work = 44;
            params_.edge_prob = 0.16;
            params_.stoppable_prob = 0.82;
            params_.extra_tool_prob = 0.22;
            params_.tight_deadline_share = 0.32;
            params_.medium_deadline_share = 0.43;
            return;
        }

        // Hard
        params_.min_tools = 70;
        params_.max_tools = 95;
        params_.min_intervals_per_tool = 165;
        params_.max_intervals_per_tool = 240;
        params_.min_gap = 30;
        params_.max_gap = 140;
        params_.min_interval = 200;
        params_.max_interval = 720;
        params_.min_works = 90;
        params_.max_works = 130;
        params_.min_ops_per_work = 30;
        params_.max_ops_per_work = 54;
        params_.edge_prob = 0.22;
        params_.stoppable_prob = 0.68;
        params_.extra_tool_prob = 0.18;
        params_.tight_deadline_share = 0.34;
        params_.medium_deadline_share = 0.40;
    }

    void Generate(uint64_t seed = 0) {
        if (seed == 0) {
            seed = std::random_device{}();
        }
        rng_.seed(seed);
        ClearAll_();

        GenerateTools_();
        GenerateWorksAndOperations_();
    }

    ProblemData* GetData() { return &data_; }

private:
    struct Slot {
        size_t tool_id = 0;
        uint64_t start = 0;
        uint64_t end = 0;
    };

    struct ToolState {
        std::vector<Slot> slots;
        size_t cursor = 0;
        uint64_t busy_until = 0;
    };

    Params params_;
    ProblemData data_;
    std::mt19937_64 rng_;
    std::vector<ToolState> tool_states_;

    void ClearAll_() {
        data_.operations.clear();
        data_.works.clear();
        data_.tools.clear();
        data_.times_matrix.clear();
        tool_states_.clear();
    }

    size_t RandSize_(size_t l, size_t r) {
        std::uniform_int_distribution<size_t> dist(l, r);
        return dist(rng_);
    }

    uint64_t RandU64_(uint64_t l, uint64_t r) {
        std::uniform_int_distribution<uint64_t> dist(l, r);
        return dist(rng_);
    }

    double Rand01_() {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng_);
    }

    void GenerateTools_() {
        const size_t tool_count = RandSize_(params_.min_tools, params_.max_tools);
        data_.tools.reserve(tool_count);
        tool_states_.resize(tool_count);

        for (size_t t = 0; t < tool_count; ++t) {
            const size_t interval_count = RandSize_(params_.min_intervals_per_tool,
                                                    params_.max_intervals_per_tool);
            uint64_t current = RandU64_(0, 120);
            std::set<Tool::TimeInterval> schedule;
            auto& state = tool_states_[t];
            state.slots.reserve(interval_count);

            for (size_t i = 0; i < interval_count; ++i) {
                current += RandU64_(params_.min_gap, params_.max_gap);
                const uint64_t span = RandU64_(params_.min_interval, params_.max_interval);
                const uint64_t end = current + span;
                schedule.insert(Tool::TimeInterval(current, end));
                state.slots.push_back({t, current, end});
                current = end;
            }
            data_.tools.push_back(Tool(schedule));
        }
    }

    // Picks a feasible slot for an operation and reserves tool time serially.
    bool ReserveOnTool_(size_t tool_id, uint64_t ready_time, uint64_t duration,
                        bool stoppable, uint64_t& out_start, uint64_t& out_end) {
        auto& ts = tool_states_[tool_id];
        while (ts.cursor < ts.slots.size()) {
            const Slot& s = ts.slots[ts.cursor];
            uint64_t start = std::max(ready_time, std::max(ts.busy_until, s.start));
            if (start >= s.end) {
                ++ts.cursor;
                continue;
            }

            if (!stoppable) {
                if (s.end - start >= duration) {
                    out_start = start;
                    out_end = start + duration;
                    ts.busy_until = out_end;
                    if (ts.busy_until >= s.end) {
                        ++ts.cursor;
                    }
                    return true;
                }
                ++ts.cursor;
                continue;
            }

            // Stoppable: we only need aggregated capacity in future slots.
            uint64_t need = duration;
            uint64_t probe_idx = ts.cursor;
            uint64_t probe_time = start;
            uint64_t end_time = start;
            while (probe_idx < ts.slots.size() && need > 0) {
                const Slot& p = ts.slots[probe_idx];
                uint64_t seg_start = std::max(probe_time, p.start);
                if (probe_idx == ts.cursor) {
                    seg_start = std::max(seg_start, std::max(ts.busy_until, s.start));
                }
                if (seg_start >= p.end) {
                    ++probe_idx;
                    probe_time = 0;
                    continue;
                }
                const uint64_t can_take = p.end - seg_start;
                const uint64_t take = std::min(can_take, need);
                need -= take;
                end_time = seg_start + take;
                ++probe_idx;
                probe_time = 0;
            }
            if (need == 0) {
                out_start = start;
                out_end = end_time;
                ts.busy_until = out_end;
                while (ts.cursor < ts.slots.size() &&
                       ts.busy_until >= ts.slots[ts.cursor].end) {
                    ++ts.cursor;
                }
                return true;
            }
            ++ts.cursor;
        }
        return false;
    }

    std::set<size_t> GenerateParents_(size_t local_idx, const std::vector<size_t>& global_ids) {
        std::set<size_t> parents;
        if (local_idx == 0) {
            return parents;
        }
        for (size_t p = 0; p < local_idx; ++p) {
            if (Rand01_() < params_.edge_prob) {
                parents.insert(global_ids[p]);
            }
        }
        // Ensure at least one incoming edge for part of nodes to avoid too many isolated roots.
        if (parents.empty() && local_idx > 0 && Rand01_() < 0.45) {
            parents.insert(global_ids[RandSize_(0, local_idx - 1)]);
        }
        return parents;
    }

    uint64_t EstimateReadyTime_(const std::set<size_t>& parents) const {
        uint64_t ready = 0;
        for (size_t p : parents) {
            ready = std::max(ready, data_.operations[p].end_time);
        }
        return ready;
    }

    uint64_t CalcDirective_(uint64_t start_time, uint64_t max_end) {
        const uint64_t span = (max_end > start_time) ? (max_end - start_time) : 1;
        const double r = Rand01_();
        double k = 1.0;
        if (r < params_.tight_deadline_share) {
            k = 1.03 + 0.12 * Rand01_();
        } else if (r < params_.tight_deadline_share + params_.medium_deadline_share) {
            k = 1.20 + 0.30 * Rand01_();
        } else {
            k = 1.60 + 0.80 * Rand01_();
        }
        return start_time + static_cast<uint64_t>(span * k);
    }

    void GenerateWorksAndOperations_() {
        const size_t work_count = RandSize_(params_.min_works, params_.max_works);
        const size_t tool_count = data_.tools.size();
        data_.works.reserve(work_count);

        for (size_t w = 0; w < work_count; ++w) {
            const size_t op_count = RandSize_(params_.min_ops_per_work,
                                              params_.max_ops_per_work);
            std::vector<size_t> work_global_ids;
            work_global_ids.reserve(op_count);

            for (size_t i = 0; i < op_count; ++i) {
                work_global_ids.push_back(data_.operations.size() + i);
            }

            uint64_t work_start = UINT64_MAX;
            uint64_t work_end = 0;
            std::set<size_t> work_ops;

            for (size_t i = 0; i < op_count; ++i) {
                const bool stoppable = Rand01_() < params_.stoppable_prob;
                auto parents = GenerateParents_(i, work_global_ids);
                const uint64_t ready_time = EstimateReadyTime_(parents);

                // Primary feasible tool
                size_t tool_id = RandSize_(0, tool_count - 1);
                const uint64_t duration = RandU64_(30, 260);

                uint64_t start = 0;
                uint64_t end = 0;
                bool ok = false;
                for (size_t tries = 0; tries < tool_count; ++tries) {
                    const size_t candidate = (tool_id + tries) % tool_count;
                    if (ReserveOnTool_(candidate, ready_time, duration, stoppable, start, end)) {
                        tool_id = candidate;
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    // Fallback: force simple chain-like placement on first tool from its tail.
                    tool_id = 0;
                    auto& ts = tool_states_[tool_id];
                    if (ts.slots.empty()) {
                        continue;
                    }
                    const Slot& last = ts.slots.back();
                    start = std::max(ready_time, std::max(ts.busy_until, last.start));
                    end = start + duration;
                    ts.busy_until = end;
                }

                std::set<size_t> possible_tools{tool_id};

                // Add extra possible tools for diversity (time values set by simple inflation).
                for (size_t t = 0; t < tool_count; ++t) {
                    if (t == tool_id) {
                        continue;
                    }
                    if (Rand01_() < params_.extra_tool_prob) {
                        possible_tools.insert(t);
                    }
                }

                data_.operations.push_back(Operation(stoppable, parents, possible_tools));
                Operation& op = data_.operations.back();
                op.start_time = start;
                op.end_time = end;

                work_start = std::min(work_start, start);
                work_end = std::max(work_end, end);
                work_ops.insert(data_.operations.size() - 1);
            }

            // Build times row for newly added operations.
            while (data_.times_matrix.size() < data_.operations.size()) {
                data_.times_matrix.push_back(std::vector<uint64_t>(tool_count, 0));
            }
            for (size_t op_id : work_ops) {
                const auto& op = data_.operations[op_id];
                const uint64_t base = std::max<uint64_t>(1, op.end_time - op.start_time);
                for (size_t t : op.possible_tools) {
                    if (t == 0 && op.possible_tools.size() == 1) {
                        // no-op, keep base
                    }
                    const double factor = (t == *op.possible_tools.begin())
                                              ? 1.0
                                              : (1.10 + 0.60 * Rand01_());
                    data_.times_matrix[op_id][t] =
                        static_cast<uint64_t>(std::max(1.0, base * factor));
                }
            }

            if (work_start == UINT64_MAX) {
                work_start = 0;
            }
            const uint64_t directive = CalcDirective_(work_start, work_end);
            const double fine = 20.0 + 280.0 * Rand01_();
            data_.works.push_back(Work(work_start, directive, fine, work_ops));
        }

        // Reset operation times: solver should produce its own plan during experiments.
        for (auto& op : data_.operations) {
            op.start_time = 0;
            op.end_time = 0;
        }
    }
};

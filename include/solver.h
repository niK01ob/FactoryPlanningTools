#pragma once

#include <algorithm>
#include <limits>

#include "problem_data.h"

// здесь будем решать задачу
class Solver {
public:
    enum class HeuristicType {
        Dummy,
        Directive,
        Fine,
        RoundRobin,
        Dependent,
        ShortestProcessingTime,
        LongestProcessingTime,
        LeastFlexible,
        SlackBased
    };

    struct SortContext {
        const ProblemData* data = nullptr;
        const std::vector<size_t>* op_to_work = nullptr;
        const std::vector<size_t>* children_count = nullptr;
        const std::vector<uint64_t>* remaining_work = nullptr;
        size_t* rr_next_work = nullptr;
    };

    using FrontSorter = void (*)(std::vector<size_t>& front,
                                 const SortContext& context);

    static void Solve(ProblemData* data,
                      HeuristicType heuristic = HeuristicType::Dummy,
                      uint64_t /*seed*/ = 0) {
        std::set<uint64_t> timestamps;
        FillStartTimes(data, timestamps);

        SolverState state = BuildSolverState(data);

        // основной цикл по событиям
        while (!timestamps.empty()) {
            const uint64_t current_time = PopNextTimestamp(timestamps);
            std::vector<size_t> front = BuildFront(data, current_time);

            SortFront(data, front, heuristic, state.op_to_work,
                      state.children_count, state.remaining_work,
                      state.rr_next_work);

            std::set<size_t> available_tools =
                BuildAvailableTools(data, front, current_time);
            AppointFrontOperations(data, front, available_tools, current_time,
                                   state, timestamps);
        }
    }

    static void SortFront(const ProblemData* data, std::vector<size_t>& front,
                          HeuristicType heuristic,
                          const std::vector<size_t>& op_to_work,
                          const std::vector<size_t>& children_count,
                          const std::vector<uint64_t>& remaining_work,
                          size_t& rr_next_work) {
        const SortContext context{data, &op_to_work, &children_count,
                                  &remaining_work, &rr_next_work};
        GetFrontSorter(heuristic)(front, context);
    }

private:
    struct SolverState {
        std::vector<size_t> op_to_work;
        std::vector<size_t> children_count;
        std::vector<uint64_t> min_processing_time;
        std::vector<uint64_t> remaining_work;
        size_t rr_next_work = 0;
    };

    static SolverState BuildSolverState(const ProblemData* data) {
        SolverState state;
        state.op_to_work = BuildOperationToWorkIndex(data);
        state.children_count = BuildChildrenCount(data);
        state.min_processing_time = BuildMinProcessingTimes(data);
        state.remaining_work =
            BuildRemainingWork(data, state.op_to_work,
                               state.min_processing_time);
        return state;
    }

    static uint64_t PopNextTimestamp(std::set<uint64_t>& timestamps) {
        const uint64_t current_time = *timestamps.begin();
        timestamps.erase(timestamps.begin());
        return current_time;
    }

    static std::vector<size_t> BuildFront(const ProblemData* data,
                                          uint64_t current_time) {
        std::vector<size_t> front;
        for (const auto& work : data->works) {
            for (auto op_idx : work.operation_ids) {
                if (data->operations[op_idx].end_time == 0 &&
                    work.CanBeAppointed(op_idx, current_time,
                                        data->operations)) {
                    front.push_back(op_idx);
                }
            }
        }
        return front;
    }

    static std::set<size_t> BuildAvailableTools(
        ProblemData* data, const std::vector<size_t>& front,
        uint64_t current_time) {
        std::set<size_t> available_tools;
        for (size_t oper : front) {
            for (size_t tool_id : data->operations[oper].possible_tools) {
                if (data->tools[tool_id].CanStartWork(
                        data->operations[oper], current_time,
                        data->times_matrix[oper][tool_id])) {
                    available_tools.insert(tool_id);
                }
            }
        }
        return available_tools;
    }

    static void AppointFrontOperations(
        ProblemData* data, const std::vector<size_t>& front,
        std::set<size_t>& available_tools, uint64_t current_time,
        SolverState& state, std::set<uint64_t>& timestamps) {
        for (size_t oper_in_front : front) {
            Operation& operation = data->operations[oper_in_front];
            for (size_t tool_id : available_tools) {
                if (operation.possible_tools.contains(tool_id) &&
                    data->tools[tool_id].CanStartWork(
                        data->operations[oper_in_front], current_time,
                        data->times_matrix[oper_in_front][tool_id])) {
                    AppointOperation(data, oper_in_front, tool_id, current_time,
                                     state, timestamps);
                    available_tools.erase(tool_id);
                    break;
                }
            }
        }
    }

    static void AppointOperation(ProblemData* data, size_t operation_id,
                                 size_t tool_id, uint64_t current_time,
                                 SolverState& state,
                                 std::set<uint64_t>& timestamps) {
        Operation& operation = data->operations[operation_id];
        data->tools[tool_id].Appoint(operation, operation_id, current_time,
                                     data->times_matrix[operation_id][tool_id]);

        const size_t work_idx = state.op_to_work[operation_id];
        if (work_idx < state.remaining_work.size()) {
            state.remaining_work[work_idx] =
                state.remaining_work[work_idx] >
                        state.min_processing_time[operation_id]
                    ? state.remaining_work[work_idx] -
                          state.min_processing_time[operation_id]
                    : 0;
        }
        timestamps.insert(operation.end_time);
    }

    static FrontSorter GetFrontSorter(HeuristicType heuristic) {
        switch (heuristic) {
            case HeuristicType::Directive:
                return &SortByDirective;
            case HeuristicType::Fine:
                return &SortByFine;
            case HeuristicType::RoundRobin:
                return &SortRoundRobin;
            case HeuristicType::Dependent:
                return &SortByDependent;
            case HeuristicType::ShortestProcessingTime:
                return &SortByShortestProcessingTime;
            case HeuristicType::LongestProcessingTime:
                return &SortByLongestProcessingTime;
            case HeuristicType::LeastFlexible:
                return &SortByLeastFlexible;
            case HeuristicType::SlackBased:
                return &SortBySlack;
            case HeuristicType::Dummy:
                return &SortDummy;
        }
        return &SortDummy;
    }

    static void SortDummy(std::vector<size_t>& /*front*/,
                          const SortContext& /*context*/) {}

    static size_t WorkOf(size_t op_id, const std::vector<size_t>& op_to_work) {
        return op_id < op_to_work.size()
                   ? op_to_work[op_id]
                   : std::numeric_limits<size_t>::max();
    }

    static void SortByDirective(std::vector<size_t>& front,
                                const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        const auto& op_to_work = *context.op_to_work;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const size_t wl = WorkOf(l, op_to_work);
            const size_t wr = WorkOf(r, op_to_work);
            const uint64_t dl = wl < data->works.size()
                                    ? data->works[wl].directive
                                    : std::numeric_limits<uint64_t>::max();
            const uint64_t dr = wr < data->works.size()
                                    ? data->works[wr].directive
                                    : std::numeric_limits<uint64_t>::max();
            if (dl != dr) {
                return dl < dr;
            }
            return l < r;
        });
    }

    static void SortByFine(std::vector<size_t>& front,
                           const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        const auto& op_to_work = *context.op_to_work;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const size_t wl = WorkOf(l, op_to_work);
            const size_t wr = WorkOf(r, op_to_work);
            const double fl = wl < data->works.size()
                                  ? data->works[wl].fine_coef
                                  : -1.0;
            const double fr = wr < data->works.size()
                                  ? data->works[wr].fine_coef
                                  : -1.0;
            if (fl != fr) {
                return fl > fr;
            }
            return l < r;
        });
    }

    static void SortRoundRobin(std::vector<size_t>& front,
                               const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        const auto& op_to_work = *context.op_to_work;
        const size_t works_count = data->works.size();
        if (works_count == 0) {
            return;
        }
        const size_t rr_next_work = *context.rr_next_work;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const size_t wl = WorkOf(l, op_to_work);
            const size_t wr = WorkOf(r, op_to_work);
            const size_t dl =
                wl >= works_count ? works_count
                                  : (wl + works_count - rr_next_work) %
                                        works_count;
            const size_t dr =
                wr >= works_count ? works_count
                                  : (wr + works_count - rr_next_work) %
                                        works_count;
            if (dl != dr) {
                return dl < dr;
            }
            return l < r;
        });
        *context.rr_next_work = (rr_next_work + 1) % works_count;
    }

    static void SortByDependent(std::vector<size_t>& front,
                                const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const auto& children_count = *context.children_count;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const size_t cl = l < children_count.size() ? children_count[l] : 0;
            const size_t cr = r < children_count.size() ? children_count[r] : 0;
            if (cl != cr) {
                return cl > cr;
            }
            return l < r;
        });
    }

    static void SortByShortestProcessingTime(std::vector<size_t>& front,
                                             const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const uint64_t tl = MinProcessingTime(data, l);
            const uint64_t tr = MinProcessingTime(data, r);
            if (tl != tr) {
                return tl < tr;
            }
            return l < r;
        });
    }

    static void SortByLongestProcessingTime(std::vector<size_t>& front,
                                            const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const uint64_t tl = MinProcessingTime(data, l);
            const uint64_t tr = MinProcessingTime(data, r);
            if (tl != tr) {
                return tl > tr;
            }
            return l < r;
        });
    }

    static void SortByLeastFlexible(std::vector<size_t>& front,
                                    const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const size_t fl = data->operations[l].possible_tools.size();
            const size_t fr = data->operations[r].possible_tools.size();
            if (fl != fr) {
                return fl < fr;
            }
            return l < r;
        });
    }

    static void SortBySlack(std::vector<size_t>& front,
                            const SortContext& context) {
        if (front.size() < 2) {
            return;
        }
        const ProblemData* data = context.data;
        const auto& op_to_work = *context.op_to_work;
        const auto& remaining_work = *context.remaining_work;
        std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
            const int64_t sl = CalcSlack(data, l, op_to_work, remaining_work);
            const int64_t sr = CalcSlack(data, r, op_to_work, remaining_work);
            if (sl != sr) {
                return sl < sr;
            }
            return l < r;
        });
    }

    static uint64_t MinProcessingTime(const ProblemData* data, size_t op_id) {
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t tool_id : data->operations[op_id].possible_tools) {
            const uint64_t t = data->times_matrix[op_id][tool_id];
            if (t > 0) {
                best = std::min(best, t);
            }
        }
        return best == std::numeric_limits<uint64_t>::max() ? 0 : best;
    }

    static std::vector<uint64_t> BuildMinProcessingTimes(
        const ProblemData* data) {
        std::vector<uint64_t> result(data->operations.size(), 0);
        for (size_t op_id = 0; op_id < data->operations.size(); ++op_id) {
            result[op_id] = MinProcessingTime(data, op_id);
        }
        return result;
    }

    static std::vector<uint64_t> BuildRemainingWork(
        const ProblemData* data, const std::vector<size_t>& op_to_work,
        const std::vector<uint64_t>& min_processing_time) {
        std::vector<uint64_t> result(data->works.size(), 0);
        for (size_t op_id = 0; op_id < op_to_work.size(); ++op_id) {
            const size_t work_idx = op_to_work[op_id];
            if (work_idx < result.size()) {
                result[work_idx] += min_processing_time[op_id];
            }
        }
        return result;
    }

    static int64_t CalcSlack(const ProblemData* data, size_t op_id,
                             const std::vector<size_t>& op_to_work,
                             const std::vector<uint64_t>& remaining_work) {
        if (op_id >= op_to_work.size()) {
            return std::numeric_limits<int64_t>::max();
        }
        const size_t work_idx = op_to_work[op_id];
        if (work_idx >= data->works.size() || work_idx >= remaining_work.size()) {
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(data->works[work_idx].directive) -
               static_cast<int64_t>(remaining_work[work_idx]);
    }

    static std::vector<size_t> BuildChildrenCount(const ProblemData* data) {
        std::vector<size_t> count(data->operations.size(), 0);
        for (const auto& op : data->operations) {
            for (size_t parent : op.previous_op_id) {
                if (parent < count.size()) {
                    ++count[parent];
                }
            }
        }
        return count;
    }

    static std::vector<size_t> BuildOperationToWorkIndex(
        const ProblemData* data) {
        std::vector<size_t> index(data->operations.size(),
                                  std::numeric_limits<size_t>::max());
        for (size_t work_idx = 0; work_idx < data->works.size(); ++work_idx) {
            for (size_t op_idx : data->works[work_idx].operation_ids) {
                if (op_idx < index.size()) {
                    index[op_idx] = work_idx;
                }
            }
        }
        return index;
    }

    static void FillStartTimes(const ProblemData* data,
                               std::set<uint64_t>& times) {
        for (const auto& work : data->works) {
            times.insert(work.start_time);
        }

        for (const auto& tool : data->tools) {
            tool.GetSheduleStarts(times);
        }
    }
};

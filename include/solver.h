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

    static void Solve(ProblemData* data,
                      HeuristicType heuristic = HeuristicType::Dummy,
                      uint64_t /*seed*/ = 0) {
        uint64_t current_time = 0;
        std::set<uint64_t> timestamps;
        FillStartTimes(data, timestamps);

        const std::vector<size_t> op_to_work = BuildOperationToWorkIndex(data);
        const std::vector<size_t> children_count = BuildChildrenCount(data);
        const std::vector<uint64_t> min_processing_time =
            BuildMinProcessingTimes(data);
        std::vector<uint64_t> remaining_work =
            BuildRemainingWork(data, op_to_work, min_processing_time);
        size_t rr_next_work = 0;

        // основной цикл по событиям
        while (!timestamps.empty()) {
            // множество доступных станков
            std::set<size_t> R;
            // фронт операций
            std::vector<size_t> F;
            current_time = *timestamps.begin();
            timestamps.erase(timestamps.begin());
            for (const auto& work : data->works) {
                for (auto op_idx : work.operation_ids) {
                    if (data->operations[op_idx].end_time == 0 &&
                        work.CanBeAppointed(op_idx, current_time,
                                            data->operations)) {
                        F.push_back(op_idx);
                    }
                }
            }

            SortFront(data, F, heuristic, op_to_work, children_count,
                      remaining_work, rr_next_work);
            // собираем множество возможных исполнителей для фронта
            for (size_t oper : F) {
                for (size_t i : data->operations[oper].possible_tools) {
                    if (data->tools[i].CanStartWork(data->operations[oper],
                                                    current_time,
                                                    data->times_matrix[oper][i])) {
                        R.insert(i);
                    }
                }
            }

            for (size_t oper_in_f : F) {
                Operation& cool_operation = data->operations[oper_in_f];
                for (size_t r : R) {
                    if (cool_operation.possible_tools.contains(r) &&
                        data->tools[r].CanStartWork(data->operations[oper_in_f],
                                                    current_time,
                                                    data->times_matrix[oper_in_f][r])) {
                        data->tools[r].Appoint(cool_operation, oper_in_f,
                                               current_time,
                                               data->times_matrix[oper_in_f][r]);
                        const size_t work_idx = op_to_work[oper_in_f];
                        if (work_idx < remaining_work.size()) {
                            remaining_work[work_idx] =
                                remaining_work[work_idx] > min_processing_time[oper_in_f]
                                    ? remaining_work[work_idx] -
                                          min_processing_time[oper_in_f]
                                    : 0;
                        }
                        timestamps.insert(cool_operation.end_time);
                        R.erase(r);
                        break;
                    }
                }
            }
        }
    }

    static void SortFront(const ProblemData* data, std::vector<size_t>& front,
                          HeuristicType heuristic,
                          const std::vector<size_t>& op_to_work,
                          const std::vector<size_t>& children_count,
                          const std::vector<uint64_t>& remaining_work,
                          size_t& rr_next_work) {
        if (front.size() < 2 || heuristic == HeuristicType::Dummy) {
            return;
        }

        const auto work_of = [&op_to_work](size_t op_id) {
            return op_id < op_to_work.size()
                       ? op_to_work[op_id]
                       : std::numeric_limits<size_t>::max();
        };

        if (heuristic == HeuristicType::Directive) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const size_t wl = work_of(l);
                const size_t wr = work_of(r);
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
            return;
        }

        if (heuristic == HeuristicType::Fine) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const size_t wl = work_of(l);
                const size_t wr = work_of(r);
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
            return;
        }

        if (heuristic == HeuristicType::RoundRobin) {
            const size_t works_count = data->works.size();
            if (works_count == 0) {
                return;
            }
            std::sort(front.begin(), front.end(),
                      [&](size_t l, size_t r) {
                          const size_t wl = work_of(l);
                          const size_t wr = work_of(r);
                          const size_t dl = wl >= works_count
                                                ? works_count
                                                : (wl + works_count - rr_next_work) %
                                                      works_count;
                          const size_t dr = wr >= works_count
                                                ? works_count
                                                : (wr + works_count - rr_next_work) %
                                                      works_count;
                          if (dl != dr) {
                              return dl < dr;
                          }
                          return l < r;
                      });
            rr_next_work = (rr_next_work + 1) % works_count;
            return;
        }

        if (heuristic == HeuristicType::Dependent) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const size_t cl =
                    l < children_count.size() ? children_count[l] : 0;
                const size_t cr =
                    r < children_count.size() ? children_count[r] : 0;
                if (cl != cr) {
                    return cl > cr;
                }
                return l < r;
            });
            return;
        }

        if (heuristic == HeuristicType::ShortestProcessingTime) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const uint64_t tl = MinProcessingTime(data, l);
                const uint64_t tr = MinProcessingTime(data, r);
                if (tl != tr) {
                    return tl < tr;
                }
                return l < r;
            });
            return;
        }

        if (heuristic == HeuristicType::LongestProcessingTime) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const uint64_t tl = MinProcessingTime(data, l);
                const uint64_t tr = MinProcessingTime(data, r);
                if (tl != tr) {
                    return tl > tr;
                }
                return l < r;
            });
            return;
        }

        if (heuristic == HeuristicType::LeastFlexible) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const size_t fl = data->operations[l].possible_tools.size();
                const size_t fr = data->operations[r].possible_tools.size();
                if (fl != fr) {
                    return fl < fr;
                }
                return l < r;
            });
            return;
        }

        if (heuristic == HeuristicType::SlackBased) {
            std::sort(front.begin(), front.end(), [&](size_t l, size_t r) {
                const int64_t sl = CalcSlack(data, l, op_to_work, remaining_work);
                const int64_t sr = CalcSlack(data, r, op_to_work, remaining_work);
                if (sl != sr) {
                    return sl < sr;
                }
                return l < r;
            });
        }
    }

private:
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

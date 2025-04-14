#pragma once

#include "problem_data.h"

// здесь будем решать задачу
class Solver {
public:
    static void Solve(ProblemData* data) {
        uint64_t current_time = 0;
        std::set<uint64_t> timestamps;
        FillStartTimes(data, timestamps);

        // основной цикл по событиям
        while (!timestamps.empty()) {
            // множество доступных станков
            std::set<size_t> R;
            // фронт операций
            std::vector<size_t> F;
            current_time = *timestamps.begin();
            timestamps.erase(timestamps.begin());
            for (auto& work : data->works) {
                for (auto& op_idx : work.operation_ids) {
                    if (data->operations[op_idx].end_time == 0 && work.CanBeAppointed(op_idx, current_time, data->operations)) {
                        F.push_back(op_idx);
                    }
                }
            }

            SortFront(data, F);
            // собираем множесво возможных исполнителей для фронта
            for (size_t oper : F) {
                for (size_t i : data->operations[oper].possible_tools) {
                    if (data->tools[i].CanStartWork(
                            data->operations[oper], current_time,
                            data->times_matrix[oper][i])) {
                        R.insert(i);
                    }
                }
            }

            for (size_t oper_in_f : F) {

                Operation& cool_operation = data->operations[oper_in_f];
                for (size_t r : R) {
                    if (cool_operation.possible_tools.contains(r) && data->tools[r].CanStartWork(data->operations[oper_in_f], current_time, data->times_matrix[oper_in_f][r])) {
                        data->tools[r].Appoint(
                            cool_operation, oper_in_f, current_time,
                            data->times_matrix[oper_in_f][r]);
                        timestamps.insert(cool_operation.end_time);
                        R.erase(r);
                        break;
                    }
                }
            }
        }
    }

    static void SortFront(const ProblemData* data, std::vector<size_t> front) {}

private:
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

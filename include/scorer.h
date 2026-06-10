#include <cstdint>
#include <iostream>

#include "problem_data.h"

class Scorer {
public:
    static constexpr double kNotAppointedOperationFine = 1e12;

    static double CalculateScore(const ProblemData* data) {
        double result = 0;
        for (const auto& work : data->works) {
            uint64_t last_end = 0;
            for (auto op_idx : work.operation_ids) {
                if (data->operations[op_idx].start_time == 0 ||
                    data->operations[op_idx].end_time == 0) {
                    result += kNotAppointedOperationFine;
                    continue;
                }
                last_end =
                    std::max(last_end, data->operations[op_idx].end_time);
            }
            if (last_end > work.directive) {
                result += (last_end - work.directive) * work.fine_coef;
            }
        }

        return result;
    }
};

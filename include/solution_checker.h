#pragma once
#include <exception>
#include <string>

#include "problem_data.h"

// Cписок необходимых проверок
// 1 - Коллизии в расписании
// 2 - Все операции назначены
// 3 - Ребенок не назначен раньше родителя
// 4 - Операция выполнялась ровно столько сколько написано в матрице времён
// 5 - Операция выполнена на своём станке
// 6 - Непрерываемая опреация опреация выполнилась в один временной промежуток

class SolutionChecker {
public:
    static void Check(const ProblemData* data);

private:
    static bool IsAppointed(const Operation& operation);
    static void CheckAncestors(const Operation& operation,
                               const ProblemData* data);
    static uint64_t GetCheckTime(size_t id_op, const ProblemData* data);
    static void CheckPossibleTool(const Operation& op, size_t tool_id,
                                  size_t op_id);
};

void SolutionChecker::Check(const ProblemData* data) {
    // проверка графиков Ганта
    for (const auto& tool : data->tools) {
        tool.CheckCollisions();
    }

    // все ли операции назначены
    // правильный порядок выполнения
    for (const auto& operation : data->operations) {
        CheckAncestors(operation, data);
    }

    // проверка времени выполнения
    // проверка возможности выполнения
    // проверка на выполнение непрерываемой операции в 1 заход
    for (size_t i = 0; i < data->operations.size(); ++i) {
        const auto& operation = data->operations[i];
        if (!IsAppointed(operation)) {
            continue;
        }
        uint64_t time = GetCheckTime(i, data);
        if (!operation.stoppable &&
            operation.end_time - operation.start_time != time) {
            throw std::runtime_error("Вы прерывали непрерываемую опреацию #" +
                                     std::to_string(i));
        }
    }
}

bool SolutionChecker::IsAppointed(const Operation& operation) {
    return operation.start_time != 0 && operation.end_time != 0;
}

void SolutionChecker::CheckAncestors(const Operation& operation,
                                     const ProblemData* data) {
    if (!IsAppointed(operation)) {
        return;
    }
    for (size_t prev : operation.previous_op_id) {
        const auto& prev_op = data->operations[prev];
        if (!IsAppointed(prev_op)) {
            throw std::runtime_error("Previous operation #" +
                                     std::to_string(prev) +
                                     " is not appointed");
        }
        if (prev_op.end_time > operation.start_time) {
            throw std::runtime_error("Предудыщая операция #" +
                                     std::to_string(prev) +
                                     " ещё не выполнена");
        }
    }
}

uint64_t SolutionChecker::GetCheckTime(size_t id_op, const ProblemData* data) {
    uint64_t time = 0;
    for (size_t i = 0; i < data->tools.size(); ++i) {
        const auto& tool = data->tools[i];
        if (tool.OperationDoneHere(id_op)) {
            time = tool.GetWorkingTime(id_op);
            if (time != data->times_matrix[id_op][i]) {
                throw std::runtime_error(
                    "Время выполнения операции не совпадает с заданным: " +
                    std::to_string(time) +
                    "!=" + std::to_string(data->times_matrix[id_op][i]));
            }

            // сразу проверим возможность выполнения на этом станке
            CheckPossibleTool(data->operations[id_op], i, id_op);
        }
    }

    return time;
}

void SolutionChecker::CheckPossibleTool(const Operation& op, size_t tool_id,
                                        size_t op_id) {
    if (!op.possible_tools.contains(tool_id)) {
        throw std::runtime_error("Операция #" + std::to_string(op_id) +
                                 " не может быть выполнена на станке #" +
                                 std::to_string(tool_id));
    }
}

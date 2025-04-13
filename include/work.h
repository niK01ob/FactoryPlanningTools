#pragma once

#include <set>
#include <vector>

// Хранит инфу о том какие операции надо назначить, чтобы выполнить работу
struct Operation {
    const bool stoppable;
    const std::set<size_t> previous_op_id;
    const std::set<size_t> possible_tools;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    Operation(bool stoppable, const std::set<size_t>& previous_op_id,
              const std::set<size_t>& possible_tools)
        : stoppable(stoppable),
          previous_op_id(previous_op_id),
          possible_tools(possible_tools){};

    void CheckAppointment() const {
        if (start_time == 0 || end_time == 0) {
            throw std::runtime_error("Операция не назначена");
        }
    }

   /*Operation& operator= (const Operation& oper) {
        this->start_time = oper.start_time;
        this->end_time = oper.end_time;
        this->stoppable = oper.stoppable;
        this->previous_op_id = oper.previous_op_id;
        this->possible_tools = oper.possible_tools;
    }*/
};

// такая структура Work должна быть круче
class Work {
public:
    const uint64_t start_time;
    const uint64_t directive;
    const double fine_coef;
    const std::set<size_t> operation_ids;

    Work(uint64_t start_time, uint64_t directive, double fine_coef,
         const std::set<size_t>& operation_ids,
         const std::vector<Operation>& ref_to_all_opers)
        : start_time(start_time),
          directive(directive),
          fine_coef(fine_coef),
          operation_ids(operation_ids),
          ref_to_all_opers_(ref_to_all_opers) {}

    bool CanBeAppointed(size_t oper_id, uint64_t timestamp) const{
        if (!operation_ids.contains(oper_id) || timestamp < start_time) {
            return false;
        }

        const auto& oper = ref_to_all_opers_[oper_id];
        for (auto prev : oper.previous_op_id) {
            if (ref_to_all_opers_[prev].end_time == 0 ||
                ref_to_all_opers_[prev].end_time > timestamp) {
                return false;
            }
        }

        return oper.start_time == 0;
    }

private:
    const std::vector<Operation>& ref_to_all_opers_;
};
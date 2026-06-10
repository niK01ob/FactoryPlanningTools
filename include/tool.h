#pragma once

#include <set>
#include <vector>

#include "work.h"

class Tool {
public:
    struct TimeInterval {
        TimeInterval(uint64_t start, uint64_t end) : start(start), end(end) {}
        TimeInterval(const TimeInterval& other)
            : start(other.start), end(other.end) {}

        uint64_t GetTimeSpan() const { return end - start; }

        uint64_t GetTimeSpan(uint64_t timestamp) const {
            if (end >= timestamp) {
                return end - timestamp;
            }

            return 0;
        }

        const uint64_t start;
        const uint64_t end;
        // проверяет интервал на пересечение с другим
        bool Intersects(const TimeInterval& other) const {
            return !(end <= other.start || other.end < start);
        }

        bool operator<(const TimeInterval& other) const {
            return start < other.start;
        }

        bool operator==(const TimeInterval& other) const {
            return start <= other.start && other.end <= end;
        }

        bool operator!=(const TimeInterval& other) const {
            return !(this->operator==(other));
        }
    };

    struct NamedTimeInterval : TimeInterval {
        NamedTimeInterval(uint64_t start, uint64_t end, uint32_t operation)
            : TimeInterval(start, end), operation(operation) {}
        const uint32_t operation;
    };

    Tool(const std::set<TimeInterval>& shedule) : shedule_(shedule) {}
    Tool(std::initializer_list<TimeInterval>& shedule) : shedule_(shedule) {}

    Tool& operator=(const Tool& other) {
        shedule_ = other.shedule_;
        work_process_ = other.work_process_;

        return *this;
    }

    // true
    bool CanStartWork(const Operation& operation, uint64_t timestamp,
                      uint64_t timespan) {
        auto it = GetStartIterator(timestamp);
        if (it == shedule_.end()) {
            return false;
        }

        TimeInterval interval{timestamp, timestamp};
        // мы можем попробовать начать выполнение операции, но
        // надо убедиться, что в рабочем расписании есть место.
        // проверим не занят ли исполнитель в это время
        for (auto& gant : work_process_) {
            if (gant.Intersects(interval)) {
                return false;
            }
        }

        if (operation.stoppable) {
            uint64_t time = 0;
            while (it != shedule_.end() && time < timespan) {
                time += it->GetTimeSpan(timestamp);
                it = std::next(it);
                timestamp = it->start;
            }

            return time >= timespan;
        } else {
            return it->GetTimeSpan(timestamp) >= timespan;
        }
    }

    // Положим в именованное расписание исполнителя выполнение операции
    void Appoint(Operation& operation, uint32_t id, uint64_t timestamp,
                 uint64_t timespan) {
        if (work_process_.contains(
                NamedTimeInterval(timestamp, timestamp, id))) {
            throw std::runtime_error("Collision!");
        }
        uint64_t time = 0;
        auto it = GetStartIterator(timestamp);
        operation.start_time = timestamp;
        while (it != shedule_.end() && time < timespan) {
            work_process_.insert(
                {timestamp, std::min(it->end, timestamp + timespan - time),
                 id});
            time += it->GetTimeSpan(timestamp);
            it = std::next(it);
            timestamp = it->start;
        }
        operation.end_time = std::prev(work_process_.end())->end;
    }

    const std::set<TimeInterval>& GetSchedule() const { return shedule_; }

    void PrintShedule(std::ostream& out_stream) const {
        for (auto inter : shedule_) {
            out_stream << "(" << inter.start << ", " << inter.end << ") ";
        }
    }

    void PrintGant(std::ostream& out_stream) {
        for (auto inter : work_process_) {
            out_stream << "(" << inter.start << ", " << inter.end << ", "
                      << inter.operation << ") ";
        }
    }

    const std::set<NamedTimeInterval>& GetWorkProcess() const {
        return work_process_;
    }

    bool OperationDoneHere(size_t op_id)  const {
        for (const auto& gant : work_process_) {
            if (gant.operation == op_id) {
                return true;
            }
        }

        return false;
    }

    uint64_t GetWorkingTime(size_t id_op) const{
        uint64_t total_time = 0;
        for (const auto& gant : work_process_) {
            if (gant.operation == id_op) {
                total_time += gant.GetTimeSpan();
            }
        }
        return total_time;
    }

    uint64_t GetTotalAvailableTime() const {
        uint64_t total_time = 0;
        for (const auto& shed : shedule_) {
            total_time += shed.GetTimeSpan();
        }
        return total_time;
    }

    // проверяем что все интервалы из work_process внутри shedule_
    // и work_process не имеет коллизий
    void CheckCollisions() const {
        if (work_process_.empty()) {
            return;
        }
        for (auto& work_proc : work_process_) {
            bool checkIntersects = false;
            for (auto& shed : shedule_) {
                if (work_proc.Intersects(shed)) {
                    checkIntersects = true;
                    if (shed != work_proc) {
                        throw std::runtime_error(
                            "График Ганта не соответствует расписанию");
                    }
                }
            }

            if (!checkIntersects) {
                throw std::runtime_error(
                    "Интервал из графика Ганта не принадлежит расписанию");
            }
        }

        for (auto it = work_process_.begin();
             it != std::prev(work_process_.end()); ++it) {
            if (it->Intersects(*std::next(it))) {
                throw std::runtime_error("Коллизия в графике Ганта");
            }
        }
    }
    
    void GetSheduleStarts(std::set<uint64_t>& timestamps) const {
        for (const auto& shed : shedule_) {
            timestamps.insert(shed.start);
        }
    }

    void DestroyWorkProcess() { work_process_.insert({1000, 1002, 0}); }

private:
    std::set<Tool::TimeInterval>::const_iterator GetStartIterator(
        uint64_t timestamp) {
        if(shedule_.empty()) {
            return shedule_.end();
        }
        TimeInterval interval{timestamp, timestamp};
        auto it = shedule_.lower_bound(interval);

        if (it == shedule_.begin() && it->start > timestamp) {
            return shedule_.end();
        }

        if (it == shedule_.end() || it->start > timestamp) {
            it = std::prev(it);
        }

        if (timestamp >= it->start && timestamp < it->end) {
            return it;
        }

        return shedule_.end();
    }

    std::set<TimeInterval> shedule_;  // изначальное расписание
    std::set<NamedTimeInterval> work_process_;  // расписание в которое будем
                                                // класть назначенную операцию
};

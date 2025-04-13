#pragma once
#include <fstream>
#include <string>

#include "tool.h"
#include "work.h"

class ProblemData {
public:
    // здесь будем парсить данные из файла
    ProblemData(){}
    ProblemData(std::ifstream& input);
    ProblemData(ProblemData& data) {
        this->operations = data.operations;
        this->works = data.works;
        this->times_matrix = data.times_matrix;
        this->tools = data.tools;
    };
    /*ProblemData(const ProblemData& other)
        : works(other.works),
          operations(other.operations),
          tools(other.tools),
          times_matrix(other.times_matrix) {}
    ProblemData(ProblemData&& other)
        : works(std::move(other.works)),
          operations(std::move(other.operations)),
          tools(std::move(other.tools)),
          times_matrix(std::move(other.times_matrix)) {}

    ProblemData& operator=(const ProblemData& other) {
        this->works = other.works;
        this->operations = other.operations;
        this->tools = other.tools;
        this->times_matrix = other.times_matrix;
        return *this;
    }*/
    // все работы (графы операций)
    std::vector<Work> works;
    // все операции необходимые для выполнения всех работ
    std::vector<Operation> operations;
    // все исполнители (ресурсы)
    std::vector<Tool> tools;
    // матрица времён выполнения i-ой Operation на j-м ресурсе
    std::vector<std::vector<uint64_t>> times_matrix;

protected:
    // нужно только на время парсинга
    std::vector<bool> oper_stoppabillity_;
    std::vector<uint64_t> work_starts_;
    std::vector<uint64_t> work_direct_;
    std::vector<double> work_fines_;
    std::vector<std::set<size_t>> work_operations_;
    std::vector<std::set<size_t>> oper_previous_;
    std::vector<std::set<size_t>> oper_tools_;
    size_t idx_work_ = 0;
    void FillTools(std::ifstream& input);
    void ParseTools(std::string& str, int idTool);
    void FillStoppable(std::ifstream& input);
    void fillOperations(std::ifstream& input);
    void fillTimes(std::ifstream& input);
    void fillGraph(std::ifstream& input);
    void fillStart(std::ifstream& input);
    void fillDirective(std::ifstream& input);
    void fillFines(std::ifstream& input);
    void fillWork(std::ifstream& input);
};

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
ProblemData::ProblemData(std::ifstream& input) {
    // секция чтения божественного формата из файла
    std::string str;
    while (!input.eof()) {
        std::getline(input, str);

        if (str == "TOOLS") {
            FillTools(input);
        } else if (str == "OPERATIONS") {
            fillOperations(input);
        } else if (str == "CONTINIOUS") {
            FillStoppable(input);
        } else if (str == "TIMES") {
            fillTimes(input);
        } else if (str == "GRAPHS") {
            fillGraph(input);
        } else if (str == "START") {
            fillStart(input);
        } else if (str == "DIRECTIVE") {
            fillDirective(input);
        } else if (str == "FINES") {
            fillFines(input);
        } else if (str == "g") {
            fillWork(input);
        }
    }

    // секция сборки Operation, Work
    for (size_t i = 0; i < oper_previous_.size(); ++i) {
        operations.push_back(Operation{oper_stoppabillity_[i],
                                       oper_previous_[i], oper_tools_[i]});
    }

    for (size_t i = 0; i < idx_work_; ++i) {
        works.push_back(Work{work_starts_[i], work_direct_[i], work_fines_[i],
                             work_operations_[i], operations});
    }
    
}

void ProblemData::ParseTools(std::string& str, int idTool) {
    std::set<Tool::TimeInterval> schedule;
    std::string time1 = "", time2 = "";
    for (size_t i = 0; i < str.size();) {
        if (str[i] == '(') {
            ++i;
            while ('0' <= str[i] && str[i] <= '9') {
                time1.push_back(str[i]);
                ++i;
            }
            continue;
        } else if (str[i] == ',') {
            i += 2;  // числа разделяются запятой с пробелом
            while ('0' <= str[i] && str[i] <= '9') {
                time2.push_back(str[i]);
                ++i;
            }
            continue;
        } else if (str[i] == ')') {
            schedule.insert(
                Tool::TimeInterval{std::stoul(time1), std::stoul(time2)});
            time1.clear();
            time2.clear();
        }
        ++i;
    }
    tools.push_back(Tool{schedule});
}

void ProblemData::FillTools(std::ifstream& input) {
    std::string str;
    std::getline(input, str);

    int cntTools = std::stoi(str);
    for (int i = 0; i < cntTools && !input.eof(); ++i) {
        std::getline(input, str);
        ParseTools(str, i);
    }
}

void ProblemData::FillStoppable(std::ifstream& input) {
    std::string str;

    size_t cntOperations = oper_stoppabillity_.size();
    for (size_t i = 0; i < cntOperations && !input.eof(); ++i) {
        input >> str;
        oper_stoppabillity_[i] = (str == "1" ? true : false);
    }
}

void ProblemData::fillOperations(std::ifstream& input) {
    std::string str;
    std::getline(input, str);
    auto size = std::stoul(str);
    oper_stoppabillity_.resize(size);
    oper_previous_.resize(size, {});
    oper_tools_.resize(size, {});
}

void ProblemData::fillTimes(std::ifstream& input) {
    std::string str;

    size_t cntOperations = oper_stoppabillity_.size();
    times_matrix.resize(cntOperations);
    for (size_t i = 0; i < cntOperations && !input.eof(); ++i) {
        std::getline(input, str);
        str += " ";
        std::string number = "";
        size_t tool_id = 0;
        for (auto e : str) {
            if ('0' <= e && e <= '9') {
                number.push_back(e);
            } else if (!number.empty()) {
                // исправление возможного бага с -1 и unsigned int
                int time = std::stoi(number);
                if (time == 0) {
                    times_matrix[i].push_back(0);
                } else {
                    times_matrix[i].push_back(time);
                    oper_tools_[i].insert(tool_id);
                }
                number.clear();
                ++tool_id;
            }
        }
    }
}

void ProblemData::fillGraph(std::ifstream& input) {
    std::string str;
    std::getline(input, str);
    auto size = std::stoi(str);
    work_starts_.resize(size);
    work_direct_.resize(size);
    work_fines_.resize(size);
    work_operations_.resize(size, {});
}

void ProblemData::fillStart(std::ifstream& input) {
    std::string str;
    for (size_t i = 0; i < work_starts_.size(); ++i) {
        input >> str;
        work_starts_[i] = std::stoi(str);
    }
}

void ProblemData::fillDirective(std::ifstream& input) {
    std::string str;
    for (size_t i = 0; i < works.size(); ++i) {
        input >> str;
        work_direct_[i] = std::stoi(str);
    }
}

void ProblemData::fillFines(std::ifstream& input) {
    std::string str;
    for (size_t i = 0; i < works.size(); ++i) {
        input >> str;
        work_fines_[i] = std::stod(str);
    }
}

void ProblemData::fillWork(std::ifstream& input) {
    std::string str1, str2;
    std::getline(input, str1);
    size_t count_edge = std::stoul(str1);
    for (size_t i = 0; i < count_edge; ++i) {
        input >> str1 >> str2;
        oper_previous_[std::stoul(str1)].insert(std::stoul(str2));
        work_operations_[idx_work_].insert(std::stoul(str1));
        work_operations_[idx_work_].insert(std::stoul(str2));
    }
  
    ++idx_work_;
}
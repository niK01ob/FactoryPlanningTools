#include "problem_data.h"

class GeneratorData{
public:
    void Generate();
    ProblemData* GetData() {return &data_;}

private:
    ProblemData data_;
    std::vector<Operation> tmpOperations_;
    std::vector<size_t> indOperations;
    std::vector<std::set<Tool::TimeInterval>> shedule;
    const uint64_t minCntTool_ = 2;
    const uint64_t maxCntTool_ = 5;
    const uint64_t minCntIntervals_ = 3;
    const uint64_t maxCntIntervals_ = 7;
    const uint64_t minTimeStep_ = 10;
    const uint64_t maxTimeStep_ = 20;
    const uint64_t minTimeWork_ = 30;
    const uint64_t maxTimeWork_ = 50;
    const uint64_t cntMix = 0;
    const uint64_t cntAddings = 1;
    void Clear();
    void GenerateFirstTools();
    void GenerateWork();
    void MixOperation();
    void AddIntervals();
};

void GeneratorData::Clear() {
    data_.operations.clear();
    data_.times_matrix.clear();
    data_.tools.clear();
    data_.works.clear();
}

void GeneratorData::Generate() {
    Clear();
    data_.times_matrix.resize(maxCntIntervals_ * maxCntTool_, std::vector<uint64_t> (maxCntTool_, 0));
    GenerateFirstTools();
    MixOperation();
    AddIntervals();
    GenerateWork();
}

void GeneratorData::GenerateFirstTools() {
    int cntTools = minCntTool_ + rand() % (maxCntTool_ - minCntTool_ + 1);
    shedule.resize(cntTools);
    //data_.tools.resize(cntTools);

    for (int tool = 0; tool < cntTools; tool++) {
        int cntIntervals = minCntIntervals_ + rand() % (maxCntIntervals_ - minCntIntervals_ + 1);
        int stTime = 0;
        int enTime = 0;
        for (int i = 0; i < cntIntervals; i++) {
            stTime = enTime + minTimeStep_ + rand() % (maxTimeStep_ - minTimeStep_ + 1);
            enTime = stTime + minTimeWork_ + rand() % (maxTimeWork_ - minTimeWork_ + 1);
            shedule[tool].insert(Tool::TimeInterval(stTime, enTime));
            //operation / matrix
            //data_.operations.push_back(Operation{false, {}, {(uint64_t)tool}});
            tmpOperations_.push_back(Operation{false, {}, {(uint64_t)tool}});
            data_.times_matrix[tmpOperations_.size() - 1][tool] = enTime - stTime;
        }
        //data_.tools.push_back(shedule);
    }
}

void GeneratorData::MixOperation() {
    for (size_t i = 0; i < tmpOperations_.size(); ++i) {
        indOperations.push_back(i);
    }
    for (uint64_t i = 0; i < cntMix; ++i) {
        int id1 = rand() % tmpOperations_.size();
        int id2 = rand() % tmpOperations_.size();
        std::swap(indOperations[id1], indOperations[id2]);
        std::swap(data_.times_matrix[id1], data_.times_matrix[id2]);
    }
    for (size_t i = 0; i < tmpOperations_.size(); ++i) {
        data_.operations.push_back(tmpOperations_[indOperations[i]]);
    }
}

void GeneratorData::GenerateWork() {
    std::set<size_t> needOp;
    for (size_t i = 0; i < data_.operations.size(); ++i) {
        needOp.insert(i);
    }
    data_.works.push_back(Work{0, 0, 100, needOp, data_.operations});
}

void GeneratorData::AddIntervals() {
    for (size_t i = 0; i < shedule.size(); ++i) {
        uint64_t stTime = (*shedule[i].rbegin()).start;
        uint64_t enTime = (*shedule[i].rbegin()).end;
        for (uint64_t j = 0; j < cntAddings; ++j) {
            stTime = enTime + minTimeStep_ + rand() % (maxTimeStep_ - minTimeStep_ + 1);
            enTime = stTime + minTimeWork_ + rand() % (maxTimeWork_ - minTimeWork_ + 1);
            shedule[i].insert(Tool::TimeInterval(stTime, enTime));
        }
        data_.tools.push_back(shedule[i]);
    }
}
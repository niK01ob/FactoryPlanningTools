#include "problem_data.h"
#include <algorithm>

const uint64_t minCntTool_ = 2;
const uint64_t maxCntTool_ = 5;
const uint64_t minCntIntervals_ = 3;
const uint64_t maxCntIntervals_ = 7;
const uint64_t minTimeStep_ = 10;
const uint64_t maxTimeStep_ = 20;
const uint64_t minTimeWork_ = 30;
const uint64_t maxTimeWork_ = 50;
const uint64_t cntMix = 10;
const uint64_t cntAddings = 0;
const uint64_t prOperationStep = 3;

class GeneratorData{
public:
    void Generate();
    ProblemData* GetData() {return &data_;}

private:
    struct GarbageO {
        bool continous_;
        uint64_t time_;
        uint64_t start_;
        uint64_t end_;
        size_t idTool_;
        uint64_t idOper_;
        GarbageO(bool continous, uint64_t time, uint64_t start, uint64_t end, uint64_t idTool) :
            continous_(continous), time_(time), start_(start), end_(end), idTool_(idTool) {}
    };
    ProblemData data_;
    std::vector<Operation> tmpOperations_;
    std::vector<size_t> indOperations_;
    std::vector<GarbageO> vecGarb_;
    std::vector<std::set<Tool::TimeInterval>> shedule;
    void Clear();
    void GenerateFirstTools();
    void GenerateOperations();
    void BuildGarbage();
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
    GenerateOperations();

    //MixOperation();
    AddIntervals();
    GenerateWork();
}

//создает решение
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

            //tmpOperations_.push_back(Operation{false, {}, {(uint64_t)tool}});
            //data_.times_matrix[tmpOperations_.size() - 1][tool] = enTime - stTime;
        }
        //data_.tools.push_back(shedule);
    }
}
//делаем операции прерываеммыми
void GeneratorData::BuildGarbage() {
    size_t cntTools = shedule.size();
    uint64_t timeOperation = 0;
    uint64_t startTime = 0;
    uint64_t endTime = 0;
    bool continous = false;

    for (size_t tool = 0; tool < cntTools; tool++) {
        for (auto timeInterval : shedule[tool]) {
            timeOperation += timeInterval.end - timeInterval.start;
            if(startTime == 0) {
                startTime = timeInterval.start;
            }
            endTime = timeInterval.end;
            if (rand() % 2) {
                vecGarb_.push_back(GarbageO{continous, timeOperation, startTime, endTime, tool});
                continous = false;
                timeOperation = 0;
                startTime = 0;
            } else {
                continous = true;
            }
        }
        if (timeOperation > 0) {
            vecGarb_.push_back(GarbageO{continous, timeOperation, startTime, endTime, tool});
            continous = false;
            timeOperation = 0;
            startTime = 0;
        }
    }
}

void GeneratorData::GenerateOperations() {
    BuildGarbage();
    MixOperation();
    for (size_t i = 0; i < vecGarb_.size(); ++i) {
        vecGarb_[i].idOper_ = i;
    } 
    std::sort(vecGarb_.begin(), vecGarb_.end(), [](GarbageO a, GarbageO b){ if(a.start_ != b.start_) return a.start_ < b.start_; return a.end_ < b.end_; });
    //std::set<std::vector<uint64_t>> sOper;
    indOperations_.resize(vecGarb_.size());
    for (size_t i = 0; i < vecGarb_.size(); ++i) {
        tmpOperations_.push_back(Operation{vecGarb_[i].continous_, {}, {vecGarb_[i].idTool_}});
        data_.times_matrix[vecGarb_[i].idOper_][vecGarb_[i].idTool_] = vecGarb_[i].time_;
        indOperations_[vecGarb_[i].idOper_] = i;
    }
    for (size_t i = 0; i < tmpOperations_.size(); ++i) {
        data_.operations.push_back(tmpOperations_[indOperations_[i]]);
    }
}

//меняем порядок операций
void GeneratorData::MixOperation() {
    /*for (size_t i = 0; i < tmpOperations_.size(); ++i) {
        indOperations_.push_back(i);
    }*/
    for (uint64_t i = 0; i < cntMix; ++i) {
        int id1 = rand() % vecGarb_.size();
        int id2 = rand() % vecGarb_.size();
        std::swap(vecGarb_[id1], vecGarb_[id2]);
        //std::swap(data_.times_matrix[id1], data_.times_matrix[id2]);
    }
    /*for (size_t i = 0; i < tmpOperations_.size(); ++i) {
        data_.operations.push_back(tmpOperations_[indOperations_[i]]);
    }*/
}

//делаем работы
void GeneratorData::GenerateWork() {
    std::set<size_t> needOp;
    for (size_t i = 0; i < data_.operations.size(); ++i) {
        needOp.insert(i);
    }
    data_.works.push_back(Work{0, 0, 100, needOp, data_.operations});
}

//увеличиваем кол-во итнервалы распмсания
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
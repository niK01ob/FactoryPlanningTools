#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "generator_data.h"
#include "solution_checker.h"
#include "solver.h"
#include "scorer.h"
/*std::ifstream input_file("../test_data/data.DAT");
    ProblemData test_data(input_file);
    input_file.close();
    Solver::Solve(&test_data);*/

/*std::cout << "Shedules:\n";
        for (auto& tool : (test_data).tools) {
            tool.PrintShedule(std::cout);
            std::cout << "\n";
        }

        std::cout << "\nGant plots:\n";
        for (auto& tool : test_data.tools) {
            tool.PrintGant(std::cout);
            std::cout << "\n";
        }*/

int main() {
    uint64_t count_fail = 0;
    srand(time(0));
    double max_score = 0;
    double min_score = 9'999'999'999;
    for (size_t i = 0; i < 1000; ++i) {
        GeneratorData data;
        data.Generate();
        //std::cout << "success\n";
        ProblemData test_data((*data.GetData()));
        Solver::Solve(&test_data);

        
        // test_data.tools[0].DestroyWorkProcess();
        try {
            SolutionChecker::Check(&test_data);
            max_score = std::max(max_score, Scorer::CalculateScore(&test_data));
            min_score = std::min(min_score, Scorer::CalculateScore(&test_data));
        } catch (std::runtime_error& e) {
            ++count_fail;
        }
    }

    std::cout << "Успешно : " << 1'000 - count_fail << " C ошибками: " << count_fail << std::endl;
    std::cout << "Худший скор = " << max_score << " Лучший скор = " << min_score;
    return 0;
}

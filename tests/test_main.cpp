#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "problem_data.h"
#include "task_profile.h"
#include "tool.h"
#include "work.h"

const std::vector<std::vector<uint64_t>> kTimesMatrix{
    {1, 0, 4, 5},  
    {0, 2, 3, 1}, 
    {0, 0, 0, 4}, 
    {5, 1, 3, 5},
    {10, 5, 1, 0}, 
    {0, 3, 4, 0}, 
    {3, 5, 3, 0}, 
    {0, 0, 10, 5}};

const std::vector<Tool> kTools{
    Tool({{5, 10}, {15, 35}, {40, 47}, {50, 62}}),
    Tool({{35, 40}, {50, 70}, {10, 30}, {75, 80}, {88, 105}}),
    Tool({{1, 5}, {15, 25}, {27, 37}, {44, 55}, {90, 100}}),
    Tool({{3, 10}, {25, 50}, {60, 90}})};

// g0: 0->1->2
// g1: 3->4
//      ->5->6
//      ->7

const std::vector<Operation> kOperations{
    Operation({true, {}, {0, 2, 3}}),
    Operation({true, {1}, {1, 2, 3}}),
    Operation({true, {2}, {3}}),
    Operation({false, {}, {0, 1, 2, 3}}),
    Operation({true, {3}, {0, 1, 2}}),
    Operation({true, {3}, {1, 2}}),
    Operation({true, {5}, {0, 1, 2}}),
    Operation({false, {3}, {2, 3}})
};

/*const std::vector<Work> kWorks{
    Work({10, 50, 1.5, {0, 1, 2}, kOperations}),
    Work({20, 70, 2.3, {3, 4, 5, 6, 7}, kOperations})
};*/

TEST(Tools__Test, can_start_work) {
    Tool test_tool({{35, 40}, {50, 70}, {10, 30}, {75, 80}});
    Operation op1{true, {}, {0}};
    Operation op2{true, {0}, {0}};
    Operation op3{false, {0, 1}, {0}};

    ASSERT_EQ(test_tool.CanStartWork(op1, 11, 10), true);
    ASSERT_EQ(test_tool.CanStartWork(op1, 9, 10), false);
    ASSERT_EQ(test_tool.CanStartWork(op3, 50, 10), true);
    ASSERT_EQ(test_tool.CanStartWork(op3, 35, 10), false);
    ASSERT_EQ(test_tool.CanStartWork(op1, 35, 100), false);
    ASSERT_EQ(test_tool.CanStartWork(op2, 51, 12), true);
}

TEST(Tools__Test, check_collisions) {
    Tool test_tool({{35, 40}, {50, 70}, {10, 30}, {75, 80}});
    Operation op1{true, {}, {0}};
    Operation op2{true, {}, {0}};

    ASSERT_EQ(test_tool.CanStartWork(op1, 10, 15), true);
    ASSERT_EQ(test_tool.CanStartWork(op2, 10, 15), true);

    test_tool.Appoint(op1, 0, 10, 10);
    test_tool.Appoint(op2, 1, 50, 12);

    ASSERT_NO_THROW(test_tool.CheckCollisions());
    test_tool.Appoint(op1, 3, 11, 5);
    ASSERT_THROW(test_tool.CheckCollisions(), std::runtime_error);
    ASSERT_THROW(test_tool.Appoint(op1, 5, 10, 3), std::runtime_error);
}

void CompareMatrix(const std::vector<std::vector<uint64_t>>& left,
                   const std::vector<std::vector<uint64_t>>& right) {
    EXPECT_EQ(left.size(), right.size());
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_EQ(left[i].size(), right[i].size());
        for (size_t j = 0; j < left[i].size(); ++j) {
            EXPECT_EQ(left[i][j], right[i][j]);
        }
    }
}

TEST(Updated_parser, base_parsing) {
    std::filesystem::path data_path = "../test_data/data.DAT";
    if (!std::filesystem::exists(data_path)) {
        data_path = "test_data/data.DAT";
    }
    std::ifstream input_file(data_path);
    ASSERT_TRUE(input_file.is_open()) << "Cannot open " << data_path;
    ProblemData test_data(input_file);
    input_file.close();
    ASSERT_EQ(test_data.operations.size(), 5u);
    ASSERT_EQ(test_data.tools.size(), 3u);
    ASSERT_EQ(test_data.works.size(), 2u);
    std::vector<std::vector<uint64_t>> true_times{
        {0, 20, 10}, {10, 0, 40}, {10, 0, 0}, {0, 0, 40}, {20, 30, 0}};
    CompareMatrix(test_data.times_matrix, true_times);

    EXPECT_TRUE((test_data.operations.begin()->possible_tools == std::set<size_t>{1, 2}));

    // START / DIRECTIVE / FINES must be parsed before works are assembled.
    ASSERT_EQ(test_data.works.size(), 2u);
    EXPECT_EQ(test_data.works[0].start_time, 10u);
    EXPECT_EQ(test_data.works[1].start_time, 30u);
    EXPECT_EQ(test_data.works[0].directive, 100u);
    EXPECT_EQ(test_data.works[1].directive, 120u);
    EXPECT_DOUBLE_EQ(test_data.works[0].fine_coef, 5.0);
    EXPECT_DOUBLE_EQ(test_data.works[1].fine_coef, 3.0);

    // Graph consistency according to current parser semantics
    // (first vertex depends on second in each pair from data.DAT):
    // 0 depends on 1, 1 depends on 2, 3 depends on 4
    EXPECT_TRUE((test_data.operations[0].previous_op_id == std::set<size_t>{1}));
    EXPECT_TRUE((test_data.operations[1].previous_op_id == std::set<size_t>{2}));
    EXPECT_TRUE(test_data.operations[2].previous_op_id.empty());
    EXPECT_TRUE((test_data.operations[3].previous_op_id == std::set<size_t>{4}));
    EXPECT_TRUE(test_data.operations[4].previous_op_id.empty());
}

TEST(TaskProfile__Test, reference_features_are_compactly_serialized) {
    ProblemData data;
    data.tools = kTools;
    for (const auto& op : kOperations) {
        data.operations.emplace_back(op.stoppable, op.previous_op_id,
                                     op.possible_tools);
    }
    data.times_matrix = kTimesMatrix;

    const TaskProfile profile = TaskProfileBuilder::Build(&data);

    EXPECT_DOUBLE_EQ(profile.avg_out_degree, 0.75);
    EXPECT_EQ(profile.max_out_degree, 3u);
    EXPECT_DOUBLE_EQ(profile.time_range_ratio, 10.0);
    EXPECT_GT(profile.time_cv, 0.0);
    EXPECT_NEAR(profile.resource_load_ratio, 19.0 / 218.0, 1e-9);

    const std::string text = TaskProfileBuilder::ToTaskProfileV1Text(profile);
    EXPECT_NE(text.find("cv="), std::string::npos);
    EXPECT_NE(text.find("range_ratio="), std::string::npos);
    EXPECT_NE(text.find("load_ratio="), std::string::npos);
    EXPECT_NE(text.find("avg_out_degree="), std::string::npos);
    EXPECT_NE(text.find("max_out_degree="), std::string::npos);
}

TEST(Appoint__Test, simple) {
    //ASSERT_EQ(kWorks[0].CanBeAppointed(0, 5), false);
    //ASSERT_EQ(kWorks[0].CanBeAppointed(0, 10), true);
    //ASSERT_EQ(kWorks[0].CanBeAppointed(1, 10), false);

}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "../3rd_party/tiny-lsm/include/lsm/engine.h"
#include <gtest/gtest.h>

class LSMTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a temporary test directory
    test_dir = "test_lsm_data";
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
    std::filesystem::create_directory(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
  }

  std::string test_dir;
};
TEST_F(LSMTest, SimpleOps) {
  tiny_lsm::LSM engine("lsm.toml");

  engine.put("hello", "world");
  auto value = engine.get("hello");
  ASSERT_TRUE(value.has_value());
  ASSERT_EQ(value.value(), "world");
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
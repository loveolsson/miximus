#include "types/node_status.hpp"
#include "types/typescript_generator.hpp"
#include "utils/process_id.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace miximus::typescript::tests {

TEST(typescript_generator, nested_arrays_preserve_nullable_element_types)
{
    EXPECT_EQ(typescript_type<std::vector<std::optional<int>>>(), "ReadonlyArray<number | null>");
    EXPECT_EQ(typescript_type<std::optional<std::vector<int>>>(), "ReadonlyArray<number> | null");
    EXPECT_EQ(typescript_type<std::vector<std::vector<std::optional<int>>>>(),
              "ReadonlyArray<ReadonlyArray<number | null>>");
}

TEST(typescript_generator, cef_status_fields_use_generated_enums)
{
    const auto output = generate_typescript();
    EXPECT_NE(output.find("export const enum cef_state_e {"), std::string::npos);
    EXPECT_NE(output.find("export const enum cef_input_state_e {"), std::string::npos);
    EXPECT_NE(output.find("readonly cef_state: cef_state_e;"), std::string::npos);
    EXPECT_NE(output.find("readonly cef_inputs_state: cef_input_state_e;"), std::string::npos);
}

TEST(typescript_generator, opaque_json_overrides_are_member_specific)
{
    struct unrelated_s
    {
        nlohmann::json                          options;
        decltype(web_message::config_s::status) status;
    };
    static_assert(member_type_override<&unrelated_s::options>.empty());
    static_assert(member_type_override<&unrelated_s::status>.empty());

    const auto output = generate_typescript();
    EXPECT_NE(output.find("readonly options: options_s;"), std::string::npos);
    EXPECT_NE(output.find("readonly status: node_status_s;"), std::string::npos);
    EXPECT_NE(output.find("readonly status?: Readonly<Record<string, node_status_s>> | null;"), std::string::npos);
}

TEST(typescript_generator, every_publishable_status_contract_is_emitted)
{
    const auto output = generate_typescript();
    std::apply(
        [&](auto... contract) {
            (
                [&] {
                    static_assert(status::registered_contract<typename decltype(contract)::type>);
                    EXPECT_NE(output.find("export interface " + std::string(contract.name) + " {"), std::string::npos);
                }(),
                ...);
        },
        status::contracts);
}

class typescript_output : public ::testing::Test
{
    std::filesystem::path directory_;

  protected:
    const std::filesystem::path& directory() const { return directory_; }

    void SetUp() override
    {
        directory_ =
            std::filesystem::temp_directory_path() / ("miximus-typescript-test-" + std::to_string(utils::process_id()));
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    static std::string read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
};

TEST_F(typescript_output, replaces_changed_output_and_preserves_unchanged_timestamp)
{
    const auto path = directory() / "generated" / "contracts.ts";
    EXPECT_TRUE(write_if_changed(path, "original"));
    EXPECT_TRUE(write_if_changed(path, "replacement"));
    EXPECT_EQ(read(path), "replacement");

    const auto timestamp = std::filesystem::last_write_time(path);
    EXPECT_FALSE(write_if_changed(path, "replacement"));
    EXPECT_EQ(std::filesystem::last_write_time(path), timestamp);
    EXPECT_EQ(
        std::distance(std::filesystem::directory_iterator(path.parent_path()), std::filesystem::directory_iterator{}),
        1);
}

TEST_F(typescript_output, staging_failure_preserves_existing_output)
{
    const auto path = directory() / "contracts.ts";
    ASSERT_TRUE(write_if_changed(path, "original"));
    auto staging = path;
    staging += ".tmp." + std::to_string(utils::process_id());
    ASSERT_TRUE(std::filesystem::create_directory(staging));

    EXPECT_THROW(write_if_changed(path, "replacement"), std::runtime_error);
    EXPECT_EQ(read(path), "original");
    EXPECT_TRUE(std::filesystem::is_directory(staging));
}

} // namespace miximus::typescript::tests

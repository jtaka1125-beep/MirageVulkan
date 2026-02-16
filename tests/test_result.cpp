// =============================================================================
// MirageVulkan - Result Type Unit Tests
// =============================================================================

#include <gtest/gtest.h>
#include "result.hpp"

using namespace mirage;

// =============================================================================
// Test: Basic Ok/Err Creation
// =============================================================================

TEST(ResultTest, OkCreation) {
    Result<int, Error> result = Ok(42);

    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_err());
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ErrCreation) {
    Result<int, Error> result = Err<int>("Something went wrong", 500);

    EXPECT_FALSE(result.is_ok());
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().message, "Something went wrong");
    EXPECT_EQ(result.error().code, 500);
}

TEST(ResultTest, BoolConversion) {
    Result<int, Error> ok = Ok(1);
    Result<int, Error> err = Err<int>("error");

    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_FALSE(static_cast<bool>(err));

    if (ok) {
        SUCCEED();
    } else {
        FAIL() << "Ok result should be truthy";
    }

    if (err) {
        FAIL() << "Err result should be falsy";
    } else {
        SUCCEED();
    }
}

// =============================================================================
// Test: Value Access
// =============================================================================

TEST(ResultTest, ValueAccess) {
    Result<std::string, Error> result = Ok(std::string("hello"));

    EXPECT_EQ(result.value(), "hello");
}

TEST(ResultTest, ValueAccessThrowsOnError) {
    Result<int, Error> result = Err<int>("error");

    EXPECT_THROW(result.value(), std::runtime_error);
}

TEST(ResultTest, ErrorAccessThrowsOnOk) {
    Result<int, Error> result = Ok(42);

    EXPECT_THROW(result.error(), std::runtime_error);
}

TEST(ResultTest, ValueOr) {
    Result<int, Error> ok = Ok(42);
    Result<int, Error> err = Err<int>("error");

    EXPECT_EQ(ok.value_or(0), 42);
    EXPECT_EQ(err.value_or(0), 0);
}

// =============================================================================
// Test: Optional-style Access
// =============================================================================

TEST(ResultTest, OkOptional) {
    Result<int, Error> ok = Ok(42);
    Result<int, Error> err = Err<int>("error");

    auto ok_opt = ok.ok();
    auto err_opt = err.ok();

    EXPECT_TRUE(ok_opt.has_value());
    EXPECT_EQ(*ok_opt, 42);
    EXPECT_FALSE(err_opt.has_value());
}

TEST(ResultTest, ErrOptional) {
    Result<int, Error> ok = Ok(42);
    Result<int, Error> err = Err<int>("error");

    auto ok_err_opt = ok.err();
    auto err_err_opt = err.err();

    EXPECT_FALSE(ok_err_opt.has_value());
    EXPECT_TRUE(err_err_opt.has_value());
    EXPECT_EQ(err_err_opt->message, "error");
}

// =============================================================================
// Test: Map Operations
// =============================================================================

TEST(ResultTest, MapSuccess) {
    Result<int, Error> result = Ok(10);

    auto mapped = result.map([](int x) { return x * 2; });

    EXPECT_TRUE(mapped.is_ok());
    EXPECT_EQ(mapped.value(), 20);
}

TEST(ResultTest, MapError) {
    Result<int, Error> result = Err<int>("original error");

    auto mapped = result.map([](int x) { return x * 2; });

    EXPECT_TRUE(mapped.is_err());
    EXPECT_EQ(mapped.error().message, "original error");
}

TEST(ResultTest, MapErrSuccess) {
    Result<int, Error> result = Err<int>("original");

    auto mapped = result.map_err([](const Error& e) {
        return Error("wrapped: " + e.message, e.code);
    });

    EXPECT_TRUE(mapped.is_err());
    EXPECT_EQ(mapped.error().message, "wrapped: original");
}

// =============================================================================
// Test: Void Result
// =============================================================================

TEST(ResultTest, VoidOk) {
    Result<void, Error> result = Ok();

    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_err());
    EXPECT_NO_THROW(result.value());
}

TEST(ResultTest, VoidErr) {
    Result<void, Error> result = Error("failed");

    EXPECT_FALSE(result.is_ok());
    EXPECT_TRUE(result.is_err());
    EXPECT_THROW(result.value(), std::runtime_error);
    EXPECT_EQ(result.error().message, "failed");
}

// =============================================================================
// Test: Expect
// =============================================================================

TEST(ResultTest, ExpectSuccess) {
    Result<int, Error> result = Ok(42);

    EXPECT_EQ(result.expect("should not fail"), 42);
}

TEST(ResultTest, ExpectFailure) {
    Result<int, Error> result = Err<int>("inner error");

    EXPECT_THROW(result.expect("custom message"), std::runtime_error);
}

// =============================================================================
// Test: Error Types
// =============================================================================

TEST(ResultTest, VulkanError) {
    Result<int, VulkanError> result = VulkanError("VK_ERROR_OUT_OF_MEMORY", -2);

    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().message, "VK_ERROR_OUT_OF_MEMORY");
    EXPECT_EQ(result.error().vk_result, -2);
}

TEST(ResultTest, IoError) {
    Result<std::string, IoError> result = IoError("File not found", IoError::Kind::NotFound);

    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().kind, IoError::Kind::NotFound);
}

// =============================================================================
// Test: Function Return
// =============================================================================

Result<int, Error> divide(int a, int b) {
    if (b == 0) return Err<int>("Division by zero");
    return Ok(a / b);
}

TEST(ResultTest, FunctionReturn) {
    auto ok_result = divide(10, 2);
    auto err_result = divide(10, 0);

    EXPECT_TRUE(ok_result.is_ok());
    EXPECT_EQ(ok_result.value(), 5);

    EXPECT_TRUE(err_result.is_err());
    EXPECT_EQ(err_result.error().message, "Division by zero");
}

// =============================================================================
// Test: Chained Operations
// =============================================================================

Result<int, Error> parse_int(const std::string& s) {
    try {
        return Ok(std::stoi(s));
    } catch (...) {
        return Err<int>("Invalid integer: " + s);
    }
}

Result<int, Error> add_ten(int x) {
    return Ok(x + 10);
}

TEST(ResultTest, ChainedOperations) {
    auto result = parse_int("32")
        .map([](int x) { return x + 10; })
        .map([](int x) { return x * 2; });

    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 84);  // (32 + 10) * 2
}

TEST(ResultTest, ChainedOperationsWithError) {
    auto result = parse_int("not a number")
        .map([](int x) { return x + 10; })
        .map([](int x) { return x * 2; });

    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().message, "Invalid integer: not a number");
}

// =============================================================================
// Test: Move Semantics
// =============================================================================

TEST(ResultTest, MoveSemantics) {
    Result<std::string, Error> result = Ok(std::string("hello world"));

    std::string value = std::move(result).value();

    EXPECT_EQ(value, "hello world");
}

// =============================================================================
// Test: Complex Types
// =============================================================================

struct ComplexData {
    int id;
    std::string name;
    std::vector<int> values;
};

TEST(ResultTest, ComplexType) {
    ComplexData data{42, "test", {1, 2, 3}};
    Result<ComplexData, Error> result = Ok(std::move(data));

    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().id, 42);
    EXPECT_EQ(result.value().name, "test");
    EXPECT_EQ(result.value().values.size(), 3u);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

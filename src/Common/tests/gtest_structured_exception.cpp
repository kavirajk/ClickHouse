#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Common/StructuredException.h>

#include <gtest/gtest.h>

namespace DB
{
namespace ErrorCodes
{
extern const int UNKNOWN_IDENTIFIER;
}

TEST(StructuredException, NativeRoundTrip)
{
    Exception exception(ErrorCodes::UNKNOWN_IDENTIFIER, "Unknown expression identifier `val2`");
    exception.addHint("Maybe you meant: ['val']");
    exception.addContext("While resolving identifiers in the SELECT list");
    exception.setRetryable(false);

    const auto structured = StructuredException::fromException(exception, false, "query-id", "SELECT val2 FROM test_play");

    WriteBufferFromOwnString out;
    structured.write(out);
    ReadBufferFromString in(out.str());
    const auto decoded = StructuredException::read(in);

    EXPECT_EQ(decoded.version, StructuredException::VERSION);
    EXPECT_EQ(decoded.code, ErrorCodes::UNKNOWN_IDENTIFIER);
    EXPECT_EQ(decoded.code_name, "UNKNOWN_IDENTIFIER");
    EXPECT_EQ(decoded.category, ExceptionCategory::UserError);
    EXPECT_EQ(decoded.query_id, "query-id");
    EXPECT_EQ(decoded.query, "SELECT val2 FROM test_play");
    ASSERT_EQ(decoded.hints.size(), 1);
    EXPECT_EQ(decoded.hints.front(), "Maybe you meant: ['val']");
    ASSERT_EQ(decoded.context.size(), 1);
    EXPECT_EQ(decoded.context.front(), "While resolving identifiers in the SELECT list");
    EXPECT_FALSE(decoded.retryable);

    const Exception client_exception = decoded.toException("Received from localhost:9000", true);
    ASSERT_NE(client_exception.getStructuredException(), nullptr);
    EXPECT_EQ(client_exception.getStructuredException()->code_name, "UNKNOWN_IDENTIFIER");
    EXPECT_EQ(client_exception.getHints(), decoded.hints);
    EXPECT_EQ(client_exception.getContexts(), decoded.context);

    const auto forwarded = StructuredException::fromException(
        client_exception, false, "initiator-query-id", "SELECT val2 FROM distributed_test");
    EXPECT_EQ(forwarded.code_name, decoded.code_name);
    EXPECT_EQ(forwarded.category, decoded.category);
    EXPECT_EQ(forwarded.hints, decoded.hints);
    EXPECT_EQ(forwarded.context, decoded.context);
    EXPECT_EQ(forwarded.query_id, "initiator-query-id");
}

TEST(StructuredException, JSONUsesTheSameModel)
{
    Exception exception(ErrorCodes::UNKNOWN_IDENTIFIER, "Unknown expression identifier `val2`");
    const String json = StructuredException::fromException(exception, false, "query-id", "SELECT val2 FROM test_play").toJSON();

    EXPECT_NE(json.find("\"version\":1"), String::npos);
    EXPECT_NE(json.find("\"code_name\":\"UNKNOWN_IDENTIFIER\""), String::npos);
    EXPECT_NE(json.find("\"category\":\"USER_ERROR\""), String::npos);
    EXPECT_NE(json.find("\"query_id\":\"query-id\""), String::npos);
}

TEST(StructuredException, JSONReplacesInvalidUTF8)
{
    StructuredException exception;
    exception.message = "invalid ";
    exception.message.push_back(static_cast<char>(0xFF));

    const String json = exception.toJSON();
    EXPECT_EQ(json.find(static_cast<char>(0xFF)), String::npos);
    EXPECT_NE(json.find("\xEF\xBF\xBD"), String::npos);
}

}

#include <Common/StructuredException.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteBufferValidUTF8.h>
#include <IO/WriteHelpers.h>
#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Formats/FormatSettings.h>

#include <algorithm>
#include <array>

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_EXCEPTION;
    extern const int UNKNOWN_PROTOCOL;
}

namespace
{

ExceptionCategory categoryForCodeName(std::string_view code_name)
{
    static constexpr std::array user_errors = {
        "ACCESS_DENIED",
        "BAD_ARGUMENTS",
        "ILLEGAL_TYPE_OF_ARGUMENT",
        "INCORRECT_QUERY",
        "SYNTAX_ERROR",
        "UNKNOWN_DATABASE",
        "UNKNOWN_IDENTIFIER",
        "UNKNOWN_TABLE",
    };
    static constexpr std::array resource_errors = {
        "MEMORY_LIMIT_EXCEEDED",
        "QUERY_WAS_CANCELLED",
        "TOO_MANY_SIMULTANEOUS_QUERIES",
        "TOO_MANY_ROWS_OR_BYTES",
    };
    static constexpr std::array transient_errors = {
        "ALL_CONNECTION_TRIES_FAILED",
        "NETWORK_ERROR",
        "NO_AVAILABLE_REPLICA",
        "SOCKET_TIMEOUT",
    };
    static constexpr std::array internal_errors = {
        "LOGICAL_ERROR",
        "UNKNOWN_EXCEPTION",
    };

    auto contains = [code_name](const auto & values) { return std::ranges::find(values, code_name) != values.end(); };

    if (contains(user_errors))
        return ExceptionCategory::UserError;
    if (contains(resource_errors))
        return ExceptionCategory::ResourceLimit;
    if (contains(transient_errors))
        return ExceptionCategory::TransientError;
    if (contains(internal_errors))
        return ExceptionCategory::InternalError;
    return ExceptionCategory::Unknown;
}

String truncated(String value, size_t maximum_size)
{
    if (value.size() > maximum_size)
        value.resize(maximum_size);
    return value;
}

String normalizedQuery(String value)
{
    value = truncated(std::move(value), StructuredException::MAX_QUERY_SIZE);
    while (!value.empty()
        && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n'
            || value.back() == '\r' || value.back() == '\f' || value.back() == '\v'))
        value.pop_back();
    return value;
}

void truncateStrings(std::vector<String> & values)
{
    if (values.size() > StructuredException::MAX_ARRAY_SIZE)
        values.resize(StructuredException::MAX_ARRAY_SIZE);

    size_t remaining = StructuredException::MAX_ARRAY_STRINGS_SIZE;
    for (auto & value : values)
    {
        value = truncated(std::move(value), std::min(remaining, StructuredException::MAX_STRING_SIZE));
        remaining -= value.size();
    }

    std::erase_if(values, [](const String & value) { return value.empty(); });
}

void writeJSONMemberName(std::string_view name, WriteBuffer & out, bool & first)
{
    if (!first)
        writeChar(',', out);
    first = false;
    writeJSONString(name, out, {});
    writeChar(':', out);
}

void writeJSONStringArray(const std::vector<String> & values, WriteBuffer & out)
{
    writeChar('[', out);
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
            writeChar(',', out);
        writeJSONString(values[i], out, {});
    }
    writeChar(']', out);
}

}

String toString(ExceptionCategory category)
{
    switch (category)
    {
        case ExceptionCategory::Unknown: return "UNKNOWN";
        case ExceptionCategory::UserError: return "USER_ERROR";
        case ExceptionCategory::ResourceLimit: return "RESOURCE_LIMIT";
        case ExceptionCategory::TransientError: return "TRANSIENT_ERROR";
        case ExceptionCategory::InternalError: return "INTERNAL_ERROR";
        case ExceptionCategory::ExternalError: return "EXTERNAL_ERROR";
    }
    return "UNKNOWN";
}

StructuredException
StructuredException::fromException(const Exception & exception, bool with_stack_trace, const String & query_id_, const String & query_)
{
    if (const auto * existing = exception.getStructuredException())
    {
        StructuredException result = *existing;
        if (!query_id_.empty())
            result.query_id = truncated(query_id_, MAX_STRING_SIZE);
        if (!query_.empty())
            result.query = normalizedQuery(query_);
        result.formatted_message = truncated(getExceptionMessage(exception, with_stack_trace), MAX_STRING_SIZE);
        result.stack_trace = with_stack_trace ? truncated(exception.getStackTraceString(), MAX_STRING_SIZE) : String{};
        return result;
    }

    StructuredException result;
    result.code = exception.code();
    result.name = truncated(exception.name(), MAX_STRING_SIZE);
    result.code_name = truncated(String(ErrorCodes::getName(exception.code())), MAX_STRING_SIZE);
    result.category = categoryForCodeName(result.code_name);
    result.message = truncated(exception.message(), MAX_STRING_SIZE);
    result.query_id = truncated(query_id_, MAX_STRING_SIZE);
    result.query = normalizedQuery(query_);
    result.hints = exception.getHints();
    result.context = exception.getContexts();
    truncateStrings(result.hints);
    truncateStrings(result.context);
    result.retryable = exception.isRetryable();
    result.formatted_message = truncated(getExceptionMessage(exception, with_stack_trace), MAX_STRING_SIZE);
    if (with_stack_trace)
        result.stack_trace = truncated(exception.getStackTraceString(), MAX_STRING_SIZE);
    return result;
}

StructuredException
StructuredException::fromExceptionPtr(std::exception_ptr exception, bool with_stack_trace, const String & query_id, const String & query)
{
    try
    {
        std::rethrow_exception(std::move(exception));
    }
    catch (const Exception & e)
    {
        return fromException(e, with_stack_trace, query_id, query);
    }
    catch (const Poco::Exception & e)
    {
        return fromException(Exception(Exception::CreateFromPocoTag{}, e), with_stack_trace, query_id, query);
    }
    catch (const std::exception & e)
    {
        return fromException(Exception(Exception::CreateFromSTDTag{}, e), with_stack_trace, query_id, query);
    }
    catch (...)
    {
        return fromException(
            Exception::createRuntime(ErrorCodes::UNKNOWN_EXCEPTION, "Unknown exception"), with_stack_trace, query_id, query);
    }
}

void StructuredException::write(WriteBuffer & out) const
{
    WriteBufferFromOwnString payload;
    writeBinaryLittleEndian(code, payload);
    writeStringBinary(name, payload);
    writeStringBinary(code_name, payload);
    writeBinary(static_cast<UInt8>(category), payload);
    writeStringBinary(message, payload);
    writeStringBinary(query_id, payload);
    writeStringBinary(query, payload);
    writeVarUInt(hints.size(), payload);
    for (const auto & hint : hints)
        writeStringBinary(hint, payload);
    writeVarUInt(context.size(), payload);
    for (const auto & item : context)
        writeStringBinary(item, payload);
    writeBinary(retryable, payload);
    writeStringBinary(formatted_message, payload);
    writeStringBinary(stack_trace, payload);

    if (payload.count() > MAX_PAYLOAD_SIZE)
        throw Exception(
            ErrorCodes::TOO_LARGE_STRING_SIZE,
            "Structured exception payload is too large: {} bytes, maximum: {}",
            payload.count(),
            MAX_PAYLOAD_SIZE);

    writeBinaryLittleEndian(version, out);
    writeStringBinary(payload.str(), out);
}

StructuredException StructuredException::read(ReadBuffer & in)
{
    StructuredException result;
    readBinaryLittleEndian(result.version, in);

    String payload;
    readStringBinary(payload, in, MAX_PAYLOAD_SIZE);
    if (result.version != VERSION)
        throw Exception(ErrorCodes::UNKNOWN_PROTOCOL, "Unsupported structured exception version {}", result.version);

    ReadBufferFromString payload_in(payload);
    readBinaryLittleEndian(result.code, payload_in);
    readStringBinary(result.name, payload_in, MAX_STRING_SIZE);
    readStringBinary(result.code_name, payload_in, MAX_STRING_SIZE);
    UInt8 category = 0;
    readBinary(category, payload_in);
    if (category > static_cast<UInt8>(ExceptionCategory::ExternalError))
        throw Exception(
            ErrorCodes::UNKNOWN_PROTOCOL, "Unknown structured exception category {}", static_cast<UInt32>(category));
    result.category = static_cast<ExceptionCategory>(category);
    readStringBinary(result.message, payload_in, MAX_STRING_SIZE);
    readStringBinary(result.query_id, payload_in, MAX_STRING_SIZE);
    readStringBinary(result.query, payload_in, MAX_QUERY_SIZE);

    UInt64 hints_count = 0;
    readVarUInt(hints_count, payload_in);
    if (hints_count > MAX_ARRAY_SIZE)
        throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Too many structured exception hints: {}", hints_count);
    result.hints.resize(hints_count);
    for (auto & hint : result.hints)
        readStringBinary(hint, payload_in, MAX_STRING_SIZE);

    UInt64 context_count = 0;
    readVarUInt(context_count, payload_in);
    if (context_count > MAX_ARRAY_SIZE)
        throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Too many structured exception context entries: {}", context_count);
    result.context.resize(context_count);
    for (auto & item : result.context)
        readStringBinary(item, payload_in, MAX_STRING_SIZE);

    readBinary(result.retryable, payload_in);
    readStringBinary(result.formatted_message, payload_in, MAX_STRING_SIZE);
    readStringBinary(result.stack_trace, payload_in, MAX_STRING_SIZE);
    assertEOF(payload_in);
    return result;
}

Exception StructuredException::toException(const String & additional_message, bool remote) const
{
    String exception_message;
    if (!additional_message.empty())
        exception_message = additional_message + ". ";
    if (name != "DB::Exception")
        exception_message += name + ". ";
    exception_message += message;

    Exception result = Exception::createDeprecated(exception_message, code, remote);
    for (const auto & hint : hints)
        result.addHint("{}", hint);
    for (const auto & item : context)
        result.addContext("{}", item);
    result.setRetryable(retryable);
    result.setStructuredException(std::make_shared<StructuredException>(*this));
    return result;
}

String StructuredException::toJSON() const
{
    WriteBufferFromOwnString raw_out;
    WriteBufferValidUTF8 out(raw_out);
    writeChar('{', out);
    bool first = true;

    writeJSONMemberName("version", out, first);
    writeIntText(version, out);
    writeJSONMemberName("code", out, first);
    writeIntText(code, out);
    writeJSONMemberName("name", out, first);
    writeJSONString(name, out, {});
    writeJSONMemberName("code_name", out, first);
    writeJSONString(code_name, out, {});
    writeJSONMemberName("category", out, first);
    writeJSONString(toString(category), out, {});
    writeJSONMemberName("message", out, first);
    writeJSONString(message, out, {});
    writeJSONMemberName("query_id", out, first);
    writeJSONString(query_id, out, {});
    writeJSONMemberName("query", out, first);
    writeJSONString(query, out, {});
    writeJSONMemberName("hints", out, first);
    writeJSONStringArray(hints, out);
    writeJSONMemberName("context", out, first);
    writeJSONStringArray(context, out);
    writeJSONMemberName("retryable", out, first);
    writeCString(retryable ? "true" : "false", out);
    writeJSONMemberName("formatted_message", out, first);
    writeJSONString(formatted_message, out, {});
    writeChar('}', out);
    out.finalize();
    return raw_out.str();
}

}

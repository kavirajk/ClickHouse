#pragma once

#include <base/types.h>

#include <exception>
#include <vector>

namespace DB
{

class Exception;
class ReadBuffer;
class WriteBuffer;

enum class ExceptionCategory : UInt8
{
    Unknown = 0,
    UserError = 1,
    ResourceLimit = 2,
    TransientError = 3,
    InternalError = 4,
    ExternalError = 5,
};

/// Versioned representation shared by the native TCP protocol and HTTP JSON errors.
/// V1 is positional on the native wire. Changing its field order or encoding requires V2.
struct StructuredException
{
    static constexpr UInt16 VERSION = 1;
    static constexpr UInt64 MAX_PAYLOAD_SIZE = 128 * 1024;
    static constexpr UInt64 MAX_STRING_SIZE = 16 * 1024;
    static constexpr UInt64 MAX_QUERY_SIZE = 4 * 1024;
    static constexpr UInt64 MAX_ARRAY_SIZE = 64;
    static constexpr UInt64 MAX_ARRAY_STRINGS_SIZE = 8 * 1024;

    UInt16 version = VERSION;
    Int32 code = 0;
    String name;
    String code_name;
    ExceptionCategory category = ExceptionCategory::Unknown;
    String message;
    String query_id;
    String query;
    std::vector<String> hints;
    std::vector<String> context;
    bool retryable = false;
    String formatted_message;
    String stack_trace;

    static StructuredException
    fromException(const Exception & exception, bool with_stack_trace, const String & query_id = {}, const String & query = {});

    static StructuredException
    fromExceptionPtr(std::exception_ptr exception, bool with_stack_trace, const String & query_id = {}, const String & query = {});

    void write(WriteBuffer & out) const;
    static StructuredException read(ReadBuffer & in);

    Exception toException(const String & additional_message = {}, bool remote = false) const;

    String toJSON() const;
};

String toString(ExceptionCategory category);

}

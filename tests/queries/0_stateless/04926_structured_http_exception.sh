#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ACCEPT='Accept: application/vnd.clickhouse.exception+json; version=1'

echo '--- exception before response data'
${CLICKHOUSE_CURL} -sS \
    -H "$ACCEPT" \
    -H 'X-ClickHouse-Query-Id: structured-http-early' \
    "${CLICKHOUSE_URL}&query=SELECT%20missing_identifier" \
    | jq -c '{version, code, name, code_name, category, query_id, query, hints, context, retryable}'

echo '--- an unsupported model version keeps the legacy response'
${CLICKHOUSE_CURL} -sS \
    -H 'Accept: application/vnd.clickhouse.exception+json; version=10' \
    "${CLICKHOUSE_URL}&query=SELECT%20missing_identifier" 2>&1 \
    | grep -o -m1 '^Code: [0-9]*'

echo '--- exception after response data'
RESPONSE=$(${CLICKHOUSE_CURL} -sS \
    -H "$ACCEPT" \
    -H 'X-ClickHouse-Query-Id: structured-http-stream' \
    "${CLICKHOUSE_URL}&http_wait_end_of_query=0&http_response_buffer_size=0" \
    --data-binary 'SELECT if(number = 2, throwIf(1), number) FROM numbers(4) FORMAT CSV SETTINGS max_block_size = 1, max_threads = 1' \
    2>/dev/null)

printf '%s\n' "$RESPONSE" \
    | tr -d '\r' \
    | awk '/^__exception__$/ { getline; getline; print; exit }' \
    | jq -c '{version, name, code_name, query_id, query, retryable}'

echo '--- framed exception is a JSON object'
${CLICKHOUSE_CURL} -sS \
    -H "$ACCEPT" \
    -H 'X-ClickHouse-Query-Id: structured-http-framed' \
    "${CLICKHOUSE_URL}&framing_output_format=JSONEachPacketString&send_profile_events=0&http_wait_end_of_query=0&http_response_buffer_size=0" \
    --data-binary 'SELECT throwIf(number = 0) FROM numbers(1) FORMAT JSONEachRow' \
    | jq -c 'select(.packet == "exception") | {packet, exception_is_object: (.exception | type == "object"), version: .exception.version, query_id: .exception.query_id}'

#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Server-formatted output with parallel formatting must be byte-identical to the output formatted by the client.
query="SELECT number, toString(number) AS s, [number, number * 2] AS arr FROM numbers(300000)"
for format in CSV JSONEachRow TSVWithNamesAndTypes; do
    for compression in 0 1; do
        client_side=$(${CLICKHOUSE_CLIENT} --max_block_size 1000 --max_threads 4 --compression "$compression" \
            --query "$query FORMAT $format" | md5sum)
        server_side=$(${CLICKHOUSE_CLIENT} --server-side-output-format --max_block_size 1000 --max_threads 4 \
            --compression "$compression" --output_format_parallel_formatting 1 --query "$query FORMAT $format" | md5sum)
        [[ "$client_side" == "$server_side" ]] && echo "$format compression=$compression: same" \
            || echo "$format compression=$compression: differs"
    done
done

# Totals and rows before limit are written by the server-side format.
${CLICKHOUSE_CLIENT} --server-side-output-format --query "
    SELECT number % 3 AS k, count() AS c FROM numbers(10) GROUP BY k WITH TOTALS ORDER BY k FORMAT TSV"
${CLICKHOUSE_CLIENT} --server-side-output-format --query "
    SELECT number FROM numbers(100) LIMIT 2 FORMAT JSON SETTINGS output_format_write_statistics = 0" \
    | grep -E '"rows"|"rows_before_limit_at_least"'

# `JSON` statistics report the rows read by the query.
${CLICKHOUSE_CLIENT} --server-side-output-format --query "SELECT number FROM numbers(1000) FORMAT JSON" \
    | grep '"rows_read"'

# An exception in the middle of a parallel-formatted stream is delivered as an exception, and the connection
# stays usable.
${CLICKHOUSE_CLIENT} --server-side-output-format --max_block_size 100 --max_threads 4 -n --query "
    SELECT throwIf(number = 50000) FROM numbers(100000) FORMAT CSV; -- { serverError FUNCTION_THROW_IF_VALUE_IS_NON_ZERO }
    SELECT 'ok';" | tail -n1
${CLICKHOUSE_CLIENT} --server-side-output-format --max_block_size 100 --max_threads 4 --query "
    SELECT throwIf(number = 50000) FROM numbers(100000) FORMAT CSV" 2>&1 >/dev/null \
    | grep -o 'FUNCTION_THROW_IF_VALUE_IS_NON_ZERO' | head -n1

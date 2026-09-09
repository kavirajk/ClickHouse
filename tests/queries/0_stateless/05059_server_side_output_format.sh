#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --server-side-output-format --compression 1 \
    --query "SELECT number FROM numbers(3) FORMAT JSONEachRow"
${CLICKHOUSE_CLIENT} --server-side-output-format --output-format CSV \
    --query "SELECT number, concat('v', toString(number)) AS value FROM numbers(3)"
${CLICKHOUSE_CLIENT} --server-side-output-format --output-format Parquet \
    --query "SELECT number FROM numbers(10)" \
    | ${CLICKHOUSE_LOCAL} --input-format Parquet --structure "number UInt64" --query "SELECT count(), sum(number) FROM table"

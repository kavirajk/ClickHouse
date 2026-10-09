#!/usr/bin/env python3
# Tags: no-fasttest
# no-fasttest: requires clickhouse_driver

# Lifecycle of a single persistent native TCP connection in `system.connections`:
# idle after a query, active while a query runs, idle again after a query fails or is killed,
# and removed after the client disconnects. The same `connection_id` is tracked through all transitions,
# and it is observed from separate connections while the tracked connection is idle.
# Requires `collect_connection_metrics = true` in the server config.

import os
import time
import urllib.parse
import urllib.request
import uuid

from clickhouse_driver import Client
from clickhouse_driver.protocol import ServerPacketTypes

CLICKHOUSE_HOST = os.environ.get("CLICKHOUSE_HOST", "localhost")
CLICKHOUSE_PORT_TCP = int(os.environ.get("CLICKHOUSE_PORT_TCP", "9000"))
CLICKHOUSE_PORT_HTTP = int(os.environ.get("CLICKHOUSE_PORT_HTTP", "8123"))
HTTP_URL = f"http://{CLICKHOUSE_HOST}:{CLICKHOUSE_PORT_HTTP}/"

TIMEOUT = 60


def observe(query):
    """Run a query on a fresh, non-keep-alive HTTP connection and return its TSV output."""
    url = HTTP_URL + "?" + urllib.parse.urlencode({"query": query})
    with urllib.request.urlopen(url, timeout=TIMEOUT) as response:
        return response.read().decode().rstrip("\n")


def wait_for(query, expected):
    deadline = time.monotonic() + TIMEOUT
    while True:
        result = observe(query)
        if result == expected:
            return
        if time.monotonic() > deadline:
            raise AssertionError(f"{query!r} returned {result!r}, expected {expected!r}")
        time.sleep(0.1)


def send_query(connection, query, query_id):
    connection.send_query(query, query_id=query_id)
    connection.send_external_tables(None)


def receive_result(connection):
    """Read the result of a query. Unlike `Client.execute`, a server exception does not close the connection."""
    while True:
        packet = connection.receive_packet()
        if packet.type == ServerPacketTypes.EXCEPTION:
            return packet.exception.code
        if packet.type == ServerPacketTypes.END_OF_STREAM:
            return 0


def state(connection_id):
    return observe(
        "SELECT protocol, status, query_id = '', user, last_query_time >= connected_time "
        f"FROM system.connections WHERE connection_id = {connection_id}"
    )


client = Client(CLICKHOUSE_HOST, port=CLICKHOUSE_PORT_TCP, user="default", password="")

# The query sees its own connection as active.
connection_id = client.execute(
    "SELECT connection_id FROM system.connections WHERE query_id = currentQueryID() AND status = 'active'"
)[0][0]
connection = client.connection

print("--- idle after a successful query")
print(state(connection_id))

print("--- active while a query runs")
running_query_id = str(uuid.uuid4())
send_query(connection, "SELECT sleepEachRow(0.1) FROM system.numbers SETTINGS max_block_size = 1", running_query_id)
wait_for(
    f"SELECT status FROM system.connections WHERE connection_id = {connection_id} AND query_id = '{running_query_id}'",
    "active",
)
print("active")
observe(f"KILL QUERY WHERE query_id = '{running_query_id}' SYNC FORMAT Null")
receive_result(connection)

print("--- idle after the query is killed")
print(state(connection_id))

print("--- idle after a query fails")
failed_query_id = str(uuid.uuid4())
send_query(connection, "SELECT throwIf(number = 3, 'intentional test exception') FROM numbers(10) SETTINGS max_block_size = 1", failed_query_id)
print(receive_result(connection))
print(state(connection_id))
print(observe(f"SELECT count() FROM system.connections WHERE query_id = '{failed_query_id}'"))

print("--- the connection is still usable")
send_query(connection, "SELECT 1", str(uuid.uuid4()))
print(receive_result(connection))

print("--- removed after disconnect")
client.disconnect()
wait_for(f"SELECT count() FROM system.connections WHERE connection_id = {connection_id}", "0")
print("removed")

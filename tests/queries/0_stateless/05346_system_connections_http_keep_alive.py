#!/usr/bin/env python3

# An HTTP keep-alive connection in `system.connections`: it is registered on its first query (not on `/ping`),
# keeps the same `connection_id` for all requests, is shown as idle between requests,
# and is removed when the socket is closed.
# Requires `collect_connection_metrics = true` in the server config.

import http.client
import os
import time
import urllib.parse
import urllib.request

CLICKHOUSE_HOST = os.environ.get("CLICKHOUSE_HOST", "localhost")
CLICKHOUSE_PORT_HTTP = int(os.environ.get("CLICKHOUSE_PORT_HTTP", "8123"))
HTTP_URL = f"http://{CLICKHOUSE_HOST}:{CLICKHOUSE_PORT_HTTP}/"

TIMEOUT = 60
USER_AGENT = "system-connections-keep-alive-test"


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


connection = http.client.HTTPConnection(CLICKHOUSE_HOST, CLICKHOUSE_PORT_HTTP, timeout=TIMEOUT)


def request(method, path, body=None):
    connection.request(method, path, body=body, headers={"User-Agent": USER_AGENT})
    response = connection.getresponse()
    data = response.read().decode().rstrip("\n")
    assert response.status == 200, (response.status, data)
    assert not response.will_close, "the server closed the keep-alive connection"
    return data


def query(sql):
    return request("POST", "/", sql)


# The keep-alive connection is the only one with this client port while it is open.
request("GET", "/ping")
client_port = connection.sock.getsockname()[1]
by_port = f"protocol = 'HTTP' AND client_port = {client_port}"

print("--- not registered before the first query")
print(observe(f"SELECT count() FROM system.connections WHERE {by_port}"))

print("--- the same connection_id for all requests on the connection")
own_id_query = "SELECT connection_id FROM system.connections WHERE query_id = currentQueryID() AND status = 'active'"
connection_id = query(own_id_query)
print(query(own_id_query) == connection_id)
print(observe(f"SELECT connection_id FROM system.connections WHERE {by_port}") == connection_id)

print("--- idle between requests")
print(
    observe(
        "SELECT protocol, status, query_id = '', user, client_name, last_query_time >= connected_time "
        f"FROM system.connections WHERE connection_id = {connection_id}"
    )
)

print("--- removed after the socket is closed")
connection.close()
wait_for(f"SELECT count() FROM system.connections WHERE connection_id = {connection_id}", "0")
print("removed")

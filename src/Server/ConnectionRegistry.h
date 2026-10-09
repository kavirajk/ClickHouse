#pragma once

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <base/defines.h>
#include <base/types.h>
#include <Poco/Net/IPAddress.h>


namespace DB
{

/// Properties of a client connection which are known when it is registered and never change afterwards.
struct ConnectionInfo
{
    String protocol;            /// "TCP" or "HTTP"
    Poco::Net::IPAddress client_address;
    UInt16 client_port = 0;
    UInt16 server_port = 0;
    String client_name;
    UInt64 client_version_major = 0;
    UInt64 client_version_minor = 0;
    UInt64 client_version_patch = 0;
    time_t connected_time = 0;
};


/// A consistent point-in-time copy of a registered connection, as returned by `ConnectionRegistry::list`.
struct ConnectionSnapshot
{
    UInt64 connection_id = 0;
    ConnectionInfo info;
    String user;
    bool is_active = false;     /// true while a query is executed, false while the connection waits for the next query
    String query_id;            /// empty while idle
    time_t last_query_time = 0; /// 0 if no query has been executed yet
};


/// A registered connection: an immutable part and a mutable part, which is updated on every query.
/// The mutable part is protected by a per-connection mutex, so that frequent updates from different
/// connections do not contend with each other on the registry-wide lock.
class ConnectionState
{
public:
    ConnectionState(UInt64 connection_id_, ConnectionInfo info_, String user_);

    /// The user is passed on every query, because an HTTP keep-alive connection
    /// may carry requests of different users.
    void setActive(const String & user_, const String & query_id_);
    void setIdle();

    ConnectionSnapshot getSnapshot() const;

    const UInt64 connection_id;
    const ConnectionInfo info;

private:
    mutable std::mutex mutex;
    String user TSA_GUARDED_BY(mutex);
    bool is_active TSA_GUARDED_BY(mutex) = false;
    String query_id TSA_GUARDED_BY(mutex);
    time_t last_query_time TSA_GUARDED_BY(mutex) = 0;
};

using ConnectionStatePtr = std::shared_ptr<ConnectionState>;


class ConnectionRegistry;

/// Owns the registration of a connection in `ConnectionRegistry` and removes it on destruction.
/// A default-constructed (or moved-from) handle is empty, and all its methods are no-ops.
class ConnectionHandle
{
public:
    ConnectionHandle() = default;
    ConnectionHandle(ConnectionRegistry & registry_, ConnectionStatePtr state_);
    ~ConnectionHandle();

    ConnectionHandle(const ConnectionHandle &) = delete;
    ConnectionHandle & operator=(const ConnectionHandle &) = delete;
    ConnectionHandle(ConnectionHandle && other) noexcept;
    ConnectionHandle & operator=(ConnectionHandle && other) noexcept;

    explicit operator bool() const { return state != nullptr; }

    void setActive(const String & user, const String & query_id);
    void setIdle();

private:
    void reset();

    ConnectionRegistry * registry = nullptr;
    ConnectionStatePtr state;
};


/// The connection-level part of an HTTP connection registration, shared by all requests of a keep-alive connection.
/// `HTTPServerConnection` serves every HTTP-based interface (queries, interserver replication, Prometheus, Keeper, ...),
/// so it cannot register the connection itself. Instead, it owns this slot for the lifetime of the socket,
/// and `HTTPHandler` registers the connection in it when it serves the first query.
/// The registration is removed when the socket is closed, not when a request is finished,
/// so that a keep-alive connection is visible as idle between requests.
struct HTTPConnectionRegistration
{
    time_t connected_time = 0;
    ConnectionHandle handle;
};


/// Global registry of client connections of the native TCP and HTTP query interfaces, exposed via `system.connections`.
/// The registry-wide lock is taken only when a connection is added, removed, or listed, but not on every query.
class ConnectionRegistry
{
public:
    static ConnectionRegistry & instance();

    /// Must be called once at server startup (from `attachSystemTablesServer`) when
    /// `collect_connection_metrics` is enabled. Until this is called, `add` returns an empty handle.
    void enable();
    bool isEnabled() const { return enabled.load(std::memory_order_relaxed); }

    /// Registers a connection which is idle until the first `setActive`.
    ConnectionHandle add(ConnectionInfo info, String user);

    /// Returns a snapshot of all currently registered connections.
    std::vector<ConnectionSnapshot> list() const;

private:
    friend class ConnectionHandle;

    void remove(UInt64 id);

    std::atomic<bool> enabled{false};
    std::atomic<UInt64> next_id{1};

    mutable std::mutex mutex;
    std::unordered_map<UInt64, ConnectionStatePtr> connections TSA_GUARDED_BY(mutex);
};

}

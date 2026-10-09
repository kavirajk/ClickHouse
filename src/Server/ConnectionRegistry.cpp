#include <Server/ConnectionRegistry.h>


namespace DB
{

ConnectionState::ConnectionState(UInt64 connection_id_, ConnectionInfo info_, String user_)
    : connection_id(connection_id_)
    , info(std::move(info_))
    , user(std::move(user_))
{
}

void ConnectionState::setActive(const String & user_, const String & query_id_)
{
    const time_t now = std::time(nullptr);
    std::lock_guard lock(mutex);
    user = user_;
    is_active = true;
    query_id = query_id_;
    last_query_time = now;
}

void ConnectionState::setIdle()
{
    std::lock_guard lock(mutex);
    is_active = false;
    query_id.clear();
}

ConnectionSnapshot ConnectionState::getSnapshot() const
{
    ConnectionSnapshot snapshot;
    snapshot.connection_id = connection_id;
    snapshot.info = info;

    std::lock_guard lock(mutex);
    snapshot.user = user;
    snapshot.is_active = is_active;
    snapshot.query_id = query_id;
    snapshot.last_query_time = last_query_time;
    return snapshot;
}


ConnectionHandle::ConnectionHandle(ConnectionRegistry & registry_, ConnectionStatePtr state_)
    : registry(&registry_)
    , state(std::move(state_))
{
}

ConnectionHandle::~ConnectionHandle()
{
    reset();
}

ConnectionHandle::ConnectionHandle(ConnectionHandle && other) noexcept
    : registry(other.registry)
    , state(std::move(other.state))
{
    other.registry = nullptr;
}

ConnectionHandle & ConnectionHandle::operator=(ConnectionHandle && other) noexcept
{
    if (this != &other)
    {
        reset();
        registry = other.registry;
        state = std::move(other.state);
        other.registry = nullptr;
    }
    return *this;
}

void ConnectionHandle::reset()
{
    if (state)
        registry->remove(state->connection_id);
    state.reset();
    registry = nullptr;
}

void ConnectionHandle::setActive(const String & user, const String & query_id)
{
    if (state)
        state->setActive(user, query_id);
}

void ConnectionHandle::setIdle()
{
    if (state)
        state->setIdle();
}


ConnectionRegistry & ConnectionRegistry::instance()
{
    static ConnectionRegistry registry;
    return registry;
}

void ConnectionRegistry::enable()
{
    enabled.store(true, std::memory_order_relaxed);
}

ConnectionHandle ConnectionRegistry::add(ConnectionInfo info, String user)
{
    if (!isEnabled())
        return {};

    const UInt64 id = next_id.fetch_add(1, std::memory_order_relaxed);
    auto state = std::make_shared<ConnectionState>(id, std::move(info), std::move(user));

    {
        std::lock_guard lock(mutex);
        connections.emplace(id, state);
    }
    return ConnectionHandle(*this, std::move(state));
}

std::vector<ConnectionSnapshot> ConnectionRegistry::list() const
{
    /// Copy the pointers with the registry-wide lock. Then release this lock and make the copies of the connections.
    /// Thus, this function does not lock a connection mutex when it holds the registry-wide lock.
    std::vector<ConnectionStatePtr> states;
    {
        std::lock_guard lock(mutex);
        states.reserve(connections.size());
        for (const auto & [_, state] : connections)
            states.push_back(state);
    }

    std::vector<ConnectionSnapshot> result;
    result.reserve(states.size());
    for (const auto & state : states)
        result.push_back(state->getSnapshot());
    return result;
}

void ConnectionRegistry::remove(UInt64 id)
{
    std::lock_guard lock(mutex);
    connections.erase(id);
}

}

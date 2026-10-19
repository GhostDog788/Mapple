# Kernel session lifecycle

[Documentation](../README.md) / Architecture

Mapple has one module-wide session manager shared by every transport. A session
owns one stream and one parser kthread. Transports provide the stream operations
and transfer ownership of a backend object when requesting a session.

## Core invariants

- The manager's session list is the source of truth for active membership.
- The list contains at most `MAPPLE_MAX_SESSIONS` entries.
- The manager mutex protects the list, the stopping flag, and session ID
  assignment.
- Session IDs increase monotonically for the lifetime of a module load.
- A session is registered and unregistered only through the manager.
- Removing a session from the list happens exactly once.
- Stream shutdown is idempotent and must unblock the parser.

There is deliberately no stored active counter, registered flag, or lifecycle
state. Active counts are derived from the list while holding the manager lock.
List membership is tested through the initialized session list node.

## Reference ownership

References allow shutdown snapshots, the parser, and the registry to use a
session concurrently without racing its destruction.

| Reference | Acquired | Released |
| --- | --- | --- |
| Creator | Session initialization | Registration returns |
| Registry | Session is added to the manager list | Manager unregisters the session |
| Parser | Before the parser kthread is created | Reaper finishes unregistering |
| Snapshot | Manager copies a session during shutdown | Shutdown has been requested for every snapshot entry |

The final `mapple_session_put()` releases the parser task reference, releases
the transport backend, and frees the session. Consequently, final destruction
can run in any sleepable context that drops the last reference.

## Registration

1. A transport transfers its backend to
   `mapple_session_manager_register()`.
2. The manager allocates and initializes a session.
3. Under the manager lock, it rejects the request if shutdown has begun or the
   list already contains eight sessions.
4. The manager assigns an ID, acquires the registry reference, and adds the
   session to the list before starting its parser.
5. The session acquires a parser reference and starts the parser kthread.
6. The manager releases the creator reference before returning.

If parser creation fails, the manager unregisters the session synchronously.
If allocation or registration fails, the final creator reference releases the
transferred backend.

Adding the session before starting its parser reserves capacity atomically. If
manager shutdown races with parser startup, the shutdown snapshot includes the
session and shuts down its stream; the newly started parser then observes that
shutdown and exits.

## Normal session termination

Every successfully started parser uses the same cleanup path, regardless of
whether it stops because of EOF, a protocol error, a transport error, or
manager shutdown:

```text
protocol loop returns
        |
        v
parser shuts down its stream
        |
        v
parser queues its reaper and returns
        |
        v
module workqueue runs the reaper
        |
        v
reaper joins the parser with kthread_stop()
        |
        v
manager removes the session from the registry
        |
        v
reaper drops the parser reference
        |
        v
last reference releases the backend and session
```

The parser cannot perform all of this synchronously because it cannot join its
own task. A workqueue callback provides a different context that can join the
parser safely. The saved task reference keeps `parser_task` valid after the
thread exits naturally and until the reaper joins it.

## Manager shutdown

`mapple_session_manager_shutdown()` performs these steps:

1. Sets the stopping flag under the manager lock. Later registration attempts
   are rejected.
2. Takes a referenced snapshot of every registered session.
3. Requests shutdown for every snapshot entry.
4. Drops all snapshot references.
5. Waits until the session list becomes empty.

Shutdown uses two loops so every parser receives shutdown before releasing any
snapshot reference that might be the final reference and trigger backend
destruction.

Only the unregister operation that removes the final list entry wakes the
manager. The wait condition rechecks the list under the manager lock, so it is
also safe if the list becomes empty before the manager begins waiting.

### What manager shutdown guarantees

When manager shutdown returns:

- New sessions cannot be registered.
- Every registered session has been removed from the manager list.
- No parser-owned session remains visible through the registry.

It does **not** guarantee that the final reaper callback has returned or that
every session object and backend has already been destroyed. The final reaper
can wake the manager immediately after removing the last registry entry and
then continue dropping references.

The module-wide workqueue must therefore be drained before module code is
unloaded. Any resource needed by a backend release callback must also remain
valid until that drain completes. Current backend release callbacks depend
only on their per-session backend objects, allowing listener and character
device service teardown before the final workqueue drain.

## Stream shutdown contract

Manager shutdown can wait indefinitely if a parser remains blocked inside a
stream operation. Every stream backend must therefore satisfy this contract:

- `shutdown` wakes any blocked `read_some` and `write_some` calls.
- After shutdown, new reads and writes return EOF or an error instead of
  blocking indefinitely.
- The shutdown callback finishes after arranging those wakeups; it does not
  need to wait for the parser itself to exit.

`mapple_stream_shutdown()` uses an atomic transition so the backend callback is
invoked only once even when the parser, transport, and manager request shutdown
concurrently.

## Inspection

The active count can be derived with `list_count_nodes()` while holding the
manager lock. Safe enumeration requires taking a session reference for each
copied list entry under the lock, then releasing those references after the
caller finishes inspecting them.

The current snapshot helper is private and used only for shutdown. If a future
kernel component needs inspection, expose a read-only count or bounded snapshot
API rather than adding duplicate counters, registration flags, or lifecycle
state.

# Design Plan: Deferred Command-Buffer Layer for GDI

> **Target system**: [GDI (Graph Database Interface)](https://github.com/spcl/GDI-RMA) —
> the ETH Zürich SPCL storage-layer API for graph databases (paper: arXiv 2305.11162, SC'23).
> This document is written for newcomers: every concept is explained before it is used.
> All code is **pseudo code** — it illustrates structure, not a real programming language.

---

## 1. Background and Motivation

### 1.1 What GDI gives us

GDI is a C-style, MPI-flavored API. A typical GDI program looks like this (taken from the
repo's `src/gdi.h` and the paper):

```c
GDI_Init(&argc, &argv);
GDI_CreateDatabase(&parameters, sizeof(params), &db);

GDI_StartTransaction(db, &transaction);
GDI_CreateVertex("alice", 5, transaction, &v1);  // external ID = the vertex key
GDI_CreateVertex("bob",   3, transaction, &v2);
GDI_CreateEdge(GDI_EDGE_DIRECTED, v1, v2, &edge);
status = GDI_CloseTransaction(&transaction, GDI_TRANSACTION_COMMIT);

GDI_FreeDatabase(&db);
GDI_Finalize();
```

(Note the `GDI_CreateVertex(external_id, size, tx, &holder)` signature: vertices carry a
caller-supplied **external ID** that GDI indexes — see §4.5. The `v1`/`v2` it returns are
transaction-local *holders*, not stable IDs — see §4.4.)

Every call is **synchronous**: the thread that calls `GDI_CreateVertex` blocks until the
backend has done the work. Relevant GDI functions we will wrap (from `src/gdi.h`):

| Category | GDI functions |
|---|---|
| Lifecycle | `GDI_Init`, `GDI_Finalize`, `GDI_CreateDatabase`, `GDI_FreeDatabase` |
| Transactions | `GDI_StartTransaction`, `GDI_CloseTransaction(commit/abort)`, `GDI_StartCollectiveTransaction`, `GDI_CloseCollectiveTransaction` |
| Vertex CRUD | `GDI_CreateVertex`, `GDI_FreeVertex`, `GDI_AssociateVertex` |
| Edge CRUD | `GDI_CreateEdge`, `GDI_FreeEdge`, `GDI_SetOriginVertexOfEdge`, `GDI_SetTargetVertexOfEdge`, `GDI_SetDirectionTypeOfEdge` |
| Labels | `GDI_AddLabelToVertex/Edge`, `GDI_RemoveLabelFromVertex/Edge`, `GDI_CreateLabel` |
| Properties | `GDI_AddPropertyToVertex/Edge`, `GDI_SetPropertyOfVertex/Edge`, `GDI_UpdatePropertyOfVertex/Edge`, `GDI_RemovePropertiesFromVertex/Edge` |
| Reads / search | `GDI_GetEdgesOfVertex`, `GDI_GetNeighborVerticesOfVertex`, `GDI_GetPropertiesOfVertex`, `GDI_GetVerticesOfEdge`, `GDI_TranslateVertexID` |
| Schema | `GDI_CreatePropertyType`, `GDI_CreateLabel`, constraints |

### 1.2 The problem with purely synchronous calls

1. **Round-trip cost**: in a distributed-memory GDI implementation (GDI-RMA), each call may
   involve network/RMA traffic. One call at a time = one network latency at a time.
2. **No batching**: creating 10,000 vertices means 10,000 sequential round trips.
3. **No overlap**: the CPU sits idle while the backend works; computation and communication
   cannot overlap (the paper explicitly identifies overlapping comm/compute as key to
   performance).
4. **Awkward API for apps**: modern applications expect to fire off a batch of work and get
   an async handle back, not to thread a `transaction` object through every line of code.

### 1.3 The solution: a deferred command buffer

Borrow an idea from graphics programming (Vulkan/Direct3D **command buffers**) and from
databases (**write-ahead / batch submission**):

- The application **records** operations into a `CommandBuffer` as lightweight `Command`
  objects. *Nothing hits the database yet.*
- When the application is done recording, it calls `CommandBuffer.submit()`, which sends
  the whole batch to the backend **at once** (ideally inside one GDI transaction, so the
  batch is atomic).
- `submit()` immediately returns a **`TaskHandle`** (an async "receipt"). The application
  can keep working, and later `wait()` on the handle, or `get()` individual command
  results out of it (new vertex IDs, search results, success/failure per command).

**Analogy**: instead of walking to the counter to order one coffee at a time, you write a
whole list, hand it over once, and get a buzzer that rings when the whole order is ready.

---

## 2. Design Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                        Application code                          │
│   buf.add(CreateVertex(...))   buf.add(CreateEdge(...))          │
│   handle = buf.submit()                                          │
└───────────────────────────┬──────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│                 CommandBuffer  (this design)                     │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐                │
│  │ Command │ │ Command │ │ Command │ │ Command │  ... (ordered)   │
│  └─────────┘ └─────────┘ └─────────┘ └─────────┘                │
│       submit() ──► validates, resolves placeholders,             │
│                    wraps in ONE GDI transaction                  │
└───────────────────────────┬──────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│              SubmissionEngine (worker thread / rank)             │
│         executes commands sequentially against GDI               │
└───────────────────────────┬──────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│        GDI API (GDI_StartTransaction, GDI_CreateVertex, ...)     │
└───────────────────────────┬──────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│        GDI implementation (e.g. GDI-RMA over MPI RMA)            │
└──────────────────────────────────────────────────────────────────┘
```

### Class inventory

| Class | Role |
|---|---|
| `Command` | Abstract base class. One instance = one deferred GDI operation. Knows its inputs, its future outputs, and how to `execute()` itself against a GDI connection. |
| `CommandResult` | Small tagged-union value returned by every executed command (status + optional payload). |
| `TaskHandle` | Async handle returned by `CommandBuffer.submit()`. Allows `isDone()`, `wait()`, `waitAll()`, `getResult(cmd)`. |
| `CommandBuffer` | Ordered collector of `Command`s. `add()`, `submit()`, `reset()`, `size()`. |
| `SubmissionEngine` | The machinery that actually runs the commands on a GDI database (possibly on a background thread / MPI rank), sets the results, and signals the `TaskHandle`. |
| Concrete commands | §5: `CreateVertexCommand`, `CreateEdgeCommand`, `GetOrCreateVertexCommand` (MERGE), `TranslateVertexIDCommand` (key lookup), bulk create commands, label/property-type/constraint schema commands, property commands (add/set/update/specific/remove), edge setter commands, read-back commands (`GetNeighbors`, `GetEdgesOfVertex`, `GetPropertiesOf*`, `GetLabelsOf*`, `Count`/`Exists`), `DeleteVertex(Guarded)Command`, `BeginTransaction`/`Commit`/`Abort`(+collective) commands, ... |

---

## 3. `CommandResult` — the universal return envelope

Every command, once executed, produces a `CommandResult`. Because pseudo-code has no
templates/generics to worry about, we model it as a tagged union ("a box that can hold
exactly one kind of thing, tagged so you know which"):

```
enum ResultKind {
    EMPTY,            // e.g. a successful delete: nothing to return
    INT,              // counts: "matched 42 vertices"
    BOOL,             // found flags: e.g. TranslateVertexIDCommand
    VERTEX_UID,       // e.g. from CreateVertexCommand / GetOrCreateVertexCommand
    EDGE_UID,         // e.g. from CreateEdgeCommand
    VERTEX_UID_PAIR,  // e.g. from GetEdgeVerticesCommand (origin + target)
    VERTEX_LIST,      // e.g. from GetNeighborsCommand (search result)
    EDGE_LIST,        // e.g. from GetEdgesOfVertexCommand
    LABEL_LIST,       // e.g. from GetLabelsOfVertexCommand
    PROPERTY_TYPE_LIST, // e.g. from GetAllPropertyTypesCommand
    PROPERTY_MAP,     // e.g. from GetPropertiesOfVertexCommand
    SNAPSHOT,         // a full VertexSnapshot / EdgeSnapshot (see below)
    ERROR             // the command failed; carries an error code + message
}

// What a "read one element" command hands back. Mirrors the reference Neo4j
// layer's read-only Node/Relationship snapshots: a stable identity plus the
// data that was asked for, decoupled from any transaction.
struct VertexSnapshot {
    GDI_Vertex_uid      uid
    List<String>        label_names
    Map<String, Value>  properties
}
struct EdgeSnapshot {
    GDI_Edge_uid   uid
    GDI_Vertex_uid origin_uid
    GDI_Vertex_uid target_uid
    int            direction   // GDI_EDGE_DIRECTED | GDI_EDGE_UNDIRECTED
    List<String>   label_names
    Map<String, Value> properties
}

struct CommandResult {
    ResultKind kind
    bool       ok            // true = success, false = failure (error below)
    int        gdi_status    // raw return code of the underlying GDI call
    bool       created       // get-or-create commands: was the element created (vs matched)?

    // exactly ONE of the following is meaningful, chosen by `kind`:
    int           int_value
    bool          bool_value
    GDI_Vertex_uid vertex_uid
    GDI_Edge_uid   edge_uid
    List<GDI_Vertex_uid> vertex_uids
    List<GDI_Edge_uid>   edge_uids
    List<String>         label_names
    List<String>         property_type_names
    Map<String, Value>   properties
    VertexSnapshot  vertex_snapshot
    EdgeSnapshot    edge_snapshot
    ErrorInfo     error      // { code, message }
}
```

Design notes:

- **`ok` vs `kind`**: `kind` tells you *what* is inside; `ok` tells you *whether the
  command succeeded*. A failed command has `kind = ERROR` and `ok = false`. A successful
  command never has `kind = ERROR`.
- Why an envelope instead of returning raw values? The `TaskHandle` must store results of
  *heterogeneous* commands in one uniform container (a `List<CommandResult>`). A single
  envelope type keeps the buffer/handle code simple and beginner-friendly.
- In a real language this would be a `std::variant` / sealed class / algebraic data type.

---

## 4. `Command` — the abstract base class

### 4.1 Lifecycle of a command

```
   RECORDED ──────► SUBMITTED ──────► EXECUTING ──────► COMPLETED
                                       │                    │
                                       └──► FAILED ◄────────┘ (on error)
```

- **RECORDED**: created by the app and sitting in a `CommandBuffer`. May still be edited.
- **SUBMITTED**: the buffer handed it to the `SubmissionEngine`. Immutable from now on.
- **EXECUTING**: the engine is running its `execute()` against GDI.
- **COMPLETED / FAILED**: terminal states. `result` is now readable.

### 4.2 The base class in pseudo code

```
abstract class Command {
    // ---- identity ----
    CommandType   type          // enum tag, e.g. CREATE_VERTEX
    CommandState  state         // RECORDED / SUBMITTED / EXECUTING / COMPLETED / FAILED
    CommandResult result        // valid only in COMPLETED or FAILED state

    // ---- deferred-input mechanism (see §4.3) ----
    // A command may reference the *future* output of another command in the
    // same buffer, e.g. "edge from vertex #3 in this buffer". We store such
    // references as placeholders, not as concrete IDs.
    Map<String, InputSlot> inputs

    // ---- behavior every concrete command implements ----
    abstract execute(GDI_Connection conn) -> CommandResult
    //   ^ runs the real GDI calls, produces the result, returns it.

    abstract validate(CommandBuffer buffer) -> List<Error>
    //   ^ pre-submit sanity checks: e.g. does the property type exist? Is a
    //     referenced placeholder pointing at a command that is really in this
    //     buffer? Fail fast, before we touch the database.

    // ---- helpers used by the engine ----
    bool hasResult()            // does this command type produce a meaningful result?
    bool isReadOnly()           // hint: reads don't modify the graph (see §8.2)
}
```

`CommandType` enumerates every operation we support:

```
enum CommandType {
    // ── schema: labels ──
    CREATE_LABEL, UPDATE_LABEL, FREE_LABEL,
    GET_LABEL_BY_NAME, GET_ALL_LABELS,
    // ── schema: property types ──
    CREATE_PROPERTY_TYPE, UPDATE_PROPERTY_TYPE, FREE_PROPERTY_TYPE,
    GET_PROPERTY_TYPE_BY_NAME, GET_ALL_PROPERTY_TYPES,
    // ── schema: constraints (filters usable by neighbor/edge reads) ──
    CREATE_CONSTRAINT, ADD_LABEL_CONDITION, ADD_PROPERTY_CONDITION,
    FREE_CONSTRAINT,
    // ── identity / key-based access ──
    TRANSLATE_VERTEX_ID,     // (label, external_id) -> uid | not-found
    ASSOCIATE_VERTEX,        // uid -> holder; mostly an engine building block (§4.4)
    GET_OR_CREATE_VERTEX,    // MERGE semantics, Neo4j MergeAsync analog (§5.7)
    // ── vertex CRUD ──
    CREATE_VERTEX, DELETE_VERTEX, DELETE_VERTEX_GUARDED,
    ADD_LABEL_TO_VERTEX, REMOVE_LABEL_FROM_VERTEX,
    ADD_PROPERTY_TO_VERTEX, SET_PROPERTY_OF_VERTEX,
    UPDATE_PROPERTY_OF_VERTEX, UPDATE_SPECIFIC_PROPERTY_OF_VERTEX,
    REMOVE_PROPERTY_OF_VERTEX, REMOVE_SPECIFIC_PROPERTY_OF_VERTEX,
    // ── edge CRUD ──
    CREATE_EDGE, DELETE_EDGE,
    SET_EDGE_ORIGIN, SET_EDGE_TARGET, SET_EDGE_DIRECTION,
    GET_EDGE_VERTICES, GET_EDGE_DIRECTION,
    ADD_LABEL_TO_EDGE, REMOVE_LABEL_FROM_EDGE,
    ADD_PROPERTY_TO_EDGE, SET_PROPERTY_OF_EDGE,
    UPDATE_PROPERTY_OF_EDGE, REMOVE_PROPERTY_OF_EDGE,
    // ── reads / search ──
    GET_NEIGHBORS, GET_EDGES_OF_VERTEX,
    GET_PROPERTIES_OF_VERTEX, GET_PROPERTIES_OF_EDGE,
    GET_LABELS_OF_VERTEX, GET_LABELS_OF_EDGE,
    COUNT_NEIGHBORS, EXISTS_NEIGHBOR,
    // ── bulk ──
    CREATE_VERTICES_BULK, CREATE_EDGES_BULK,
    // ── composite (engine-side, phase 2) ──
    TRAVERSE_BFS, SHORTEST_PATH,
    // ── transaction control ──
    BEGIN_TRANSACTION, COMMIT, ABORT,
    BEGIN_COLLECTIVE_TRANSACTION, COMMIT_COLLECTIVE
}

// Notes on what changed relative to a naive CRUD enumeration:
// - UPDATE_VERTEX is gone: a GDI vertex has no mutable fields of its own —
//   only labels and properties, which have their own commands.
// - UPDATE_EDGE is replaced by the explicit GDI setters: SET_EDGE_ORIGIN,
//   SET_EDGE_TARGET, SET_EDGE_DIRECTION (plus label/property commands).
// - GDI distinguishes ADD_PROPERTY (append a value to a MULTIPLE_ENTITY
//   property) from SET_PROPERTY (replace); both are exposed.
// - The "specific" property variants operate on ONE value inside a
//   multi-valued property (GDI_UpdateSpecificPropertyOfVertex etc.).
```

### 4.3 Deferred inputs and placeholders — the key idea

The tricky part of batching: **later commands may need the results of earlier commands**.

Example: you want "create vertex A, create vertex B, create an edge A→B, and remember
A's ID". But A's real `GDI_Vertex_uid` does not exist until the backend executes the
`GDI_CreateVertex` call. At recording time we don't have it.

Solution: **placeholders**. A command input is either a concrete value *or* a reference
to the future output of another command in the same buffer.

```
class InputSlot {
    bool is_placeholder
    Value concrete_value // used when is_placeholder == false
    Command source_command // used when is_placeholder == true
    string source_field // which output field, e.g. "vertex_uid" or "created"
}

// Convenience constructors:
InputSlot literal(Value v)                 -> InputSlot { is_placeholder=false, concrete_value=v }
InputSlot ref(Command c, string field)     -> InputSlot { is_placeholder=true,  source_command=c, source_field=field }
```

At `submit()` time the engine **resolves** every placeholder (topologically — commands
execute in buffer order, so by the time command *N* executes, all commands before it have
already produced their results). This gives us intra-batch dependencies **for free**,
just by keeping the buffer ordered.

### 4.4 Stable UIDs vs transaction-local handles — a GDI subtlety that shapes every command

GDI offers *two* ways to refer to a vertex (and similarly for edges):

| Reference kind | Type | Lifetime | How obtained |
|---|---|---|---|
| **stable UID** | `GDI_Vertex_uid` / `GDI_Edge_uid` | survives transactions; globally valid | returned at create; from `GDI_GetEdgesOfVertex`, `GDI_GetNeighborVerticesOfVertex`, `GDI_TranslateVertexID` |
| **transaction-local holder** | `GDI_VertexHolder` / `GDI_EdgeHolder` | valid only *inside one transaction* | `GDI_CreateVertex`, `GDI_AssociateVertex`, `GDI_CreateEdge` |

Every GDI mutator takes a **holder**, never a UID — but holders die with their
transaction, while a command's results must remain meaningful after it completes
(they are read through the `TaskHandle` later, possibly feeding placeholders of a
*later* batch). So the command layer adopts one rule:

> **Placeholders and `CommandResult`s always carry stable UIDs. Holders are
> re-created on demand inside `execute()`.**

The engine provides a small helper so concrete commands never deal with this:

```
function attachVertex(GDI_Transaction tx, GDI_Vertex_uid uid) -> GDI_VertexHolder:
    GDI_VertexHolder v
    status = GDI_AssociateVertex(uid, tx, &v)  // re-attach a known vertex to this tx
    if status != GDI_SUCCESS: raise ExecutionError(status)
    return v
```

Two consequences:

1. A placeholder `ref(CreateVertexCommand, "vertex_uid")` consumed by a command that
   runs in its *own* transaction still works: the consumer re-associates the UID first.
2. Edge commands are the exception that proves the rule — see §5.9: GDI-RMA has no
   `GDI_AssociateEdge`, so an existing edge cannot be re-attached to a new transaction.

### 4.5 External IDs — GDI's built-in primary key

`GDI_CreateVertex(const void* external_id, size_t size, ...)` stores `external_id`
under a built-in property type (`GDI_PROPERTY_TYPE_ID`, see `gdi_vertex.c`), and
`GDI_TranslateVertexID(&found, &uid, label, external_id, size, tx)` resolves it through
a hash index keyed on **(label, external_id)** (`gdi_index.c`).

This plays exactly the role that `(label, keyProperty, keyValue)` plays in the
reference Neo4j layer's `GetAsync` / `MergeAsync`. Two design consequences:

- `CreateVertexCommand` takes `label` + `external_id` as **first-class inputs** —
  they form the vertex's key, not optional decoration (see the corrected §5.1).
- The get-or-create (MERGE) pattern that every idempotent graph loader needs becomes
  a single command: `GetOrCreateVertexCommand` (§5.7).

Caveat: uniqueness enforcement for external IDs is unfinished in GDI-RMA (explicit
TODOs in `gdi_index.c` / `gdi_vertex.c`). The command layer therefore (a) detects
duplicate `(label, external_id)` pairs **within one buffer** at `validate()` time, and
(b) surfaces `GDI_WARNING_NON_UNIQUE_ID` as a first-class warning state instead of
assuming it cannot happen (§8.7).

---

## 5. Concrete command classes

Each class stores its own arguments and implements `execute()` by calling the matching
GDI functions. Shown for the most important ones; the rest follow the same pattern.

### 5.1 `CreateVertexCommand`

```
class CreateVertexCommand extends Command {
    type = CREATE_VERTEX
    InputSlot label        // GDI_Label (or placeholder) — part of the vertex's key (§4.5)
    InputSlot external_id  // optional key bytes; NULL/0 = anonymous vertex
    InputSlot properties   // optional Map<String, Value>, keyed by PROPERTY-TYPE NAME
                           // (the engine resolves names -> GDI_PropertyType handles,
                           //  cached per database; see note after the code block)

    function execute(conn) -> CommandResult {
        GDI_Transaction tx
        status = GDI_StartTransaction(conn.db, &tx)
        if status != GDI_SUCCESS:
            return CommandResult { kind=ERROR, ok=false, gdi_status=status, error=... }

        // GDI_CreateVertex takes the external ID *and* returns a transaction-local holder
        GDI_VertexHolder v
        status = GDI_CreateVertex(external_id_bytes, external_id_size, tx, &v)
        if status != GDI_SUCCESS:
            GDI_CloseTransaction(&tx, GDI_TRANSACTION_ABORT)
            return CommandResult { kind=ERROR, ok=false, ... }

        if label was provided: GDI_AddLabelToVertex(label_resolved, v)
        for each (name, value) in properties_resolved:
            ptype = conn.propertyTypeByName(name)   // GDI_GetPropertyTypeFromName on cache miss
            GDI_SetPropertyOfVertex(value.bytes, value.count, ptype, v)

        status = GDI_CloseTransaction(&tx, GDI_TRANSACTION_COMMIT)
        if status != GDI_SUCCESS:
            return CommandResult { kind=ERROR, ok=false, ... }

        // Success: expose the STABLE uid (the holder is dead with its transaction — §4.4)
        return CommandResult { kind=VERTEX_UID, ok=true, vertex_uid=v.uid }
    }

    function validate(buffer) -> List<Error> {
        errors = []
        for each placeholder input (label, external_id, properties):
            if not buffer.contains(input.source_command):
                errors.add("placeholder references a command not in this buffer")
        for each property name in properties:
            if not conn.knowsPropertyType(name):
                errors.add("no such property type registered: " + name)
        if buffer.duplicateKey(label, external_id):
            errors.add("another command in this buffer creates the same (label, external_id)")
        return errors
    }
}
```

> **Beginner note**: we wrap each command in its own small transaction *by default*.
> §8.1 shows how `BEGIN_TRANSACTION` / `COMMIT` commands can instead group many commands
> into one single GDI transaction for atomicity and speed.

### 5.2 `CreateEdgeCommand` — demonstrates placeholder use

```
class CreateEdgeCommand extends Command {
    type        = CREATE_EDGE
    InputSlot   origin          // vertex uid or ref(CreateVertexCommand, "vertex_uid")
    InputSlot   target          // vertex uid or ref(CreateVertexCommand, "vertex_uid")
                                // (uids are re-associated to holders inside execute — §4.4)
    InputSlot   direction       // GDI_EDGE_DIRECTED or GDI_EDGE_UNDIRECTED
    InputSlot   properties      // optional

    function execute(conn) -> CommandResult {
        tx = start transaction on conn
        // uids (possibly from placeholders) are re-associated to holders first — §4.4
        GDI_VertexHolder o = attachVertex(tx, origin_resolved)
        GDI_VertexHolder t = attachVertex(tx, target_resolved)
        GDI_EdgeHolder e
        status = GDI_CreateEdge(direction_resolved, o, t, &e)
        if status != GDI_SUCCESS: abort; return error result
        // fill labels/properties on e (same pattern as §5.1)
        commit
        // return the STABLE edge uid; the holder dies with the transaction.
        // caveat: only this batch can use it until GDI_AssociateEdge exists (§5.9)
        return { kind=EDGE_UID, ok=true, edge_uid=e.uid }
    }
}
```

### 5.3 Read commands — `GetNeighborsCommand` (a "search")

```
class GetNeighborsCommand extends Command {
    type = GET_NEIGHBORS
    InputSlot  vertex      // uid or placeholder
    InputSlot  orientation // bitwise OR of GDI_EDGE_INCOMING | GDI_EDGE_OUTGOING
                           // (Neo4j Direction.Outgoing / Incoming / Any)
    InputSlot  constraint  // optional ref(CreateConstraintCommand) — label/property filter
    InputSlot  max_count   // optional cap (maps to GDI's count argument)

    function isReadOnly() -> bool { return true }

    function execute(conn) -> CommandResult {
        tx = start transaction on conn
        v = attachVertex(tx, vertex_resolved)          // §4.4 helper
        List<GDI_Vertex_uid> uids
        size_t resultcount
        status = GDI_GetNeighborVerticesOfVertex(uids.buffer, max_count, &resultcount,
                                                 constraint_resolved,
                                                 orientation_resolved, v)
        close tx
        if status != GDI_SUCCESS: return error result
        return CommandResult { kind=VERTEX_LIST, ok=true, vertex_uids=uids[0..resultcount] }
    }
}
```

### 5.4 Update / delete commands

```
class UpdatePropertyCommand extends Command {
    type      = UPDATE_PROPERTY_OF_VERTEX
    InputSlot vertex        // ID or placeholder
    InputSlot property_type
    InputSlot new_value
    execute: GDI_UpdatePropertyOfVertex(...) inside a transaction
    result:  { kind=EMPTY, ok=status==GDI_SUCCESS }
}

class DeleteVertexCommand extends Command {
    type = DELETE_VERTEX
    InputSlot vertex // uid or placeholder
    execute:
        tx = start transaction on conn
        v = attachVertex(tx, vertex_resolved)   // §4.4 helper
        status = GDI_FreeVertex(&v)
        commit
    result:  { kind=EMPTY, ok=status==GDI_SUCCESS }
}

class DeleteVertexGuardedCommand extends Command {
    type = DELETE_VERTEX_GUARDED   // Neo4j DeleteAsync(detach: false)
    InputSlot vertex
    execute:
        tx = start transaction on conn
        v = attachVertex(tx, vertex_resolved)
        // pre-check: refuse to delete a vertex that still has edges
        size_t edgecount = probe with GDI_GetEdgesOfVertex(buf_size=0, ..., v)
        if edgecount > 0:
            abort tx
            return { kind=ERROR, ok=false,
                     error="vertex still has relationships (RelationshipExists analog)" }
        GDI_FreeVertex(&v)
        commit
    result:  { kind=EMPTY, ok=true }
}
```

> **Delete semantics, grounded in `gdi_vertex.c`**: `GDI_FreeVertex` *already cascades* —
> it marks every edge associated with the vertex for deletion. So plain
> `DeleteVertexCommand` corresponds to Neo4j's `DETACH DELETE`, and the *guarded* variant
> (fail if any relationship exists, like `DeleteAsync(detach: false)`) is implemented as a
> cheap `GDI_GetEdgesOfVertex` count probe before the free.

### 5.5 Transaction-control commands (optional but powerful)

```
class BeginTransactionCommand extends Command {
    type = BEGIN_TRANSACTION
    execute(conn):
        conn.user_tx = start GDI transaction      // engine parks it on the connection
        return { kind=EMPTY, ok=true }
}

class CommitCommand extends Command {
    type = COMMIT
    execute(conn):
        status = GDI_CloseTransaction(&conn.user_tx, GDI_TRANSACTION_COMMIT)
        conn.user_tx = NULL
        return { kind=EMPTY, ok=status==GDI_SUCCESS }
}

class AbortCommand extends Command {
    type = ABORT
    execute(conn):
        status = GDI_CloseTransaction(&conn.user_tx, GDI_TRANSACTION_ABORT)
        conn.user_tx = NULL
        return { kind=EMPTY, ok=status==GDI_SUCCESS }
}

class BeginCollectiveTransactionCommand extends Command {
    type = BEGIN_COLLECTIVE_TRANSACTION
    execute(conn):
        status = GDI_StartCollectiveTransaction(conn.db, &conn.user_tx)
        return { kind=EMPTY, ok=status==GDI_SUCCESS }
}
// COMMIT_COLLECTIVE mirrors COMMIT but calls GDI_CloseCollectiveTransaction.
```

When the engine sees `BEGIN_TRANSACTION`, it starts one GDI transaction and keeps it open
on the connection; subsequent commands *reuse* it instead of opening their own; `COMMIT`/`ABORT`
closes it. This maps 1:1 to GDI's `GDI_StartTransaction` / `GDI_CloseTransaction` model.
The collective variants map to `GDI_StartCollectiveTransaction` / `GDI_CloseCollectiveTransaction`
(§8.6). Note that collective transactions are **read-only** in GDI, so `validate()` must
reject any non-read command between `BEGIN_COLLECTIVE_TRANSACTION` and `COMMIT_COLLECTIVE`.

### 5.6 Key-based access: `TranslateVertexIDCommand` — the "find by key" read

The Neo4j reference layer fetches nodes precisely via
`GetAsync(label, keyProperty, keyValue)`. The GDI equivalent is the (label, external_id)
hash index of §4.5:

```
class TranslateVertexIDCommand extends Command {   // Neo4j FindAsync / GetAsync
    type = TRANSLATE_VERTEX_ID
    InputSlot label
    InputSlot external_id

    function isReadOnly() -> bool { return true }

    function execute(conn) -> CommandResult {
        tx = start transaction on conn
        bool found; GDI_Vertex_uid uid
        status = GDI_TranslateVertexID(&found, &uid, label_resolved,
                                       external_id_bytes, external_id_size, tx)
        close tx
        if status != GDI_SUCCESS: return error result
        // kind=EMPTY + found=false means "valid lookup, no such vertex"
        // (the Neo4j analog returns null rather than throwing)
        return { kind = found ? VERTEX_UID : EMPTY, ok=true, vertex_uid=uid, bool_value=found }
    }
}
```

If the caller wants the vertex's data (not just its uid), follow with
`GetPropertiesOfVertexCommand` / `GetLabelsOfVertexCommand` (§5.11), or read the built-in
`GDI_PROPERTY_TYPE_ID` property back as part of a snapshot.

### 5.7 `GetOrCreateVertexCommand` — MERGE semantics

Neo4j's `MergeAsync` ("find by key; create only if missing; report Created vs Matched")
is the workhorse of idempotent loaders. Composed from §5.6 + §5.1:

```
class GetOrCreateVertexCommand extends Command {   // Neo4j MergeAsync
    type = GET_OR_CREATE_VERTEX
    InputSlot label
    InputSlot external_id
    InputSlot on_create_properties // applied ONLY when the vertex is created
    InputSlot on_match_properties  // applied ONLY when it already exists
                                   // (Neo4j's ON CREATE SET / ON MATCH SET)

    function execute(conn) -> CommandResult {
        tx = start transaction on conn
        bool found; GDI_Vertex_uid uid
        GDI_TranslateVertexID(&found, &uid, label_resolved, external_id_bytes, size, tx)
        GDI_VertexHolder v
        if found:
            GDI_AssociateVertex(uid, tx, &v)
            apply on_match_properties to v
            created = false
        else:
            GDI_CreateVertex(external_id_bytes, size, tx, &v)
            if label provided: GDI_AddLabelToVertex(label_resolved, v)
            apply on_create_properties to v
            created = true
        commit tx  // on failure: abort, return error result
        return { kind=VERTEX_UID, ok=true, vertex_uid=v.uid, created=created }
    }
}
```

The `created` flag mirrors Neo4j's `MergeResult.Outcome`; loaders use it for statistics
and to decide whether to (re)link edges.

### 5.8 Bulk commands — one transaction per N rows

Neo4j's `CreateManyAsync` batches N rows into a single `UNWIND` statement (one network
round trip). GDI has no multi-create call, but the command layer can still deliver the
essence: **one transaction and one engine dispatch for N creates** instead of N.

```
class CreateVerticesBulkCommand extends Command {  // Neo4j CreateManyAsync
    type = CREATE_VERTICES_BULK
    List<VertexRow> rows   // each row: {label, external_id, properties}
                           // external_id may be a placeholder per row

    function execute(conn) -> CommandResult {
        tx = start ONE transaction on conn
        n = 0
        for row in rows:
            GDI_VertexHolder v
            status = GDI_CreateVertex(row.external_id.bytes, row.external_id.size, tx, &v)
            if status != GDI_SUCCESS: abort; return error result  // FAIL_FAST inside the bulk
            apply row label + properties
            n++
        commit tx
        return { kind=INT, ok=true, int_value=n }
    }
}
```

- Under `FAIL_FAST` a single bad row aborts the whole bulk (they share a transaction);
  under `CONTINUE_ON_ERROR` the engine may fall back to one transaction per row so good
  rows still land (§8.3).
- `CreateEdgesBulkCommand` is the same shape with `{origin_uid, direction, target_uid,
  properties}` rows, where the uids may be placeholders.
- Bulk rows are a natural fit for placeholders: a loader typically does
  `bulk(GetOrCreateVertex…)` for endpoints first, then `bulk(CreateEdge…)` referencing them.

### 5.9 Edge mutation and inspection — plus the `GDI_AssociateEdge` caveat

GDI mutates edges through explicit setters, each of which becomes a command:

```
class SetEdgeDirectionCommand extends Command {
    type = SET_EDGE_DIRECTION
    InputSlot edge      // EdgeHolder ref or placeholder (see caveat below)
    InputSlot direction // GDI_EDGE_DIRECTED | GDI_EDGE_UNDIRECTED
    execute: GDI_SetDirectionTypeOfEdge(direction_resolved, edge_holder)
}
class SetEdgeOriginCommand  ... GDI_SetOriginVertexOfEdge(new_origin_holder, edge_holder)
class SetEdgeTargetCommand  ... GDI_SetTargetVertexOfEdge(new_target_holder, edge_holder)

class GetEdgeVerticesCommand extends Command {   // read both endpoints
    type = GET_EDGE_VERTICES
    execute: GDI_GetVerticesOfEdge(&origin_uid, &target_uid, edge_holder)
    result:  { kind=VERTEX_UID_PAIR, ok=..., vertex_uid=origin_uid,
               /* second uid carried in a pair field */ ... }
}
class GetEdgeDirectionCommand ... GDI_GetDirectionTypeOfEdge(&dtype, edge_holder) -> { kind=INT }
```

> **Caveat — existing edges are out of reach in current GDI-RMA.** `GDI_EdgeHolder`s are
> only obtainable inside the transaction that created the edge, and GDI-RMA does **not**
> implement `GDI_AssociateEdge` (listed under "Not Implemented Functions" in
> `src/README.md`; the Linkbench benchmark in `benchmarks/benchmark.oltp.lb.c` had to drop
> its delete/update-edge queries for exactly this reason). Therefore every edge-taking
> command (`SET_EDGE_*`, `DELETE_EDGE`, edge property/label commands) accepts **only** a
> placeholder referring to a `CreateEdgeCommand` *in the same buffer* — the holder is still
> alive — and `validate()` rejects a literal edge-uid input with a clear
> "GDI_AssociateEdge not available in this GDI implementation" error.
> When a future GDI implements `GDI_AssociateEdge`, the same commands start accepting
> stable edge uids with no API change (the §4.4 rule kicks in).

### 5.10 Schema-management commands (labels, property types, constraints)

The original sketch only had `CREATE_LABEL` / `CREATE_PROPERTY_TYPE`. Full schema CRUD:

| Command | GDI call(s) | Result |
|---|---|---|
| `CreateLabelCommand` | `GDI_CreateLabel(name, db, &label)` | LABEL_LIST(names) / handle cached on conn |
| `GetLabelByNameCommand` | `GDI_GetLabelFromName(&label, name, db)` | BOOL + found |
| `GetAllLabelsCommand` | `GDI_GetAllLabelsOfDatabase(...)` | LABEL_LIST |
| `UpdateLabelCommand` | `GDI_UpdateLabel(name, label)` | EMPTY |
| `FreeLabelCommand` | `GDI_FreeLabel(&label)` | EMPTY |
| `CreatePropertyTypeCommand` | `GDI_CreatePropertyType(name, etype, dtype, stype, count, db, &ptype)` | EMPTY |
| `GetPropertyTypeByNameCommand` | `GDI_GetPropertyTypeFromName(&ptype, name, db)` | BOOL + found |
| `GetAllPropertyTypesCommand` | `GDI_GetAllPropertyTypesOfDatabase(...)` | PROPERTY_TYPE_LIST |
| `UpdatePropertyTypeCommand` / `FreePropertyTypeCommand` | `GDI_UpdatePropertyType(...)` / `GDI_FreePropertyType(&ptype)` | EMPTY |
| `CreateConstraintCommand` | `GDI_CreateConstraint(db, &constraint)` + `GDI_CreateSubconstraint(db, &sub)` + `GDI_AddSubconstraintToConstraint(sub, constraint)` | handle (usable as placeholder) |
| `AddLabelConditionCommand` | `GDI_AddLabelConditionToSubconstraint(label, op, sub)` | EMPTY |
| `AddPropertyConditionCommand` | `GDI_AddPropertyConditionToSubconstraint(ptype, op, value, count, sub)` | EMPTY |

Why constraints matter: `GDI_GetEdgesOfVertex` and `GDI_GetNeighborVerticesOfVertex` take
a `GDI_Constraint` as their **only** structured filter. `GDI_Op` offers EQUAL, NOTEQUAL,
GREATER, EQGREATER, SMALLER, EQSMALLER (`gdi_operation.h`) — so Neo4j-style
`Where(property, op, value)` filters on neighborhoods map onto constraint subconditions,
while label conditions cover `Where(label)`-style filtering. (Arbitrary *global* scans
remain impossible — see the coverage matrix §11.)

Callers refer to labels and property types **by name** in all commands; the engine
resolves names to handles through per-database caches (`GDI_GetLabelFromName` /
`GDI_GetPropertyTypeFromName` on miss), so application code never touches raw handles.

### 5.11 Read-back and cheap-read commands

| Command | GDI call(s) | Result |
|---|---|---|
| `GetPropertiesOfVertexCommand` | `GDI_GetPropertiesOfVertex` (two-phase: query size, then fetch into buffer) | PROPERTY_MAP or SNAPSHOT |
| `GetPropertiesOfEdgeCommand` | `GDI_GetPropertiesOfEdge` | PROPERTY_MAP — **not implemented in GDI-RMA; phase 2 / degrades** (§8.8) |
| `GetLabelsOfVertexCommand` / `GetLabelsOfEdgeCommand` | `GDI_GetAllLabelsOfVertex/Edge` | LABEL_LIST |
| `GetAllPropertyTypesOfVertex/EdgeCommand` | `GDI_GetAllPropertyTypesOfVertex/Edge` | PROPERTY_TYPE_LIST |
| `GetEdgesOfVertexCommand` | `GDI_GetEdgesOfVertex(uids, count, &rc, constraint, orientation, holder)` | EDGE_LIST |
| `CountNeighborsCommand` | wraps `GDI_GetNeighborVerticesOfVertex` with a 0-sized probe or capped fetch | INT |
| `ExistsNeighborCommand` | neighbor fetch capped at 1, short-circuits | BOOL |

`CountNeighborsCommand` / `ExistsNeighborCommand` exist because Neo4j's `CountAsync` /
`ExistsAsync` are the most common read terminals; a caller should not have to pull a whole
neighborhood just to know whether it is empty.

### 5.12 Composite / engine-side commands (phase 2)

Some Neo4j-reference features have **no single GDI equivalent**; the engine composes them
from the primitives above, inside one transaction:

- `TraverseBFSCommand` (Neo4j `Paths.Walk`): repeated `GDI_GetNeighborVerticesOfVertex`
  level by level, up to `maxHops`, with optional per-hop constraint filters; returns
  VERTEX_LIST per level or PATH-like structures. Phase 2, parallelizable across frontier
  shards (§8.2).
- `ShortestPathCommand` (Neo4j `ShortestAsync`): bidirectional BFS until the fronts meet;
  returns a VERTEX_UID list or EMPTY. Phase 2.
- Aggregates (Neo4j `AggregateAsync`: count/collect/sum/avg/min/max): no GDI aggregate
  exists at all. The engine can compute them only over data it has already fetched
  (e.g. a property read over a neighbor list). Documented as a **semantic gap** in §11 —
  graph-wide aggregates are out of reach for this layer by design.

---

## 6. `TaskHandle` — the async receipt

### 6.1 Semantics

`CommandBuffer.submit()` returns immediately with a `TaskHandle`. The handle lets the
caller:

- **poll**: `isDone()` — non-blocking; for UI loops / progress bars.
- **block**: `wait(timeout)` — sleep until the whole buffer finished (or timeout).
- **collect**: `getResult(command)` — wait if needed, then return that command's
  `CommandResult`. Only valid after completion.
- **bulk collect**: `getAllResults()` — list of results in command order.

### 6.2 Implementation sketch

```
class TaskHandle {
    CommandBuffer     buffer        // the submitted commands (now immutable)
    AtomicInt         completed_count   // engine increments as commands finish
    AtomicBool        finished          // true when all commands done (or batch failed)
    Optional<Error>   batch_error       // set if the whole batch was aborted
    ConditionVariable cv              // engine signals on each completion

    function isDone() -> bool:
        return finished.load()

    function wait(timeout = FOREVER) -> bool:
        cv.waitUntil(() -> finished.load(), timeout)
        return finished.load()

    function getResult(Command cmd) -> CommandResult:
        assert buffer.contains(cmd)          // paranoia: right handle?
        wait()                                // block until batch finished
        if batch_error.isSet: rethrow/return error result
        return cmd.result

    function getAllResults() -> List<CommandResult>:
        wait()
        return [ cmd.result for cmd in buffer.commands ]
}
```

Internals the engine uses:

```
function markDone(Command cmd, CommandResult r):
    cmd.state  = r.ok ? COMPLETED : FAILED
    cmd.result = r
    completed_count.increment()
    if completed_count == buffer.size():
        finished.store(true)
    cv.notifyAll()
```

**Why one handle per buffer (not per command)?** Simplicity and batch atomicity. The
application's mental model is "this whole batch is one unit of work". Per-command
granular waiting is still available via `getResult(cmd)`, which blocks only until the
*batch* completes — fine because commands run sequentially anyway (see §8.2 for the
read-parallel alternative).

---

## 7. `CommandBuffer` and `SubmissionEngine`

### 7.1 `CommandBuffer`

```
class CommandBuffer {
    List<Command>   commands
    SubmissionEngine engine     // shared executor (thread pool / MPI rank)
    GDI_Connection  conn        // database this buffer targets
    bool            submitted   // one-shot: a buffer can be submitted exactly once

    // ---- recording phase ----
    function add(Command cmd) -> Command:
        assert not submitted
        commands.add(cmd)
        return cmd                       // lets the caller keep a reference for getResult()

    function size()  -> int
    function clear()                  // only allowed before submit

    // ---- submission phase ----
    function submit() -> TaskHandle:
        assert not submitted
        submitted = true

        // 1. validate everything BEFORE touching the database
        for cmd in commands:
            for err in cmd.validate(this):
                fail fast: return a TaskHandle pre-loaded with batch_error = err

        // 2. snapshot commands into an immutable batch
        batch = copyOf(commands)

        // 3. create the handle and hand the batch to the engine
        handle = TaskHandle { buffer=batch }
        engine.enqueue(batch, handle)
        return handle
}
```

### 7.2 `SubmissionEngine`

The engine is a small scheduler. Minimal version: a single background worker thread with
a FIFO queue. Distributed version: one rank/worker per GDI database (GDI-RMA is
MPI-based, so a natural mapping is "buffer executes on the rank owning the data").

```
class SubmissionEngine {
    Queue<Job>        queue       // Job = { batch, handle }
    BackgroundThread  worker

    function enqueue(List<Command> batch, TaskHandle handle):
        queue.push({batch, handle})

    workerLoop():
        forever:
            job = queue.popBlocking()
            executeBatch(job.batch, job.handle)

    function executeBatch(batch, handle):
        for cmd in batch:
            // resolve placeholders using results of earlier commands
            resolveInputs(cmd, batch)

            cmd.state = EXECUTING
            result = cmd.execute(conn)     // real GDI calls happen HERE

            // policy: fail-fast vs. continue-on-error (§8.3)
            if not result.ok and POLICY == FAIL_FAST:
                handle.batch_error = result.error
                handle.finished = true
                handle.cv.notifyAll()
                return

            handle.markDone(cmd, result)

        handle.finished = true
        handle.cv.notifyAll()
```

### 7.3 Placeholder resolution

```
function resolveInputs(Command cmd, List<Command> batch):
    for each InputSlot slot in cmd.inputs:
        if slot.is_placeholder:
            source = slot.source_command
            assert batch.indexOf(source) < batch.indexOf(cmd)   // must be EARLIER
            assert source.state == COMPLETED                     // engine runs in order
            slot.concrete_value = source.result.getField(slot.source_field)
```

Because the engine executes strictly in buffer order and resolves just-in-time, a command
can only depend on commands *before* it — which is exactly the intuitive semantics of
"record in the order you want things to happen".

---

## 8. Design decisions, trade-offs, and edge cases

### 8.1 One transaction per command vs. one transaction per buffer

| Strategy | Pros | Cons |
|---|---|---|
| **Auto-transaction per command** (default) | Simple; partial failures leave earlier commands intact | More `GDI_Start/CloseTransaction` overhead |
| **`BEGIN_TRANSACTION` … `COMMIT` commands** | Single atomic unit; matches GDI's transaction model directly; fewer round trips | One failing command may force you to abort the whole batch |

Recommendation: default to per-command transactions; document that power users wrap
groups in explicit `BEGIN_TRANSACTION`/`COMMIT` for performance and atomicity.
Note GDI also offers `GDI_StartCollectiveTransaction` / `GDI_CloseCollectiveTransaction`
for multi-rank coordination — the same two commands can map to those when the engine is
running in collective mode.

### 8.2 Sequential vs. parallel reads

- **Sequential (chosen here)**: trivially correct; results deterministic; single worker
  thread is enough for a first version.
- **Parallel reads**: `isReadOnly()` commands with no placeholder dependencies could be
  farmed out to a thread pool. This complicates `TaskHandle` (completion counting) and
  ordering. Phase-2 optimization.

### 8.3 Error policy: fail-fast vs. continue

When command *k* of *n* fails:

- **FAIL_FAST** (default): stop the batch, set `handle.batch_error`, mark remaining
  commands as `FAILED` with error "batch aborted". Safest for writes.
- **CONTINUE_ON_ERROR**: run the rest, let the caller inspect each result. Useful for
  bulk best-effort ingestion. Make it a per-`submit()` option:
  `submit(policy = FAIL_FAST | CONTINUE_ON_ERROR)`.

### 8.4 Buffer submitted twice

Forbidden — `submit()` consumes the buffer (like a moved value). Attempting `add()` or
a second `submit()` after submission is a programming error and should raise/assert
immediately. Rationale: silent reuse would invite double-execution of writes.

### 8.5 Lifetime of inputs

Command arguments (labels, property values) must outlive execution. Rule: `submit()`
**deep-copies** everything it needs into the batch snapshot, so the caller may freely
reuse/modify its own buffers right after `submit()` returns. This is essential for the
async model to be usable.

### 8.6 Interaction with MPI/GDI-RMA specifics

- `GDI_Init`/`GDI_Finalize` remain outside this layer (they bracket the whole process,
  like `MPI_Init`/`MPI_Finalize`). The command layer starts *after* initialization.
- Collective transactions: if a buffer is built identically on all ranks (e.g. graph
  loading), the engine can choose `GDI_StartCollectiveTransaction` for the whole batch —
  one more reason to keep `BEGIN_TRANSACTION`/`COMMIT` as explicit commands.

### 8.7 Uniqueness of external IDs

Duplicate `(label, external_id)` pairs are detectable within one buffer at `validate()`
time (cheap, offline). Across buffers / processes, GDI-RMA currently under-enforces
uniqueness (TODOs in `gdi_index.c` / `gdi_vertex.c`), so the engine treats
`GDI_WARNING_NON_UNIQUE_ID` as a first-class *warning* state on the `CommandResult`
(success + warning), never silently swallowed. Applications that rely on the key
uniqueness invariant (MERGE-style loaders) should enable a strict mode where the warning
is promoted to an error.

### 8.8 Working around GDI-RMA's not-yet-implemented surface

`src/README.md` lists GDI functions that GDI-RMA does not implement. The ones that touch
this design, and the layer's policy for each:

| Missing in GDI-RMA | Impact on commands | Policy |
|---|---|---|
| `GDI_AssociateEdge` | cannot re-attach an existing edge to a new transaction | edge commands take same-buffer placeholders only; `validate()` rejects literal edge uids (§5.9) |
| `GDI_GetPropertiesOfEdge`, `GDI_GetAllPropertyTypesOfEdge` | edge property read-back unavailable | `GET_PROPERTIES_OF_EDGE` marked phase 2; fails validation with a clear message until the backend catches up |
| `GDI_GetErrorClass`, `GDI_GetErrorString` | no error strings from the backend | the layer ships its own error table for the GDI error codes in `gdi.h` |
| indexes (`GDI_CreateIndex` & friends) | none for the command layer (constraints cover filtering) | out of scope |

Rule of thumb: **degrade or reject at `validate()` time — never fail silently halfway
through a batch.**

## 9. End-to-end usage example

```
// ---------- setup (once per process) ----------
GDI_Init(...)
GDI_CreateDatabase(&params, sizeof(params), &db)
engine = new SubmissionEngine(db)

// ---------- application code ----------
buf = new CommandBuffer(engine, db)

// record: two vertices + an edge between them, all deferred
// (label + external_id form the vertex key, §4.5 — MERGE-style loaders use
//  GetOrCreateVertexCommand instead, §5.7)
vAlice = buf.add(new CreateVertexCommand(label = person_label,
                                         external_id = "alice",
                                         properties = {"name": "Alice"}))
vBob   = buf.add(new CreateVertexCommand(label = person_label,
                                         external_id = "bob",
                                         properties = {"name": "Bob"}))
eKnows = buf.add(new CreateEdgeCommand(
                     origin   = ref(vAlice, "vertex_uid"),  // placeholder!
                     target   = ref(vBob,   "vertex_uid"),  // placeholder!
                     direction = GDI_EDGE_DIRECTED,
                     properties = {"since": 2020}))

// record a read that depends on vAlice
neighbors = buf.add(new GetNeighborsCommand(
                        vertex = ref(vAlice, "vertex_uid"),
                        orientation = GDI_EDGE_OUTGOING))

// ---------- submit and do other work ----------
handle = buf.submit()          // returns IMMEDIATELY

... do unrelated CPU work here ...   // overlap computation with the batch

// ---------- collect results ----------
ok = handle.wait(timeout = 30s)
if not ok: panic "batch timed out"

rEdge = handle.getResult(eKnows)
if rEdge.ok:
    print "created edge", rEdge.edge_uid
else:
    print "edge creation failed:", rEdge.error.message

rNbrs = handle.getResult(neighbors)
for uid in rNbrs.vertex_uids:
    print "neighbor uid:", uid

// bulk variant:
for (cmd, res) in zip(buf.commands, handle.getAllResults()):
    log cmd.type, "->", res.ok

// ---------- teardown ----------
engine.shutdown()
GDI_FreeDatabase(&db)
GDI_Finalize()
```

Key observations from the example:

1. **Nothing touched the database** until `submit()`.
2. `vAlice`/`vBob` were used to build the edge *before their uids existed* — placeholders
   made this possible.
3. `submit()` overlapped with "unrelated CPU work": this is the comm/compute overlap the
   GDI paper calls essential for performance at scale.
4. Results were fetched per command, after a single `wait()`.

---

## 10. Minimal viable implementation order

A suggested build sequence, easiest pieces first:
1. `CommandResult` + `CommandType` + `CommandState` — plain data types.
2. `Command` base class with `execute()` and `validate()`; implement
   `CreateVertexCommand` only.
3. `TaskHandle` with `wait()` + `getResult()` (backed by a mutex/condition variable).
4. `SubmissionEngine` with a single worker thread and an in-memory queue.
5. `CommandBuffer.add()` / `submit()` with validation + fail-fast policy.
6. Add `InputSlot` / placeholder resolution (resolveInputs) + the §4.4
   `attachVertex` helper.
7. Vertex-side commands: key access (`TRANSLATE_VERTEX_ID`), `GET_OR_CREATE_VERTEX`,
   labels, properties (incl. add/set/update/specific/remove variants),
   `DELETE_VERTEX` (+guarded).
8. Edge creation + same-buffer edge commands; schema commands (labels, property types);
   read-backs (`GET_PROPERTIES_OF_*`, `GET_LABELS_OF_*`, `COUNT`/`EXISTS`).
9. `BEGIN_TRANSACTION` / `COMMIT` / `ABORT` (+ collective variants) for batch atomicity;
   bulk commands; constraint commands.
10. (Phase 2) parallel read execution; collective transactions for GDI-RMA;
    continue-on-error policy; command-buffer reuse via `reset()` after completion;
    composite `TRAVERSE_BFS` / `SHORTEST_PATH`; edge commands on stable uids once
    `GDI_AssociateEdge` lands.

## 11. Neo4j-style CRUD coverage matrix

Does this command set truly cover what a modern graph-database CRUD API (the reference
Neo4j layer in `semevia-next`: `Graph` / `INodeApi` / `IRelationshipApi` / `IPathApi` /
`GraphTransaction`) offers? Operation by operation:

| Neo4j reference API | Command(s) here | Underlying GDI call(s) | Status |
|---|---|---|---|
| `Nodes.CreateAsync` | `CreateVertexCommand` | `GDI_CreateVertex` + `GDI_AddLabelToVertex` + `GDI_SetPropertyOfVertex` | ✅ §5.1 |
| `Nodes.CreateManyAsync` | `CreateVerticesBulkCommand` | loop of the above in ONE transaction | ✅ §5.8 (batched, not single-call) |
| `Nodes.MergeAsync` (ON CREATE / ON MATCH) | `GetOrCreateVertexCommand` | `GDI_TranslateVertexID` → `GDI_AssociateVertex` or `GDI_CreateVertex` | ✅ §5.7 |
| `Nodes.GetAsync` / `FindAsync` (by unique key) | `TranslateVertexIDCommand` (+ read-backs) | `GDI_TranslateVertexID` | ✅ §5.6 |
| `Nodes.Match(...).Where/WhereIn/WhereBetween/WhereNull/WhereAny` | `CreateConstraintCommand` + `AddLabelConditionCommand` / `AddPropertyConditionCommand`, consumed by `GET_NEIGHBORS` / `GET_EDGES_OF_VERTEX` | constraint subconditions (`GDI_Op`: =, ≠, >, ≥, <, ≤) | ⚠ partial — filters apply to **neighborhoods**, not global scans (gap G1) |
| `Nodes.Match(...).OrderBy/Skip/Limit` | ordering/paging of fetched lists | — | ⚠ engine-side, phase 2 |
| `NodeQuery.SetAsync` (merge/replace, remove keys) | `SetPropertyOfVertexCommand`, `RemovePropertyOfVertexCommand` | `GDI_SetPropertyOfVertex`, `GDI_RemovePropertiesFromVertex` | ✅ (Expr.Increment-style atomic increments: engine-side RMW, phase 2) |
| `NodeQuery.AddLabelsAsync` / `RemoveLabelsAsync` | `AddLabelToVertexCommand` / `RemoveLabelFromVertexCommand` | `GDI_AddLabelToVertex` / `GDI_RemoveLabelFromVertex` | ✅ |
| `NodeQuery.DeleteAsync(detach: true)` | `DeleteVertexCommand` | `GDI_FreeVertex` (cascades to edges) | ✅ §5.4 |
| `NodeQuery.DeleteAsync(detach: false)` | `DeleteVertexGuardedCommand` | `GDI_GetEdgesOfVertex` probe + `GDI_FreeVertex` | ✅ §5.4 |
| `NodeQuery.CountAsync` / `ExistsAsync` / `AggregateAsync` | `CountNeighborsCommand` / `ExistsNeighborCommand`; aggregates over fetched sets only | neighbor fetches | ⚠ neighborhood-level only; graph-wide aggregates unsupported (gap G4) |
| `Relationships.CreateAsync` | `CreateEdgeCommand` | `GDI_CreateEdge` (+ endpoint `attachVertex`) | ✅ §5.2 |
| `Relationships.CreateAsync(createMissingNodes: true)` | endpoints as `GET_OR_CREATE_VERTEX` placeholders feeding `CreateEdgeCommand` | composed | ✅ via placeholders |
| `Relationships.MergeAsync` | get-or-create endpoints + existence check over `GetEdgesOfVertex` | composed | ⚠ phase 2; O(degree) check, no edge index (gap G2) |
| `Relationships.OfType().From/To/Between().Where(...)` | `GetEdgesOfVertexCommand` + `GetEdgeVerticesCommand` (+ constraint filters) | `GDI_GetEdgesOfVertex`, `GDI_GetVerticesOfEdge` | ⚠ over same-buffer/attachable edges (gap G2) |
| relationship `SetAsync` / `RemovePropertiesAsync` / `DeleteAsync` | edge property commands / `DeleteEdgeCommand` | `GDI_SetPropertyOfEdge`, `GDI_RemovePropertiesFromEdge`, `GDI_FreeEdge` | ⚠ same-buffer edges only until `GDI_AssociateEdge` (gap G2) |
| `Paths.Walk` (variable length) | `TraverseBFSCommand` | composed `GDI_GetNeighborVerticesOfVertex` | ⚠ phase 2 (gap G3) |
| `Paths.ShortestAsync` | `ShortestPathCommand` | composed BFS | ⚠ phase 2 (gap G3) |
| `GraphTransaction` (Commit/Rollback, auto-rollback, finished-guard) | `BeginTransactionCommand` / `CommitCommand` / `AbortCommand` (+ collective) | `GDI_StartTransaction` / `GDI_CloseTransaction(COMMIT/ABORT)` | ✅ §5.5 |

### Honest gaps (G1–G4) — documented, not hidden

- **G1 — No global scans.** GDI offers no way to iterate/filter the whole vertex set;
  discovery always starts from a known key (`TranslateVertexID`) or a known vertex's
  neighborhood. Any "find all vertices where prop > x" query must be reframed by the
  application (or future index support in GDI).
- **G2 — Existing edges are unreachable.** Without `GDI_AssociateEdge`, edge update/delete
  and edge property reads only work on edges created in the same batch. Additionally
  `GDI_GetPropertiesOfEdge` is unimplemented, so edge read-back is limited to labels +
  endpoints.
- **G3 — No native multi-hop traversal.** BFS/shortest-path are engine-side compositions;
  fine for small diameters, but each hop is a GDI call — no Cypher-style `*1..5` in hardware.
- **G4 — No aggregates.** count/exists exist at neighborhood level via capped fetches;
  sum/avg/min/max only over data the engine already holds.

These gaps are properties of the GDI backend, not of the command layer — the layer's job
is to expose everything GDI *can* do batched and async, and to fail loudly (at
`validate()` time) for what it cannot.

## 12. Summary

| Requirement | Where it is satisfied |
|---|---|
| `Command` base class for all CRUD operations | §4 — abstract `Command` with `execute()`/`validate()`, state machine, typed `CommandType` enum |
| `CommandBuffer` collects commands, submits at once | §7.1 — ordered recording, one-shot `submit()` that validates then hands a snapshot to the engine |
| Returns an async task handle | §6 — `TaskHandle` with `isDone()`, `wait()`, `getResult(cmd)`, `getAllResults()` |
| Wait on handle → get per-command return values (retrieve/search/success) | §6.2 + §9 — uniform `CommandResult` envelope carrying uids, uid lists, property maps, snapshots, or errors |
| Grounded in this repo's GDI | Commands map 1:1 to functions in `src/gdi.h` (`GDI_CreateVertex`, `GDI_CreateEdge`, `GDI_GetNeighborVerticesOfVertex`, `GDI_StartTransaction`, …) |
| Full vertex/edge CRUD incl. keys & MERGE | §4.5 + §5.6–5.8 — external-ID keys, `TRANSLATE_VERTEX_ID`, `GET_OR_CREATE_VERTEX`, bulk creates |
| Full schema management (labels, property types, constraints) | §5.10 — create/update/free/get-by-name/get-all for labels and property types; constraint construction |
| Neo4j-style coverage | §11 — operation-by-operation coverage matrix with honest gaps G1–G4 and the `GDI_AssociateEdge` caveat §5.9 |

The design deliberately mirrors the mental model of GPU command buffers and MPI's
"submit collective work, wait later" style — which is exactly the programming model GDI
itself advocates for building portable, high-performance graph systems.

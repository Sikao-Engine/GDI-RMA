/*
 * gdi_rma_api.h -- the C FFI surface of the gdi_rma shared library.
 *
 * WHAT THIS IS: a thin, ABI-stable wrapper around the ETH Zurich GDI-RMA graph
 * database library (src/*.c, arXiv 2305.11162) that turns it into a shared
 * library a managed process can P/Invoke.  It is compiled INTO the single
 * `gdi_rma` target (see xmake.lua) and therefore calls the GDI_* / GDA_*
 * functions directly; nothing here is exported by the .def file -- each
 * declaration below carries GDI_RMA_API (dllexport) explicitly.
 *
 * THIS HEADER IS MPI-FREE ON PURPOSE: it never includes gdi.h or mpi.h, so a
 * consumer (C# generator, a test host, another language) can compile against
 * the ABI without an MPI SDK.  Every GDI value that appears at the boundary is
 * spelled out below with its numeric value; the numbers ARE the contract and
 * are asserted against the real GDI headers inside gdi_rma_api.c.
 *
 * ABI CONTRACT (docs/ffi-linking.md, the same rules as kimix_api):
 *  1. Scalars only at the edge: <stdint.h> widths, size_t, pointers, `bool`
 *     (ONE byte, C99 _Bool), and plain C enums (int-sized).  No C++ anywhere.
 *  2. Strings and binary blobs are (pointer, byte-length) pairs.  Names are
 *     UTF-8; the length is explicit, so the bytes need no NUL terminator
 *     (a NUL inside a name is still rejected: GDI stores names as C strings).
 *  3. One heap: every buffer this library hands back is malloc'd INSIDE this
 *     library and must be released through gdi_rma_free() or the dedicated
 *     destructor named in the function's doc comment -- never with the caller's
 *     free/NativeMemory.Free/Marshal.FreeCoTaskMem.  `const char*` results
 *     (gdi_rma_error_string, gdi_rma_last_error, gdi_rma_build_info) point to
 *     static or thread-local storage owned by the library and are NEVER freed.
 *  4. No exceptions / no structured errors cross the boundary.  Every fallible
 *     entry point returns a gdi_rma_status; out-parameters are written only
 *     when the call returns GDI_RMA_OK (the exceptions are documented).
 *  5. POD structs shared across the boundary are sequential, naturally aligned
 *     (gdi_rma_edge_uid is the one packed exception, see below), and always
 *     passed by pointer.  Sizes are stated in the doc comment of each struct;
 *     the C implementation static_asserts them against the GDI internals.
 *  6. Threading: MPI is process-global.  gdi_rma_init()/gdi_rma_finalize() are
 *     refcounted and mutex-guarded; everything else follows GDI-RMA's own rule
 *     -- a database/transaction object is driven by one thread at a time.  The
 *     last_error text is thread-local, so concurrent failures on different
 *     threads do not overwrite each other.
 *
 * COVERAGE / KNOWN GAPS the caller must plan around (details at each entry
 * point): GDI-RMA implements neither GDI_AssociateEdge nor any edge property
 * read-back, so an edge is addressable only through the transaction that
 * created it (see gdi_rma_tx and GDI_RMA_ERR_EDGE_NOT_ASSOCIATED).
 */
#ifndef GDI_RMA_API_H
#define GDI_RMA_API_H

#include "dll_export.h"

#include <stddef.h>  /* size_t, NULL */
#include <stdint.h>  /* uint8_t ... uint64_t, int64_t */
#include <stdbool.h> /* bool: ONE byte on both sides of the boundary */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* ABI identity                                                              */
/* ------------------------------------------------------------------------- */

/* Bumped whenever the exported surface or a POD layout changes in a way a
 * compiled binding must know about.  gdi_rma_api_version() returns the value
 * this build was compiled with; a binding asserts on it at load time.
 * v1 = the initial Phase-4 surface. */
#define GDI_RMA_ABI_VERSION 1

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

/* The whole failure vocabulary of the FFI.
 *
 * Codes 0..62 are the GDI error classes of src/gdi.h with the SAME numeric
 * value, so a GDI status crosses the boundary untranslated (the wrapper
 * documents the mapping table below by name).  Codes 1000+ are wrapper-local
 * and cannot collide with GDI's range (GDI_ERROR_LASTCODE == 62).
 *
 * A plain C enum: `int` sized on both sides of the boundary. */
typedef enum gdi_rma_status {
    GDI_RMA_OK = 0,                        /* GDI_SUCCESS */

    /* ---- GDI codes, 1:1 (see src/gdi.h for the authoritative list) ---- */
    GDI_RMA_WARNING_NON_UNIQUE_ID = 1,     /* GDI_WARNING_NON_UNIQUE_ID */
    GDI_RMA_WARNING_OTHER = 2,             /* GDI_WARNING_OTHER */
    GDI_RMA_ERROR_ACCESS = 3,
    GDI_RMA_ERROR_ARGUMENT = 4,
    GDI_RMA_ERROR_BAD_FILE = 5,
    GDI_RMA_ERROR_BUFFER = 6,
    GDI_RMA_ERROR_CONSTRAINT = 7,
    GDI_RMA_ERROR_CONVERSION = 8,
    GDI_RMA_ERROR_COUNT = 9,
    GDI_RMA_ERROR_DATABASE = 10,
    GDI_RMA_ERROR_DATATYPE = 11,
    GDI_RMA_ERROR_DATE = 12,
    GDI_RMA_ERROR_DATETIME = 13,
    GDI_RMA_ERROR_DECIMAL = 14,
    GDI_RMA_ERROR_EDGE = 15,
    GDI_RMA_ERROR_EDGE_ORIENTATION = 16,
    GDI_RMA_ERROR_EMPTY_NAME = 17,
    GDI_RMA_ERROR_ERROR_CODE = 18,
    GDI_RMA_ERROR_FILE_EXISTS = 19,
    GDI_RMA_ERROR_FILE_IN_USE = 20,
    GDI_RMA_ERROR_INCOMPATIBLE_TRANSACTIONS = 21,
    GDI_RMA_ERROR_INDEX = 22,
    GDI_RMA_ERROR_LABEL = 23,
    GDI_RMA_ERROR_NAME_EXISTS = 24,
    GDI_RMA_ERROR_NO_MEMORY = 25,
    GDI_RMA_ERROR_NO_PROPERTY = 26,
    GDI_RMA_ERROR_NO_SPACE = 27,
    GDI_RMA_ERROR_NO_SUCH_FILE = 28,
    GDI_RMA_ERROR_NON_UNIQUE_ID = 29,
    GDI_RMA_ERROR_NOT_SAME = 30,
    GDI_RMA_ERROR_OBJECT_MISMATCH = 31,
    GDI_RMA_ERROR_OP = 32,
    GDI_RMA_ERROR_OP_DATATYPE_MISMATCH = 33,
    GDI_RMA_ERROR_PROPERTY_EXISTS = 34,
    GDI_RMA_ERROR_PROPERTY_TYPE = 35,
    GDI_RMA_ERROR_PROPERTY_TYPE_EXISTS = 36,
    GDI_RMA_ERROR_RANGE = 37,
    GDI_RMA_ERROR_READ_ONLY_FILE = 38,
    GDI_RMA_ERROR_READ_ONLY_PROPERTY_TYPE = 39,
    GDI_RMA_ERROR_READ_ONLY_TRANSACTION = 40,
    GDI_RMA_ERROR_RESOURCE = 41,
    GDI_RMA_ERROR_SIZE = 42,
    GDI_RMA_ERROR_SIZE_LIMIT = 43,
    GDI_RMA_ERROR_STALE = 44,
    GDI_RMA_ERROR_STATE = 45,
    GDI_RMA_ERROR_SUBCONSTRAINT = 46,
    GDI_RMA_ERROR_TIME = 47,
    GDI_RMA_ERROR_TRANSACTION = 48,
    GDI_RMA_ERROR_UID = 49,
    GDI_RMA_ERROR_VERTEX = 50,
    GDI_RMA_ERROR_WRONG_TYPE = 51,
    GDI_RMA_ERROR_INVALID_DATE = 52,
    GDI_RMA_ERROR_COMMUNICATOR = 53,
    GDI_RMA_ERROR_BLOCK_SIZE = 54,
    GDI_RMA_ERROR_QUOTA = 55,
    GDI_RMA_ERROR_TRUNCATE = 56,
    GDI_RMA_ERROR_TRANSACTION_CRITICAL = 57,
    GDI_RMA_ERROR_TRANSACTION_COMMIT_FAIL = 58,
    GDI_RMA_ERROR_INTERN = 59,
    GDI_RMA_ERROR_IO = 60,
    GDI_RMA_ERROR_OTHER = 61,
    GDI_RMA_ERROR_UNKNOWN = 62,

    /* ---- wrapper-local codes (1000+) ---- */
    /* MPI_Init or GDI_Init has not run (or already unwound). */
    GDI_RMA_ERR_NOT_INITIALIZED = 1000,
    /* MPI_Init / GDI_Init itself failed; gdi_rma_last_error() carries the MPI
     * code.  Raised instead of crashing when the process is not an MPI rank
     * (e.g. no mpiexec and no MPI service). */
    GDI_RMA_ERR_MPI_UNAVAILABLE = 1001,
    /* A pointer argument was NULL where one is required, a count/length was
     * out of range, or an enum field held a value this ABI does not define. */
    GDI_RMA_ERR_INVALID_ARGUMENT = 1002,
    /* The handle is dead: a database/transaction already freed, or a pointer
     * that never came from this library. */
    GDI_RMA_ERR_INVALID_HANDLE = 1003,
    /* An edge UID that this transaction did not create (GDI-RMA has no
     * GDI_AssociateEdge, so edge state is only reachable inside the creating
     * transaction).  The caller's capability flags must advertise this. */
    GDI_RMA_ERR_EDGE_NOT_ASSOCIATED = 1004,
    /* A label / property-type name that does not exist in the database. */
    GDI_RMA_ERR_NOT_FOUND = 1005,
    /* A name longer than 63 bytes (GDI_MAX_OBJECT_NAME-1).  GDI truncates
     * names silently, so this wrapper rejects them instead. */
    GDI_RMA_ERR_NAME_TOO_LONG = 1006,
    /* The dtype tag of a gdi_rma_property_ref disagrees with the dtype the
     * property type was declared with, or value_bytes != count * dtype size. */
    GDI_RMA_ERR_TYPE_MISMATCH = 1007,
    /* A vertex still has incident edges; raised by gdi_rma_free_vertex_guarded
     * (the "GDI edges still exist" semantics of a guarded delete). */
    GDI_RMA_ERR_EDGES_EXIST = 1008,
    /* The operation is not implemented by GDI-RMA at all (edge property read,
     * edge property type enumeration).  Never silently returns "empty". */
    GDI_RMA_ERR_UNSUPPORTED = 1009,
    /* The library could not allocate its own bookkeeping. */
    GDI_RMA_ERR_OUT_OF_MEMORY = 1010
} gdi_rma_status;

/* ------------------------------------------------------------------------- */
/* Value enums mirroring the GDI constants                                   */
/* ------------------------------------------------------------------------- */

/* GDI_Datatype (src/gdi_datatype.h).  The numeric values are GDI's; the
 * wrapper passes them through untouched. */
typedef enum gdi_rma_datatype {
    GDI_RMA_DT_CHAR = 100,     /* GDI_CHAR    (1 byte)  */
    GDI_RMA_DT_INT8 = 101,     /* GDI_INT8_T  (1 byte)  */
    GDI_RMA_DT_UINT8 = 102,    /* GDI_UINT8_T (1 byte)  */
    GDI_RMA_DT_BOOL = 103,     /* GDI_BOOL    (1 byte)  */
    GDI_RMA_DT_BYTE = 104,     /* GDI_BYTE    (1 byte, the blob dtype) */
    GDI_RMA_DT_INT16 = 105,    /* GDI_INT16_T (2 bytes) */
    GDI_RMA_DT_UINT16 = 106,   /* GDI_UINT16_T (2 bytes) */
    GDI_RMA_DT_INT32 = 107,    /* GDI_INT32_T (4 bytes) */
    GDI_RMA_DT_UINT32 = 108,   /* GDI_UINT32_T (4 bytes) */
    GDI_RMA_DT_FLOAT = 109,    /* GDI_FLOAT   (4 bytes) */
    GDI_RMA_DT_DATE = 110,     /* GDI_DATE    (4 bytes, GDI_SetDate) */
    GDI_RMA_DT_TIME = 111,     /* GDI_TIME    (4 bytes, GDI_SetTime) */
    GDI_RMA_DT_INT64 = 112,    /* GDI_INT64_T (8 bytes) */
    GDI_RMA_DT_UINT64 = 113,   /* GDI_UINT64_T (8 bytes) */
    GDI_RMA_DT_DOUBLE = 114,   /* GDI_DOUBLE  (8 bytes) */
    GDI_RMA_DT_DATETIME = 115, /* GDI_DATETIME (8 bytes, GDI_SetDatetime) */
    GDI_RMA_DT_DECIMAL = 116   /* GDI_DECIMAL (67 bytes, GDI_Decimal struct) */
} gdi_rma_datatype;

/* GDI_Op (src/gdi_operation.h). */
typedef enum gdi_rma_op {
    GDI_RMA_OP_EQUAL = 0,
    GDI_RMA_OP_NOTEQUAL = 1,
    GDI_RMA_OP_GREATER = 2,
    GDI_RMA_OP_EQGREATER = 3,
    GDI_RMA_OP_SMALLER = 4,
    GDI_RMA_OP_EQSMALLER = 5
} gdi_rma_op;

/* Property entity type (src/gdi_property_type.h: GDI_SINGLE_ENTITY /
 * GDI_MULTIPLE_ENTITY).  Raw GDI values -- the wrapper does not renormalize. */
typedef enum gdi_rma_entity_type {
    GDI_RMA_ENTITY_SINGLE = 198,
    GDI_RMA_ENTITY_MULTIPLE = 199
} gdi_rma_entity_type;

/* Property size limit (GDI_FIXED_SIZE / GDI_MAX_SIZE / GDI_NO_SIZE_LIMIT). */
typedef enum gdi_rma_size_limit {
    GDI_RMA_SIZE_FIXED = 200,
    GDI_RMA_SIZE_MAX = 201,
    GDI_RMA_SIZE_NONE = 202
} gdi_rma_size_limit;

/* Edge direction written to the store (GDI_EDGE_DIRECTED / GDI_EDGE_UNDIRECTED
 * -- the two values GDI_CreateEdge and GDI_SetDirectionTypeOfEdge accept). */
typedef enum gdi_rma_edge_direction {
    GDI_RMA_DIR_DIRECTED = 259,
    GDI_RMA_DIR_UNDIRECTED = 260
} gdi_rma_edge_direction;

/* Read orientation: the low-3-bit mask GDI_GetEdgesOfVertex /
 * GDI_GetNeighborVerticesOfVertex expect (GDI_EDGE_INCOMING = 257 -> bit 0,
 * GDI_EDGE_OUTGOING = 258 -> bit 1, GDI_EDGE_UNDIRECTED = 260 -> bit 2).
 * Any value in 1..7 is accepted by GDI; 0 and >7 is GDI_ERROR_EDGE_ORIENTATION. */
#define GDI_RMA_ORIENT_INCOMING 1u
#define GDI_RMA_ORIENT_OUTGOING 2u
#define GDI_RMA_ORIENT_UNDIRECTED 4u
#define GDI_RMA_ORIENT_DIRECTED_BOTH 3u /* incoming | outgoing */
#define GDI_RMA_ORIENT_ALL 7u           /* the "both" a graph API means: all three */

/* gdi_rma_condition.kind */
#define GDI_RMA_COND_LABEL 0u
#define GDI_RMA_COND_PROPERTY 1u

/* gdi_rma_create_vertex.flags */
#define GDI_RMA_CREATE_F_AUTO_LABELS 1u /* create a missing label on demand */
#define GDI_RMA_CREATE_F_CHECK_UNIQUE 2u /* probe (label, external_id) first and
                                         * report GDI_RMA_VERTEX_F_NON_UNIQUE_ID.
                                         * GDI-RMA itself never raises the
                                         * NON_UNIQUE_ID warning (grep: TODO),
                                         * so without this flag the warning bit
                                         * is only set if GDI starts returning it. */

/* gdi_rma_create_vertex.out_flags bits */
#define GDI_RMA_VERTEX_F_NON_UNIQUE_ID 1u /* the key already existed in the index
                                           * while the vertex was created anyway */
#define GDI_RMA_VERTEX_F_LABEL_CREATED 2u /* at least one label was auto-created
                                           * (needs GDI_RMA_CREATE_F_AUTO_LABELS) */

/* ------------------------------------------------------------------------- */
/* Opaque handles                                                            */
/* ------------------------------------------------------------------------- */

typedef struct gdi_rma_db gdi_rma_db;                /* a GDI_Database + caches */
typedef struct gdi_rma_tx gdi_rma_tx;                /* a GDI_Transaction       */
typedef struct gdi_rma_label gdi_rma_label;          /* borrowed GDI_Label      */
typedef struct gdi_rma_ptype gdi_rma_ptype;          /* borrowed GDI_PropertyType */
typedef struct gdi_rma_constraint gdi_rma_constraint; /* owned GDI_Constraint   */

/* ------------------------------------------------------------------------- */
/* POD structs shared across the boundary                                    */
/* ------------------------------------------------------------------------- */

/* A vertex UID is GDA_DPointer: ONE 64-bit word (rank in the high bits, offset
 * in the low GDA_DPOINTER_OFFSETBITS).  Size 8, align 8. */
typedef struct gdi_rma_vertex_uid {
    uint64_t value; /* the raw GDI_Vertex_uid; GDI_RMA_UID_NULL = all-ones */
} gdi_rma_vertex_uid;
#define GDI_RMA_UID_NULL 0xFFFFFFFFFFFFFFFFull

/* An edge UID is GDA_Edge_uid: 12 BYTES -- the origin vertex UID (8) followed
 * by the offset of the edge record inside the origin vertex's lightweight-edge
 * block (4).  GDI stores it as `uint8_t[12]`, i.e. densely packed with
 * alignment 1, so this struct is packed to match: sizeof == 12, alignof == 1,
 * an ARRAY of it is therefore 12-byte-strided exactly like GDI's own array and
 * can be handed to GDI_GetEdgesOfVertex unchanged.  The C# mirror needs
 * [StructLayout(Pack = 1)] (or a 12-byte InlineArray). */
#pragma pack(push, 1)
typedef struct gdi_rma_edge_uid {
    uint64_t origin_vertex_uid; /* == the UID of the vertex the edge hangs on */
    uint32_t origin_offset;     /* slot in that vertex's lightweight-edge data */
} gdi_rma_edge_uid;
#pragma pack(pop)

/* gdi_rma_init: options block.  Zero-size-safe: pass NULL for the defaults
 * (block_size 256, memory_size 16 MiB per rank).  Size 24 bytes, align 8.
 * struct_size MUST be sizeof(gdi_rma_init_options) -- it is the version check
 * for this struct, not a payload length. */
typedef struct gdi_rma_init_options {
    uint32_t struct_size;  /* == 24 */
    uint32_t flags;        /* reserved, pass 0 */
    uint32_t block_size;   /* database block size in bytes (0 = default 256) */
    uint32_t _pad;         /* keep the layout explicit; pass 0 */
    uint64_t memory_size;  /* local memory per rank in bytes (0 = default) */
} gdi_rma_init_options;

/* A (pointer, byte-length) text reference.  16 bytes, align 8.  UTF-8 for
 * names; the pointer is BORROWED for the duration of the call only. */
typedef struct gdi_rma_name_ref {
    const char* name; /* not NUL-terminated unless it happens to be */
    size_t len;       /* byte length; 0 < len < 64 for schema names */
} gdi_rma_name_ref;

/* One property value as an input argument.  40 bytes, align 8.
 *   count       = number of ELEMENTS of dtype (1 for a scalar)
 *   value_bytes = count * sizeof(dtype); validated against the declared
 *                 property type's dtype (GDI_RMA_ERR_TYPE_MISMATCH)
 * The value bytes are the GDI element representation: native endianness
 * (little-endian on every supported platform: x64/arm64), fixed-width
 * scalars, GDI_CHAR = UTF-8 bytes, GDI_DATE/TIME/DATETIME = the GDI packed
 * integers, GDI_DECIMAL = the 67-byte GDI_Decimal blob. */
typedef struct gdi_rma_property_ref {
    const char* ptype_name; /* property-type name, UTF-8, borrowed */
    size_t ptype_name_len;
    uint32_t dtype;         /* gdi_rma_datatype */
    uint32_t count;         /* element count */
    const void* value;      /* value_bytes of payload, borrowed */
    size_t value_bytes;
} gdi_rma_property_ref;

/* A constraint condition fed to gdi_rma_constraint_create / the add_* calls.
 * 48 bytes, align 8.  kind == GDI_RMA_COND_LABEL uses only name/op
 * (GDI accepts EQUAL and NOTEQUAL for labels); kind == GDI_RMA_COND_PROPERTY
 * additionally uses dtype/value/count.  Note that GDI-RMA REJECTS property
 * conditions when a constraint is evaluated for an edge/neighbor read
 * (GDA_EvalConstraintInLightweightEdgeContext -> GDI_ERROR_CONSTRAINT), so
 * those constraints are only useful for vertex-side filtering a caller does
 * itself; see gdi_rma_constraint_create. */
typedef struct gdi_rma_condition {
    const char* name;      /* label or property-type name, UTF-8, borrowed */
    size_t name_len;
    uint32_t kind;         /* GDI_RMA_COND_LABEL | GDI_RMA_COND_PROPERTY */
    uint32_t op;           /* gdi_rma_op */
    uint32_t dtype;        /* gdi_rma_datatype (property kind) */
    uint32_t count;        /* element count (property kind) */
    const void* value;     /* payload (property kind); may be NULL when count 0 */
    size_t value_bytes;
} gdi_rma_condition;

/* A library-allocated result buffer: {data, size}.  16 bytes, align 8.
 * OWNED by the caller after a successful call; release with
 * gdi_rma_blob_free() (or gdi_rma_free(data) -- the block is one malloc). */
typedef struct gdi_rma_blob {
    void* data; /* NULL + size 0 means "empty" */
    size_t size;
} gdi_rma_blob;

/* A list of strings allocated by the library.  24 bytes, align 8.
 * Release with gdi_rma_strlist_free() -- it frees items[i], the lens array and
 * the item pointer array in one go and zeroes the struct. */
typedef struct gdi_rma_strlist {
    char** items;   /* items[i] is a NUL-terminated UTF-8 copy, individually malloc'd */
    size_t* lens;   /* lens[i] = byte length WITHOUT the NUL (lens may be NULL if count 0) */
    size_t count;
} gdi_rma_strlist;

/* A property type's full declaration + handle.  48 bytes, align 8.
 * `name` BORROWS the GDI-owned C string (valid while the property type lives);
 * do not free it.  handle may be fed back into the API (it is the same object
 * the caches hand out) but is likewise borrowed. */
typedef struct gdi_rma_ptype_info {
    const char* name; /* borrowed, NUL-terminated UTF-8 */
    uint64_t name_len;
    uint32_t dtype;   /* gdi_rma_datatype */
    uint32_t etype;   /* gdi_rma_entity_type (raw GDI values) */
    uint32_t stype;   /* gdi_rma_size_limit (raw GDI values) */
    uint32_t _pad;
    uint64_t count;   /* element limit implied by stype */
    gdi_rma_ptype* handle; /* borrowed handle */
} gdi_rma_ptype_info;

/* A page of edge UIDs (the two-phase read of GDI_GetEdgesOfVertex).
 * 24 bytes, align 8.  Release with gdi_rma_edge_page_free().
 *   full_count: total matches in the database (authoritative for cap 0 and
 *               cap >= 2; for cap == 1 it is the short-circuit answer 0/1 and
 *               NOT the true count)
 *   returned:   rows in `uids` (<= cap, 0 when cap == 0)
 *   uids:       `returned` entries, 12 bytes each; NULL when returned == 0 */
typedef struct gdi_rma_edge_page {
    uint64_t full_count;
    uint64_t returned;
    gdi_rma_edge_uid* uids;
} gdi_rma_edge_page;

/* The vertex counterpart (GDI_GetNeighborVerticesOfVertex).  24 bytes, align 8.
 * Release with gdi_rma_vertex_page_free(). */
typedef struct gdi_rma_vertex_page {
    uint64_t full_count;
    uint64_t returned;
    gdi_rma_vertex_uid* uids;
} gdi_rma_vertex_page;

/* ---- packed property blob (gdi_rma_get_properties_of_vertex) -------------
 * One malloc'd buffer carrying every property of one vertex:
 *
 *   offset 0  : uint32 count          property entries that follow
 *   offset 4  : uint32 reserved       (0)
 *   then, per entry i, at the running offset O:
 *       O+0  uint32 name_len
 *       O+4  uint32 dtype            gdi_rma_datatype
 *       O+8  uint32 etype            gdi_rma_entity_type (raw GDI value)
 *       O+12 uint32 elem_count       number of dtype elements in `value`
 *       O+16 name bytes[name_len]    UTF-8, NOT NUL-terminated
 *       then padding to the next multiple of 8
 *       value bytes[elem_count * sizeof(dtype)]
 *   next entry starts right after `value` (values are not padded themselves).
 *
 * All headers are 16 bytes and every value payload therefore starts 8-aligned
 * relative to the blob base, so a caller can read scalars through a typed
 * pointer.  Multiple values of one property type (GDI etype MULTIPLE) are
 * concatenated into ONE entry with the summed elem_count -- the managed model is
 * one value per property name.  Predefined property types (GDI_PROPERTY_TYPE_ID,
 * _DEGREE, _INDEGREE, _OUTDEGREE) appear as ordinary entries with their GDI
 * names. */

/* ------------------------------------------------------------------------- */
/* Library identity, error reporting, the one-heap free                      */
/* ------------------------------------------------------------------------- */

GDI_RMA_API uint32_t gdi_rma_api_version(void);
/* Always safe: no MPI, no allocation, no state.  The load test calls this. */

GDI_RMA_API const char* gdi_rma_build_info(void);
/* "gdi_rma <abi> <platform> <mode> <compiler>" -- static storage, never freed. */

GDI_RMA_API const char* gdi_rma_error_string(int code);
/* Name + short description of a gdi_rma_status / GDI error class, e.g.
 * "GDI_RMA_ERROR_VERTEX".  Static storage, never freed; NULL for a code
 * outside both ranges.  GDI-RMA has no GDI_GetErrorString (src/README.md), so
 * this table is the wrapper's own. */

GDI_RMA_API const char* gdi_rma_last_error(void);
/* The diagnostic of the LAST call on the calling thread that had something to
 * say: every failure, and also the success-with-warning notes of the edge
 * property writes.  Thread-local, static storage, never freed, valid until the
 * next call that reports something; an empty string means the library had
 * nothing to add. */

GDI_RMA_API bool gdi_rma_free(void* ptr);
/* THE free for everything this library allocated (blobs, uid arrays, strlists,
 * pages, constraint objects are NOT freed this way -- see their own calls).
 * true when the block was released, false for NULL (a no-op).  Callers must
 * never free library memory with their own CRT. */

GDI_RMA_API void gdi_rma_blob_free(gdi_rma_blob* blob);
/* Free blob->data and zero the struct.  NULL-safe. */

GDI_RMA_API void gdi_rma_strlist_free(gdi_rma_strlist* list);
/* Free every item, the lens array and the pointer array; zero the struct. */

GDI_RMA_API void gdi_rma_edge_page_free(gdi_rma_edge_page* page);
GDI_RMA_API void gdi_rma_vertex_page_free(gdi_rma_vertex_page* page);

GDI_RMA_API size_t gdi_rma_datatype_size(uint32_t dtype);
/* Byte width of one element of `dtype` (gdi_rma_datatype), 0 when invalid.
 * Lets a caller size a gdi_rma_property_ref payload without a switch table. */

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* MPI_Init + GDI_Init, refcounted.  The first call (count 0 -> 1) performs the
 * real initialization; further calls only bump the count, so several backend
 * instances share one MPI environment.  opts may be NULL (defaults); when
 * non-NULL opts->struct_size must be sizeof(gdi_rma_init_options) and the block
 * is copied -- the values it carries (block_size/memory_size) are the defaults
 * used by gdi_rma_db_create.  A non-NULL opts passed on a repeat call only
 * re-validates struct_size; the captured first-call options stay in effect
 * (GDI memory sizing is process-wide here).
 * Returns GDI_RMA_OK, GDI_RMA_ERR_INVALID_ARGUMENT, or GDI_RMA_ERR_MPI_UNAVAILABLE.
 * If the host process has already called MPI_Init, this reuses it and
 * gdi_rma_finalize() will NOT call MPI_Finalize. */
GDI_RMA_API int gdi_rma_init(const gdi_rma_init_options* opts);

/* One release.  When the last reference goes (count 1 -> 0) the wrapper calls
 * GDI_Finalize and MPI_Finalize (unless the environment was borrowed, see
 * gdi_rma_init).  Databases must be freed first; an open database makes the
 * call return GDI_RMA_ERROR_DATABASE without touching the count. */
GDI_RMA_API int gdi_rma_finalize(void);

GDI_RMA_API bool gdi_rma_is_initialized(void);
/* True while at least one reference is held.  Never touches MPI state. */

GDI_RMA_API uint32_t gdi_rma_refcount(void);
/* The current init reference count. */

GDI_RMA_API int gdi_rma_mpi_rank(int* out_rank);
GDI_RMA_API int gdi_rma_mpi_size(int* out_size);
/* MPI_Comm_rank / MPI_Comm_size of MPI_COMM_WORLD (the communicator every
 * database is built on).  GDI_RMA_ERR_NOT_INITIALIZED before gdi_rma_init. */

/* ------------------------------------------------------------------------- */
/* Database                                                                  */
/* ------------------------------------------------------------------------- */

/* GDI_CreateDatabase on MPI_COMM_WORLD with the sizing captured at init, plus
 * the per-database caches this wrapper keeps (label name -> handle,
 * property-type name -> handle, so graph commands refer to schema objects by
 * name only).  Adds one reference to the init refcount.
 * Returns GDI_RMA_ERR_NOT_INITIALIZED / GDI_RMA_ERROR_* from GDI. */
GDI_RMA_API int gdi_rma_db_create(gdi_rma_db** out_db);

/* GDI_FreeDatabase + cache teardown; releases the reference taken by
 * gdi_rma_db_create.  The *out_db handle is consumed. */
GDI_RMA_API int gdi_rma_db_free(gdi_rma_db* db);

GDI_RMA_API int gdi_rma_db_block_size(const gdi_rma_db* db, uint32_t* out_block_size);
GDI_RMA_API int gdi_rma_db_memory_size(const gdi_rma_db* db, uint64_t* out_memory_size);
/* The sizing this database was created with (diagnostics; GDI stores both in
 * the database descriptor). */

/* ------------------------------------------------------------------------- */
/* Transactions                                                              */
/* ------------------------------------------------------------------------- */

/* Every graph op (vertex/edge/read) needs a live transaction; GDI has no
 * implicit one.  "Auto-transaction" (the managed seam's tx == null case) is the
 * caller's job: begin, run, commit -- which keeps the failure semantics of a
 * commit (GDI_RMA_ERROR_TRANSACTION_CRITICAL / _COMMIT_FAIL) visible.
 *
 * GDI_StartTransaction: a read-write single-process transaction. */
GDI_RMA_API int gdi_rma_tx_begin(gdi_rma_db* db, gdi_rma_tx** out_tx);

/* GDI_StartCollectiveTransaction: a read-only collective transaction (the
 * whole communicator must call it).  A collective transaction rejects every
 * write with GDI_ERROR_READ_ONLY_TRANSACTION, and a database that has one open
 * rejects gdi_rma_tx_begin with GDI_ERROR_INCOMPATIBLE_TRANSACTIONS. */
GDI_RMA_API int gdi_rma_tx_begin_collective(gdi_rma_db* db, gdi_rma_tx** out_tx);

/* GDI_CloseTransaction(COMMIT) / GDI_CloseCollectiveTransaction(COMMIT).
 * Consumes tx (its edge-association registry dies with it).  A commit that was
 * forced to abort by a transaction-critical error returns
 * GDI_RMA_ERROR_TRANSACTION_COMMIT_FAIL. */
GDI_RMA_API int gdi_rma_tx_commit(gdi_rma_tx* tx);

/* The same with GDI_TRANSACTION_ABORT. */
GDI_RMA_API int gdi_rma_tx_abort(gdi_rma_tx* tx);

GDI_RMA_API int gdi_rma_tx_is_collective(const gdi_rma_tx* tx, bool* out_collective);

/* ------------------------------------------------------------------------- */
/* Schema: labels                                                            */
/* ------------------------------------------------------------------------- */

/* GDI_CreateLabel.  The handle is owned by the database and BORROWED here
 * (valid until gdi_rma_label_free / db teardown); the wrapper also inserts it
 * in the name cache.  len must be 1..63 bytes and contain no NUL. */
GDI_RMA_API int gdi_rma_label_create(gdi_rma_db* db, const char* name, size_t name_len,
                                     gdi_rma_label** out_label);

/* GDI_GetLabelFromName through the cache.  *out_label is set to NULL and the
 * status to GDI_RMA_ERR_NOT_FOUND when no such label exists (an empty name is
 * NOT an error in GDI: it maps to the predefined GDI_LABEL_NONE, which this
 * wrapper refuses as GDI_RMA_ERR_INVALID_ARGUMENT). */
GDI_RMA_API int gdi_rma_label_by_name(gdi_rma_db* db, const char* name, size_t name_len,
                                      gdi_rma_label** out_label);

/* GDI_UpdateLabel -- renames in place, refreshes the cache. */
GDI_RMA_API int gdi_rma_label_rename(gdi_rma_db* db, gdi_rma_label* label,
                                     const char* new_name, size_t new_name_len);

/* GDI_FreeLabel.  NOTE: GDI-RMA only removes the label from the database's
 * registry; it does not strip it from vertices/edges (src/README.md: "not
 * fully implemented").  The handle is consumed; cached entries are dropped. */
GDI_RMA_API int gdi_rma_label_free(gdi_rma_db* db, gdi_rma_label* label);

/* The label's name: borrowed pointer into GDI-owned storage (NUL-terminated). */
GDI_RMA_API int gdi_rma_label_name(const gdi_rma_label* label, const char** out_ptr,
                                   size_t* out_len);

/* GDI_GetAllLabelsOfDatabase -> the labels' names.  Release with
 * gdi_rma_strlist_free. */
GDI_RMA_API int gdi_rma_db_all_labels(gdi_rma_db* db, gdi_rma_strlist* out);

/* ------------------------------------------------------------------------- */
/* Schema: property types                                                    */
/* ------------------------------------------------------------------------- */

/* GDI_CreatePropertyType.  etype/stype take the raw gdi_rma_entity_type /
 * gdi_rma_size_limit values; count is the element limit (interpreted under
 * stype).  The handle is owned by the database, BORROWED here, and cached.
 * The four predefined types (GDI_PROPERTY_TYPE_ID/_DEGREE/_INDEGREE/_OUTDEGREE)
 * resolve by name through gdi_rma_ptype_by_name but cannot be created, renamed
 * or freed by the caller. */
GDI_RMA_API int gdi_rma_ptype_create(gdi_rma_db* db, const char* name, size_t name_len,
                                     uint32_t dtype, uint32_t etype, uint32_t stype,
                                     uint64_t count, gdi_rma_ptype** out_ptype);

GDI_RMA_API int gdi_rma_ptype_by_name(gdi_rma_db* db, const char* name, size_t name_len,
                                      gdi_rma_ptype** out_ptype);

/* GDI_UpdatePropertyType -- GDI-RMA updates metadata only (not the stored
 * data); the wrapper refreshes its cache and reports the GDI status. */
GDI_RMA_API int gdi_rma_ptype_update(gdi_rma_db* db, gdi_rma_ptype* ptype,
                                     const char* new_name, size_t new_name_len,
                                     uint32_t dtype, uint32_t etype, uint32_t stype,
                                     uint64_t count);

/* GDI_FreePropertyType -- metadata only in GDI-RMA; handle consumed. */
GDI_RMA_API int gdi_rma_ptype_free(gdi_rma_db* db, gdi_rma_ptype* ptype);

/* Read one handle's declaration into a POD the caller can cache. */
GDI_RMA_API int gdi_rma_ptype_info_of(const gdi_rma_ptype* ptype, gdi_rma_ptype_info* out);

/* GDI_GetAllPropertyTypesOfDatabase -> a library-allocated array of
 * gdi_rma_ptype_info (release with gdi_rma_free(*out_array)). */
GDI_RMA_API int gdi_rma_db_all_ptypes(gdi_rma_db* db, gdi_rma_ptype_info** out_array,
                                      size_t* out_count);

/* ------------------------------------------------------------------------- */
/* Schema: constraints                                                       */
/* ------------------------------------------------------------------------- */

/* Build a GDI_Constraint (one implicit subconstraint) out of `conditions` and
 * register it with the database.  NULL conds / nconds == 0 creates the empty
 * constraint (matches everything).
 * SUPPORT LEVEL, important: GDI-RMA evaluates a constraint in the edge/neighbor
 * read ONLY from label conditions with EQUAL (whitelist; at most one label
 * because a lightweight edge carries one label) or NOTEQUAL (blacklist); any
 * property condition makes the evaluation fail with GDI_ERROR_CONSTRAINT, and
 * label conditions with a comparison op other than EQUAL/NOTEQUAL are rejected
 * by GDI_AddLabelConditionToSubconstraint (GDI_ERROR_OP_DATATYPE_MISMATCH).
 * Property conditions are accepted at creation (they are part of the GDI
 * surface and useful to a caller that filters itself) -- the failure comes out
 * of the read call, faithfully translated.
 * The object is owned by the caller: free it with gdi_rma_constraint_free. */
GDI_RMA_API int gdi_rma_constraint_create(gdi_rma_db* db, const gdi_rma_condition* conditions,
                                          size_t count, gdi_rma_constraint** out);

GDI_RMA_API int gdi_rma_constraint_add_label_condition(gdi_rma_constraint* constraint,
                                                       const char* label, size_t label_len,
                                                       uint32_t op);

GDI_RMA_API int gdi_rma_constraint_add_property_condition(gdi_rma_constraint* constraint,
                                                          const char* ptype_name,
                                                          size_t ptype_name_len, uint32_t op,
                                                          uint32_t dtype, uint32_t count,
                                                          const void* value, size_t value_bytes);

/* GDI_FreeConstraint (also releases the implicit subconstraint). */
GDI_RMA_API int gdi_rma_constraint_free(gdi_rma_constraint* constraint);

/* ------------------------------------------------------------------------- */
/* Vertices                                                                  */
/* ------------------------------------------------------------------------- */

/* GDI_CreateVertex + labels + initial properties, all inside tx.
 *  - external_id/len: the vertex key payload (GDI stores it as the
 *    GDI_PROPERTY_TYPE_ID property; the (label, external_id) pair is indexed at
 *    COMMIT time, so a translate in this same transaction will NOT find it).
 *  - labels: resolved (or auto-created with GDI_RMA_CREATE_F_AUTO_LABELS) via
 *    the db cache; a missing label without the flag is GDI_RMA_ERR_NOT_FOUND.
 *  - properties: each ptype is resolved by name and its dtype checked.
 *  - out_uid: the new vertex UID (primary block pointer).
 *  - out_flags: may be NULL; the GDI_RMA_VERTEX_F_* bits (see the NON_UNIQUE_ID
 *    note at GDI_RMA_CREATE_F_CHECK_UNIQUE).
 * If any step fails the wrapper propagates the status; the vertex it already
 * created stays inside tx and is rolled back by gdi_rma_tx_abort (or written by
 * a commit -- GDI has no statement-level rollback). */
GDI_RMA_API int gdi_rma_create_vertex(gdi_rma_tx* tx, const void* external_id, size_t external_id_len,
                                      const gdi_rma_name_ref* labels, uint32_t label_count,
                                      const gdi_rma_property_ref* properties, uint32_t property_count,
                                      uint32_t flags, gdi_rma_vertex_uid* out_uid,
                                      uint32_t* out_flags);

/* MERGE (sec.5.7): gdi_rma_translate_vertex first; on a miss, gdi_rma_create_vertex.
 * out_created (1 byte) says whether the vertex was created.  GDI's index is a
 * distributed hash table keyed by (label, external id) and is written at COMMIT,
 * so a vertex whose creating transaction has not committed yet is invisible to
 * the probe (the caller's own placeholder machinery covers that case).
 * key_label must be one of `labels` (or label_count == 0, which indexes the
 * vertex under GDI_LABEL_NONE): GDI only indexes the labels the vertex carries. */
GDI_RMA_API int gdi_rma_get_or_create_vertex(gdi_rma_tx* tx, const gdi_rma_name_ref* key_label,
                                             const void* external_id, size_t external_id_len,
                                             const gdi_rma_name_ref* labels, uint32_t label_count,
                                             const gdi_rma_property_ref* properties,
                                             uint32_t property_count, uint32_t flags,
                                             gdi_rma_vertex_uid* out_uid, bool* out_created,
                                             uint32_t* out_flags);

/* GDI_TranslateVertexID: the (label, external_id) key -> UID.  found is written
 * (1 byte) even when the status is GDI_RMA_OK and no vertex was found: found=0
 * is a successful "not there" answer, NOT an error.  The lookup associates the
 * vertex with tx when found. */
GDI_RMA_API int gdi_rma_translate_vertex(gdi_rma_tx* tx, const gdi_rma_name_ref* label,
                                         const void* external_id, size_t external_id_len,
                                         gdi_rma_vertex_uid* out_uid, bool* out_found);

/* GDI_FreeVertex: cascades -- GDI-RMA marks every incident edge deleted and
 * removes the reverse edge records (src/gdi_vertex.c).  uid is resolved through
 * GDI_AssociateVertex inside tx. */
GDI_RMA_API int gdi_rma_free_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid);

/* Guarded delete: probe the neighborhood first (GDI_GetEdgesOfVertex count
 * pass, orientation GDI_RMA_ORIENT_ALL); when any incident edge exists return
 * GDI_RMA_ERR_EDGES_EXIST and delete nothing, otherwise behave like
 * gdi_rma_free_vertex.  out_edges_found (1 byte) is written when non-NULL. */
GDI_RMA_API int gdi_rma_free_vertex_guarded(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                            bool* out_edges_found);

GDI_RMA_API int gdi_rma_add_label_to_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                            const char* label, size_t label_len);
GDI_RMA_API int gdi_rma_remove_label_from_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                                 const char* label, size_t label_len);

/* The vertex's labels as names (release with gdi_rma_strlist_free). */
GDI_RMA_API int gdi_rma_get_labels_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                             gdi_rma_strlist* out);

/* add = GDI_AddPropertyToVertex (fails with GDI_RMA_ERROR_PROPERTY_EXISTS when
 * the property is already there), set = GDI_SetPropertyOfVertex (replace),
 * update = GDI_UpdatePropertyOfVertex (merge), remove = GDI_RemovePropertiesFromVertex
 * (all values of that property type). */
GDI_RMA_API int gdi_rma_vertex_add_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                             const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_vertex_set_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                             const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_vertex_update_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                                const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_vertex_remove_properties(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                                 const char* const* ptype_names,
                                                 const size_t* ptype_name_lens,
                                                 uint32_t ptype_count);

/* All properties of a vertex packed into one buffer (format at the top of this
 * file).  Release with gdi_rma_blob_free. */
GDI_RMA_API int gdi_rma_get_properties_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                                 gdi_rma_blob* out);

/* Which property types are present on the vertex (release with gdi_rma_free).
 * The counterpart for edges is NOT implemented by GDI-RMA and returns
 * GDI_RMA_ERR_UNSUPPORTED (kept in the surface so a backend can advertise it). */
GDI_RMA_API int gdi_rma_get_all_property_types_of_vertex(gdi_rma_tx* tx,
                                                         const gdi_rma_vertex_uid* uid,
                                                         gdi_rma_ptype_info** out_array,
                                                         size_t* out_count);
GDI_RMA_API int gdi_rma_get_all_property_types_of_edge(gdi_rma_tx* tx,
                                                       const gdi_rma_edge_uid* uid,
                                                       gdi_rma_ptype_info** out_array,
                                                       size_t* out_count);

/* ------------------------------------------------------------------------- */
/* Edges                                                                     */
/* ------------------------------------------------------------------------- */

/* EDGE ADDRESSING (the G2 gap): GDI-RMA has no GDI_AssociateEdge, so a
 * GDI_EdgeHolder exists only inside the transaction that created it.  This
 * wrapper keeps a tx-local UID -> holder registry filled by
 * gdi_rma_create_edge; every edge call below takes (tx, uid) and answers
 * GDI_RMA_ERR_EDGE_NOT_ASSOCIATED when the uid comes from another transaction.
 * A caller must therefore advertise SupportsEdgeByStableUid = false and route
 * edge mutations through same-batch/same-tx placeholders. */
GDI_RMA_API int gdi_rma_create_edge(gdi_rma_tx* tx, const gdi_rma_vertex_uid* origin,
                                    const gdi_rma_vertex_uid* target, uint32_t direction,
                                    const gdi_rma_name_ref* labels, uint32_t label_count,
                                    const gdi_rma_property_ref* properties,
                                    uint32_t property_count, gdi_rma_edge_uid* out_uid);

GDI_RMA_API int gdi_rma_free_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid);

/* GDI_SetOriginVertexOfEdge / GDI_SetTargetVertexOfEdge.
 * IMPORTANT: an edge UID is (origin vertex UID, offset in the origin's
 * lightweight-edge block), so replacing an ENDPOINT MOVES the edge's UID.
 * When out_new_uid is non-NULL it receives the edge's UID after the update, and
 * the wrapper re-keys its transaction registry (the old UID stops resolving).
 * A managed layer that identified an edge by its old UID must remap it. */
GDI_RMA_API int gdi_rma_edge_set_origin(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                         const gdi_rma_vertex_uid* new_origin,
                                         gdi_rma_edge_uid* out_new_uid);
GDI_RMA_API int gdi_rma_edge_set_target(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                        const gdi_rma_vertex_uid* new_target,
                                        gdi_rma_edge_uid* out_new_uid);
GDI_RMA_API int gdi_rma_edge_set_direction(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                           uint32_t direction);
GDI_RMA_API int gdi_rma_edge_get_direction(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                           uint32_t* out_direction);
GDI_RMA_API int gdi_rma_edge_get_vertices(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                           gdi_rma_vertex_uid* out_origin,
                                           gdi_rma_vertex_uid* out_target);

/* GDI_AddLabelToEdge / GDI_RemoveLabelFromEdge.  NOTE: a lightweight edge
 * carries AT MOST ONE label in GDI-RMA; adding a second fails with a GDI error. */
GDI_RMA_API int gdi_rma_edge_add_label(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                        const char* label, size_t label_len);
GDI_RMA_API int gdi_rma_edge_remove_label(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                          const char* label, size_t label_len);
GDI_RMA_API int gdi_rma_get_labels_of_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                           gdi_rma_strlist* out);

/* The edge property writes below are "input parsing only" in GDI-RMA
 * (src/README.md): the calls run GDI's validation and return its status, but
 * GDI stores nothing.  They are exposed so a backend can translate faithfully
 * and advertise the gap, not because they work.  The read has no GDI
 * implementation at all: gdi_rma_get_properties_of_edge always returns
 * GDI_RMA_ERR_UNSUPPORTED. */
GDI_RMA_API int gdi_rma_edge_add_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                          const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_edge_set_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                          const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_edge_update_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                             const gdi_rma_property_ref* property);
GDI_RMA_API int gdi_rma_edge_remove_properties(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                               const char* const* ptype_names,
                                               const size_t* ptype_name_lens,
                                               uint32_t ptype_count);
GDI_RMA_API int gdi_rma_get_properties_of_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                               gdi_rma_blob* out);

/* ------------------------------------------------------------------------- */
/* Reads (two-phase, paged)                                                  */
/* ------------------------------------------------------------------------- */

/* GDI_GetEdgesOfVertex.  cap semantics (the managed page contract):
 *   cap == 0 : count probe only -- full_count is the true match count,
 *              returned == 0, uids == NULL.
 *   cap == 1 : existence short-circuit -- the count pass is SKIPPED,
 *              full_count carries 0/1 and is not authoritative.
 *   cap >= 2 : the count pass plus min(cap, matches) rows.
 * orientation: GDI_RMA_ORIENT_* bitmask in 1..7.
 * constraint: NULL = unfiltered; otherwise a gdi_rma_constraint_create object
 *             (label conditions only -- see the support note there).
 * Release out with gdi_rma_edge_page_free. */
GDI_RMA_API int gdi_rma_get_edges_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                            const gdi_rma_constraint* constraint,
                                            uint32_t orientation, uint64_t cap,
                                            gdi_rma_edge_page* out);

/* GDI_GetNeighborVerticesOfVertex -- same cap/orientation/constraint contract,
 * release with gdi_rma_vertex_page_free. */
GDI_RMA_API int gdi_rma_get_neighbors_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                                const gdi_rma_constraint* constraint,
                                                uint32_t orientation, uint64_t cap,
                                                gdi_rma_vertex_page* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GDI_RMA_API_H */

/*
 * gdi_rma_api.c -- identity, error reporting, the one-heap free, lifecycle,
 * database and transaction management of the gdi_rma C FFI (see gdi_rma_api.h),
 * plus the shared internals the other src/api translation units use (name
 * cache, edge registry, byte writer).
 *
 * Layout/lifetime rules this file enforces:
 *  - Everything the library hands back is malloc'd HERE and freed HERE
 *    (gdi_rma_free and the dedicated destructors).  The GDI sources use the
 *    same CRT heap because they are compiled into this same DLL, so a buffer
 *    produced inside GDI and returned through the FFI is still one heap.
 *  - gdi_rma_api_version() is the only export that runs before initialization:
 *    it touches no state, so a load test can call it without MPI.
 */
#include "gdi_rma_internal.h"

/* ------------------------------------------------------------------------- */
/* ABI assertions: the published POD shapes and enum values are GDI's          */
/* ------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#define GDI_RMA_ALIGNOF(type) __alignof(type)
#else
#define GDI_RMA_ALIGNOF(type) _Alignof(type)
#endif

/* GDI_Vertex_uid is a plain 64-bit DPointer. */
_Static_assert(sizeof(GDI_Vertex_uid) == 8, "GDI_Vertex_uid changed width");
_Static_assert(sizeof(gdi_rma_vertex_uid) == sizeof(GDI_Vertex_uid),
               "gdi_rma_vertex_uid must match GDI_Vertex_uid");
_Static_assert(GDI_RMA_ALIGNOF(gdi_rma_vertex_uid) == 8,
               "gdi_rma_vertex_uid must be 8-byte aligned");

/* GDI_Edge_uid is a dense 12-byte blob: vertex UID then a 4-byte offset. */
_Static_assert(sizeof(GDI_Edge_uid) == 12, "GDA_Edge_uid changed width");
_Static_assert(sizeof(gdi_rma_edge_uid) == sizeof(GDI_Edge_uid),
               "gdi_rma_edge_uid must be 12 bytes so uid arrays stay 12-byte strided");
_Static_assert(GDI_RMA_ALIGNOF(gdi_rma_edge_uid) == 1,
               "gdi_rma_edge_uid must be packed (alignment 1)");

/* GDA_Init_params is only used internally; the FFI options struct is fixed. */
_Static_assert(sizeof(gdi_rma_init_options) == 24, "gdi_rma_init_options layout drift");
_Static_assert(sizeof(gdi_rma_name_ref) == 16, "gdi_rma_name_ref layout drift");
_Static_assert(sizeof(gdi_rma_property_ref) == 40, "gdi_rma_property_ref layout drift");
_Static_assert(sizeof(gdi_rma_condition) == 48, "gdi_rma_condition layout drift");
_Static_assert(sizeof(gdi_rma_blob) == 16, "gdi_rma_blob layout drift");
_Static_assert(sizeof(gdi_rma_strlist) == 24, "gdi_rma_strlist layout drift");
_Static_assert(sizeof(gdi_rma_ptype_info) == 48, "gdi_rma_ptype_info layout drift");
_Static_assert(sizeof(gdi_rma_edge_page) == 24, "gdi_rma_edge_page layout drift");
_Static_assert(sizeof(gdi_rma_vertex_page) == 24, "gdi_rma_vertex_page layout drift");
_Static_assert(sizeof(bool) == 1, "bool must be one byte at the boundary");

/* The status space shares GDI's numbering for 0..62: a GDI code crosses over
 * untouched, which is what makes the wrapper a faithful translation. */
_Static_assert(GDI_RMA_OK == GDI_SUCCESS, "status 0 must be GDI_SUCCESS");
_Static_assert(GDI_RMA_WARNING_NON_UNIQUE_ID == GDI_WARNING_NON_UNIQUE_ID,
               "GDI warning numbering drifted");
_Static_assert(GDI_RMA_ERROR_ACCESS == GDI_ERROR_ACCESS, "GDI error numbering drifted");
_Static_assert(GDI_RMA_ERROR_COMMUNICATOR == GDI_ERROR_COMMUNICATOR,
               "GDI error numbering drifted");
_Static_assert(GDI_RMA_ERROR_UNKNOWN == GDI_ERROR_LASTCODE, "GDI last code drifted");
_Static_assert(GDI_RMA_ERR_NOT_INITIALIZED > GDI_ERROR_LASTCODE,
               "wrapper codes must not collide with the GDI range");

/* Value enums mirror the GDI constants. */
_Static_assert(GDI_RMA_DT_CHAR == GDI_CHAR && GDI_RMA_DT_BYTE == GDI_BYTE &&
                   GDI_RMA_DT_DECIMAL == GDI_DECIMAL,
               "gdi_rma_datatype must mirror GDI_Datatype");
_Static_assert(GDI_RMA_OP_EQUAL == GDI_EQUAL && GDI_RMA_OP_EQSMALLER == GDI_OP_END,
               "gdi_rma_op must mirror GDI_Op");
_Static_assert(GDI_RMA_ENTITY_SINGLE == GDI_SINGLE_ENTITY &&
                   GDI_RMA_ENTITY_MULTIPLE == GDI_MULTIPLE_ENTITY,
               "gdi_rma_entity_type must mirror the GDI etype states");
_Static_assert(GDI_RMA_SIZE_FIXED == GDI_FIXED_SIZE && GDI_RMA_SIZE_NONE == GDI_NO_SIZE_LIMIT,
               "gdi_rma_size_limit must mirror the GDI stype states");
_Static_assert(GDI_RMA_DIR_DIRECTED == GDI_EDGE_DIRECTED &&
                   GDI_RMA_DIR_UNDIRECTED == GDI_EDGE_UNDIRECTED,
               "gdi_rma_edge_direction must mirror the GDI direction states");
_Static_assert(GDI_RMA_ORIENT_INCOMING == (GDI_EDGE_INCOMING & 0xFF) &&
                   GDI_RMA_ORIENT_OUTGOING == (GDI_EDGE_OUTGOING & 0xFF) &&
                   GDI_RMA_ORIENT_UNDIRECTED == (GDI_EDGE_UNDIRECTED & 0xFF),
               "orientation must be GDI's low-byte bitmask");

/* ------------------------------------------------------------------------- */
/* library identity                                                           */
/* ------------------------------------------------------------------------- */

#define GDI_RMA_STR2(x) #x
#define GDI_RMA_STR(x) GDI_RMA_STR2(x)

uint32_t gdi_rma_api_version(void) { return GDI_RMA_ABI_VERSION; }

const char* gdi_rma_build_info(void) {
  /* A compile-time constant string: static storage, never freed. */
  return "gdi_rma abi " GDI_RMA_STR(GDI_RMA_ABI_VERSION) " ("
#if defined(_MSC_VER)
         "msvc"
#elif defined(__clang__)
         "clang"
#elif defined(__GNUC__)
         "gcc"
#else
         "unknown-compiler"
#endif
#if defined(GDI_RMA_DEBUG_BUILD)
         " debug"
#else
         " release"
#endif
         ")";
}

const char* gdi_rma_error_string(int code) {
  /* GDI's own classes plus the wrapper-local ones; static storage. */
  switch (code) {
    case GDI_RMA_OK: return "GDI_RMA_OK";
    case GDI_RMA_WARNING_NON_UNIQUE_ID: return "GDI_RMA_WARNING_NON_UNIQUE_ID";
    case GDI_RMA_WARNING_OTHER: return "GDI_RMA_WARNING_OTHER";
    case GDI_RMA_ERROR_ACCESS: return "GDI_RMA_ERROR_ACCESS";
    case GDI_RMA_ERROR_ARGUMENT: return "GDI_RMA_ERROR_ARGUMENT";
    case GDI_RMA_ERROR_BAD_FILE: return "GDI_RMA_ERROR_BAD_FILE";
    case GDI_RMA_ERROR_BUFFER: return "GDI_RMA_ERROR_BUFFER";
    case GDI_RMA_ERROR_CONSTRAINT: return "GDI_RMA_ERROR_CONSTRAINT";
    case GDI_RMA_ERROR_CONVERSION: return "GDI_RMA_ERROR_CONVERSION";
    case GDI_RMA_ERROR_COUNT: return "GDI_RMA_ERROR_COUNT";
    case GDI_RMA_ERROR_DATABASE: return "GDI_RMA_ERROR_DATABASE";
    case GDI_RMA_ERROR_DATATYPE: return "GDI_RMA_ERROR_DATATYPE";
    case GDI_RMA_ERROR_DATE: return "GDI_RMA_ERROR_DATE";
    case GDI_RMA_ERROR_DATETIME: return "GDI_RMA_ERROR_DATETIME";
    case GDI_RMA_ERROR_DECIMAL: return "GDI_RMA_ERROR_DECIMAL";
    case GDI_RMA_ERROR_EDGE: return "GDI_RMA_ERROR_EDGE";
    case GDI_RMA_ERROR_EDGE_ORIENTATION: return "GDI_RMA_ERROR_EDGE_ORIENTATION";
    case GDI_RMA_ERROR_EMPTY_NAME: return "GDI_RMA_ERROR_EMPTY_NAME";
    case GDI_RMA_ERROR_ERROR_CODE: return "GDI_RMA_ERROR_ERROR_CODE";
    case GDI_RMA_ERROR_FILE_EXISTS: return "GDI_RMA_ERROR_FILE_EXISTS";
    case GDI_RMA_ERROR_FILE_IN_USE: return "GDI_RMA_ERROR_FILE_IN_USE";
    case GDI_RMA_ERROR_INCOMPATIBLE_TRANSACTIONS:
      return "GDI_RMA_ERROR_INCOMPATIBLE_TRANSACTIONS";
    case GDI_RMA_ERROR_INDEX: return "GDI_RMA_ERROR_INDEX";
    case GDI_RMA_ERROR_LABEL: return "GDI_RMA_ERROR_LABEL";
    case GDI_RMA_ERROR_NAME_EXISTS: return "GDI_RMA_ERROR_NAME_EXISTS";
    case GDI_RMA_ERROR_NO_MEMORY: return "GDI_RMA_ERROR_NO_MEMORY";
    case GDI_RMA_ERROR_NO_PROPERTY: return "GDI_RMA_ERROR_NO_PROPERTY";
    case GDI_RMA_ERROR_NO_SPACE: return "GDI_RMA_ERROR_NO_SPACE";
    case GDI_RMA_ERROR_NO_SUCH_FILE: return "GDI_RMA_ERROR_NO_SUCH_FILE";
    case GDI_RMA_ERROR_NON_UNIQUE_ID: return "GDI_RMA_ERROR_NON_UNIQUE_ID";
    case GDI_RMA_ERROR_NOT_SAME: return "GDI_RMA_ERROR_NOT_SAME";
    case GDI_RMA_ERROR_OBJECT_MISMATCH: return "GDI_RMA_ERROR_OBJECT_MISMATCH";
    case GDI_RMA_ERROR_OP: return "GDI_RMA_ERROR_OP";
    case GDI_RMA_ERROR_OP_DATATYPE_MISMATCH: return "GDI_RMA_ERROR_OP_DATATYPE_MISMATCH";
    case GDI_RMA_ERROR_PROPERTY_EXISTS: return "GDI_RMA_ERROR_PROPERTY_EXISTS";
    case GDI_RMA_ERROR_PROPERTY_TYPE: return "GDI_RMA_ERROR_PROPERTY_TYPE";
    case GDI_RMA_ERROR_PROPERTY_TYPE_EXISTS: return "GDI_RMA_ERROR_PROPERTY_TYPE_EXISTS";
    case GDI_RMA_ERROR_RANGE: return "GDI_RMA_ERROR_RANGE";
    case GDI_RMA_ERROR_READ_ONLY_FILE: return "GDI_RMA_ERROR_READ_ONLY_FILE";
    case GDI_RMA_ERROR_READ_ONLY_PROPERTY_TYPE: return "GDI_RMA_ERROR_READ_ONLY_PROPERTY_TYPE";
    case GDI_RMA_ERROR_READ_ONLY_TRANSACTION: return "GDI_RMA_ERROR_READ_ONLY_TRANSACTION";
    case GDI_RMA_ERROR_RESOURCE: return "GDI_RMA_ERROR_RESOURCE";
    case GDI_RMA_ERROR_SIZE: return "GDI_RMA_ERROR_SIZE";
    case GDI_RMA_ERROR_SIZE_LIMIT: return "GDI_RMA_ERROR_SIZE_LIMIT";
    case GDI_RMA_ERROR_STALE: return "GDI_RMA_ERROR_STALE";
    case GDI_RMA_ERROR_STATE: return "GDI_RMA_ERROR_STATE";
    case GDI_RMA_ERROR_SUBCONSTRAINT: return "GDI_RMA_ERROR_SUBCONSTRAINT";
    case GDI_RMA_ERROR_TIME: return "GDI_RMA_ERROR_TIME";
    case GDI_RMA_ERROR_TRANSACTION: return "GDI_RMA_ERROR_TRANSACTION";
    case GDI_RMA_ERROR_UID: return "GDI_RMA_ERROR_UID";
    case GDI_RMA_ERROR_VERTEX: return "GDI_RMA_ERROR_VERTEX";
    case GDI_RMA_ERROR_WRONG_TYPE: return "GDI_RMA_ERROR_WRONG_TYPE";
    case GDI_RMA_ERROR_INVALID_DATE: return "GDI_RMA_ERROR_INVALID_DATE";
    case GDI_RMA_ERROR_COMMUNICATOR: return "GDI_RMA_ERROR_COMMUNICATOR";
    case GDI_RMA_ERROR_BLOCK_SIZE: return "GDI_RMA_ERROR_BLOCK_SIZE";
    case GDI_RMA_ERROR_QUOTA: return "GDI_RMA_ERROR_QUOTA";
    case GDI_RMA_ERROR_TRUNCATE: return "GDI_RMA_ERROR_TRUNCATE";
    case GDI_RMA_ERROR_TRANSACTION_CRITICAL: return "GDI_RMA_ERROR_TRANSACTION_CRITICAL";
    case GDI_RMA_ERROR_TRANSACTION_COMMIT_FAIL: return "GDI_RMA_ERROR_TRANSACTION_COMMIT_FAIL";
    case GDI_RMA_ERROR_INTERN: return "GDI_RMA_ERROR_INTERN";
    case GDI_RMA_ERROR_IO: return "GDI_RMA_ERROR_IO";
    case GDI_RMA_ERROR_OTHER: return "GDI_RMA_ERROR_OTHER";
    case GDI_RMA_ERROR_UNKNOWN: return "GDI_RMA_ERROR_UNKNOWN";

    case GDI_RMA_ERR_NOT_INITIALIZED:
      return "GDI_RMA_ERR_NOT_INITIALIZED: gdi_rma_init has not run (or already unwound)";
    case GDI_RMA_ERR_MPI_UNAVAILABLE:
      return "GDI_RMA_ERR_MPI_UNAVAILABLE: MPI_Init or GDI_Init failed";
    case GDI_RMA_ERR_INVALID_ARGUMENT:
      return "GDI_RMA_ERR_INVALID_ARGUMENT: a required argument was NULL or out of range";
    case GDI_RMA_ERR_INVALID_HANDLE:
      return "GDI_RMA_ERR_INVALID_HANDLE: the handle is NULL or already freed";
    case GDI_RMA_ERR_EDGE_NOT_ASSOCIATED:
      return "GDI_RMA_ERR_EDGE_NOT_ASSOCIATED: GDI-RMA has no GDI_AssociateEdge; an edge is "
             "addressable only inside the transaction that created it";
    case GDI_RMA_ERR_NOT_FOUND:
      return "GDI_RMA_ERR_NOT_FOUND: no such label or property type in this database";
    case GDI_RMA_ERR_NAME_TOO_LONG:
      return "GDI_RMA_ERR_NAME_TOO_LONG: schema names must be 1..63 bytes (GDI_MAX_OBJECT_NAME-1)";
    case GDI_RMA_ERR_TYPE_MISMATCH:
      return "GDI_RMA_ERR_TYPE_MISMATCH: the value dtype/size disagrees with the property type";
    case GDI_RMA_ERR_EDGES_EXIST:
      return "GDI_RMA_ERR_EDGES_EXIST: the vertex still has incident edges (guarded delete)";
    case GDI_RMA_ERR_UNSUPPORTED:
      return "GDI_RMA_ERR_UNSUPPORTED: not implemented by GDI-RMA";
    case GDI_RMA_ERR_OUT_OF_MEMORY:
      return "GDI_RMA_ERR_OUT_OF_MEMORY: the wrapper could not allocate its own bookkeeping";
    default: return NULL;
  }
}

/* ------------------------------------------------------------------------- */
/* thread-local error context                                                 */
/* ------------------------------------------------------------------------- */

GDI_RMA_THREAD_LOCAL char gdi_rma_last_error_buffer[GDI_RMA_ERRBUF_SIZE] = {0};

const char* gdi_rma_last_error(void) { return gdi_rma_last_error_buffer; }

void gdi_rma_clear_error(void) { gdi_rma_last_error_buffer[0] = '\0'; }

int gdi_rma_fail(int code, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(gdi_rma_last_error_buffer, sizeof(gdi_rma_last_error_buffer), fmt, args);
  va_end(args);
  return code;
}

int gdi_rma_from_gdi(int gdi_status, const char* call) {
  if (gdi_status == GDI_SUCCESS) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  const char* name = gdi_rma_error_string(gdi_status);
  if (name != NULL) {
    return gdi_rma_fail(gdi_status, "%s failed: %s (%d)", call, name, gdi_status);
  }
  return gdi_rma_fail(gdi_status, "%s failed: unknown GDI status (%d)", call, gdi_status);
}

/* ------------------------------------------------------------------------- */
/* one-heap free + result destructors                                          */
/* ------------------------------------------------------------------------- */

bool gdi_rma_free(void* ptr) {
  if (ptr == NULL) {
    /* nothing to do; reported as false so a caller can tell the difference */
    return false;
  }
  free(ptr);
  return true;
}

void gdi_rma_blob_free(gdi_rma_blob* blob) {
  if (blob == NULL) {
    return;
  }
  free(blob->data);
  blob->data = NULL;
  blob->size = 0;
}

void gdi_rma_strlist_free(gdi_rma_strlist* list) {
  if (list == NULL) {
    return;
  }
  if (list->items != NULL) {
    for (size_t i = 0; i < list->count; i++) {
      free(list->items[i]);
    }
    free(list->items);
  }
  free(list->lens);
  list->items = NULL;
  list->lens = NULL;
  list->count = 0;
}

void gdi_rma_edge_page_free(gdi_rma_edge_page* page) {
  if (page == NULL) {
    return;
  }
  free(page->uids);
  page->uids = NULL;
  page->returned = 0;
  page->full_count = 0;
}

void gdi_rma_vertex_page_free(gdi_rma_vertex_page* page) {
  if (page == NULL) {
    return;
  }
  free(page->uids);
  page->uids = NULL;
  page->returned = 0;
  page->full_count = 0;
}

size_t gdi_rma_datatype_size(uint32_t dtype) {
  size_t size = 0;
  if (GDI_GetSizeOfDatatype(&size, (GDI_Datatype)dtype) != GDI_SUCCESS) {
    return 0;
  }
  return size;
}

/* ------------------------------------------------------------------------- */
/* lifecycle: refcounted MPI + GDI init                                       */
/* ------------------------------------------------------------------------- */

#define GDI_RMA_DEFAULT_BLOCK_SIZE 256u
#define GDI_RMA_DEFAULT_MEMORY_SIZE 0x1000000ull /* 16 MiB per rank */

static gdi_rma_atomic g_refcount;      /* init references held */
static gdi_rma_atomic g_open_databases; /* live gdi_rma_db objects */
/* non-zero while the reference count is held and this library called MPI_Init
 * itself (if the host had already initialized MPI we must not finalize it);
 * only read/written under g_init_lock, so a plain int is enough */
static int g_mpi_owned = 0;
static gdi_rma_spinlock g_init_lock;

static uint32_t g_block_size = GDI_RMA_DEFAULT_BLOCK_SIZE;
static uint64_t g_memory_size = GDI_RMA_DEFAULT_MEMORY_SIZE;

static void g_lock(void) { gdi_rma_spin_lock(&g_init_lock); }
static void g_unlock(void) { gdi_rma_spin_unlock(&g_init_lock); }

int gdi_rma_init(const gdi_rma_init_options* opts) {
  g_lock();
  if (opts != NULL) {
    if (opts->struct_size != sizeof(gdi_rma_init_options)) {
      g_unlock();
      return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                          "gdi_rma_init: opts->struct_size is %u, expected %u",
                          opts->struct_size, (uint32_t)sizeof(gdi_rma_init_options));
    }
    /* the first call captures the sizing; a later call with different values is
     * ignored (process-wide state) rather than silently reconfiguring. */
    if (gdi_rma_atomic_load(&g_refcount) == 0) {
      g_block_size = opts->block_size != 0 ? opts->block_size : GDI_RMA_DEFAULT_BLOCK_SIZE;
      g_memory_size = opts->memory_size != 0 ? opts->memory_size : GDI_RMA_DEFAULT_MEMORY_SIZE;
    }
  }

  const long count = gdi_rma_atomic_load(&g_refcount);
  if (count == 0) {
    int mpi_already = 0;
    int st = MPI_Initialized(&mpi_already);
    if (st != MPI_SUCCESS) {
      g_unlock();
      return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                          "gdi_rma_init: MPI_Initialized failed with MPI code %d", st);
    }
    if (!mpi_already) {
      st = MPI_Init(NULL, NULL);
      if (st != MPI_SUCCESS) {
        g_unlock();
        return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                            "gdi_rma_init: MPI_Init failed with MPI code %d "
                            "(is this process an MPI rank? run it under mpiexec)",
                            st);
      }
      g_mpi_owned = 1;
    } else {
      g_mpi_owned = 0; /* borrowed environment: never MPI_Finalize() */
    }
    st = GDI_Init(NULL, NULL);
    if (st != GDI_SUCCESS) {
      if (g_mpi_owned) {
        MPI_Finalize();
        g_mpi_owned = 0;
      }
      g_unlock();
      return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                          "gdi_rma_init: GDI_Init failed, GDI status %d (%s)", st,
                          gdi_rma_error_string(st));
    }
  }
  gdi_rma_atomic_store(&g_refcount, count + 1);
  g_unlock();
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_finalize(void) {
  g_lock();
  const long count = gdi_rma_atomic_load(&g_refcount);
  if (count == 0) {
    g_unlock();
    return GDI_RMA_FAIL(GDI_RMA_ERR_NOT_INITIALIZED, "gdi_rma_finalize: no reference held");
  }
  const long dbs = gdi_rma_atomic_load(&g_open_databases);
  if (dbs != 0) {
    g_unlock();
    return GDI_RMA_FAIL(GDI_RMA_ERROR_DATABASE,
                        "gdi_rma_finalize: %ld database(s) still open (free them first)", dbs);
  }
  gdi_rma_atomic_store(&g_refcount, count - 1);
  if (count - 1 == 0) {
    int st = GDI_Finalize();
    int mpi_st = MPI_SUCCESS;
    if (g_mpi_owned) {
      mpi_st = MPI_Finalize();
      g_mpi_owned = 0;
    }
    g_block_size = GDI_RMA_DEFAULT_BLOCK_SIZE;
    g_memory_size = GDI_RMA_DEFAULT_MEMORY_SIZE;
    g_unlock();
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, "GDI_Finalize");
    }
    if (mpi_st != MPI_SUCCESS) {
      return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                          "gdi_rma_finalize: MPI_Finalize failed with MPI code %d", mpi_st);
    }
  } else {
    g_unlock();
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

bool gdi_rma_is_initialized(void) { return gdi_rma_atomic_load(&g_refcount) > 0; }

uint32_t gdi_rma_refcount(void) {
  long count = gdi_rma_atomic_load(&g_refcount);
  return count > 0 ? (uint32_t)count : 0u;
}

int gdi_rma_mpi_rank(int* out_rank) {
  if (out_rank == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_mpi_rank: out_rank is NULL");
  }
  if (gdi_rma_atomic_load(&g_refcount) == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_NOT_INITIALIZED, "gdi_rma_mpi_rank: not initialized");
  }
  int st = MPI_Comm_rank(MPI_COMM_WORLD, out_rank);
  if (st != MPI_SUCCESS) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                        "gdi_rma_mpi_rank: MPI_Comm_rank failed with MPI code %d", st);
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_mpi_size(int* out_size) {
  if (out_size == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_mpi_size: out_size is NULL");
  }
  if (gdi_rma_atomic_load(&g_refcount) == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_NOT_INITIALIZED, "gdi_rma_mpi_size: not initialized");
  }
  int st = MPI_Comm_size(MPI_COMM_WORLD, out_size);
  if (st != MPI_SUCCESS) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_MPI_UNAVAILABLE,
                        "gdi_rma_mpi_size: MPI_Comm_size failed with MPI code %d", st);
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* database                                                                   */
/* ------------------------------------------------------------------------- */

int gdi_rma_db_create(gdi_rma_db** out_db) {
  if (out_db == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_db_create: out_db is NULL");
  }
  *out_db = NULL;
  if (gdi_rma_atomic_load(&g_refcount) == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_NOT_INITIALIZED,
                        "gdi_rma_db_create: call gdi_rma_init first");
  }

  gdi_rma_db* db = (gdi_rma_db*)calloc(1, sizeof(gdi_rma_db));
  if (db == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_db_create: out of memory");
  }
  db->block_size = g_block_size;
  db->memory_size = g_memory_size;

  GDA_Init_params params;
  memset(&params, 0, sizeof(params));
  params.comm = MPI_COMM_WORLD;
  params.memory_size = (MPI_Aint)db->memory_size;
  params.block_size = db->block_size;

  GDI_Database gdi_db = GDI_DATABASE_NULL;
  int st = GDI_CreateDatabase(&params, sizeof(GDA_Init_params), &gdi_db);
  if (st != GDI_SUCCESS) {
    free(db);
    return gdi_rma_from_gdi(st, "GDI_CreateDatabase");
  }

  db->magic = GDI_RMA_MAGIC_DB;
  db->gdi = gdi_db;
  gdi_rma_cache_init(&db->labels);
  gdi_rma_cache_init(&db->ptypes);
  gdi_rma_boxes_init(&db->boxes);

  gdi_rma_atomic_add(&g_open_databases, 1);
  *out_db = db;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_db_free(gdi_rma_db* db) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "gdi_rma_db_free: not a live database handle");
  }
  GDI_Database gdi_db = db->gdi;
  int st = GDI_FreeDatabase(&gdi_db);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreeDatabase");
  }
  gdi_rma_cache_free(&db->labels);
  gdi_rma_cache_free(&db->ptypes);
  gdi_rma_boxes_free(&db->boxes);
  db->magic = GDI_RMA_MAGIC_FREED;
  free(db);
  gdi_rma_atomic_add(&g_open_databases, -1);
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_db_block_size(const gdi_rma_db* db, uint32_t* out_block_size) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "gdi_rma_db_block_size: bad database handle");
  }
  if (out_block_size == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_db_block_size: out is NULL");
  }
  *out_block_size = db->gdi->block_size;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_db_memory_size(const gdi_rma_db* db, uint64_t* out_memory_size) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "gdi_rma_db_memory_size: bad database handle");
  }
  if (out_memory_size == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_db_memory_size: out is NULL");
  }
  *out_memory_size = (uint64_t)db->gdi->memsize;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* transactions                                                               */
/* ------------------------------------------------------------------------- */

static gdi_rma_tx* tx_alloc(gdi_rma_db* db, GDI_Transaction gdi_tx, int collective) {
  gdi_rma_tx* tx = (gdi_rma_tx*)calloc(1, sizeof(gdi_rma_tx));
  if (tx == NULL) {
    return NULL;
  }
  tx->magic = GDI_RMA_MAGIC_TX;
  tx->gdi = gdi_tx;
  tx->db = db;
  tx->collective = collective != 0;
  gdi_rma_registry_init(&tx->edges);
  return tx;
}

int gdi_rma_tx_begin(gdi_rma_db* db, gdi_rma_tx** out_tx) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "gdi_rma_tx_begin: bad database handle");
  }
  if (out_tx == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_tx_begin: out_tx is NULL");
  }
  *out_tx = NULL;
  GDI_Transaction gdi_tx = GDI_TRANSACTION_NULL;
  int st = GDI_StartTransaction(db->gdi, &gdi_tx);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_StartTransaction");
  }
  gdi_rma_tx* tx = tx_alloc(db, gdi_tx, 0);
  if (tx == NULL) {
    GDI_CloseTransaction(&gdi_tx, GDI_TRANSACTION_ABORT);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_tx_begin: out of memory");
  }
  *out_tx = tx;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_tx_begin_collective(gdi_rma_db* db, gdi_rma_tx** out_tx) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE,
                        "gdi_rma_tx_begin_collective: bad database handle");
  }
  if (out_tx == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_tx_begin_collective: out_tx is NULL");
  }
  *out_tx = NULL;
  GDI_Transaction gdi_tx = GDI_TRANSACTION_NULL;
  int st = GDI_StartCollectiveTransaction(db->gdi, &gdi_tx);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_StartCollectiveTransaction");
  }
  gdi_rma_tx* tx = tx_alloc(db, gdi_tx, 1);
  if (tx == NULL) {
    GDI_CloseCollectiveTransaction(&gdi_tx, GDI_TRANSACTION_COMMIT);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY,
                        "gdi_rma_tx_begin_collective: out of memory");
  }
  *out_tx = tx;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

static int tx_close(gdi_rma_tx* tx, int commit) {
  if (tx == NULL || tx->magic != GDI_RMA_MAGIC_TX) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE,
                        "gdi_rma_tx_close: not a live transaction handle");
  }
  /* the caller keeps the GDI_Transaction inside the wrapper box: GDI writes the
   * out-parameter (GDI_TRANSACTION_NULL) while closing */
  int ctype = commit ? GDI_TRANSACTION_COMMIT : GDI_TRANSACTION_ABORT;
  int st = tx->collective ? GDI_CloseCollectiveTransaction(&tx->gdi, ctype)
                         : GDI_CloseTransaction(&tx->gdi, ctype);
  gdi_rma_registry_free(&tx->edges);
  tx->magic = GDI_RMA_MAGIC_FREED;
  free(tx);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, commit ? "GDI_CloseTransaction(COMMIT)"
                                       : "GDI_CloseTransaction(ABORT)");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_tx_commit(gdi_rma_tx* tx) { return tx_close(tx, 1); }

int gdi_rma_tx_abort(gdi_rma_tx* tx) { return tx_close(tx, 0); }

int gdi_rma_tx_is_collective(const gdi_rma_tx* tx, bool* out_collective) {
  if (tx == NULL || tx->magic != GDI_RMA_MAGIC_TX) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "gdi_rma_tx_is_collective: bad tx handle");
  }
  if (out_collective == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_tx_is_collective: out is NULL");
  }
  *out_collective = tx->collective;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* name -> handle cache                                                       */
/* ------------------------------------------------------------------------- */

static uint64_t g_name_hash(const char* name, size_t name_len) {
  /* djb2 over the explicit byte length (the FFI passes names that are not
   * NUL-terminated, so GDA_djb2_hash on the raw pointer is not usable here). */
  uint64_t hash = 5381ull;
  const unsigned char* bytes = (const unsigned char*)name;
  for (size_t i = 0; i < name_len; i++) {
    hash = ((hash << 5) + hash) + bytes[i];
  }
  return hash;
}

void gdi_rma_cache_init(gdi_rma_cache* cache) {
  memset(cache, 0, sizeof(*cache));
  GDA_hashmap_create(&cache->index, sizeof(uint64_t) /* key */, 32 /* capacity */,
                     sizeof(size_t) /* value: index into items */, &GDA_int64_to_int);
}

void gdi_rma_cache_free(gdi_rma_cache* cache) {
  for (size_t i = 0; i < cache->count; i++) {
    free(cache->items[i].name);
  }
  free(cache->items);
  cache->items = NULL;
  cache->count = 0;
  cache->capacity = 0;
  GDA_hashmap_free(&cache->index);
}

static int gdi_rma_cache_grow(gdi_rma_cache* cache) {
  size_t capacity = cache->capacity == 0 ? 8 : cache->capacity * 2;
  gdi_rma_cache_entry* items =
      (gdi_rma_cache_entry*)realloc(cache->items, capacity * sizeof(gdi_rma_cache_entry));
  if (items == NULL) {
    return 0;
  }
  cache->items = items;
  cache->capacity = capacity;
  return 1;
}

void* gdi_rma_cache_get(gdi_rma_cache* cache, const char* name, size_t name_len) {
  uint64_t key = g_name_hash(name, name_len);
  int32_t pos = GDA_hashmap_find(cache->index, &key);
  if (pos != GDA_HASHMAP_NOT_FOUND) {
    size_t idx = *(size_t*)GDA_hashmap_get_at(cache->index, (size_t)pos);
    gdi_rma_cache_entry* entry = &cache->items[idx];
    /* validate the stored name byte-for-byte: a 64-bit hash collision must not
     * silently resolve to a different schema object */
    if (entry->alive && strlen(entry->name) == name_len &&
        memcmp(entry->name, name, name_len) == 0) {
      return entry->handle;
    }
  }
  return NULL;
}

void gdi_rma_cache_put(gdi_rma_cache* cache, const char* name, size_t name_len, void* handle) {
  if (name_len == 0 || name == NULL) {
    return;
  }
  char* copy = (char*)malloc(name_len + 1);
  if (copy == NULL) {
    return; /* the cache is an optimization; GDI stays the source of truth */
  }
  memcpy(copy, name, name_len);
  copy[name_len] = '\0';

  /* replace an existing entry for the same name, if any */
  for (size_t i = 0; i < cache->count; i++) {
    gdi_rma_cache_entry* entry = &cache->items[i];
    if (entry->alive && strlen(entry->name) == name_len &&
        memcmp(entry->name, name, name_len) == 0) {
      free(copy);
      entry->handle = handle;
      return;
    }
  }

  if (cache->count == cache->capacity && !gdi_rma_cache_grow(cache)) {
    free(copy);
    return;
  }
  size_t idx = cache->count++;
  cache->items[idx].name = copy;
  cache->items[idx].handle = handle;
  cache->items[idx].alive = 1;

  uint64_t key = g_name_hash(name, name_len);
  /* a hash collision on the index map just means the fast path misses for one
   * of the two names; the strcmp validation keeps the answer correct */
  GDA_hashmap_insert(cache->index, &key, &idx);
}

void gdi_rma_cache_forget_handle(gdi_rma_cache* cache, void* handle) {
  if (handle == NULL) {
    return;
  }
  for (size_t i = 0; i < cache->count; i++) {
    if (cache->items[i].alive && cache->items[i].handle == handle) {
      cache->items[i].alive = 0;
      cache->items[i].handle = NULL;
      uint64_t key = g_name_hash(cache->items[i].name, strlen(cache->items[i].name));
      GDA_hashmap_erase(cache->index, &key);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* handle boxes                                                               */
/* ------------------------------------------------------------------------- */

void gdi_rma_boxes_init(gdi_rma_box_list* boxes) {
  boxes->items = NULL;
  boxes->count = 0;
  boxes->capacity = 0;
}

void gdi_rma_boxes_push(gdi_rma_box_list* boxes, void* box) {
  if (boxes->count == boxes->capacity) {
    size_t capacity = boxes->capacity == 0 ? 16 : boxes->capacity * 2;
    void** items = (void**)realloc(boxes->items, capacity * sizeof(void*));
    if (items == NULL) {
      return;
    }
    boxes->items = items;
    boxes->capacity = capacity;
  }
  boxes->items[boxes->count++] = box;
}

void gdi_rma_boxes_free(gdi_rma_box_list* boxes) {
  for (size_t i = 0; i < boxes->count; i++) {
    free(boxes->items[i]);
  }
  free(boxes->items);
  boxes->items = NULL;
  boxes->count = 0;
  boxes->capacity = 0;
}

/* ------------------------------------------------------------------------- */
/* edge registry                                                              */
/* ------------------------------------------------------------------------- */

static uint32_t g_uid12_to_int(void* key, size_t key_size, size_t capacity) {
  /* FNV-1a over the packed 12-byte uid */
  const unsigned char* bytes = (const unsigned char*)key;
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < key_size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return (uint32_t)(hash % (uint64_t)capacity);
}

void gdi_rma_registry_init(gdi_rma_edge_registry* reg) {
  reg->map = NULL;
  GDA_hashmap_create(&reg->map, sizeof(gdi_rma_edge_uid), 64, sizeof(GDI_EdgeHolder),
                     &g_uid12_to_int);
}

void gdi_rma_registry_free(gdi_rma_edge_registry* reg) {
  if (reg->map != NULL) {
    GDA_hashmap_free(&reg->map);
  }
}

void gdi_rma_registry_put(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid,
                         GDI_EdgeHolder holder) {
  GDA_hashmap_insert(reg->map, (void*)uid, &holder);
}

GDI_EdgeHolder gdi_rma_registry_get(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid) {
  void* slot = GDA_hashmap_get(reg->map, (void*)uid);
  if (slot == NULL) {
    return GDI_EDGE_NULL;
  }
  return *(GDI_EdgeHolder*)slot;
}

void gdi_rma_registry_erase(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid) {
  GDA_hashmap_erase(reg->map, (void*)uid);
}

/* ------------------------------------------------------------------------- */
/* name / property validation helpers                                          */
/* ------------------------------------------------------------------------- */

char* gdi_rma_dup_name(const char* name, size_t name_len, int* out_code) {
  if (name == NULL) {
    if (out_code != NULL) {
      *out_code = GDI_RMA_ERR_INVALID_ARGUMENT;
    }
    gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT, "a name pointer is NULL");
    return NULL;
  }
  if (name_len == 0) {
    if (out_code != NULL) {
      *out_code = GDI_RMA_ERR_INVALID_ARGUMENT;
    }
    gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT, "an empty name was passed");
    return NULL;
  }
  if (name_len > (size_t)(GDI_MAX_OBJECT_NAME - 1)) {
    /* GDI truncates silently at GDI_MAX_OBJECT_NAME-1 bytes; refusing here keeps
     * a caller from creating a label whose real name differs from its request. */
    if (out_code != NULL) {
      *out_code = GDI_RMA_ERR_NAME_TOO_LONG;
    }
    gdi_rma_fail(GDI_RMA_ERR_NAME_TOO_LONG,
                 "name of %u bytes exceeds the GDI limit of %d", (unsigned)name_len,
                 GDI_MAX_OBJECT_NAME - 1);
    return NULL;
  }
  for (size_t i = 0; i < name_len; i++) {
    if (name[i] == '\0') {
      if (out_code != NULL) {
        *out_code = GDI_RMA_ERR_INVALID_ARGUMENT;
      }
      gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT, "name contains an embedded NUL at byte %u",
                   (unsigned)i);
      return NULL;
    }
  }
  char* copy = (char*)malloc(name_len + 1);
  if (copy == NULL) {
    if (out_code != NULL) {
      *out_code = GDI_RMA_ERR_OUT_OF_MEMORY;
    }
    gdi_rma_fail(GDI_RMA_ERR_OUT_OF_MEMORY, "dup_name: out of memory for a %u-byte name",
                 (unsigned)name_len);
    return NULL;
  }
  memcpy(copy, name, name_len);
  copy[name_len] = '\0';
  if (out_code != NULL) {
    *out_code = GDI_RMA_OK;
  }
  return copy;
}

int gdi_rma_check_property(const gdi_rma_property_ref* property, GDI_PropertyType ptype) {
  if (property == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "property ref is NULL");
  }
  if (property->ptype_name == NULL || property->ptype_name_len == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "property ref carries no name");
  }
  if (property->dtype != (uint32_t)ptype->dtype) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_TYPE_MISMATCH,
                        "property '%s' is declared with dtype %u but %u was passed",
                        ptype->name, (unsigned)ptype->dtype, property->dtype);
  }
  size_t element_size = 0;
  if (GDI_GetSizeOfDatatype(&element_size, (GDI_Datatype)property->dtype) != GDI_SUCCESS) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_DATATYPE, "dtype %u has no size (invalid)",
                        property->dtype);
  }
  if (property->count == 0 && property->value_bytes != 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_SIZE, "count 0 with %u value bytes",
                        (unsigned)property->value_bytes);
  }
  if (property->value_bytes != (size_t)property->count * element_size) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_TYPE_MISMATCH,
                        "property '%s': %u value bytes for count %u and a %u-byte dtype",
                        ptype->name, (unsigned)property->value_bytes, property->count,
                        (unsigned)element_size);
  }
  if (property->count > 0 && property->value == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_BUFFER, "property '%s': count %u with a NULL value",
                        ptype->name, property->count);
  }
  return GDI_RMA_OK;
}

int gdi_rma_check_condition_value(uint32_t dtype, uint32_t count, const void* value,
                                  size_t value_bytes) {
  size_t element_size = 0;
  if (GDI_GetSizeOfDatatype(&element_size, (GDI_Datatype)dtype) != GDI_SUCCESS) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_DATATYPE, "condition dtype %u is invalid", dtype);
  }
  if (value_bytes != (size_t)count * element_size) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_TYPE_MISMATCH,
                        "condition: %u value bytes for count %u and a %u-byte dtype",
                        (unsigned)value_bytes, count, (unsigned)element_size);
  }
  if (count > 0 && value == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_BUFFER, "condition: count %u with a NULL value", count);
  }
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* byte writer (packed read buffers)                                          */
/* ------------------------------------------------------------------------- */

void gdi_rma_writer_init(gdi_rma_writer* w) {
  w->data = NULL;
  w->size = 0;
  w->capacity = 0;
  w->ok = 1;
}

int gdi_rma_writer_reserve(gdi_rma_writer* w, size_t extra) {
  size_t need = w->size + extra;
  if (need <= w->capacity) {
    return 1;
  }
  size_t capacity = w->capacity == 0 ? 64 : w->capacity;
  while (capacity < need) {
    capacity *= 2;
  }
  unsigned char* data = (unsigned char*)realloc(w->data, capacity);
  if (data == NULL) {
    w->ok = 0;
    return 0;
  }
  w->data = data;
  w->capacity = capacity;
  return 1;
}

void gdi_rma_writer_put(gdi_rma_writer* w, const void* bytes, size_t len) {
  if (!w->ok || !gdi_rma_writer_reserve(w, len)) {
    return;
  }
  if (len > 0) {
    memcpy(w->data + w->size, bytes, len);
    w->size += len;
  }
}

void gdi_rma_writer_put_u32(gdi_rma_writer* w, uint32_t v) { gdi_rma_writer_put(w, &v, 4); }

void gdi_rma_writer_pad_to(gdi_rma_writer* w, size_t alignment) {
  if (!w->ok) {
    return;
  }
  size_t rem = w->size % alignment;
  if (rem != 0) {
    static const unsigned char zeros[8] = {0};
    gdi_rma_writer_put(w, zeros, alignment - rem);
  }
}

void gdi_rma_writer_free(gdi_rma_writer* w) {
  free(w->data);
  w->data = NULL;
  w->size = 0;
  w->capacity = 0;
}

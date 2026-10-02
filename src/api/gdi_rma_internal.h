/*
 * gdi_rma_internal.h -- shared internals of the gdi_rma FFI layer (src/api).
 *
 * Included ONLY by the src/api/*.c translation units, which are compiled into
 * the same gdi_rma target as the GDI sources and may therefore reach into the
 * GDI/GDA internals (struct fields, GDA_hashmap, the predefined handles).
 * Nothing here is part of the published ABI; gdi_rma_api.h is.
 */
#ifndef GDI_RMA_INTERNAL_H
#define GDI_RMA_INTERNAL_H

#include "gdi.h"        /* the GDI public API + gda_* internals */
#include "gdi_rma_api.h" /* the C ABI this layer implements */
#include "gdi_rma_atomic.h" /* the lock-free refcount helpers (no stdatomic.h) */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* thread-local error context                                                 */
/* ------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#define GDI_RMA_THREAD_LOCAL __declspec(thread)
#else
#define GDI_RMA_THREAD_LOCAL _Thread_local
#endif

#define GDI_RMA_ERRBUF_SIZE 1024

/* The thread-local message buffer behind gdi_rma_last_error(). */
GDI_RMA_THREAD_LOCAL extern char gdi_rma_last_error_buffer[GDI_RMA_ERRBUF_SIZE];

/* Record a failure message (printf-style, truncated into the buffer) and
 * return `code` -- so the call sites read `return GDI_RMA_FAIL(code, "...")`. */
int gdi_rma_fail(int code, const char* fmt, ...);
/* Clear the message (called on the success path of a fallible entry point). */
void gdi_rma_clear_error(void);

#define GDI_RMA_FAIL(code, ...) gdi_rma_fail((code), __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* handle guards                                                              */
/* ------------------------------------------------------------------------- */

#define GDI_RMA_MAGIC_DB 0x47444230u /* "DB0" */
#define GDI_RMA_MAGIC_TX 0x47445458u /* "TX"  */
#define GDI_RMA_MAGIC_LBL 0x47444c42u /* "LB" */
#define GDI_RMA_MAGIC_PTP 0x47445054u /* "PT" */
#define GDI_RMA_MAGIC_CON 0x4744434eu /* "CN" */
#define GDI_RMA_MAGIC_FREED 0xDEADBEEFu

/* ------------------------------------------------------------------------- */
/* name -> handle cache (per database)                                        */
/* ------------------------------------------------------------------------- */

typedef struct gdi_rma_cache_entry {
  char* name;        /* owned NUL-terminated copy of the schema name */
  void* handle;      /* GDI_Label / GDI_PropertyType, owned by the database */
  int alive;         /* 0 after forget (the name array is kept till teardown) */
} gdi_rma_cache_entry;

typedef struct gdi_rma_cache {
  gdi_rma_cache_entry* items;   /* owned array, grows geometrically */
  size_t count;
  size_t capacity;
  GDA_HashMap* index;           /* uint64 name hash -> index into `items` */
} gdi_rma_cache;

void gdi_rma_cache_init(gdi_rma_cache* cache);
void gdi_rma_cache_free(gdi_rma_cache* cache);
/* Returns the handle for `name`, or NULL when the name is not cached.
 * The lookup validates the stored name with strcmp, so a 64-bit hash collision
 * reports a miss instead of handing back the wrong schema object. */
void* gdi_rma_cache_get(gdi_rma_cache* cache, const char* name, size_t name_len);
/* Insert (or replace) `name` -> `handle`.  Silently ignored on allocation
 * failure: the cache is an optimization, GDI remains the source of truth. */
void gdi_rma_cache_put(gdi_rma_cache* cache, const char* name, size_t name_len, void* handle);
/* Drop the cached entry carrying this handle (free/rename of a schema object). */
void gdi_rma_cache_forget_handle(gdi_rma_cache* cache, void* handle);

/* ------------------------------------------------------------------------- */
/* edge registry (per transaction)                                            */
/* ------------------------------------------------------------------------- */

/* GDI-RMA has no GDI_AssociateEdge, so an edge holder only exists inside the
 * transaction that created it.  The registry maps the 12-byte edge UID to the
 * live GDI_EdgeHolder; it dies with the transaction. */
typedef struct gdi_rma_edge_registry {
  GDA_HashMap* map; /* key: gdi_rma_edge_uid (12 bytes), value: GDI_EdgeHolder */
} gdi_rma_edge_registry;

void gdi_rma_registry_init(gdi_rma_edge_registry* reg);
void gdi_rma_registry_free(gdi_rma_edge_registry* reg);
void gdi_rma_registry_put(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid,
                         GDI_EdgeHolder holder);
GDI_EdgeHolder gdi_rma_registry_get(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid);
void gdi_rma_registry_erase(gdi_rma_edge_registry* reg, const gdi_rma_edge_uid* uid);

/* ------------------------------------------------------------------------- */
/* the real shape of the opaque handles                                       */
/* ------------------------------------------------------------------------- */

/* Label/property-type handle boxes are allocated by the database and retired
 * (magic -> FREED) instead of being released, so a stale handle that a caller
 * kept after GDI_FreeLabel/GDI_FreePropertyType is reported as
 * GDI_RMA_ERR_INVALID_HANDLE rather than dereferenced.  They are bounded by the
 * schema size and freed with the database. */
struct gdi_rma_label {
  uint32_t magic;
  GDI_Label gdi;
};

struct gdi_rma_ptype {
  uint32_t magic;
  GDI_PropertyType gdi;
};

/* A constraint condition kept by the wrapper (deep copies of name + value) so
 * the constraint can be REBUILT whenever the caller extends it: GDI copies a
 * subconstraint into the constraint at AddSubconstraintToConstraint time and
 * offers no way to remove one again. */
typedef struct gdi_rma_condition_stored {
  char* name;        /* owned, NUL-terminated */
  size_t name_len;
  uint32_t kind;
  uint32_t op;
  uint32_t dtype;
  uint32_t count;
  void* value;       /* owned copy, NULL when count == 0 */
  size_t value_bytes;
} gdi_rma_condition_stored;

struct gdi_rma_constraint {
  uint32_t magic;
  struct gdi_rma_db* db_owner;     /* the wrapper database (name caches) */
  GDI_Database db;                 /* the GDI database it is registered at */
  GDI_Constraint gdi;              /* republished on every extension */
  gdi_rma_condition_stored* conds;  /* the full AND-list */
  size_t cond_count;
  size_t cond_capacity;
};

typedef struct gdi_rma_box_list {
  void** items;
  size_t count;
  size_t capacity;
} gdi_rma_box_list;

void gdi_rma_boxes_init(gdi_rma_box_list* boxes);
void gdi_rma_boxes_push(gdi_rma_box_list* boxes, void* box);
void gdi_rma_boxes_free(gdi_rma_box_list* boxes);

struct gdi_rma_db {
  uint32_t magic;
  GDI_Database gdi;
  gdi_rma_cache labels;
  gdi_rma_cache ptypes;
  /* the wrapper-allocated ptype/label handle boxes, retired on free and
   * released at teardown */
  gdi_rma_box_list boxes;
  /* schema defaults captured at gdi_rma_init (block_size / memory_size) */
  uint32_t block_size;
  uint64_t memory_size;
};

struct gdi_rma_tx {
  uint32_t magic;
  GDI_Transaction gdi;
  gdi_rma_db* db;
  gdi_rma_edge_registry edges;
  bool collective;
};

/* The GDI constraint handle behind a wrapper constraint (GDI_CONSTRAINT_NULL for
 * a dead or NULL box).  Used by the read layer. */
GDI_Constraint gdi_rma_constraint_handle(const gdi_rma_constraint* constraint);

/* Turn an array of GDI_PropertyType handles into a library-allocated
 * gdi_rma_ptype_info array (release with gdi_rma_free).  Shared by the database
 * enumeration and the per-vertex property-type read. */
int gdi_rma_ptype_handles_to_info(gdi_rma_db* db, GDI_PropertyType* handles, size_t count,
                                 gdi_rma_ptype_info** out_array, size_t* out_count);

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

/* Translate a GDI status 1:1 into the ABI status space (they share the numeric
 * range 0..62) and record the failing GDI call in the thread-local context.
 * `GDI_RMA_OK` clears the message and returns 0. */
int gdi_rma_from_gdi(int gdi_status, const char* call);

/* Copy a (pointer,length) name into a fresh NUL-terminated C string that GDI
 * can consume.  Returns NULL on failure and stores the ABI status that
 * describes it in *out_code (when out_code is not NULL): EMPTY/NULL argument,
 * GDI_RMA_ERR_NAME_TOO_LONG, embedded NUL, or GDI_RMA_ERR_OUT_OF_MEMORY.  The
 * reason is also recorded in the thread-local last-error buffer. */
char* gdi_rma_dup_name(const char* name, size_t name_len, int* out_code);

/* Resolve a label by name through the database cache, creating it first when
 * `auto_create` is set.  Returns the wrapper box (never NULL on success). */
struct gdi_rma_label* gdi_rma_resolve_label(gdi_rma_db* db, const char* name, size_t name_len,
                                            int auto_create, int* out_status,
                                            int* out_created);

/* Resolve a property type by name through the database cache (never creates). */
struct gdi_rma_ptype* gdi_rma_resolve_ptype(gdi_rma_db* db, const char* name, size_t name_len,
                                            int* out_status);

/* Check a property ref against a resolved property type and normalize it:
 * fills *out_elements (the count GDI expects) and validates value_bytes. */
int gdi_rma_check_property(const gdi_rma_property_ref* property, GDI_PropertyType ptype);

/* Validate a (value,count) payload for a property type used as a constraint
 * condition value. */
int gdi_rma_check_condition_value(uint32_t dtype, uint32_t count, const void* value,
                                  size_t value_bytes);

/* Associate a vertex UID with tx and hand back the holder (GDI_AssociateVertex).
 * On GDI_RMA_ERROR_TRANSACTION_CRITICAL the message explains that the caller
 * must abort the transaction. */
int gdi_rma_associate_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                             GDI_VertexHolder* out);

/* The UID of a live holder: the primary block pointer. */
static inline gdi_rma_vertex_uid gdi_rma_vertex_uid_of(GDI_VertexHolder vertex) {
  gdi_rma_vertex_uid uid;
  uid.value = *(GDA_DPointer*)vertex->blocks->data;
  return uid;
}

/* The UID of an edge holder: origin vertex UID + origin slot offset (exactly
 * how GDI_GetEdgesOfVertex packs it). */
static inline gdi_rma_edge_uid gdi_rma_edge_uid_of(GDI_EdgeHolder edge) {
  gdi_rma_edge_uid uid;
  uid.origin_vertex_uid = *(GDA_DPointer*)edge->origin->blocks->data;
  uid.origin_offset = edge->origin_lightweight_edge_offset;
  return uid;
}

/* Append-only byte writer used by the property packer. */
typedef struct gdi_rma_writer {
  unsigned char* data;
  size_t size;
  size_t capacity;
  int ok;
} gdi_rma_writer;

void gdi_rma_writer_init(gdi_rma_writer* w);
int gdi_rma_writer_reserve(gdi_rma_writer* w, size_t extra);
void gdi_rma_writer_put(gdi_rma_writer* w, const void* bytes, size_t len);
void gdi_rma_writer_put_u32(gdi_rma_writer* w, uint32_t v);
void gdi_rma_writer_pad_to(gdi_rma_writer* w, size_t alignment);
void gdi_rma_writer_free(gdi_rma_writer* w);

#endif /* GDI_RMA_INTERNAL_H */

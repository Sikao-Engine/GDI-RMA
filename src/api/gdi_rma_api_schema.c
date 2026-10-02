/*
 * gdi_rma_api_schema.c -- labels, property types and constraints over the GDI
 * schema API, plus the per-database name -> handle resolution the graph calls
 * use (src/gdi_label.c, src/gdi_property_type.c, src/gdi_constraint.c).
 *
 * Lifetime model (documented in gdi_rma_api.h):
 *  - GDI owns every GDI_Label / GDI_PropertyType object; the wrapper only hands
 *    out thin boxes around those pointers so a stale handle can be detected.
 *  - the boxes live in db->boxes: a freed label/property type has its box
 *    retired (magic = FREED) but the memory stays allocated until
 *    gdi_rma_db_free, which bounds the leak by the schema size and turns
 *    use-after-free into GDI_RMA_ERR_INVALID_HANDLE.
 *  - the name caches speed up the by-name resolution the managed layer uses for
 *    every command; GDI's own name -> handle map stays the source of truth, and
 *    every cache hit is re-validated byte-for-byte (GDI resolves names through a
 *    64-bit hash map, so a collision would otherwise silently mean another name).
 *  - a constraint keeps its full condition list in the wrapper object and is
 *    REPUBLISHED (freed + rebuilt) on every extension, because GDI copies a
 *    subconstraint into the constraint at GDI_AddSubconstraintToConstraint time
 *    and provides no way to take one out again.  The caller's handle stays valid
 *    across the rebuild.
 */
#include "gdi_rma_internal.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static int check_db(const gdi_rma_db* db, const char* call) {
  if (db == NULL || db->magic != GDI_RMA_MAGIC_DB) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE, "%s: not a live database handle", call);
    return 0;
  }
  return 1;
}

static struct gdi_rma_label* label_box_new(gdi_rma_db* db, GDI_Label handle) {
  struct gdi_rma_label* box = (struct gdi_rma_label*)calloc(1, sizeof(*box));
  if (box == NULL) {
    return NULL;
  }
  box->magic = GDI_RMA_MAGIC_LBL;
  box->gdi = handle;
  gdi_rma_boxes_push(&db->boxes, box);
  return box;
}

static struct gdi_rma_ptype* ptype_box_new(gdi_rma_db* db, GDI_PropertyType handle) {
  struct gdi_rma_ptype* box = (struct gdi_rma_ptype*)calloc(1, sizeof(*box));
  if (box == NULL) {
    return NULL;
  }
  box->magic = GDI_RMA_MAGIC_PTP;
  box->gdi = handle;
  gdi_rma_boxes_push(&db->boxes, box);
  return box;
}

/* The stable box for a GDI handle the caller cannot name: look it up through
 * the handle's own name so repeated enumerations hand out the same pointer.
 * Returns NULL only on allocation failure. */
static struct gdi_rma_ptype* ptype_box_for(gdi_rma_db* db, GDI_PropertyType handle) {
  const char* name = handle->name;
  if (name != NULL) {
    struct gdi_rma_ptype* existing =
        (struct gdi_rma_ptype*)gdi_rma_cache_get(&db->ptypes, name, strlen(name));
    if (existing != NULL) {
      return existing;
    }
  }
  struct gdi_rma_ptype* box = ptype_box_new(db, handle);
  if (box != NULL && name != NULL) {
    gdi_rma_cache_put(&db->ptypes, name, strlen(name), box);
  }
  return box;
}

static int check_label_box(const struct gdi_rma_label* label, const char* call) {
  if (label == NULL || label->magic != GDI_RMA_MAGIC_LBL || label->gdi == GDI_LABEL_NULL) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE, "%s: not a live label handle", call);
    return 0;
  }
  return 1;
}

static int check_ptype_box(const struct gdi_rma_ptype* ptype, const char* call) {
  if (ptype == NULL || ptype->magic != GDI_RMA_MAGIC_PTP ||
      ptype->gdi == GDI_PROPERTY_TYPE_NULL) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE, "%s: not a live property type handle", call);
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* resolution used by the graph + read layers                                 */
/* ------------------------------------------------------------------------- */

struct gdi_rma_label* gdi_rma_resolve_label(gdi_rma_db* db, const char* name, size_t name_len,
                                            int auto_create, int* out_status, int* out_created) {
  if (out_status != NULL) {
    *out_status = GDI_RMA_OK;
  }
  if (out_created != NULL) {
    *out_created = 0;
  }
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(name, name_len, &code);
  if (copy == NULL) {
    if (out_status != NULL) {
      *out_status = code;
    }
    return NULL;
  }

  struct gdi_rma_label* box =
      (struct gdi_rma_label*)gdi_rma_cache_get(&db->labels, copy, strlen(copy));
  if (box != NULL) {
    free(copy);
    return box;
  }

  GDI_Label handle = GDI_LABEL_NULL;
  int st = GDI_GetLabelFromName(&handle, copy, db->gdi);
  if (st != GDI_SUCCESS) {
    free(copy);
    if (out_status != NULL) {
      *out_status = gdi_rma_from_gdi(st, "GDI_GetLabelFromName");
    }
    return NULL;
  }

  if (handle == GDI_LABEL_NULL) {
    if (!auto_create) {
      if (out_status != NULL) {
        *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_NOT_FOUND, "no label '%s' in this database", copy);
      }
      free(copy);
      return NULL;
    }
    st = GDI_CreateLabel(copy, db->gdi, &handle);
    if (st != GDI_SUCCESS) {
      free(copy);
      if (out_status != NULL) {
        *out_status = gdi_rma_from_gdi(st, "GDI_CreateLabel");
      }
      return NULL;
    }
    if (out_created != NULL) {
      *out_created = 1;
    }
  }

  /* GDI resolves names through a 64-bit hash map; re-validate the bytes so a
   * collision can never make one name mean another label. */
  if (handle->name == NULL || strlen(handle->name) != name_len ||
      memcmp(handle->name, name, name_len) != 0) {
    free(copy);
    if (out_status != NULL) {
      *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_NOT_FOUND,
                                 "GDI resolved the name to a different label (hash collision)");
    }
    return NULL;
  }
  free(copy);

  box = label_box_new(db, handle);
  if (box == NULL) {
    if (out_status != NULL) {
      *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "resolve_label: out of memory");
    }
    return NULL;
  }
  gdi_rma_cache_put(&db->labels, name, name_len, box);
  return box;
}

struct gdi_rma_ptype* gdi_rma_resolve_ptype(gdi_rma_db* db, const char* name, size_t name_len,
                                            int* out_status) {
  if (out_status != NULL) {
    *out_status = GDI_RMA_OK;
  }
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(name, name_len, &code);
  if (copy == NULL) {
    if (out_status != NULL) {
      *out_status = code;
    }
    return NULL;
  }

  struct gdi_rma_ptype* box =
      (struct gdi_rma_ptype*)gdi_rma_cache_get(&db->ptypes, copy, strlen(copy));
  if (box != NULL) {
    free(copy);
    return box;
  }

  GDI_PropertyType handle = GDI_PROPERTY_TYPE_NULL;
  int st = GDI_GetPropertyTypeFromName(&handle, copy, db->gdi);
  free(copy);
  if (st != GDI_SUCCESS) {
    if (out_status != NULL) {
      *out_status = gdi_rma_from_gdi(st, "GDI_GetPropertyTypeFromName");
    }
    return NULL;
  }
  if (handle == GDI_PROPERTY_TYPE_NULL) {
    if (out_status != NULL) {
      *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_NOT_FOUND,
                                 "no property type of that name in this database");
    }
    return NULL;
  }
  if (handle->name == NULL || strlen(handle->name) != name_len ||
      memcmp(handle->name, name, name_len) != 0) {
    if (out_status != NULL) {
      *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_NOT_FOUND,
                                 "GDI resolved the name to a different property type "
                                 "(hash collision)");
    }
    return NULL;
  }

  box = ptype_box_new(db, handle);
  if (box == NULL) {
    if (out_status != NULL) {
      *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "resolve_ptype: out of memory");
    }
    return NULL;
  }
  gdi_rma_cache_put(&db->ptypes, name, name_len, box);
  return box;
}

/* ------------------------------------------------------------------------- */
/* labels                                                                     */
/* ------------------------------------------------------------------------- */

int gdi_rma_label_create(gdi_rma_db* db, const char* name, size_t name_len,
                         gdi_rma_label** out_label) {
  if (!check_db(db, "gdi_rma_label_create")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_label == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_label_create: out_label is NULL");
  }
  *out_label = NULL;
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(name, name_len, &code);
  if (copy == NULL) {
    return code;
  }
  GDI_Label handle = GDI_LABEL_NULL;
  int st = GDI_CreateLabel(copy, db->gdi, &handle);
  free(copy);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_CreateLabel");
  }
  struct gdi_rma_label* box = label_box_new(db, handle);
  if (box == NULL) {
    GDI_Label to_free = handle;
    GDI_FreeLabel(&to_free);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_label_create: out of memory");
  }
  gdi_rma_cache_put(&db->labels, name, name_len, box);
  *out_label = box;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_label_by_name(gdi_rma_db* db, const char* name, size_t name_len,
                          gdi_rma_label** out_label) {
  if (!check_db(db, "gdi_rma_label_by_name")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_label == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_label_by_name: out_label is NULL");
  }
  *out_label = NULL;
  int status = GDI_RMA_OK;
  struct gdi_rma_label* box = gdi_rma_resolve_label(db, name, name_len, 0, &status, NULL);
  if (box == NULL) {
    return status;
  }
  *out_label = box;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_label_rename(gdi_rma_db* db, gdi_rma_label* label, const char* new_name,
                         size_t new_name_len) {
  if (!check_db(db, "gdi_rma_label_rename")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (!check_label_box(label, "gdi_rma_label_rename")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (label->gdi == GDI_LABEL_NONE) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_label_rename: the predefined GDI_LABEL_NONE cannot be renamed");
  }
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(new_name, new_name_len, &code);
  if (copy == NULL) {
    return code;
  }
  int st = GDI_UpdateLabel(copy, label->gdi);
  free(copy);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_UpdateLabel");
  }
  /* the cached name is stale now: drop it and re-insert under the new one */
  gdi_rma_cache_forget_handle(&db->labels, label);
  gdi_rma_cache_put(&db->labels, new_name, new_name_len, label);
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_label_free(gdi_rma_db* db, gdi_rma_label* label) {
  if (!check_db(db, "gdi_rma_label_free")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (!check_label_box(label, "gdi_rma_label_free")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (label->gdi == GDI_LABEL_NONE) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_label_free: the predefined GDI_LABEL_NONE cannot be freed");
  }
  GDI_Label handle = label->gdi;
  int st = GDI_FreeLabel(&handle);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreeLabel");
  }
  gdi_rma_cache_forget_handle(&db->labels, label);
  label->magic = GDI_RMA_MAGIC_FREED;
  label->gdi = GDI_LABEL_NULL;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_label_name(const gdi_rma_label* label, const char** out_ptr, size_t* out_len) {
  if (!check_label_box(label, "gdi_rma_label_name")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_ptr == NULL || out_len == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_label_name: out argument is NULL");
  }
  *out_ptr = label->gdi->name;
  *out_len = label->gdi->name != NULL ? strlen(label->gdi->name) : 0;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_db_all_labels(gdi_rma_db* db, gdi_rma_strlist* out) {
  if (!check_db(db, "gdi_rma_db_all_labels")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_db_all_labels: out is NULL");
  }
  memset(out, 0, sizeof(*out));

  size_t count = 0;
  int st = GDI_GetAllLabelsOfDatabase(NULL, 0, &count, db->gdi);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfDatabase(size)");
  }
  if (count == 0) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  GDI_Label* handles = (GDI_Label*)calloc(count, sizeof(GDI_Label));
  if (handles == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_db_all_labels: out of memory");
  }
  size_t got = 0;
  st = GDI_GetAllLabelsOfDatabase(handles, count, &got, db->gdi);
  if (st != GDI_SUCCESS) {
    free(handles);
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfDatabase");
  }
  char** items = (char**)calloc(got, sizeof(char*));
  size_t* lens = (size_t*)calloc(got, sizeof(size_t));
  if (items == NULL || lens == NULL) {
    free(items);
    free(lens);
    free(handles);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_db_all_labels: out of memory");
  }
  for (size_t i = 0; i < got; i++) {
    const char* name = handles[i]->name != NULL ? handles[i]->name : "";
    size_t len = strlen(name);
    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) {
      break; /* a short list is better than an error after a partial fill */
    }
    memcpy(copy, name, len + 1);
    items[i] = copy;
    lens[i] = len;
    out->count = i + 1;
  }
  free(handles);
  out->items = items;
  out->lens = lens;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* property types                                                             */
/* ------------------------------------------------------------------------- */

static int check_etype_stype(uint32_t etype, uint32_t stype) {
  if (etype != (uint32_t)GDI_SINGLE_ENTITY && etype != (uint32_t)GDI_MULTIPLE_ENTITY) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT,
                 "invalid entity type %u (expected 198 single or 199 multiple)", etype);
    return 0;
  }
  if (stype != (uint32_t)GDI_FIXED_SIZE && stype != (uint32_t)GDI_MAX_SIZE &&
      stype != (uint32_t)GDI_NO_SIZE_LIMIT) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT,
                 "invalid size limit %u (expected 200 fixed, 201 max, 202 none)", stype);
    return 0;
  }
  return 1;
}

int gdi_rma_ptype_create(gdi_rma_db* db, const char* name, size_t name_len, uint32_t dtype,
                         uint32_t etype, uint32_t stype, uint64_t count,
                         gdi_rma_ptype** out_ptype) {
  if (!check_db(db, "gdi_rma_ptype_create")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_ptype == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_ptype_create: out_ptype is NULL");
  }
  *out_ptype = NULL;
  if (gdi_rma_datatype_size(dtype) == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_DATATYPE, "gdi_rma_ptype_create: invalid dtype %u", dtype);
  }
  if (!check_etype_stype(etype, stype)) {
    return GDI_RMA_ERR_INVALID_ARGUMENT;
  }
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(name, name_len, &code);
  if (copy == NULL) {
    return code;
  }
  GDI_PropertyType handle = GDI_PROPERTY_TYPE_NULL;
  int st = GDI_CreatePropertyType(copy, (int)etype, (GDI_Datatype)dtype, (int)stype,
                                 (size_t)count, db->gdi, &handle);
  free(copy);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_CreatePropertyType");
  }
  struct gdi_rma_ptype* box = ptype_box_new(db, handle);
  if (box == NULL) {
    GDI_PropertyType to_free = handle;
    GDI_FreePropertyType(&to_free);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_ptype_create: out of memory");
  }
  gdi_rma_cache_put(&db->ptypes, name, name_len, box);
  *out_ptype = box;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_ptype_by_name(gdi_rma_db* db, const char* name, size_t name_len,
                          gdi_rma_ptype** out_ptype) {
  if (!check_db(db, "gdi_rma_ptype_by_name")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_ptype == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_ptype_by_name: out_ptype is NULL");
  }
  *out_ptype = NULL;
  int status = GDI_RMA_OK;
  struct gdi_rma_ptype* box = gdi_rma_resolve_ptype(db, name, name_len, &status);
  if (box == NULL) {
    return status;
  }
  *out_ptype = box;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_ptype_update(gdi_rma_db* db, gdi_rma_ptype* ptype, const char* new_name,
                         size_t new_name_len, uint32_t dtype, uint32_t etype, uint32_t stype,
                         uint64_t count) {
  if (!check_db(db, "gdi_rma_ptype_update")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (!check_ptype_box(ptype, "gdi_rma_ptype_update")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (ptype->gdi->db == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_ptype_update: a predefined property type cannot be updated");
  }
  if (gdi_rma_datatype_size(dtype) == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_DATATYPE, "gdi_rma_ptype_update: invalid dtype %u", dtype);
  }
  if (!check_etype_stype(etype, stype)) {
    return GDI_RMA_ERR_INVALID_ARGUMENT;
  }
  int code = GDI_RMA_OK;
  char* copy = gdi_rma_dup_name(new_name, new_name_len, &code);
  if (copy == NULL) {
    return code;
  }
  /* GDI-RMA updates the metadata only (src/README.md): stored property values
   * are NOT reinterpreted or migrated. */
  int st = GDI_UpdatePropertyType(copy, (int)etype, (GDI_Datatype)dtype, (int)stype,
                                 (size_t)count, NULL, ptype->gdi);
  free(copy);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_UpdatePropertyType");
  }
  gdi_rma_cache_forget_handle(&db->ptypes, ptype);
  gdi_rma_cache_put(&db->ptypes, new_name, new_name_len, ptype);
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_ptype_free(gdi_rma_db* db, gdi_rma_ptype* ptype) {
  if (!check_db(db, "gdi_rma_ptype_free")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (!check_ptype_box(ptype, "gdi_rma_ptype_free")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (ptype->gdi->db == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_ptype_free: a predefined property type cannot be freed");
  }
  GDI_PropertyType handle = ptype->gdi;
  int st = GDI_FreePropertyType(&handle);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreePropertyType");
  }
  gdi_rma_cache_forget_handle(&db->ptypes, ptype);
  ptype->magic = GDI_RMA_MAGIC_FREED;
  ptype->gdi = GDI_PROPERTY_TYPE_NULL;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

static void fill_ptype_info(gdi_rma_db* db, GDI_PropertyType ptype, gdi_rma_ptype_info* out) {
  out->name = ptype->name;
  out->name_len = ptype->name != NULL ? (uint64_t)strlen(ptype->name) : 0;
  out->dtype = (uint32_t)ptype->dtype;
  out->etype = (uint32_t)ptype->etype;
  out->stype = (uint32_t)ptype->stype;
  out->_pad = 0;
  out->count = (uint64_t)ptype->count;
  /* the ABI handle is the wrapper box, never the raw GDI pointer, so a caller
   * can feed it back into gdi_rma_ptype_info_of / the constraint builder */
  out->handle = db != NULL ? (gdi_rma_ptype*)ptype_box_for(db, ptype) : NULL;
}

int gdi_rma_ptype_info_of(const gdi_rma_ptype* ptype, gdi_rma_ptype_info* out) {
  if (!check_ptype_box(ptype, "gdi_rma_ptype_info_of")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_ptype_info_of: out is NULL");
  }
  /* the box already is the stable handle, and its db pointer is NULL for the
   * predefined types (which own no cache) */
  GDI_PropertyType handle = ptype->gdi;
  out->name = handle->name;
  out->name_len = handle->name != NULL ? (uint64_t)strlen(handle->name) : 0;
  out->dtype = (uint32_t)handle->dtype;
  out->etype = (uint32_t)handle->etype;
  out->stype = (uint32_t)handle->stype;
  out->_pad = 0;
  out->count = (uint64_t)handle->count;
  out->handle = (gdi_rma_ptype*)ptype;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_db_all_ptypes(gdi_rma_db* db, gdi_rma_ptype_info** out_array, size_t* out_count) {
  if (!check_db(db, "gdi_rma_db_all_ptypes")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_array == NULL || out_count == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_db_all_ptypes: out argument NULL");
  }
  *out_array = NULL;
  *out_count = 0;

  size_t count = 0;
  int st = GDI_GetAllPropertyTypesOfDatabase(NULL, 0, &count, db->gdi);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfDatabase(size)");
  }
  if (count == 0) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  GDI_PropertyType* handles = (GDI_PropertyType*)calloc(count, sizeof(GDI_PropertyType));
  if (handles == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_db_all_ptypes: out of memory");
  }
  size_t got = 0;
  st = GDI_GetAllPropertyTypesOfDatabase(handles, count, &got, db->gdi);
  if (st != GDI_SUCCESS) {
    free(handles);
    return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfDatabase");
  }
  gdi_rma_ptype_info* infos =
      (gdi_rma_ptype_info*)calloc(got == 0 ? 1 : got, sizeof(gdi_rma_ptype_info));
  if (infos == NULL) {
    free(handles);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_db_all_ptypes: out of memory");
  }
  for (size_t i = 0; i < got; i++) {
    fill_ptype_info(db, handles[i], &infos[i]);
  }
  free(handles);
  *out_array = infos;
  *out_count = got;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* The shared helper the graph layer uses to report the property types present
 * on a vertex (declared here so the info-array code lives in one place). */
int gdi_rma_ptype_handles_to_info(gdi_rma_db* db, GDI_PropertyType* handles, size_t count,
                                 gdi_rma_ptype_info** out_array, size_t* out_count) {
  gdi_rma_ptype_info* infos =
      (gdi_rma_ptype_info*)calloc(count == 0 ? 1 : count, sizeof(gdi_rma_ptype_info));
  if (infos == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "out of memory for %u property types",
                        (unsigned)count);
  }
  for (size_t i = 0; i < count; i++) {
    fill_ptype_info(db, handles[i], &infos[i]);
  }
  *out_array = infos;
  *out_count = count;
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* constraints                                                                */
/* ------------------------------------------------------------------------- */

static void stored_conditions_free(gdi_rma_constraint* constraint) {
  for (size_t i = 0; i < constraint->cond_count; i++) {
    free(constraint->conds[i].name);
    free(constraint->conds[i].value);
  }
  free(constraint->conds);
  constraint->conds = NULL;
  constraint->cond_count = 0;
  constraint->cond_capacity = 0;
}

/* Append one condition (deep copy) to the wrapper's AND-list. */
static int stored_condition_push(gdi_rma_constraint* constraint, const gdi_rma_condition* c) {
  if (constraint->cond_count == constraint->cond_capacity) {
    size_t capacity = constraint->cond_capacity == 0 ? 4 : constraint->cond_capacity * 2;
    gdi_rma_condition_stored* grown = (gdi_rma_condition_stored*)realloc(
        constraint->conds, capacity * sizeof(gdi_rma_condition_stored));
    if (grown == NULL) {
      return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "constraint: out of memory growing the list");
    }
    constraint->conds = grown;
    constraint->cond_capacity = capacity;
  }
  gdi_rma_condition_stored* slot = &constraint->conds[constraint->cond_count];
  memset(slot, 0, sizeof(*slot));
  slot->name = gdi_rma_dup_name(c->name, c->name_len, NULL);
  if (slot->name == NULL) {
    return GDI_RMA_ERR_INVALID_ARGUMENT;
  }
  slot->name_len = c->name_len;
  slot->kind = c->kind;
  slot->op = c->op;
  slot->dtype = c->dtype;
  slot->count = c->count;
  if (c->value_bytes > 0) {
    slot->value = malloc(c->value_bytes);
    if (slot->value == NULL) {
      free(slot->name);
      return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "constraint: out of memory copying a value");
    }
    memcpy(slot->value, c->value, c->value_bytes);
    slot->value_bytes = c->value_bytes;
  }
  constraint->cond_count++;
  return GDI_RMA_OK;
}

/* Apply one stored condition to the live GDI subconstraint being built. */
static int subconstraint_apply(gdi_rma_db* db, GDI_Subconstraint subconstraint,
                               const gdi_rma_condition_stored* stored) {
  int status = GDI_RMA_OK;
  if (stored->kind == GDI_RMA_COND_LABEL) {
    struct gdi_rma_label* label =
        gdi_rma_resolve_label(db, stored->name, stored->name_len, 0, &status, NULL);
    if (label == NULL) {
      return status;
    }
    int st = GDI_AddLabelConditionToSubconstraint(label->gdi, (GDI_Op)stored->op, subconstraint);
    if (st != GDI_SUCCESS) {
      /* GDI accepts only EQUAL / NOTEQUAL on a label condition */
      return gdi_rma_from_gdi(st, "GDI_AddLabelConditionToSubconstraint");
    }
    return GDI_RMA_OK;
  }
  struct gdi_rma_ptype* ptype = gdi_rma_resolve_ptype(db, stored->name, stored->name_len, &status);
  if (ptype == NULL) {
    return status;
  }
  if ((uint32_t)ptype->gdi->dtype != stored->dtype) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_TYPE_MISMATCH,
                        "condition on '%s' uses dtype %u but the property type is dtype %u",
                        ptype->gdi->name, stored->dtype, (unsigned)ptype->gdi->dtype);
  }
  int rc = gdi_rma_check_condition_value(stored->dtype, stored->count, stored->value,
                                         stored->value_bytes);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  /* GDI copies the payload into the condition */
  int st = GDI_AddPropertyConditionToSubconstraint(ptype->gdi, (GDI_Op)stored->op, stored->value,
                                                  (size_t)stored->count, subconstraint);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_AddPropertyConditionToSubconstraint");
  }
  return GDI_RMA_OK;
}

/* (Re)build the GDI constraint object from the wrapper's condition list. */
static int constraint_publish(gdi_rma_constraint* constraint) {
  if (constraint->gdi != GDI_CONSTRAINT_NULL) {
    GDI_Constraint old = constraint->gdi;
    int st = GDI_FreeConstraint(&old);
    constraint->gdi = GDI_CONSTRAINT_NULL;
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, "GDI_FreeConstraint");
    }
  }
  GDI_Database gdi_db = constraint->db;
  GDI_Constraint gdi_constraint = GDI_CONSTRAINT_NULL;
  int st = GDI_CreateConstraint(gdi_db, &gdi_constraint);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_CreateConstraint");
  }
  GDI_Subconstraint subconstraint = GDI_SUBCONSTRAINT_NULL;
  st = GDI_CreateSubconstraint(gdi_db, &subconstraint);
  if (st != GDI_SUCCESS) {
    GDI_Constraint to_free = gdi_constraint;
    GDI_FreeConstraint(&to_free);
    return gdi_rma_from_gdi(st, "GDI_CreateSubconstraint");
  }
  for (size_t i = 0; i < constraint->cond_count; i++) {
    int rc = subconstraint_apply(constraint->db_owner, subconstraint, &constraint->conds[i]);
    if (rc != GDI_RMA_OK) {
      GDI_Subconstraint sc = subconstraint;
      GDI_FreeSubconstraint(&sc);
      GDI_Constraint to_free = gdi_constraint;
      GDI_FreeConstraint(&to_free);
      constraint->gdi = GDI_CONSTRAINT_NULL;
      return rc;
    }
  }
  st = GDI_AddSubconstraintToConstraint(subconstraint, gdi_constraint);
  if (st != GDI_SUCCESS) {
    GDI_Subconstraint sc = subconstraint;
    GDI_FreeSubconstraint(&sc);
    GDI_Constraint to_free = gdi_constraint;
    GDI_FreeConstraint(&to_free);
    constraint->gdi = GDI_CONSTRAINT_NULL;
    return gdi_rma_from_gdi(st, "GDI_AddSubconstraintToConstraint");
  }
  /* GDI copied the subconstraint into the constraint; drop the registered
   * original, exactly like benchmarks/queries.c does. */
  st = GDI_FreeSubconstraint(&subconstraint);
  if (st != GDI_SUCCESS) {
    GDI_Constraint to_free = gdi_constraint;
    GDI_FreeConstraint(&to_free);
    constraint->gdi = GDI_CONSTRAINT_NULL;
    return gdi_rma_from_gdi(st, "GDI_FreeSubconstraint");
  }
  constraint->gdi = gdi_constraint;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_constraint_create(gdi_rma_db* db, const gdi_rma_condition* conditions, size_t count,
                              gdi_rma_constraint** out) {
  if (!check_db(db, "gdi_rma_constraint_create")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_constraint_create: out is NULL");
  }
  *out = NULL;
  if (count > 0 && conditions == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_constraint_create: %u conditions with a NULL array",
                        (unsigned)count);
  }

  gdi_rma_constraint* constraint = (gdi_rma_constraint*)calloc(1, sizeof(*constraint));
  if (constraint == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_constraint_create: out of memory");
  }
  constraint->magic = GDI_RMA_MAGIC_CON;
  constraint->db_owner = db;
  constraint->db = db->gdi;

  for (size_t i = 0; i < count; i++) {
    if (conditions[i].kind != GDI_RMA_COND_LABEL && conditions[i].kind != GDI_RMA_COND_PROPERTY) {
      free(constraint);
      return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                          "gdi_rma_constraint_create: condition %u has kind %u", (unsigned)i,
                          conditions[i].kind);
    }
    if (conditions[i].op > (uint32_t)GDI_OP_END) {
      free(constraint);
      return GDI_RMA_FAIL(GDI_RMA_ERROR_OP,
                          "gdi_rma_constraint_create: condition %u op %u is not a GDI_Op",
                          (unsigned)i, conditions[i].op);
    }
    if (conditions[i].kind == GDI_RMA_COND_PROPERTY) {
      int rc = gdi_rma_check_condition_value(conditions[i].dtype, conditions[i].count,
                                            conditions[i].value, conditions[i].value_bytes);
      if (rc != GDI_RMA_OK) {
        free(constraint);
        return rc;
      }
    }
    int rc = stored_condition_push(constraint, &conditions[i]);
    if (rc != GDI_RMA_OK) {
      stored_conditions_free(constraint);
      free(constraint);
      return rc;
    }
  }

  int rc = constraint_publish(constraint);
  if (rc != GDI_RMA_OK) {
    stored_conditions_free(constraint);
    free(constraint);
    return rc;
  }
  *out = constraint;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

static int constraint_extend(gdi_rma_constraint* constraint, const gdi_rma_condition* condition) {
  int rc = stored_condition_push(constraint, condition);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  return constraint_publish(constraint);
}

int gdi_rma_constraint_add_label_condition(gdi_rma_constraint* constraint, const char* label,
                                          size_t label_len, uint32_t op) {
  if (constraint == NULL || constraint->magic != GDI_RMA_MAGIC_CON) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE,
                        "gdi_rma_constraint_add_label_condition: bad constraint handle");
  }
  if (op > (uint32_t)GDI_OP_END) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_OP,
                        "gdi_rma_constraint_add_label_condition: op %u is not a GDI_Op", op);
  }
  gdi_rma_condition condition;
  memset(&condition, 0, sizeof(condition));
  condition.kind = GDI_RMA_COND_LABEL;
  condition.name = label;
  condition.name_len = label_len;
  condition.op = op;
  int rc = constraint_extend(constraint, &condition);
  if (rc == GDI_RMA_OK) {
    gdi_rma_clear_error();
  }
  return rc;
}

int gdi_rma_constraint_add_property_condition(gdi_rma_constraint* constraint,
                                             const char* ptype_name, size_t ptype_name_len,
                                             uint32_t op, uint32_t dtype, uint32_t count,
                                             const void* value, size_t value_bytes) {
  if (constraint == NULL || constraint->magic != GDI_RMA_MAGIC_CON) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE,
                        "gdi_rma_constraint_add_property_condition: bad constraint handle");
  }
  if (op > (uint32_t)GDI_OP_END) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_OP,
                        "gdi_rma_constraint_add_property_condition: op %u is not a GDI_Op", op);
  }
  int rc = gdi_rma_check_condition_value(dtype, count, value, value_bytes);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_condition condition;
  memset(&condition, 0, sizeof(condition));
  condition.kind = GDI_RMA_COND_PROPERTY;
  condition.name = ptype_name;
  condition.name_len = ptype_name_len;
  condition.op = op;
  condition.dtype = dtype;
  condition.count = count;
  condition.value = value;
  condition.value_bytes = value_bytes;
  rc = constraint_extend(constraint, &condition);
  if (rc == GDI_RMA_OK) {
    gdi_rma_clear_error();
  }
  return rc;
}

int gdi_rma_constraint_free(gdi_rma_constraint* constraint) {
  if (constraint == NULL || constraint->magic != GDI_RMA_MAGIC_CON) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE,
                        "gdi_rma_constraint_free: bad constraint handle");
  }
  int rc = GDI_RMA_OK;
  if (constraint->gdi != GDI_CONSTRAINT_NULL) {
    GDI_Constraint to_free = constraint->gdi;
    rc = gdi_rma_from_gdi(GDI_FreeConstraint(&to_free), "GDI_FreeConstraint");
    constraint->gdi = GDI_CONSTRAINT_NULL;
  }
  constraint->magic = GDI_RMA_MAGIC_FREED;
  stored_conditions_free(constraint);
  free(constraint);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

GDI_Constraint gdi_rma_constraint_handle(const gdi_rma_constraint* constraint) {
  if (constraint == NULL || constraint->magic != GDI_RMA_MAGIC_CON) {
    return GDI_CONSTRAINT_NULL;
  }
  return constraint->gdi;
}

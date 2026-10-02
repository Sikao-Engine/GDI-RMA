/*
 * gdi_rma_api_read.c -- the read-backs of the gdi_rma C FFI: the two-phase paged
 * neighborhood reads, the label / property-type / property reads and the packed
 * result buffers they deliver.
 *
 * Buffer rule: every array, blob and string list produced here is malloc'd in
 * this library and released by gdi_rma_free / gdi_rma_blob_free /
 * gdi_rma_strlist_free / gdi_rma_edge_page_free / gdi_rma_vertex_page_free.
 * Nothing a caller gets from these calls may be freed by the caller's own CRT.
 */
#include "gdi_rma_internal.h"

/* ------------------------------------------------------------------------- */
/* shared helpers                                                             */
/* ------------------------------------------------------------------------- */

static int check_tx_ro(gdi_rma_tx* tx, const char* call) {
  if (tx == NULL || tx->magic != GDI_RMA_MAGIC_TX) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE,
                 "%s: not a live transaction handle (reads also run inside a GDI transaction)",
                 call);
    return 0;
  }
  if (tx->db == NULL || tx->db->magic != GDI_RMA_MAGIC_DB) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE, "%s: the transaction's database is gone", call);
    return 0;
  }
  return 1;
}

static GDI_Constraint constraint_of(const gdi_rma_constraint* constraint, const char* call,
                                   int* out_status) {
  *out_status = GDI_RMA_OK;
  if (constraint == NULL) {
    return GDI_CONSTRAINT_NULL;
  }
  if (constraint->magic != GDI_RMA_MAGIC_CON) {
    *out_status = GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_HANDLE, "%s: not a live constraint handle",
                               call);
    return GDI_CONSTRAINT_NULL;
  }
  return constraint->gdi;
}

static int check_orientation(uint32_t orientation, const char* call) {
  /* GDI's own range check (gdi_vertex.c): 0 and > 7 are GDI_ERROR_EDGE_ORIENTATION */
  if (orientation == 0 || orientation > 7) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_ARGUMENT,
                 "%s: orientation %u is outside the GDI bitmask range 1..7 (incoming 1, "
                 "outgoing 2, undirected 4, all 7)",
                 call, orientation);
    return 0;
  }
  return 1;
}

/* Assemble a strlist out of an array of GDI_Label handles. */
static int labels_to_strlist(GDI_Label* handles, size_t count, gdi_rma_strlist* out) {
  char** items = (char**)calloc(count == 0 ? 1 : count, sizeof(char*));
  size_t* lens = (size_t*)calloc(count == 0 ? 1 : count, sizeof(size_t));
  if (items == NULL || lens == NULL) {
    free(items);
    free(lens);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "out of memory for %u label names",
                        (unsigned)count);
  }
  out->items = items;
  out->lens = lens;
  out->count = 0;
  for (size_t i = 0; i < count; i++) {
    const char* name = handles[i]->name != NULL ? handles[i]->name : "";
    size_t len = strlen(name);
    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) {
      break;
    }
    memcpy(copy, name, len + 1);
    items[i] = copy;
    lens[i] = len;
    out->count = i + 1;
  }
  return GDI_RMA_OK;
}

static int vertex_labels(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid, gdi_rma_strlist* out,
                         const char* call) {
  memset(out, 0, sizeof(*out));
  if (!check_tx_ro(tx, call)) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "%s: the vertex uid pointer is NULL", call);
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  size_t count = 0;
  int st = GDI_GetAllLabelsOfVertex(NULL, 0, &count, holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfVertex(size)");
  }
  if (count == 0) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  GDI_Label* handles = (GDI_Label*)calloc(count, sizeof(GDI_Label));
  if (handles == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "%s: out of memory", call);
  }
  size_t got = 0;
  st = GDI_GetAllLabelsOfVertex(handles, count, &got, holder);
  if (st != GDI_SUCCESS) {
    free(handles);
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfVertex");
  }
  rc = labels_to_strlist(handles, got, out);
  free(handles);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* labels                                                                     */
/* ------------------------------------------------------------------------- */

int gdi_rma_get_labels_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                 gdi_rma_strlist* out) {
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_labels_of_vertex: out is NULL");
  }
  return vertex_labels(tx, uid, out, "gdi_rma_get_labels_of_vertex");
}

int gdi_rma_get_labels_of_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid, gdi_rma_strlist* out) {
  memset(out, 0, sizeof(*out));
  if (!check_tx_ro(tx, "gdi_rma_get_labels_of_edge")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_labels_of_edge: the edge uid pointer is NULL");
  }
  GDI_EdgeHolder edge = gdi_rma_registry_get(&tx->edges, uid);
  if (edge == GDI_EDGE_NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_EDGE_NOT_ASSOCIATED,
                        "gdi_rma_get_labels_of_edge: GDI-RMA has no GDI_AssociateEdge; edge uid "
                        "%llu/%u is only reachable inside the transaction that created it",
                        (unsigned long long)uid->origin_vertex_uid, uid->origin_offset);
  }
  size_t count = 0;
  int st = GDI_GetAllLabelsOfEdge(NULL, 0, &count, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfEdge(size)");
  }
  if (count == 0) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  GDI_Label* handles = (GDI_Label*)calloc(count, sizeof(GDI_Label));
  if (handles == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "gdi_rma_get_labels_of_edge: out of memory");
  }
  size_t got = 0;
  st = GDI_GetAllLabelsOfEdge(handles, count, &got, edge);
  if (st != GDI_SUCCESS) {
    free(handles);
    return gdi_rma_from_gdi(st, "GDI_GetAllLabelsOfEdge");
  }
  int rc = labels_to_strlist(handles, got, out);
  free(handles);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* property types present on a vertex / edge                                  */
/* ------------------------------------------------------------------------- */

int gdi_rma_get_all_property_types_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                             gdi_rma_ptype_info** out_array, size_t* out_count) {
  if (out_array == NULL || out_count == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_all_property_types_of_vertex: an out argument is NULL");
  }
  *out_array = NULL;
  *out_count = 0;
  if (!check_tx_ro(tx, "gdi_rma_get_all_property_types_of_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  size_t count = 0;
  int st = GDI_GetAllPropertyTypesOfVertex(NULL, 0, &count, holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfVertex(size)");
  }
  if (count == 0) {
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  GDI_PropertyType* handles = (GDI_PropertyType*)calloc(count, sizeof(GDI_PropertyType));
  if (handles == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY,
                        "gdi_rma_get_all_property_types_of_vertex: out of memory");
  }
  size_t got = 0;
  st = GDI_GetAllPropertyTypesOfVertex(handles, count, &got, holder);
  if (st != GDI_SUCCESS) {
    free(handles);
    return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfVertex");
  }
  rc = gdi_rma_ptype_handles_to_info(tx->db, handles, got, out_array, out_count);
  free(handles);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_get_all_property_types_of_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                           gdi_rma_ptype_info** out_array, size_t* out_count) {
  (void)tx;
  (void)uid;
  if (out_array != NULL) {
    *out_array = NULL;
  }
  if (out_count != NULL) {
    *out_count = 0;
  }
  /* GDI_GetAllPropertyTypesOfEdge is on the "Not Implemented Functions" list of
   * src/README.md -- reported as unsupported instead of pretending to be empty. */
  return GDI_RMA_FAIL(GDI_RMA_ERR_UNSUPPORTED,
                      "gdi_rma_get_all_property_types_of_edge: GDI-RMA does not implement "
                      "GDI_GetAllPropertyTypesOfEdge");
}

/* ------------------------------------------------------------------------- */
/* the packed property blob                                                   */
/* ------------------------------------------------------------------------- */

int gdi_rma_get_properties_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                     gdi_rma_blob* out) {
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_properties_of_vertex: out is NULL");
  }
  memset(out, 0, sizeof(*out));
  if (!check_tx_ro(tx, "gdi_rma_get_properties_of_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }

  size_t count = 0;
  int st = GDI_GetAllPropertyTypesOfVertex(NULL, 0, &count, holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfVertex(size)");
  }

  gdi_rma_writer writer;
  gdi_rma_writer_init(&writer);
  /* header: count + reserved; the count is patched once the entries are known */
  gdi_rma_writer_put_u32(&writer, 0);
  gdi_rma_writer_put_u32(&writer, 0);

  GDI_PropertyType* handles = count == 0 ? NULL
                                        : (GDI_PropertyType*)calloc(count,
                                                                    sizeof(GDI_PropertyType));
  if (count > 0 && handles == NULL) {
    gdi_rma_writer_free(&writer);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY,
                        "gdi_rma_get_properties_of_vertex: out of memory");
  }
  size_t got = 0;
  if (count > 0) {
    st = GDI_GetAllPropertyTypesOfVertex(handles, count, &got, holder);
    if (st != GDI_SUCCESS) {
      free(handles);
      gdi_rma_writer_free(&writer);
      return gdi_rma_from_gdi(st, "GDI_GetAllPropertyTypesOfVertex");
    }
  }

  uint32_t entries = 0;
  for (size_t i = 0; i < got; i++) {
    GDI_PropertyType ptype = handles[i];
    size_t element_size = 0;
    if (GDI_GetSizeOfDatatype(&element_size, ptype->dtype) != GDI_SUCCESS ||
        element_size == 0) {
      continue; /* an undetectable dtype cannot be packed; skip rather than lie */
    }
    /* phase one: how many elements and how many value records */
    size_t elements = 0;
    size_t offsets = 0;
    st = GDI_GetPropertiesOfVertex(NULL, 0, &elements, NULL, 0, &offsets, ptype, holder);
    if (st != GDI_SUCCESS) {
      free(handles);
      gdi_rma_writer_free(&writer);
      return gdi_rma_from_gdi(st, "GDI_GetPropertiesOfVertex(size)");
    }
    if (elements == 0) {
      continue;
    }
    /* phase two: fetch them all (multi-valued property types are concatenated
     * into one entry, which is what the managed one-value-per-name model wants) */
    void* value = malloc(elements * element_size);
    size_t* record_offsets = (size_t*)calloc(offsets == 0 ? 1 : offsets, sizeof(size_t));
    if (value == NULL || record_offsets == NULL) {
      free(value);
      free(record_offsets);
      free(handles);
      gdi_rma_writer_free(&writer);
      return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY,
                          "gdi_rma_get_properties_of_vertex: out of memory for a value buffer");
    }
    size_t got_elements = 0;
    size_t got_offsets = 0;
    st = GDI_GetPropertiesOfVertex(value, elements, &got_elements, record_offsets, offsets,
                                  &got_offsets, ptype, holder);
    if (st != GDI_SUCCESS) {
      free(value);
      free(record_offsets);
      free(handles);
      gdi_rma_writer_free(&writer);
      return gdi_rma_from_gdi(st, "GDI_GetPropertiesOfVertex");
    }
    const char* name = ptype->name != NULL ? ptype->name : "";
    uint32_t name_len = (uint32_t)strlen(name);
    gdi_rma_writer_put_u32(&writer, name_len);
    gdi_rma_writer_put_u32(&writer, (uint32_t)ptype->dtype);
    gdi_rma_writer_put_u32(&writer, (uint32_t)ptype->etype);
    gdi_rma_writer_put_u32(&writer, (uint32_t)got_elements);
    gdi_rma_writer_put(&writer, name, name_len);
    gdi_rma_writer_pad_to(&writer, 8);
    gdi_rma_writer_put(&writer, value, got_elements * element_size);
    free(value);
    free(record_offsets);
    entries++;
  }
  free(handles);

  if (!writer.ok) {
    gdi_rma_writer_free(&writer);
    return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY,
                        "gdi_rma_get_properties_of_vertex: out of memory packing the blob");
  }
  /* patch the entry count into the header */
  memcpy(writer.data, &entries, sizeof(uint32_t));
  out->data = writer.data;
  out->size = writer.size;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_get_properties_of_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                   gdi_rma_blob* out) {
  (void)tx;
  (void)uid;
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }
  /* GDI_GetPropertiesOfEdge is on the "Not Implemented Functions" list of
   * src/README.md; edge property writes are input-parsing only, so there is
   * nothing to read back. */
  return GDI_RMA_FAIL(GDI_RMA_ERR_UNSUPPORTED,
                      "gdi_rma_get_properties_of_edge: GDI-RMA does not implement "
                      "GDI_GetPropertiesOfEdge");
}

/* ------------------------------------------------------------------------- */
/* paged neighborhood reads                                                   */
/* ------------------------------------------------------------------------- */

/* The shared two-phase body.  `edges` selects the GDI entry point. */
static int paged_read(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                      const gdi_rma_constraint* constraint, uint32_t orientation, uint64_t cap,
                      void* out_page, const char* call, int edges) {
  memset(out_page, 0, edges ? sizeof(gdi_rma_edge_page) : sizeof(gdi_rma_vertex_page));
  if (!check_tx_ro(tx, call)) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (!check_orientation(orientation, call)) {
    return GDI_RMA_ERR_INVALID_ARGUMENT;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  GDI_Constraint gdi_constraint = constraint_of(constraint, call, &status);
  if (status != GDI_RMA_OK) {
    return status;
  }

  uint64_t full_count = 0;
  uint64_t returned = 0;
  void* uids = NULL;

  if (cap == 0) {
    /* count probe: the GDI size trick (NULL array, 0) */
    size_t count = 0;
    int st = edges
                 ? GDI_GetEdgesOfVertex(NULL, 0, &count, gdi_constraint, (int)orientation, holder)
                 : GDI_GetNeighborVerticesOfVertex(NULL, 0, &count, gdi_constraint,
                                                  (int)orientation, holder);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, edges ? "GDI_GetEdgesOfVertex(count)"
                                       : "GDI_GetNeighborVerticesOfVertex(count)");
    }
    full_count = (uint64_t)count;
  } else if (cap == 1) {
    /* existence short-circuit: one row is enough, the count pass is skipped */
    size_t element_size = edges ? sizeof(gdi_rma_edge_uid) : sizeof(gdi_rma_vertex_uid);
    uids = calloc(1, element_size);
    if (uids == NULL) {
      return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "%s: out of memory", call);
    }
    size_t got = 0;
    int st = edges
                 ? GDI_GetEdgesOfVertex((GDI_Edge_uid*)uids, 1, &got, gdi_constraint,
                                       (int)orientation, holder)
                 : GDI_GetNeighborVerticesOfVertex((GDI_Vertex_uid*)uids, 1, &got, gdi_constraint,
                                                  (int)orientation, holder);
    if (st != GDI_SUCCESS) {
      free(uids);
      return gdi_rma_from_gdi(st, edges ? "GDI_GetEdgesOfVertex"
                                       : "GDI_GetNeighborVerticesOfVertex");
    }
    returned = (uint64_t)got;
    /* NOT the authoritative count: this branch answers "is there any?" */
    full_count = (uint64_t)got;
  } else {
    size_t count = 0;
    int st = edges
                 ? GDI_GetEdgesOfVertex(NULL, 0, &count, gdi_constraint, (int)orientation, holder)
                 : GDI_GetNeighborVerticesOfVertex(NULL, 0, &count, gdi_constraint,
                                                  (int)orientation, holder);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, edges ? "GDI_GetEdgesOfVertex(count)"
                                       : "GDI_GetNeighborVerticesOfVertex(count)");
    }
    full_count = (uint64_t)count;
    if (count == 0) {
      /* nothing to fetch */
    } else {
      size_t element_size = edges ? sizeof(gdi_rma_edge_uid) : sizeof(gdi_rma_vertex_uid);
      uint64_t want = (uint64_t)count < cap ? (uint64_t)count : cap;
      /* GDI's GDI_GetEdgesOfVertex mallocs count*4 bytes internally, so a huge
       * cap would allocate a huge temporary: the caller caps the page anyway. */
      uids = calloc((size_t)want, element_size);
      if (uids == NULL) {
        return GDI_RMA_FAIL(GDI_RMA_ERR_OUT_OF_MEMORY, "%s: out of memory for %u rows", call,
                            (unsigned)want);
      }
      size_t got = 0;
      st = edges ? GDI_GetEdgesOfVertex((GDI_Edge_uid*)uids, (size_t)want, &got, gdi_constraint,
                                       (int)orientation, holder)
                  : GDI_GetNeighborVerticesOfVertex((GDI_Vertex_uid*)uids, (size_t)want, &got,
                                                   gdi_constraint, (int)orientation, holder);
      if (st != GDI_SUCCESS) {
        free(uids);
        return gdi_rma_from_gdi(st, edges ? "GDI_GetEdgesOfVertex"
                                         : "GDI_GetNeighborVerticesOfVertex");
      }
      returned = (uint64_t)got;
    }
  }

  if (edges) {
    gdi_rma_edge_page* page = (gdi_rma_edge_page*)out_page;
    page->full_count = full_count;
    page->returned = returned;
    page->uids = (gdi_rma_edge_uid*)uids;
  } else {
    gdi_rma_vertex_page* page = (gdi_rma_vertex_page*)out_page;
    page->full_count = full_count;
    page->returned = returned;
    page->uids = (gdi_rma_vertex_uid*)uids;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_get_edges_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                const gdi_rma_constraint* constraint, uint32_t orientation,
                                uint64_t cap, gdi_rma_edge_page* out) {
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_get_edges_of_vertex: out is NULL");
  }
  return paged_read(tx, uid, constraint, orientation, cap, out, "gdi_rma_get_edges_of_vertex", 1);
}

int gdi_rma_get_neighbors_of_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                    const gdi_rma_constraint* constraint, uint32_t orientation,
                                    uint64_t cap, gdi_rma_vertex_page* out) {
  if (out == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_neighbors_of_vertex: out is NULL");
  }
  return paged_read(tx, uid, constraint, orientation, cap, out, "gdi_rma_get_neighbors_of_vertex",
                    0);
}

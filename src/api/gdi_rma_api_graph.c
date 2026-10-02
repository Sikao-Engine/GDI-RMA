/*
 * gdi_rma_api_graph.c -- vertex and edge operations of the gdi_rma C FFI.
 *
 * Everything here runs inside a caller-owned transaction (GDI has no implicit
 * one).  Vertex state is reached through GDI_AssociateVertex(uid, tx, &holder),
 * the association GDI-RMA does provide; edge state is reached through the
 * wrapper's transaction-local UID registry, because GDI-RMA implements no
 * GDI_AssociateEdge (src/README.md) -- an edge holder only exists inside the
 * transaction that created it.
 */
#include "gdi_rma_internal.h"

/* ------------------------------------------------------------------------- */
/* handle checks + association                                                */
/* ------------------------------------------------------------------------- */

static int check_tx(gdi_rma_tx* tx, const char* call) {
  if (tx == NULL || tx->magic != GDI_RMA_MAGIC_TX) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE,
                 "%s: not a live transaction handle (GDI has no implicit transaction: call "
                 "gdi_rma_tx_begin first)",
                 call);
    return 0;
  }
  if (tx->db == NULL || tx->db->magic != GDI_RMA_MAGIC_DB) {
    gdi_rma_fail(GDI_RMA_ERR_INVALID_HANDLE, "%s: the transaction's database is gone", call);
    return 0;
  }
  return 1;
}

int gdi_rma_associate_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                             GDI_VertexHolder* out) {
  *out = GDI_VERTEX_NULL;
  if (uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "a vertex uid pointer is NULL");
  }
  if (uid->value == GDI_RMA_UID_NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERROR_UID, "the vertex uid is the GDI null uid");
  }
  int st = GDI_AssociateVertex(uid->value, tx->gdi, out);
  if (st != GDI_SUCCESS) {
    if (st == GDI_ERROR_TRANSACTION_CRITICAL) {
      *out = GDI_VERTEX_NULL;
      return gdi_rma_fail(st,
                          "GDI_AssociateVertex(%llu): a concurrent writer holds the vertex lock; "
                          "the transaction is now critical and must be aborted",
                          (unsigned long long)uid->value);
    }
    *out = GDI_VERTEX_NULL;
    return gdi_rma_from_gdi(st, "GDI_AssociateVertex");
  }
  if (*out == GDI_VERTEX_NULL || (*out)->delete_flag) {
    *out = GDI_VERTEX_NULL;
    return GDI_RMA_FAIL(GDI_RMA_ERROR_VERTEX,
                        "the vertex is deleted (or marked deleted inside this transaction)");
  }
  return GDI_RMA_OK;
}

/* Resolve an edge uid through the transaction registry. */
static int resolve_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid, const char* call,
                        GDI_EdgeHolder* out) {
  *out = GDI_EDGE_NULL;
  if (uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "%s: the edge uid pointer is NULL", call);
  }
  GDI_EdgeHolder holder = gdi_rma_registry_get(&tx->edges, uid);
  if (holder == GDI_EDGE_NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_EDGE_NOT_ASSOCIATED,
                        "%s: GDI-RMA has no GDI_AssociateEdge, so edge uid %llu/%u is only "
                        "reachable inside the transaction that created it",
                        call, (unsigned long long)uid->origin_vertex_uid, uid->origin_offset);
  }
  *out = holder;
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* shared "with labels + properties" body                                     */
/* ------------------------------------------------------------------------- */

/* Apply initial labels and properties to a freshly created vertex or edge
 * (exactly one of `vertex` / `edge` is non-NULL). */
static int apply_initial_schema(gdi_rma_tx* tx, GDI_VertexHolder vertex, GDI_EdgeHolder edge,
                                const gdi_rma_name_ref* labels, uint32_t label_count,
                                const gdi_rma_property_ref* properties, uint32_t property_count,
                                int auto_create, uint32_t* out_flags) {
  for (uint32_t i = 0; i < label_count; i++) {
    int status = GDI_RMA_OK;
    int created = 0;
    struct gdi_rma_label* label = gdi_rma_resolve_label(tx->db, labels[i].name, labels[i].len,
                                                       auto_create, &status, &created);
    if (label == NULL) {
      return status;
    }
    if (created != 0 && out_flags != NULL) {
      *out_flags |= GDI_RMA_VERTEX_F_LABEL_CREATED;
    }
    int st = vertex != NULL ? GDI_AddLabelToVertex(label->gdi, vertex)
                            : GDI_AddLabelToEdge(label->gdi, edge);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, vertex != NULL ? "GDI_AddLabelToVertex"
                                                 : "GDI_AddLabelToEdge");
    }
  }
  for (uint32_t i = 0; i < property_count; i++) {
    int status = GDI_RMA_OK;
    struct gdi_rma_ptype* ptype = gdi_rma_resolve_ptype(tx->db, properties[i].ptype_name,
                                                       properties[i].ptype_name_len, &status);
    if (ptype == NULL) {
      return status;
    }
    int rc = gdi_rma_check_property(&properties[i], ptype->gdi);
    if (rc != GDI_RMA_OK) {
      return rc;
    }
    int st = vertex != NULL
                 ? GDI_AddPropertyToVertex(properties[i].value, (size_t)properties[i].count,
                                           ptype->gdi, vertex)
                 : GDI_AddPropertyToEdge(properties[i].value, (size_t)properties[i].count,
                                         ptype->gdi, edge);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st,
                              vertex != NULL ? "GDI_AddPropertyToVertex"
                                             : "GDI_AddPropertyToEdge");
    }
  }
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* vertices                                                                   */
/* ------------------------------------------------------------------------- */

int gdi_rma_create_vertex(gdi_rma_tx* tx, const void* external_id, size_t external_id_len,
                          const gdi_rma_name_ref* labels, uint32_t label_count,
                          const gdi_rma_property_ref* properties, uint32_t property_count,
                          uint32_t flags, gdi_rma_vertex_uid* out_uid, uint32_t* out_flags) {
  if (!check_tx(tx, "gdi_rma_create_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_create_vertex: out_uid is NULL");
  }
  if (external_id == NULL || external_id_len == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_create_vertex: a vertex needs a non-empty external id "
                        "(the GDI primary key is (label, external_id))");
  }
  if ((label_count > 0 && labels == NULL) || (property_count > 0 && properties == NULL)) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_create_vertex: a non-zero count with a NULL array");
  }
  if (out_flags != NULL) {
    *out_flags = 0;
  }
  *out_uid = (gdi_rma_vertex_uid){0};

  /* Resolve the labels first: the uniqueness probe needs the first one, and a
   * missing label should fail before GDI allocates a block. */
  GDI_Label key_label = GDI_LABEL_NULL;
  for (uint32_t i = 0; i < label_count; i++) {
    int status = GDI_RMA_OK;
    int created = 0;
    struct gdi_rma_label* label =
        gdi_rma_resolve_label(tx->db, labels[i].name, labels[i].len,
                              (flags & GDI_RMA_CREATE_F_AUTO_LABELS) != 0, &status, &created);
    if (label == NULL) {
      return status;
    }
    if (created != 0 && out_flags != NULL) {
      *out_flags |= GDI_RMA_VERTEX_F_LABEL_CREATED;
    }
    if (i == 0) {
      key_label = label->gdi;
    }
  }

  /* The NON_UNIQUE_ID warning: GDI-RMA never raises it itself (the check is a
   * TODO in gdi_index.c / gdi_vertex.c), so the wrapper probes the distributed
   * index for the (label, external_id) key when asked.  GDI updates that index
   * at COMMIT time, so the probe sees committed vertices only. */
  if ((flags & GDI_RMA_CREATE_F_CHECK_UNIQUE) != 0 && key_label != GDI_LABEL_NULL) {
    bool found = false;
    GDI_Vertex_uid existing = 0;
    int st = GDI_TranslateVertexID(&found, &existing, key_label, external_id, external_id_len,
                                  tx->gdi);
    if (st == GDI_ERROR_TRANSACTION_CRITICAL) {
      return gdi_rma_fail(st,
                          "GDI_TranslateVertexID: a concurrent writer holds the vertex lock; "
                          "the transaction is now critical and must be aborted");
    }
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, "GDI_TranslateVertexID(uniqueness probe)");
    }
    if (found && out_flags != NULL) {
      *out_flags |= GDI_RMA_VERTEX_F_NON_UNIQUE_ID;
    }
  }

  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int st = GDI_CreateVertex(external_id, external_id_len, tx->gdi, &holder);
  if (st == GDI_WARNING_NON_UNIQUE_ID) {
    /* a success-with-warning: keep going, report the bit */
    if (out_flags != NULL) {
      *out_flags |= GDI_RMA_VERTEX_F_NON_UNIQUE_ID;
    }
    st = GDI_SUCCESS;
  }
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_CreateVertex");
  }

  *out_uid = gdi_rma_vertex_uid_of(holder);
  int rc = apply_initial_schema(tx, holder, NULL, labels, label_count, properties, property_count,
                               (flags & GDI_RMA_CREATE_F_AUTO_LABELS) != 0, out_flags);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_translate_vertex(gdi_rma_tx* tx, const gdi_rma_name_ref* label, const void* external_id,
                             size_t external_id_len, gdi_rma_vertex_uid* out_uid, bool* out_found) {
  if (!check_tx(tx, "gdi_rma_translate_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (label == NULL || out_uid == NULL || out_found == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_translate_vertex: label / out_uid / out_found is NULL");
  }
  if (external_id == NULL || external_id_len == 0) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_translate_vertex: an empty external id cannot be indexed");
  }
  *out_found = false;
  out_uid->value = GDI_RMA_UID_NULL;

  int status = GDI_RMA_OK;
  struct gdi_rma_label* resolved =
      gdi_rma_resolve_label(tx->db, label->name, label->len, 0, &status, NULL);
  if (resolved == NULL) {
    return status;
  }

  GDI_Vertex_uid uid = 0;
  bool found = false;
  int st = GDI_TranslateVertexID(&found, &uid, resolved->gdi, external_id, external_id_len,
                                 tx->gdi);
  if (st == GDI_ERROR_TRANSACTION_CRITICAL) {
    return gdi_rma_fail(st,
                        "GDI_TranslateVertexID: a concurrent writer holds the vertex lock; the "
                        "transaction is now critical and must be aborted");
  }
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_TranslateVertexID");
  }
  out_uid->value = uid;
  *out_found = found ? true : false;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_get_or_create_vertex(gdi_rma_tx* tx, const gdi_rma_name_ref* key_label,
                                 const void* external_id, size_t external_id_len,
                                 const gdi_rma_name_ref* labels, uint32_t label_count,
                                 const gdi_rma_property_ref* properties, uint32_t property_count,
                                 uint32_t flags, gdi_rma_vertex_uid* out_uid, bool* out_created,
                                 uint32_t* out_flags) {
  if (!check_tx(tx, "gdi_rma_get_or_create_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_uid == NULL || key_label == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_or_create_vertex: out_uid / key_label is NULL");
  }
  if (out_created != NULL) {
    *out_created = false;
  }
  if (out_flags != NULL) {
    *out_flags = 0;
  }

  bool found = false;
  int rc = gdi_rma_translate_vertex(tx, key_label, external_id, external_id_len, out_uid, &found);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  if (found) {
    /* MERGE: an existing vertex comes back untouched (no label/property merge),
     * matching the in-memory reference backend's semantics. */
    if (out_created != NULL) {
      *out_created = false;
    }
    gdi_rma_clear_error();
    return GDI_RMA_OK;
  }
  /* GDI indexes (label, external_id) at commit for the labels the vertex ACTUALLY
   * carries (gdi_transaction.c), so a key label the vertex does not have would
   * make the next MERGE miss and duplicate the vertex. */
  int key_is_a_label = 0;
  for (uint32_t i = 0; i < label_count; i++) {
    if (labels[i].len == key_label->len &&
        memcmp(labels[i].name, key_label->name, key_label->len) == 0) {
      key_is_a_label = 1;
      break;
    }
  }
  if (label_count == 0) {
    key_is_a_label = 1; /* indexed under GDI_LABEL_NONE, which is what a keyless
                           vertex does; the caller asked for no labels */
  }
  if (!key_is_a_label) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_get_or_create_vertex: the MERGE key label must be one of the "
                        "vertex labels, otherwise GDI indexes the vertex under another key");
  }
  rc = gdi_rma_create_vertex(tx, external_id, external_id_len, labels, label_count, properties,
                            property_count, flags, out_uid, out_flags);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  if (out_created != NULL) {
    *out_created = true;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_free_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid) {
  if (!check_tx(tx, "gdi_rma_free_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  GDI_VertexHolder to_free = holder;
  int st = GDI_FreeVertex(&to_free);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreeVertex");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_free_vertex_guarded(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                bool* out_edges_found) {
  if (!check_tx(tx, "gdi_rma_free_vertex_guarded")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_edges_found != NULL) {
    *out_edges_found = false;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  /* count probe over every orientation (the GDI size trick: NULL array, 0) */
  size_t edges = 0;
  int st = GDI_GetEdgesOfVertex(NULL, 0, &edges, GDI_CONSTRAINT_NULL, (int)GDI_RMA_ORIENT_ALL,
                                holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetEdgesOfVertex(count probe)");
  }
  if (edges > 0) {
    if (out_edges_found != NULL) {
      *out_edges_found = true;
    }
    return GDI_RMA_FAIL(GDI_RMA_ERR_EDGES_EXIST,
                        "the vertex still has %u incident edge(s): GDI edges still exist",
                        (unsigned)edges);
  }
  GDI_VertexHolder to_free = holder;
  st = GDI_FreeVertex(&to_free);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreeVertex");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_add_label_to_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid, const char* label,
                                size_t label_len) {
  if (!check_tx(tx, "gdi_rma_add_label_to_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_label* resolved =
      gdi_rma_resolve_label(tx->db, label, label_len, 0, &status, NULL);
  if (resolved == NULL) {
    return status;
  }
  int st = GDI_AddLabelToVertex(resolved->gdi, holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_AddLabelToVertex");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_remove_label_from_vertex(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                     const char* label, size_t label_len) {
  if (!check_tx(tx, "gdi_rma_remove_label_from_vertex")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_label* resolved =
      gdi_rma_resolve_label(tx->db, label, label_len, 0, &status, NULL);
  if (resolved == NULL) {
    return status;
  }
  int st = GDI_RemoveLabelFromVertex(resolved->gdi, holder);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_RemoveLabelFromVertex");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* One property write on a vertex: mode 0 add, 1 set, 2 update. */
static int vertex_property_op(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                              const gdi_rma_property_ref* property, const char* call, int mode) {
  if (!check_tx(tx, call)) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (property == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "%s: property is NULL", call);
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_ptype* ptype =
      gdi_rma_resolve_ptype(tx->db, property->ptype_name, property->ptype_name_len, &status);
  if (ptype == NULL) {
    return status;
  }
  rc = gdi_rma_check_property(property, ptype->gdi);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int st;
  const char* gdi_call;
  switch (mode) {
    case 1:
      st = GDI_SetPropertyOfVertex(property->value, (size_t)property->count, ptype->gdi, holder);
      gdi_call = "GDI_SetPropertyOfVertex";
      break;
    case 2:
      st = GDI_UpdatePropertyOfVertex(property->value, (size_t)property->count, ptype->gdi,
                                      holder);
      gdi_call = "GDI_UpdatePropertyOfVertex";
      break;
    default:
      st = GDI_AddPropertyToVertex(property->value, (size_t)property->count, ptype->gdi, holder);
      gdi_call = "GDI_AddPropertyToVertex";
      break;
  }
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, gdi_call);
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_vertex_add_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                 const gdi_rma_property_ref* property) {
  return vertex_property_op(tx, uid, property, "gdi_rma_vertex_add_property", 0);
}

int gdi_rma_vertex_set_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                 const gdi_rma_property_ref* property) {
  return vertex_property_op(tx, uid, property, "gdi_rma_vertex_set_property", 1);
}

int gdi_rma_vertex_update_property(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                    const gdi_rma_property_ref* property) {
  return vertex_property_op(tx, uid, property, "gdi_rma_vertex_update_property", 2);
}

int gdi_rma_vertex_remove_properties(gdi_rma_tx* tx, const gdi_rma_vertex_uid* uid,
                                     const char* const* ptype_names, const size_t* ptype_name_lens,
                                     uint32_t ptype_count) {
  if (!check_tx(tx, "gdi_rma_vertex_remove_properties")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (ptype_names == NULL || ptype_name_lens == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_vertex_remove_properties: name array or length array is NULL");
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, uid, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  for (uint32_t i = 0; i < ptype_count; i++) {
    int status = GDI_RMA_OK;
    struct gdi_rma_ptype* ptype =
        gdi_rma_resolve_ptype(tx->db, ptype_names[i], ptype_name_lens[i], &status);
    if (ptype == NULL) {
      return status;
    }
    int st = GDI_RemovePropertiesFromVertex(ptype->gdi, holder);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, "GDI_RemovePropertiesFromVertex");
    }
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* ------------------------------------------------------------------------- */
/* edges                                                                      */
/* ------------------------------------------------------------------------- */

int gdi_rma_create_edge(gdi_rma_tx* tx, const gdi_rma_vertex_uid* origin,
                        const gdi_rma_vertex_uid* target, uint32_t direction,
                        const gdi_rma_name_ref* labels, uint32_t label_count,
                        const gdi_rma_property_ref* properties, uint32_t property_count,
                        gdi_rma_edge_uid* out_uid) {
  if (!check_tx(tx, "gdi_rma_create_edge")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_uid == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_create_edge: out_uid is NULL");
  }
  if (direction != (uint32_t)GDI_EDGE_DIRECTED && direction != (uint32_t)GDI_EDGE_UNDIRECTED) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_create_edge: direction %u is neither 259 directed nor 260 "
                        "undirected",
                        direction);
  }
  if ((label_count > 0 && labels == NULL) || (property_count > 0 && properties == NULL)) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_create_edge: a non-zero count with a NULL array");
  }
  memset(out_uid, 0, sizeof(*out_uid));

  GDI_VertexHolder origin_holder = GDI_VERTEX_NULL;
  int rc = gdi_rma_associate_vertex(tx, origin, &origin_holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  GDI_VertexHolder target_holder = GDI_VERTEX_NULL;
  rc = gdi_rma_associate_vertex(tx, target, &target_holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }

  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int st = GDI_CreateEdge((int)direction, origin_holder, target_holder, &edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_CreateEdge");
  }
  gdi_rma_edge_uid uid = gdi_rma_edge_uid_of(edge);
  gdi_rma_registry_put(&tx->edges, &uid, edge);

  /* A lightweight edge carries at most ONE label in GDI-RMA; a second label
   * fails with the GDI error, faithfully translated. */
  rc = apply_initial_schema(tx, NULL, edge, labels, label_count, properties, property_count, 0,
                            NULL);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  *out_uid = uid;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_free_edge(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid) {
  if (!check_tx(tx, "gdi_rma_free_edge")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_free_edge", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  GDI_EdgeHolder to_free = edge;
  int st = GDI_FreeEdge(&to_free);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_FreeEdge");
  }
  /* GDI frees the holder when the transaction closes; drop our registry entry
   * now so a later call on the same uid reports EDGE_NOT_ASSOCIATED. */
  gdi_rma_registry_erase(&tx->edges, uid);
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

static int edge_set_endpoint(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                             const gdi_rma_vertex_uid* new_endpoint, gdi_rma_edge_uid* out_new_uid,
                             const char* call, int is_origin) {
  if (!check_tx(tx, call)) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (new_endpoint == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "%s: the new endpoint uid is NULL", call);
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, call, &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  GDI_VertexHolder holder = GDI_VERTEX_NULL;
  rc = gdi_rma_associate_vertex(tx, new_endpoint, &holder);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int st = is_origin ? GDI_SetOriginVertexOfEdge(holder, edge)
                     : GDI_SetTargetVertexOfEdge(holder, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st,
                            is_origin ? "GDI_SetOriginVertexOfEdge" : "GDI_SetTargetVertexOfEdge");
  }
  /* the edge physically moved inside the origin's lightweight-edge block: the
   * uid changes, so the registry is re-keyed */
  gdi_rma_edge_uid moved = gdi_rma_edge_uid_of(edge);
  if (moved.origin_vertex_uid != uid->origin_vertex_uid ||
      moved.origin_offset != uid->origin_offset) {
    gdi_rma_registry_erase(&tx->edges, uid);
    gdi_rma_registry_put(&tx->edges, &moved, edge);
  }
  if (out_new_uid != NULL) {
    *out_new_uid = moved;
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_edge_set_origin(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                             const gdi_rma_vertex_uid* new_origin, gdi_rma_edge_uid* out_new_uid) {
  return edge_set_endpoint(tx, uid, new_origin, out_new_uid, "gdi_rma_edge_set_origin", 1);
}

int gdi_rma_edge_set_target(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                            const gdi_rma_vertex_uid* new_target, gdi_rma_edge_uid* out_new_uid) {
  return edge_set_endpoint(tx, uid, new_target, out_new_uid, "gdi_rma_edge_set_target", 0);
}

int gdi_rma_edge_set_direction(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid, uint32_t direction) {
  if (!check_tx(tx, "gdi_rma_edge_set_direction")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (direction != (uint32_t)GDI_EDGE_DIRECTED && direction != (uint32_t)GDI_EDGE_UNDIRECTED) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_edge_set_direction: direction %u is neither 259 nor 260",
                        direction);
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_set_direction", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int st = GDI_SetDirectionTypeOfEdge((int)direction, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_SetDirectionTypeOfEdge");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_edge_get_direction(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                               uint32_t* out_direction) {
  if (!check_tx(tx, "gdi_rma_edge_get_direction")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_direction == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "gdi_rma_edge_get_direction: out is NULL");
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_get_direction", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int dtype = 0;
  int st = GDI_GetDirectionTypeOfEdge(&dtype, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetDirectionTypeOfEdge");
  }
  *out_direction = (uint32_t)dtype;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_edge_get_vertices(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                               gdi_rma_vertex_uid* out_origin, gdi_rma_vertex_uid* out_target) {
  if (!check_tx(tx, "gdi_rma_edge_get_vertices")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (out_origin == NULL || out_target == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_edge_get_vertices: an out pointer is NULL");
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_get_vertices", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  GDI_Vertex_uid origin_uid = 0;
  GDI_Vertex_uid target_uid = 0;
  int st = GDI_GetVerticesOfEdge(&origin_uid, &target_uid, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_GetVerticesOfEdge");
  }
  out_origin->value = origin_uid;
  out_target->value = target_uid;
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_edge_add_label(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid, const char* label,
                            size_t label_len) {
  if (!check_tx(tx, "gdi_rma_edge_add_label")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_add_label", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_label* resolved =
      gdi_rma_resolve_label(tx->db, label, label_len, 0, &status, NULL);
  if (resolved == NULL) {
    return status;
  }
  int st = GDI_AddLabelToEdge(resolved->gdi, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_AddLabelToEdge");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

int gdi_rma_edge_remove_label(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid, const char* label,
                               size_t label_len) {
  if (!check_tx(tx, "gdi_rma_edge_remove_label")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_remove_label", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_label* resolved =
      gdi_rma_resolve_label(tx->db, label, label_len, 0, &status, NULL);
  if (resolved == NULL) {
    return status;
  }
  int st = GDI_RemoveLabelFromEdge(resolved->gdi, edge);
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, "GDI_RemoveLabelFromEdge");
  }
  gdi_rma_clear_error();
  return GDI_RMA_OK;
}

/* The edge property writes: GDI-RMA validates and returns, but stores nothing
 * (src/README.md, "Functions Which Implement Only Input Parsing").  The status
 * is GDI_RMA_OK with a warning left in gdi_rma_last_error(). */
static int edge_property_op(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                            const gdi_rma_property_ref* property, const char* call, int mode) {
  if (!check_tx(tx, call)) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (property == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT, "%s: property is NULL", call);
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, call, &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int status = GDI_RMA_OK;
  struct gdi_rma_ptype* ptype =
      gdi_rma_resolve_ptype(tx->db, property->ptype_name, property->ptype_name_len, &status);
  if (ptype == NULL) {
    return status;
  }
  rc = gdi_rma_check_property(property, ptype->gdi);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  int st;
  const char* gdi_call;
  switch (mode) {
    case 1:
      st = GDI_SetPropertyOfEdge(property->value, (size_t)property->count, ptype->gdi, edge);
      gdi_call = "GDI_SetPropertyOfEdge";
      break;
    case 2:
      st = GDI_UpdatePropertyOfEdge(property->value, (size_t)property->count, ptype->gdi, edge);
      gdi_call = "GDI_UpdatePropertyOfEdge";
      break;
    default:
      st = GDI_AddPropertyToEdge(property->value, (size_t)property->count, ptype->gdi, edge);
      gdi_call = "GDI_AddPropertyToEdge";
      break;
  }
  if (st != GDI_SUCCESS) {
    return gdi_rma_from_gdi(st, gdi_call);
  }
  return gdi_rma_fail(GDI_RMA_OK,
                      "%s: GDI-RMA implements only input parsing for edge properties; the call "
                      "validated but stored nothing",
                      call);
}

int gdi_rma_edge_add_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                               const gdi_rma_property_ref* property) {
  return edge_property_op(tx, uid, property, "gdi_rma_edge_add_property", 0);
}

int gdi_rma_edge_set_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                               const gdi_rma_property_ref* property) {
  return edge_property_op(tx, uid, property, "gdi_rma_edge_set_property", 1);
}

int gdi_rma_edge_update_property(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                  const gdi_rma_property_ref* property) {
  return edge_property_op(tx, uid, property, "gdi_rma_edge_update_property", 2);
}

int gdi_rma_edge_remove_properties(gdi_rma_tx* tx, const gdi_rma_edge_uid* uid,
                                   const char* const* ptype_names, const size_t* ptype_name_lens,
                                   uint32_t ptype_count) {
  if (!check_tx(tx, "gdi_rma_edge_remove_properties")) {
    return GDI_RMA_ERR_INVALID_HANDLE;
  }
  if (ptype_names == NULL || ptype_name_lens == NULL) {
    return GDI_RMA_FAIL(GDI_RMA_ERR_INVALID_ARGUMENT,
                        "gdi_rma_edge_remove_properties: name or length array is NULL");
  }
  GDI_EdgeHolder edge = GDI_EDGE_NULL;
  int rc = resolve_edge(tx, uid, "gdi_rma_edge_remove_properties", &edge);
  if (rc != GDI_RMA_OK) {
    return rc;
  }
  for (uint32_t i = 0; i < ptype_count; i++) {
    int status = GDI_RMA_OK;
    struct gdi_rma_ptype* ptype =
        gdi_rma_resolve_ptype(tx->db, ptype_names[i], ptype_name_lens[i], &status);
    if (ptype == NULL) {
      return status;
    }
    int st = GDI_RemovePropertiesFromEdge(ptype->gdi, edge);
    if (st != GDI_SUCCESS) {
      return gdi_rma_from_gdi(st, "GDI_RemovePropertiesFromEdge");
    }
  }
  return gdi_rma_fail(GDI_RMA_OK,
                      "gdi_rma_edge_remove_properties: GDI-RMA implements only input parsing for "
                      "edge properties; the call validated but stored nothing");
}

// Smoke test for the Phase-4 C FFI export layer (src/api/gdi_rma_api.*).
// Lives in tests-smoke/, built only by the explicit `xmake build smoke_gdi_rma_api`
// target (set_default(false)) - never by a plain `xmake`.
//
// It walks the ABI the way the Phase-5 managed backend will: version probe ->
// init -> database -> schema (label, property type, constraint) -> transaction
// -> vertex create/translate/property/label reads -> edge create + reads ->
// guarded delete -> commit -> collective read transaction -> teardown.
// Everything runs through the exported functions only; no GDI_* symbol is called
// here, which is exactly the property the test is checking.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi_rma_api.h"

static int failures = 0;

#define CHECK(call)                                                            \
  do {                                                                        \
    int st_ = (call);                                                          \
    if (st_ != GDI_RMA_OK) {                                                   \
      fprintf(stderr, "gdi_rma_api_smoke: FAIL %s -> %d (%s) | %s\n", #call,   \
              st_, gdi_rma_error_string(st_), gdi_rma_last_error());           \
      failures++;                                                             \
    } else {                                                                  \
      printf("gdi_rma_api_smoke: ok   %s\n", #call);                          \
    }                                                                         \
  } while (0)

#define REQUIRE(expr)                                                          \
  do {                                                                        \
    if (!(expr)) {                                                            \
      fprintf(stderr, "gdi_rma_api_smoke: FAIL condition %s\n", #expr);        \
      failures++;                                                             \
    } else {                                                                  \
      printf("gdi_rma_api_smoke: ok   %s\n", #expr);                          \
    }                                                                         \
  } while (0)

int main(void) {
  // ---- identity probe (safe before MPI is initialized) ---------------------
  REQUIRE(gdi_rma_api_version() == GDI_RMA_ABI_VERSION);
  printf("gdi_rma_api_smoke: build %s\n", gdi_rma_build_info());
  REQUIRE(gdi_rma_error_string(GDI_RMA_OK) != NULL);
  REQUIRE(gdi_rma_error_string(GDI_RMA_ERR_EDGE_NOT_ASSOCIATED) != NULL);
  REQUIRE(gdi_rma_datatype_size(GDI_RMA_DT_DOUBLE) == 8);
  REQUIRE(gdi_rma_datatype_size(GDI_RMA_DT_DECIMAL) == 67);
  REQUIRE(gdi_rma_datatype_size(999) == 0);
  // a graph op before init must be refused, not crash
  gdi_rma_db* early = NULL;
  REQUIRE(gdi_rma_db_create(&early) == GDI_RMA_ERR_NOT_INITIALIZED);
  REQUIRE(gdi_rma_last_error()[0] != '\0');

  // ---- lifecycle -----------------------------------------------------------
  gdi_rma_init_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.struct_size = sizeof(opts);
  opts.block_size = 256;
  opts.memory_size = 4u * 1024u * 1024u;
  CHECK(gdi_rma_init(&opts));
  CHECK(gdi_rma_init(&opts));  // refcounted: the second call just adds a ref
  REQUIRE(gdi_rma_refcount() == 2);
  REQUIRE(gdi_rma_is_initialized());
  int rank = -1, size = -1;
  CHECK(gdi_rma_mpi_rank(&rank));
  CHECK(gdi_rma_mpi_size(&size));
  printf("gdi_rma_api_smoke: rank %d of %d\n", rank, size);

  // ---- database ------------------------------------------------------------
  gdi_rma_db* db = NULL;
  CHECK(gdi_rma_db_create(&db));
  uint32_t block_size = 0;
  uint64_t memory_size = 0;
  CHECK(gdi_rma_db_block_size(db, &block_size));
  CHECK(gdi_rma_db_memory_size(db, &memory_size));
  REQUIRE(block_size == 256);
  REQUIRE(memory_size == 4u * 1024u * 1024u);

  // ---- schema: labels and property types -----------------------------------
  gdi_rma_label* vlabel = NULL;
  gdi_rma_label* elabel = NULL;
  CHECK(gdi_rma_label_create(db, "Person", 6, &vlabel));
  CHECK(gdi_rma_label_create(db, "knows", 5, &elabel));
  // duplicate name -> GDI_RMA_ERROR_NAME_EXISTS, and the lookup finds the first
  gdi_rma_label* dup = NULL;
  REQUIRE(gdi_rma_label_create(db, "Person", 6, &dup) == GDI_RMA_ERROR_NAME_EXISTS);
  gdi_rma_label* found_label = NULL;
  CHECK(gdi_rma_label_by_name(db, "Person", 6, &found_label));
  REQUIRE(found_label == vlabel);  // the cache hands out the same box
  gdi_rma_label* missing = NULL;
  REQUIRE(gdi_rma_label_by_name(db, "NoSuchLabel", 11, &missing) == GDI_RMA_ERR_NOT_FOUND);
  REQUIRE(gdi_rma_label_by_name(db, "A VERY LONG LABEL NAME OVER SIXTY-FOUR BYTES FOR SURE NO",
                               64, &missing) == GDI_RMA_ERR_NAME_TOO_LONG);
  const char* name_ptr = NULL;
  size_t name_len = 0;
  CHECK(gdi_rma_label_name(vlabel, &name_ptr, &name_len));
  REQUIRE(name_len == 6 && memcmp(name_ptr, "Person", 6) == 0);

  gdi_rma_ptype* age = NULL;
  gdi_rma_ptype* name_pt = NULL;
  CHECK(gdi_rma_ptype_create(db, "age", 3, GDI_RMA_DT_INT64, GDI_RMA_ENTITY_SINGLE,
                             GDI_RMA_SIZE_FIXED, 1, &age));
  CHECK(gdi_rma_ptype_create(db, "name", 4, GDI_RMA_DT_CHAR, GDI_RMA_ENTITY_SINGLE,
                             GDI_RMA_SIZE_MAX, 32, &name_pt));
  gdi_rma_ptype_info info;
  CHECK(gdi_rma_ptype_info_of(age, &info));
  REQUIRE(info.dtype == GDI_RMA_DT_INT64 && info.count == 1 && info.handle == age);
  gdi_rma_ptype_info* all_ptypes = NULL;
  size_t all_count = 0;
  CHECK(gdi_rma_db_all_ptypes(db, &all_ptypes, &all_count));
  REQUIRE(all_count == 2);
  gdi_rma_free(all_ptypes);
  gdi_rma_strlist labels_of_db;
  CHECK(gdi_rma_db_all_labels(db, &labels_of_db));
  REQUIRE(labels_of_db.count == 2);
  gdi_rma_strlist_free(&labels_of_db);

  // ---- transaction ---------------------------------------------------------
  gdi_rma_tx* tx = NULL;
  CHECK(gdi_rma_tx_begin(db, &tx));
  bool collective = true;
  CHECK(gdi_rma_tx_is_collective(tx, &collective));
  REQUIRE(!collective);

  // ---- vertices -------------------------------------------------------------
  int64_t id0 = 1000, id1 = 2000;
  gdi_rma_name_ref vlabels[1] = {{ "Person", 6 }};
  int64_t age_value = 42;
  char name_value[5] = "zaph";  // 4 UTF-8 bytes, no NUL at the edge
  gdi_rma_property_ref props[2] = {
      { "age", 3, GDI_RMA_DT_INT64, 1, &age_value, sizeof(age_value) },
      { "name", 4, GDI_RMA_DT_CHAR, 4, name_value, 4 },
  };
  gdi_rma_vertex_uid v0, v1;
  uint32_t vflags = 0;
  CHECK(gdi_rma_create_vertex(tx, &id0, sizeof(id0), vlabels, 1, props, 2,
                             GDI_RMA_CREATE_F_CHECK_UNIQUE, &v0, &vflags));
  // Nothing is committed yet, so on one rank the probe cannot hit.  Under mpiexec
  // -n > 1 every rank runs this same body, so a peer may already have COMMITTED
  // its copy of the (Person, 1000) key while this rank is still creating: the
  // warning then appears, and that is precisely what it is for.  Only the
  // single-rank case is asserted.
  if (size == 1) {
    REQUIRE(!(vflags & GDI_RMA_VERTEX_F_NON_UNIQUE_ID));
  } else if ((vflags & GDI_RMA_VERTEX_F_NON_UNIQUE_ID) != 0) {
    printf("gdi_rma_api_smoke: note rank %d saw a cross-rank duplicate key\n", rank);
  }
  CHECK(gdi_rma_create_vertex(tx, &id1, sizeof(id1), vlabels, 1, NULL, 0, 0, &v1, NULL));

  // a wrong dtype must be refused before it reaches GDI
  double wrong = 1.5;
  gdi_rma_property_ref bad = { "age", 3, GDI_RMA_DT_DOUBLE, 1, &wrong, sizeof(wrong) };
  REQUIRE(gdi_rma_vertex_add_property(tx, &v0, &bad) == GDI_RMA_ERR_TYPE_MISMATCH);

  int64_t age_new = 43;
  gdi_rma_property_ref age_prop = { "age", 3, GDI_RMA_DT_INT64, 1, &age_new, sizeof(age_new) };
  CHECK(gdi_rma_vertex_set_property(tx, &v0, &age_prop));
  // GDI_UpdatePropertyOfVertex merges into an EXISTING value: on v1, which has no
  // "age" yet, GDI answers GDI_RMA_ERROR_NO_PROPERTY (26), translated 1:1.
  REQUIRE(gdi_rma_vertex_update_property(tx, &v1, &age_prop) == GDI_RMA_ERROR_NO_PROPERTY);
  CHECK(gdi_rma_vertex_add_property(tx, &v1, &age_prop));
  CHECK(gdi_rma_vertex_update_property(tx, &v1, &age_prop));

  gdi_rma_strlist v0_labels;
  CHECK(gdi_rma_get_labels_of_vertex(tx, &v0, &v0_labels));
  REQUIRE(v0_labels.count == 1 && strcmp(v0_labels.items[0], "Person") == 0);
  gdi_rma_strlist_free(&v0_labels);

  gdi_rma_ptype_info* v0_ptypes = NULL;
  size_t v0_ptype_count = 0;
  CHECK(gdi_rma_get_all_property_types_of_vertex(tx, &v0, &v0_ptypes, &v0_ptype_count));
  REQUIRE(v0_ptype_count >= 2);  // age + name (GDI also keeps the ID property)
  gdi_rma_free(v0_ptypes);

  // ---- edges ----------------------------------------------------------------
  gdi_rma_name_ref elabels[1] = {{ "knows", 5 }};
  gdi_rma_edge_uid e0;
  CHECK(gdi_rma_create_edge(tx, &v0, &v1, GDI_RMA_DIR_DIRECTED, elabels, 1, NULL, 0, &e0));
  REQUIRE(e0.origin_vertex_uid == v0.value);
  gdi_rma_vertex_uid e_origin, e_target;
  CHECK(gdi_rma_edge_get_vertices(tx, &e0, &e_origin, &e_target));
  REQUIRE(e_origin.value == v0.value && e_target.value == v1.value);
  uint32_t direction = 0;
  CHECK(gdi_rma_edge_get_direction(tx, &e0, &direction));
  REQUIRE(direction == GDI_RMA_DIR_DIRECTED);
  gdi_rma_strlist e_labels;
  CHECK(gdi_rma_get_labels_of_edge(tx, &e0, &e_labels));
  REQUIRE(e_labels.count == 1 && strcmp(e_labels.items[0], "knows") == 0);
  gdi_rma_strlist_free(&e_labels);
  // an edge uid from outside this transaction is unreachable (G2 gap)
  gdi_rma_edge_uid foreign = e0;
  foreign.origin_offset += 1000;
  REQUIRE(gdi_rma_free_edge(tx, &foreign) == GDI_RMA_ERR_EDGE_NOT_ASSOCIATED);
  // and the unsupported edge reads say so explicitly
  gdi_rma_blob edge_props;
  REQUIRE(gdi_rma_get_properties_of_edge(tx, &e0, &edge_props) == GDI_RMA_ERR_UNSUPPORTED);

  // ---- paged reads -----------------------------------------------------------
  gdi_rma_edge_page page;
  CHECK(gdi_rma_get_edges_of_vertex(tx, &v0, NULL, GDI_RMA_ORIENT_OUTGOING, 0, &page));
  gdi_rma_edge_page_free(&page);
  CHECK(gdi_rma_get_edges_of_vertex(tx, &v0, NULL, GDI_RMA_ORIENT_OUTGOING, 1, &page));
  REQUIRE(page.returned == 1);
  REQUIRE(page.uids[0].origin_vertex_uid == v0.value);
  gdi_rma_edge_page_free(&page);

  gdi_rma_vertex_page npage;
  CHECK(gdi_rma_get_neighbors_of_vertex(tx, &v0, NULL, GDI_RMA_ORIENT_OUTGOING, 8, &npage));
  REQUIRE(npage.full_count == 1 && npage.returned == 1);
  REQUIRE(npage.uids[0].value == v1.value);
  gdi_rma_vertex_page_free(&npage);

  // a guarded delete must refuse while the edge exists
  REQUIRE(gdi_rma_free_vertex_guarded(tx, &v0, NULL) == GDI_RMA_ERR_EDGES_EXIST);

  CHECK(gdi_rma_tx_commit(tx));

  // ---- committed state: the external id index is visible now -----------------
  CHECK(gdi_rma_tx_begin(db, &tx));
  gdi_rma_vertex_uid translated;
  bool found = false;
  gdi_rma_name_ref key = { "Person", 6 };
  CHECK(gdi_rma_translate_vertex(tx, &key, &id0, sizeof(id0), &translated, &found));
  REQUIRE(found);
  REQUIRE(translated.value == v0.value);

  // MERGE on the committed vertex must NOT create a second one
  gdi_rma_vertex_uid merged;
  bool created = true;
  CHECK(gdi_rma_get_or_create_vertex(tx, &key, &id0, sizeof(id0), vlabels, 1, NULL, 0, 0, &merged,
                                     &created, NULL));
  REQUIRE(!created && merged.value == v0.value);

  // the same call with an unknown id does create
  int64_t id2 = 3000;
  CHECK(gdi_rma_get_or_create_vertex(tx, &key, &id2, sizeof(id2), vlabels, 1, NULL, 0, 0, &merged,
                                     &created, NULL));
  REQUIRE(created);

  // NON_UNIQUE_ID detection through the wrapper's own probe
  vflags = 0;
  CHECK(gdi_rma_create_vertex(tx, &id0, sizeof(id0), vlabels, 1, NULL, 0,
                             GDI_RMA_CREATE_F_CHECK_UNIQUE, &v1, &vflags));
  REQUIRE((vflags & GDI_RMA_VERTEX_F_NON_UNIQUE_ID) != 0);

  // the property blob of the committed vertex round-trips
  gdi_rma_blob blob;
  CHECK(gdi_rma_get_properties_of_vertex(tx, &translated, &blob));
  REQUIRE(blob.size >= 8);
  uint32_t entries = 0;
  memcpy(&entries, blob.data, sizeof(entries));
  REQUIRE(entries >= 2);  // age + name + the ID property
  printf("gdi_rma_api_smoke: property blob %u bytes, %u entries\n", (unsigned)blob.size,
         (unsigned)entries);
  gdi_rma_blob_free(&blob);

  // a constraint with a label condition filters the edge reads; the same
  // constraint with a property condition is refused by GDI at evaluation time
  gdi_rma_condition conds[1] = {{0}};
  conds[0].kind = GDI_RMA_COND_LABEL;
  conds[0].name = "knows";
  conds[0].name_len = 5;
  conds[0].op = GDI_RMA_OP_EQUAL;
  gdi_rma_constraint* c = NULL;
    CHECK(gdi_rma_constraint_create(db, conds, 1, &c));
    CHECK(gdi_rma_get_edges_of_vertex(tx, &translated, c, GDI_RMA_ORIENT_OUTGOING, 0, &page));
    REQUIRE(page.full_count == 1);
    gdi_rma_label* likes = NULL;
    CHECK(gdi_rma_label_create(db, "likes", 5, &likes));
    CHECK(gdi_rma_constraint_add_label_condition(c, "likes", 5, GDI_RMA_OP_NOTEQUAL));
  CHECK(gdi_rma_get_edges_of_vertex(tx, &translated, c, GDI_RMA_ORIENT_OUTGOING, 0, &page));
  REQUIRE(page.full_count == 1);
  gdi_rma_edge_page_free(&page);
  CHECK(gdi_rma_constraint_free(c));
  CHECK(gdi_rma_tx_abort(tx));  // the extra vertices created above are discarded

  // ---- collective (read-only) transaction -------------------------------------
  gdi_rma_tx* ctx = NULL;
  CHECK(gdi_rma_tx_begin_collective(db, &ctx));
  REQUIRE(gdi_rma_tx_is_collective(ctx, &collective) == GDI_RMA_OK && collective);
  CHECK(gdi_rma_get_neighbors_of_vertex(ctx, &translated, NULL, GDI_RMA_ORIENT_ALL, 16, &npage));
  gdi_rma_vertex_page_free(&npage);
  // a write in a collective transaction is refused by GDI itself
  REQUIRE(gdi_rma_create_vertex(ctx, &id1, sizeof(id1), vlabels, 1, NULL, 0, 0, &v1, NULL) ==
          GDI_RMA_ERROR_READ_ONLY_TRANSACTION);
  CHECK(gdi_rma_tx_commit(ctx));

  // ---- teardown ---------------------------------------------------------------
  CHECK(gdi_rma_db_free(db));
  CHECK(gdi_rma_finalize());
  CHECK(gdi_rma_finalize());
  REQUIRE(!gdi_rma_is_initialized());
  REQUIRE(gdi_rma_finalize() == GDI_RMA_ERR_NOT_INITIALIZED);

  if (failures != 0) {
    fprintf(stderr, "gdi_rma_api_smoke: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("gdi_rma_api_smoke: PASS (FFI surface exercised end to end)\n");
  return 0;
}

// Minimal end-to-end smoke test for the gdi_rma shared library.
// Lives in tests-smoke/, built only by the explicit `xmake build smoke_gdi_rma`
// target (set_default(false)) - never by a plain `xmake`.
//
// Flow (as on benchmarks/graph.c): MPI_Init -> GDI_Init ->
// GDI_CreateDatabase(GDA_Init_params{block_size=256, memory_size=4096,
// MPI_COMM_WORLD}) -> GDI_StartTransaction -> 2x GDI_CreateVertex ->
// GDI_CreateEdge -> GDI_CloseTransaction(COMMIT) -> GDI_FreeDatabase ->
// GDI_Finalize -> MPI_Finalize.
#include <stdio.h>
#include <stdlib.h>

#include "gdi.h"

static int fail(const char* what, int status)
{
    fprintf(stderr, "gdi_smoke: %s failed, status %d\n", what, status);
    return 1;
}

#define GDICHECK(call) do { \
    int st_ = (call); \
    if (st_ != GDI_SUCCESS) return fail(#call, st_); \
    printf("gdi_smoke: ok  %s\n", #call); \
} while (0)

#define MPICHECK(call) do { \
    int st_ = (call); \
    if (st_ != MPI_SUCCESS) return fail(#call, st_); \
    printf("gdi_smoke: ok  %s\n", #call); \
} while (0)

int main(int argc, char** argv)
{
    int rank = -1, size = -1;
    MPICHECK(MPI_Init(&argc, &argv));
    MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
    MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &size));
    printf("gdi_smoke: rank %d of %d\n", rank, size);

    GDICHECK(GDI_Init(&argc, &argv));

    GDA_Init_params params;
    params.block_size = 256;
    params.memory_size = 4096;
    params.comm = MPI_COMM_WORLD;

    GDI_Database db = NULL;
    GDICHECK(GDI_CreateDatabase(&params, sizeof(GDA_Init_params), &db));

    GDI_Transaction transaction = NULL;
    GDICHECK(GDI_StartTransaction(db, &transaction));

    uint64_t id0 = 0, id1 = 1;
    GDI_VertexHolder v0 = NULL, v1 = NULL;
    GDICHECK(GDI_CreateVertex(&id0, sizeof(id0), transaction, &v0));
    GDICHECK(GDI_CreateVertex(&id1, sizeof(id1), transaction, &v1));

    GDI_EdgeHolder edge = NULL;
    GDICHECK(GDI_CreateEdge(GDI_EDGE_DIRECTED, v0, v1, &edge));

    GDICHECK(GDI_CloseTransaction(&transaction, GDI_TRANSACTION_COMMIT));
    GDICHECK(GDI_FreeDatabase(&db));
    GDICHECK(GDI_Finalize());
    MPICHECK(MPI_Finalize());

    printf("gdi_smoke: PASS (2 vertices + 1 directed edge committed)\n");
    return 0;
}

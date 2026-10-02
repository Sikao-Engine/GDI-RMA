/*
 * dll_export.h -- linkage macro of the gdi_rma C-FFI surface (src/api).
 *
 * Mirrors thirdparty/kimix-native/src/core/dll_export.h, reduced to the one
 * macro family this library needs, and keeps it valid for a pure C11 compiler
 * (no C++ attributes, no inline variables) because src/api is compiled into the
 * C library gdi_rma.dll / libgdi_rma.so.
 *
 * Three states, chosen by which macro the includer (or the build) defines:
 *
 *  1. GDI_RMA_STATIC
 *     The consumer either links the objects statically (merged build) or
 *     resolves every symbol itself at run time (LoadLibrary/dlopen +
 *     GetProcAddress).  GDI_RMA_API expands to nothing: plain declarations,
 *     no linkage decoration.  This is also what a caller MUST define when it
 *     includes gdi_rma_api.h from a translation unit that is linked into the
 *     DLL itself.
 *
 *  2. GDI_RMA_EXPORT_DLL
 *     We are building the shared library: dllexport on Windows, default
 *     visibility elsewhere.  The gdi_rma target in xmake.lua defines this
 *     (target-scoped `add_defines`) -- xmake does NOT auto-define *_EXPORTS
 *     for MSVC, so the macro has to come from the build file.
 *
 *  3. neither (the default for a client)
 *     A consumer linking gdi_rma.dll: dllimport on Windows (so the compiler
 *     may use the import thunk), nothing elsewhere.
 *
 * Windows detail that makes this necessary: xmake drives link.exe for a shared
 * target with a generated module-definition file (build/gdi_rma.def, written by
 * the gdi_mpi rule from the prototypes in src/gdi*.h).  MSVC unions the /DEF
 * list with the __declspec(dllexport) symbols, so the GDI_* API keeps flowing
 * through the .def while this FFI surface exports through the macro below --
 * without either side having to know about the other.
 */
#ifndef GDI_RMA_DLL_EXPORT_H
#define GDI_RMA_DLL_EXPORT_H

#if defined(GDI_RMA_STATIC)
/* State 1: undecorated declarations. */
#define GDI_RMA_API
#elif defined(GDI_RMA_EXPORT_DLL)
/* State 2: building the shared library. */
#ifdef _WIN32
#define GDI_RMA_API __declspec(dllexport)
#else
#define GDI_RMA_API __attribute__((visibility("default")))
#endif
#else
/* State 3: consuming the shared library. */
#ifdef _WIN32
#define GDI_RMA_API __declspec(dllimport)
#else
#define GDI_RMA_API
#endif
#endif

#endif /* GDI_RMA_DLL_EXPORT_H */

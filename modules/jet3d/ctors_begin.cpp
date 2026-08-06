// Lower bound marker for the .ctors array.
//
// This Xtensa toolchain is configured without --enable-initfini-array, so GCC
// emits global constructors into .ctors and the linker defines no
// __init_array_start/end for them. The ELF loader resolves those weak
// undefined symbols to NULL, so walking only __init_array_* skips every
// static constructor without reporting anything.
//
// A linker-script fragment cannot bracket the section here: the default script
// has already consumed .ctors by the time an input script's SECTIONS command
// is merged, so both markers land past the data. Instead this file emits its
// own entry INTO .ctors and relies on link order — build.ps1 places this
// object first and ctors_end.cpp last, so the real constructors sit between
// them. Same mechanism crtbegin.o/crtend.o use.
//
// The slot holds NULL; main_tdeck.cpp starts one past it and skips nulls.

typedef void (*init_fn_t)(void);

__attribute__((section(".ctors"), used, aligned(4)))
init_fn_t __meshpunk_ctors_begin = 0;

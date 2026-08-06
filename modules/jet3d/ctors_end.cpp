// Upper bound marker for the .ctors array. Linked last; see ctors_begin.cpp.

typedef void (*init_fn_t)(void);

__attribute__((section(".ctors"), used, aligned(4)))
init_fn_t __meshpunk_ctors_end_marker = 0;

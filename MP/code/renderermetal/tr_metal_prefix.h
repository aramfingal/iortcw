// tr_metal_prefix.h — included ahead of every C++ file of the Metal renderer (-include).
//
// q_shared.h defines min() and max() as macros. metal-cpp, <simd/simd.h> and the C++
// standard library cannot be parsed with them, and nothing in this renderer uses them,
// so they are removed for the whole translation unit.
#ifndef TR_METAL_PREFIX_H
#define TR_METAL_PREFIX_H

#ifdef __cplusplus
extern "C" {
#include "../qcommon/q_shared.h"
}
#undef min
#undef max
#endif

#endif // TR_METAL_PREFIX_H

#pragma once

#include <stddef.h>

// Boot-reserved PSRAM pool for LoRa-protocol modules (.loraproto.elf
// segments + the protocol's MeshHostApi mem_alloc heap). Init once, very
// early in setup(), before the mesh allocation and luaBringUp(). See
// proto_pool.cpp.
void   proto_pool_init(void);
void*  proto_pool_alloc(size_t size);
void   proto_pool_free(void* p);
size_t proto_pool_free_bytes(void);

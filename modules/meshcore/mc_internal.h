#pragma once

// Package-internal host handle: set once in mt init before any MeshCore code
// runs; the Arduino shim (mcshim.cpp) and every Mc* host-class implementation
// route through it.

#include "lora_proto_abi.h"

extern const MeshHostApi* MCH;

// miniaudio implementation unit. Only the device layer is needed — decoding,
// the node graph and the high-level engine are compiled out.
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

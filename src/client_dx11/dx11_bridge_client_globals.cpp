#include "util_guid.h"

// DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC
// These globals are consumed by util_devicecommand/util_modulecommand and must
// be the same symbols used by the DX11 client when launching NvRemixBridge.exe.
bridge_util::Guid gUniqueIdentifier;
bool gbBridgeRunning = true;
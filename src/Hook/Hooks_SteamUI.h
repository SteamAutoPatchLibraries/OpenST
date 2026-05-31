#pragma once

#include "dllmain.h"

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    void Install();
    void Uninstall();

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
}

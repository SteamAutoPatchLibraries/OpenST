#include "Hooks_SteamUI.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/VehCommon.h"
#include <mutex>

namespace {

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    HOOK_FUNC(FillInAppOverview,void*,void* pThis,void* pAppOverview,CSteamApp* pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID,false)) {
            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if(t) {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // The library-state change originates on the FileWatcher background thread.
    // MarkAppChange assumes the UI thread and takes no lock, so the watcher thread
    // only enqueues appIds here; the RunFrame hook drains them on the UI thread.
    std::mutex            g_removalMutex;
    std::vector<AppId_t>  g_pendingRemovals;
    constexpr uint32 kBudgetDivisor = 3;

    // Clears the ownership flag for appId and queues an app change so the overview
    // flush re-evaluates it.
    bool RemoveAppAndSendChange(AppId_t appId) {
        // skip on owned apps
        if(LuaConfig::IsOwned(appId)){
            LOG_STEAMUI_WARN("RemoveAppAndSendChange: appId={} is owned, skipping", appId);
            return false;
        }
        if(CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange)) {
            CSteamApp* pApp = oGetAppByID(g_pController, appId, false);
            if(pApp) {
                pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                LOG_STEAMUI_DEBUG("RemoveAppAndSendChange: cleared owned flag for appId={}", appId);
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AddedOrCreated);
                return true;
            }
            LOG_STEAMUI_WARN("RemoveAppAndSendChange: appId={} not found in GetAppByID", appId);
        }
        return false;
    }

    // CSteamUIAppController::RunFrame - the controller's per-frame tick on the UI
    // thread; its tail flushes pending overview changes to the JS library. We drain
    // a budgeted batch of removals here so each flush stays on the delta path.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void*, void* pController)
    {
        static std::vector<AppId_t> s_draining;

        // Pull anything the FileWatcher thread queued into our UI-thread work set.
        {
            std::lock_guard<std::mutex> lock(g_removalMutex);
            if (!g_pendingRemovals.empty()) {
                s_draining.insert(s_draining.end(),g_pendingRemovals.begin(), g_pendingRemovals.end());
                g_pendingRemovals.clear();
            }
        }

        if (!s_draining.empty() && CAPTURE_READY(GetAppByID)) {
            // Recompute the budget from the library-visible apps still queued
            // remove a third of them this frame. 
            size_t existing = 0;
            for (AppId_t id : s_draining) {
                if (oGetAppByID(g_pController, id, false)){
                    ++existing;
                }
            }
            LOG_STEAMUI_DEBUG("RunFrame: {} pending removals, {} still exist", s_draining.size(), existing);

            size_t budget = existing / kBudgetDivisor;
            if (budget == 0) budget = 1;
            size_t marked = 0;
            while (!s_draining.empty() && marked < budget) {
                AppId_t id = s_draining.back();
                s_draining.pop_back();
                if (RemoveAppAndSendChange(id))   // depotids/unknown ids are free
                    ++marked;
            }
            LOG_STEAMUI_DEBUG("RunFrame: removed {} app(s), {} left", marked, s_draining.size());
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI {
    void Install() {

        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        HOOK_BEGIN();
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        HOOK_END();

    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

}

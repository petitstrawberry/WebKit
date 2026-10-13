// Native positive control for the real JavaScriptCore bytecode-entry counter.
// Separate executable: the offline renderer always keeps scripts disabled.
#include "config.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentView.h"
#include "DocumentWriter.h"
#include "Element.h"
#include "EmptyClients.h"
#include "FrameLoader.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "SandboxFlags.h"
#include "ScarletOfflineStrategies.h"
#include "ScarletScriptEntryCounter.h"
#include "ScriptController.h"
#include "Settings.h"
#include "ThreadGlobalData.h"
#include "ThreadTimers.h"
#include <pal/SessionID.h>
#include <wtf/text/WTFString.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>

using namespace WebCore;
using namespace WTF::StringLiterals;

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || strcmp(argv[1], "--run-id") || !argv[2][0]) {
        printf("SCARLET_SCRIPT_COUNTER_CONTROL_ERROR expected --run-id TOKEN\n");
        return 1;
    }
    const char* runID = argv[2];
    size_t runIDLength = strlen(runID);
    if (runIDLength > 96)
        return 1;
    for (size_t i = 0; i < runIDLength; ++i) {
        char c = runID[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 1;
    }
    printf("SCARLET_SCRIPT_COUNTER_CONTROL_BEGIN run_id=%s\n", runID);
    ScriptController::initializeMainThread();
    initializeScarletOfflineStrategies();
    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    configuration.loadsSubresources = false;
    // The upstream empty-client page is fully sandboxed. Remove only its
    // script prohibition for this positive control; the renderer keeps it.
    auto* mainFrameParameters = std::get_if<PageConfiguration::LocalMainFrameCreationParameters>(&configuration.mainFrameCreationParameters);
    RELEASE_ASSERT(mainFrameParameters);
    mainFrameParameters->effectiveSandboxFlags.remove(SandboxFlag::Scripts);
    RefPtr page = Page::create(WTF::move(configuration));
    page->settings().setScriptEnabled(true);
    RefPtr frame = page->localMainFrame();
    RELEASE_ASSERT(frame);
    frame->setView(LocalFrameView::create(*frame, IntSize { 320, 200 }));
    frame->init();
    RefPtr loader = frame->loader().activeDocumentLoader();
    RELEASE_ASSERT(loader);
    auto before = scarlet_jsc_execution_entry_count();
    auto permissionAttemptsBefore = ScriptController::scriptExecutionCount();
    auto& writer = loader->writer();
    writer.setMIMEType("text/html"_s);
    RELEASE_ASSERT(writer.begin());
    writer.insertDataSynchronously("<!doctype html><html><body><div id='script-marker'>script-disabled</div><script>document.getElementById('script-marker').textContent='SCRIPT-RAN';</script></body></html>"_s);
    writer.end();
    RefPtr document = frame->document();
    RELEASE_ASSERT(document);
    auto marker = document->getElementById(String::fromUTF8("script-marker"));
    bool markerChanged = marker && marker->textContent() == "SCRIPT-RAN"_s;
    auto after = scarlet_jsc_execution_entry_count();
    auto permissionAttemptsAfter = ScriptController::scriptExecutionCount();
    bool counted = after > before && permissionAttemptsAfter > permissionAttemptsBefore;
    frame->loader().stopAllLoaders();
    marker = nullptr;
    document = nullptr;
    loader = nullptr;
    page = nullptr;
    frame = nullptr;
    auto& threadData = threadGlobalDataSingleton();
    threadData.destroy();
    threadData.threadTimers().setSharedTimer(nullptr);
    bool success = counted && markerChanged;
    printf("SCARLET_SCRIPT_COUNTER_CONTROL_%s run_id=%s execution_entries=%" PRIu64 " permission_attempts=%" PRIu64 " marker=%s\n",
        success ? "PASS" : "FAIL", runID, after - before, permissionAttemptsAfter - permissionAttemptsBefore,
        markerChanged ? "SCRIPT-RAN" : "unchanged");
    return success ? 0 : 1;
}

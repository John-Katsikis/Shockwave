#pragma once

// RmlUi's process-wide side: its system, file and font interfaces, initialization and
// shutdown, and the bookkeeping that lets several GameUIs (contexts) share it. RmlUi is
// global and single-threaded, so all of this is main-thread only.

#include <Core/Log.h>
#include <GameUI/GameUI.h>

#include <RmlUi/Core.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace shockwave {

class Input;

SW_LOG_CATEGORY(LogUI);

namespace ui {

// What a document load saw: RmlUi's warnings and errors, and every file it opened (the
// document, its stylesheets and templates), for problems() and hot reload.
struct Capture {
    std::string file;   // the document, inside Content
    std::vector<UIProblem>* problems = nullptr;
    std::set<std::filesystem::path>* opened = nullptr;
    bool missingFont = false;
};

// Starts RmlUi for the first user and shuts it down after the last.
void acquire();
void release();
int users();   // GameUIs alive

// Where relative paths are read from: the Content folder of the GameUI working now.
void setRoot(const std::filesystem::path& root);
const std::filesystem::path& root();
std::filesystem::path resolve(const std::string& path);

// While set, log messages and opened files are recorded into it.
void setCapture(Capture* capture);
Capture* capture();

// The Input that RmlUi's on-screen keyboard requests turn text input on for.
void setTextInput(Input* input);

// Loads a font file once per process (RmlUi can't unload fonts).
bool loadFont(const std::filesystem::path& path, bool fallback);
// The font shipped with the engine (build/bin/fonts): Noto Sans, regular and bold.
void loadDefaultFonts();
const std::string& defaultFontsError();

// Maps a context back to the GameUI that owns it, for inline event handlers.
struct ContextOwner {
    virtual ~ContextOwner() = default;
    virtual void command(Rml::Event& event, const std::string& command) = 0;
};
void registerContext(Rml::Context* context, ContextOwner* owner);
void unregisterContext(Rml::Context* context);
ContextOwner* contextOwner(Rml::Context* context);

}   // namespace ui
}   // namespace shockwave

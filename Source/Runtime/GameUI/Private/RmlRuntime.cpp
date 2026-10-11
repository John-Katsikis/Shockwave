#include "RmlRuntime.h"

#include "FontEngineInterfaceHarfBuzz.h"

#include <Input/Input.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace shockwave::ui {

namespace fs = std::filesystem;

namespace {

// Logs RmlUi's messages under LogUI (so the editor's Console shows them), records them
// for the document being loaded, and connects clipboard and text input to SDL.
class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& rmlMessage) override {
        const bool problem = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING;
        // RmlUi words a missing font as advice; say what's wrong instead.
        const bool missingFont = rmlMessage.rfind("No font face defined", 0) == 0;
        const std::string message = missingFont ? missingFontMessage(rmlMessage) : rmlMessage;
        if (Capture* c = capture(); c && problem && c->problems) {
            if (!missingFont || !c->missingFont) c->problems->push_back({c->file, message});
            c->missingFont |= missingFont;
        }
        // The same complaint can come every frame (a missing font, say): log it once.
        if (problem && !m_logged.insert(message).second) return true;
        if (m_logged.size() > 512) m_logged.clear();
        const LogLevel level = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? LogLevel::Error
                               : type == Rml::Log::LT_WARNING                            ? LogLevel::Warning
                               : type == Rml::Log::LT_ALWAYS                             ? LogLevel::Info
                                                                                         : LogLevel::Verbose;
        logMessage(LogUI, level, message);
        return true;
    }

    static std::string missingFontMessage(const std::string& message) {
        // "...the specified font face 'Nope' [bold] has been successfully loaded..."
        const size_t face = message.find("font face '");
        const size_t end = face == std::string::npos ? face : message.find(" has been", face);
        const std::string which =
            end != std::string::npos ? message.substr(face + 10, end - face - 10) : std::string("for some text");
        return "missing font " + which +
               ": no loaded font matches its font-family, so the text isn't drawn. Use \"Noto Sans\" or add the "
               "font file to Content.";
    }

    void SetClipboardText(const Rml::String& text) override { SDL_SetClipboardText(text.c_str()); }
    void GetClipboardText(Rml::String& text) override {
        if (char* clip = SDL_GetClipboardText()) {
            text = clip;
            SDL_free(clip);
        }
    }
    void ActivateKeyboard(Rml::Vector2f, float) override {
        if (m_textInput) m_textInput->setTextInput(true);
    }
    void DeactivateKeyboard() override {
        if (m_textInput) m_textInput->setTextInput(false);
    }

    void forgetLogged() { m_logged.clear(); }
    Input* m_textInput = nullptr;

private:
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    std::unordered_set<std::string> m_logged;
};

// Reads documents, stylesheets, images and fonts relative to the Content folder, and
// notes which files a document load used.
class FileInterface final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const fs::path file = resolve(path);
        std::FILE* handle = std::fopen(file.string().c_str(), "rb");
        if (handle)
            if (Capture* c = capture(); c && c->opened) c->opened->insert(file);
        return reinterpret_cast<Rml::FileHandle>(handle);
    }
    void Close(Rml::FileHandle file) override { std::fclose(reinterpret_cast<std::FILE*>(file)); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return std::fread(buffer, 1, size, reinterpret_cast<std::FILE*>(file));
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return std::fseek(reinterpret_cast<std::FILE*>(file), offset, origin) == 0;
    }
    size_t Tell(Rml::FileHandle file) override {
        return static_cast<size_t>(std::ftell(reinterpret_cast<std::FILE*>(file)));
    }
};

// RmlUi's HarfBuzz font engine sample, plus loading a file under a given family name,
// which @font-face in a stylesheet needs.
class FontEngine final : public FontEngineInterfaceHarfBuzz {
public:
    using FontEngineInterfaceHarfBuzz::LoadFontFace;
    bool LoadFontFace(
        const Rml::String& path, int faceIndex, const Rml::String& family, Rml::Style::FontStyle style,
        Rml::Style::FontWeight weight, bool fallback) override {
        Rml::String bytes;
        if (!Rml::GetFileInterface()->LoadFile(path, bytes)) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "Couldn't open font file '%s'.", path.c_str());
            return false;
        }
        // The font engine keeps pointing at the data until RmlUi shuts down.
        auto& data = m_data.emplace_back(bytes.begin(), bytes.end());
        return FontEngineInterfaceHarfBuzz::LoadFontFace(
            Rml::Span<const Rml::byte>(data.data(), data.size()), faceIndex, family, style, weight, fallback);
    }
    void clear() { m_data.clear(); }

private:
    std::deque<std::vector<Rml::byte>> m_data;
};

// onclick="resume" and friends: inline handlers name a command for game code.
class CommandListener final : public Rml::EventListener {
public:
    explicit CommandListener(Rml::String command) : m_command(std::move(command)) {}
    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* element = event.GetCurrentElement();
        if (ContextOwner* owner = element ? contextOwner(element->GetContext()) : nullptr)
            owner->command(event, m_command);
    }
    void OnDetach(Rml::Element*) override { delete this; }

private:
    Rml::String m_command;
};

class CommandInstancer final : public Rml::EventListenerInstancer {
public:
    Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override {
        return new CommandListener(value);
    }
};

struct Runtime {
    SystemInterface system;
    FileInterface files;
    FontEngine fonts;
    CommandInstancer commands;
    int users = 0;
    fs::path root;
    Capture* capture = nullptr;
    std::set<fs::path> loadedFonts;
    std::string defaultFontsError;
    std::unordered_map<Rml::Context*, ContextOwner*> contexts;
};

Runtime& runtime() {
    static Runtime r;
    return r;
}

}   // namespace

void acquire() {
    Runtime& r = runtime();
    if (r.users++ > 0) return;
    Rml::SetSystemInterface(&r.system);
    Rml::SetFileInterface(&r.files);
    Rml::SetFontEngineInterface(&r.fonts);
    // The lang attribute picks the script and direction; without it, they're guessed from
    // the text.
    struct Language {
        const char* code;
        const char* script;
        TextFlowDirection direction;
    };
    constexpr Language languages[] = {
        {"en", "Latn", TextFlowDirection::LeftToRight}, {"el", "Grek", TextFlowDirection::LeftToRight},
        {"ru", "Cyrl", TextFlowDirection::LeftToRight}, {"uk", "Cyrl", TextFlowDirection::LeftToRight},
        {"ar", "Arab", TextFlowDirection::RightToLeft}, {"fa", "Arab", TextFlowDirection::RightToLeft},
        {"ur", "Arab", TextFlowDirection::RightToLeft}, {"he", "Hebr", TextFlowDirection::RightToLeft},
        {"hi", "Deva", TextFlowDirection::LeftToRight}, {"th", "Thai", TextFlowDirection::LeftToRight},
        {"zh", "Hani", TextFlowDirection::LeftToRight}, {"ko", "Hang", TextFlowDirection::LeftToRight},
    };
    for (const Language& language : languages)
        r.fonts.RegisterLanguage(language.code, language.script, language.direction);
    Rml::Initialise();
    Rml::Factory::RegisterEventListenerInstancer(&r.commands);
    loadDefaultFonts();
}

void release() {
    Runtime& r = runtime();
    if (r.users <= 0 || --r.users > 0) return;
    Rml::Shutdown();
    r.fonts.clear();
    r.loadedFonts.clear();
    r.system.forgetLogged();
    r.contexts.clear();
}

int users() { return runtime().users; }

void setRoot(const fs::path& root) { runtime().root = root; }
const fs::path& root() { return runtime().root; }

fs::path resolve(const std::string& path) {
    const fs::path p(path);
    if (p.is_absolute() || runtime().root.empty()) return p.lexically_normal();
    return (runtime().root / p).lexically_normal();
}

void setCapture(Capture* capture) { runtime().capture = capture; }
Capture* capture() { return runtime().capture; }

void setTextInput(Input* input) {
    Runtime& r = runtime();
    if (r.system.m_textInput && r.system.m_textInput != input) r.system.m_textInput->setTextInput(false);
    r.system.m_textInput = input;
}

bool loadFont(const fs::path& path, bool fallback) {
    Runtime& r = runtime();
    const fs::path key = path.lexically_normal();
    if (r.loadedFonts.count(key)) return true;
    if (!Rml::LoadFontFace(key.string(), fallback)) return false;
    r.loadedFonts.insert(key);
    return true;
}

void loadDefaultFonts() {
    Runtime& r = runtime();
    r.defaultFontsError.clear();
    const char* base = SDL_GetBasePath();
    const fs::path folder = fs::path(base ? base : "") / "fonts";
    const struct {
        const char* file;
        bool fallback;
    } fonts[] = {{"NotoSans-Regular.ttf", true}, {"NotoSans-Bold.ttf", false}};
    for (const auto& font : fonts) {
        const fs::path path = folder / font.file;
        std::error_code ec;
        if (!fs::exists(path, ec) || !loadFont(path, font.fallback)) {
            if (!r.defaultFontsError.empty()) r.defaultFontsError += "; ";
            r.defaultFontsError += "the default font " + path.string() + " is missing";
        }
    }
}

const std::string& defaultFontsError() { return runtime().defaultFontsError; }

void registerContext(Rml::Context* context, ContextOwner* owner) { runtime().contexts[context] = owner; }
void unregisterContext(Rml::Context* context) { runtime().contexts.erase(context); }
ContextOwner* contextOwner(Rml::Context* context) {
    const auto& contexts = runtime().contexts;
    const auto it = contexts.find(context);
    return it == contexts.end() ? nullptr : it->second;
}

}   // namespace shockwave::ui

// Flow test scenarios and their runner: see GameFramework/FlowTest.h.
#include <GameFramework/FlowTest.h>

#include <GameFramework/AppShell.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace shockwave {

namespace {

// ---- A small strict JSON reader --------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;   // in order; duplicates refused
};

struct Reader {
    std::string_view s;
    size_t i = 0;
    int depth = 0;
    std::string error;

    bool fail(const std::string& why) {
        size_t line = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k) line += s[k] == '\n';
        error = "line " + std::to_string(line) + ": " + why;
        return false;
    }
    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool string(std::string& out) {
        ++i;   // the opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("a control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > s.size()) return fail("a short \\u escape");
                const unsigned long code = std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16);
                i += 4;
                if (code < 0x80) {
                    out += char(code);
                } else if (code < 0x800) {
                    out += char(0xC0 | (code >> 6));
                    out += char(0x80 | (code & 0x3F));
                } else {
                    out += char(0xE0 | (code >> 12));
                    out += char(0x80 | ((code >> 6) & 0x3F));
                    out += char(0x80 | (code & 0x3F));
                }
                break;
            }
            default: return fail(std::string("an unknown escape \\") + e);
            }
        }
        return fail("a string that doesn't end");
    }
    bool value(Json& out) {
        space();
        if (i >= s.size()) return fail("the text ends where a value should be");
        if (++depth > 16) return fail("nested too deeply");
        const char c = s[i];
        bool ok = true;
        if (c == '{') {
            out.type = Json::Type::Object;
            ++i;
            space();
            if (i < s.size() && s[i] == '}') {
                ++i;
            } else {
                while (true) {
                    space();
                    if (i >= s.size() || s[i] != '"') return fail("expected a key in quotes");
                    std::string key;
                    if (!string(key)) return false;
                    for (const auto& [k, v] : out.fields)
                        if (k == key) return fail("\"" + key + "\" is there twice");
                    space();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':' after \"" + key + "\"");
                    ++i;
                    Json v;
                    if (!value(v)) return false;
                    out.fields.emplace_back(std::move(key), std::move(v));
                    space();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        break;
                    }
                    return fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            out.type = Json::Type::Array;
            ++i;
            space();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                while (true) {
                    Json v;
                    if (!value(v)) return false;
                    out.items.push_back(std::move(v));
                    if (out.items.size() > kMaxFlowSteps)
                        return fail("more than " + std::to_string(kMaxFlowSteps) + " items");
                    space();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        break;
                    }
                    return fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            out.type = Json::Type::String;
            ok = string(out.text);
        } else if (s.substr(i, 4) == "true" || s.substr(i, 5) == "false") {
            out.type = Json::Type::Bool;
            out.number = s[i] == 't';
            i += s[i] == 't' ? 4 : 5;
        } else if (s.substr(i, 4) == "null") {
            i += 4;
        } else {
            size_t end = i;
            while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '-' ||
                                      s[end] == '+' || s[end] == '.' || s[end] == 'e' || s[end] == 'E'))
                ++end;
            if (end == i) return fail(std::string("unexpected '") + c + "'");
            const std::string token(s.substr(i, end - i));
            char* stop = nullptr;
            out.number = std::strtod(token.c_str(), &stop);
            if (stop != token.c_str() + token.size() || !std::isfinite(out.number)) return fail("a bad number");
            out.type = Json::Type::Number;
            i = end;
        }
        --depth;
        return ok;
    }
};

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", unsigned(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string number(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", v);
    return buffer;
}

bool phaseFromName(std::string_view name, TravelPhase& out) {
    for (int p = int(TravelPhase::Queued); p <= int(TravelPhase::Cancelled); ++p)
        if (name == travelPhaseName(TravelPhase(p))) {
            out = TravelPhase(p);
            return true;
        }
    return false;
}

bool errorFromName(std::string_view name) {
    for (int e = int(TravelError::None); e <= int(TravelError::RedirectCycle); ++e)
        if (name == travelErrorName(TravelError(e))) return true;
    return false;
}

}   // namespace

const char* flowStepKindName(FlowStep::Kind kind) {
    switch (kind) {
    case FlowStep::Kind::Travel: return "travel";
    case FlowStep::Kind::Click: return "click";
    case FlowStep::Kind::Press: return "press";
    case FlowStep::Kind::WaitPhase: return "wait_phase";
    case FlowStep::Kind::AssertLevel: return "assert_level";
    case FlowStep::Kind::AssertValue: return "assert_value";
    case FlowStep::Kind::Save: return "save";
    case FlowStep::Kind::Load: return "load";
    case FlowStep::Kind::Quit: return "quit";
    case FlowStep::Kind::Hold: return "hold";
    case FlowStep::Kind::Tap: return "tap";
    case FlowStep::Kind::Look: return "look";
    case FlowStep::Kind::Wait: return "wait";
    case FlowStep::Kind::WaitValue: return "wait_value";
    }
    return "quit";
}

bool parseFlowScenario(std::string_view text, FlowScenario& out, std::string& error) {
    if (text.size() > (1u << 20)) {
        error = "the scenario is over 1 MiB";
        return false;
    }
    Reader reader;
    reader.s = text;
    Json root;
    if (!reader.value(root)) {
        error = reader.error;
        return false;
    }
    reader.space();
    if (reader.i != text.size()) {
        error = "text after the scenario's end";
        return false;
    }
    if (root.type != Json::Type::Object) {
        error = "a scenario is an object: {\"format\": 1, \"steps\": [...]}";
        return false;
    }
    FlowScenario scenario;
    bool format = false, steps = false;
    auto seconds = [&](const Json& v, const std::string& where, double& to) {
        if (v.type != Json::Type::Number || !(v.number > 0) || v.number > kMaxFlowTimeout) {
            error = where + "timeout is a number of seconds, more than 0 and at most " +
                    std::to_string(int(kMaxFlowTimeout));
            return false;
        }
        to = v.number;
        return true;
    };
    for (const auto& [key, v] : root.fields) {
        if (key == "format") {
            if (v.type != Json::Type::Number || v.number != 1) {
                error = "format must be 1";
                return false;
            }
            format = true;
        } else if (key == "name") {
            if (v.type != Json::Type::String) {
                error = "name is text";
                return false;
            }
            scenario.name = v.text;
        } else if (key == "timeout") {
            if (!seconds(v, "", scenario.timeout)) return false;
        } else if (key == "steps") {
            if (v.type != Json::Type::Array) {
                error = "steps is a list";
                return false;
            }
            steps = true;
            for (size_t n = 0; n < v.items.size(); ++n) {
                const Json& s = v.items[n];
                const std::string where = "step " + std::to_string(n + 1) + ": ";
                if (s.type != Json::Type::Object) {
                    error = where + "a step is an object, like {\"click\": \"new_game\"}";
                    return false;
                }
                static const std::map<std::string, FlowStep::Kind, std::less<>> kinds = {
                    {"travel", FlowStep::Kind::Travel},
                    {"click", FlowStep::Kind::Click},
                    {"press", FlowStep::Kind::Press},
                    {"wait_phase", FlowStep::Kind::WaitPhase},
                    {"assert_level", FlowStep::Kind::AssertLevel},
                    {"assert_value", FlowStep::Kind::AssertValue},
                    {"save", FlowStep::Kind::Save},
                    {"load", FlowStep::Kind::Load},
                    {"quit", FlowStep::Kind::Quit},
                    {"hold", FlowStep::Kind::Hold},
                    {"tap", FlowStep::Kind::Tap},
                    {"look", FlowStep::Kind::Look},
                    {"wait", FlowStep::Kind::Wait},
                    {"wait_value", FlowStep::Kind::WaitValue}};
                FlowStep step;
                int found = 0;
                for (const auto& [k, f] : s.fields) {
                    const auto kind = kinds.find(k);
                    if (kind != kinds.end()) {
                        ++found;
                        step.kind = kind->second;
                        if (step.kind == FlowStep::Kind::Quit) {
                            if (f.type != Json::Type::Bool || !f.number) {
                                error = where + "quit is written {\"quit\": true}";
                                return false;
                            }
                        } else if (step.kind == FlowStep::Kind::Look) {
                            if (f.type != Json::Type::Array || f.items.size() != 2 ||
                                f.items[0].type != Json::Type::Number || f.items[1].type != Json::Type::Number ||
                                std::abs(f.items[0].number) > 10000 || std::abs(f.items[1].number) > 10000) {
                                error = where + "look is written {\"look\": [x, y]}, in pixels (at most 10000)";
                                return false;
                            }
                            step.lookX = float(f.items[0].number);
                            step.lookY = float(f.items[1].number);
                            step.target = number(f.items[0].number) + " " + number(f.items[1].number);
                        } else if (step.kind == FlowStep::Kind::Wait) {
                            if (f.type != Json::Type::Number || !(f.number > 0) || f.number > kMaxFlowTimeout) {
                                error = where + "wait is a number of seconds, more than 0";
                                return false;
                            }
                            step.seconds = f.number;
                            step.target = number(f.number);
                            step.timeout = std::max(step.timeout, f.number + 10);
                        } else if (f.type != Json::Type::String || f.text.empty()) {
                            error = where + k + " needs a name in quotes";
                            return false;
                        } else {
                            step.target = f.text;
                        }
                    }
                }
                if (found != 1) {
                    error =
                        where + (found ? "only one of travel, click, press, wait_phase, assert_level, assert_value, "
                                         "save, load, hold, tap, look, wait, wait_value or quit"
                                       : "it needs one of travel, click, press, wait_phase, assert_level, "
                                         "assert_value, save, load, hold, tap, look, wait, wait_value or quit");
                    return false;
                }
                const char* kindName = flowStepKindName(step.kind);
                for (const auto& [k, f] : s.fields) {
                    if (kinds.count(k)) continue;
                    auto text = [&](std::string& to) {
                        if (f.type != Json::Type::String) {
                            error = where + k + " is text";
                            return false;
                        }
                        to = f.text;
                        return true;
                    };
                    bool ok = true;
                    if (k == "timeout") {
                        ok = seconds(f, where, step.timeout);
                    } else if (k == "spawn" && step.kind == FlowStep::Kind::Travel) {
                        ok = text(step.spawn);
                    } else if (k == "expect" && step.kind == FlowStep::Kind::Travel) {
                        ok = text(step.expect);
                        if (ok && !errorFromName(step.expect)) {
                            error =
                                where + "expect \"" + step.expect + "\" isn't a travel code (UnknownLevel, Busy, ...)";
                            ok = false;
                        }
                    } else if (k == "document" && step.kind == FlowStep::Kind::Click) {
                        ok = text(step.document);
                    } else if (
                        k == "equals" &&
                        (step.kind == FlowStep::Kind::AssertValue || step.kind == FlowStep::Kind::WaitValue ||
                         step.kind == FlowStep::Kind::Hold)) {
                        ok = text(step.equals);
                    } else if (k == "until" && step.kind == FlowStep::Kind::Hold) {
                        ok = text(step.until);
                    } else if (k == "value" && step.kind == FlowStep::Kind::Hold) {
                        ok = f.type == Json::Type::Number && f.number > 0 && f.number <= 1;
                        if (ok)
                            step.value = float(f.number);
                        else
                            error = where + "value is how far it's held, more than 0 and at most 1";
                    } else if (k == "seconds" && step.kind == FlowStep::Kind::Hold) {
                        ok = f.type == Json::Type::Number && f.number > 0 && f.number <= kMaxFlowTimeout;
                        if (ok)
                            step.seconds = f.number;
                        else
                            error = where + "seconds is a number, more than 0";
                    } else if (k == "frames" && step.kind == FlowStep::Kind::Look) {
                        ok = f.type == Json::Type::Number && f.number >= 1 && f.number <= 10000 &&
                             f.number == std::floor(f.number);
                        if (ok)
                            step.frames = int(f.number);
                        else
                            error = where + "frames is a whole number from 1";
                    } else {
                        error = where + kindName + " has no \"" + k + "\"";
                        ok = false;
                    }
                    if (!ok) return false;
                }
                auto has = [&](const char* name) {
                    for (const auto& [k, f] : s.fields)
                        if (k == name) return true;
                    return false;
                };
                if (step.kind == FlowStep::Kind::Hold || step.kind == FlowStep::Kind::Tap) {
                    std::string why;
                    if (!parseBinding(step.target, step.binding, why)) {
                        error = where + "\"" + step.target + "\" isn't a binding: " + why;
                        return false;
                    }
                    using Source = InputBinding::Source;
                    if (step.binding.source != Source::Key && step.binding.source != Source::Mouse &&
                        step.binding.source != Source::GamepadButton && step.binding.source != Source::GamepadAxis) {
                        error = where + "flow tests press keys, mouse buttons and gamepads, not joysticks";
                        return false;
                    }
                }
                if (step.kind == FlowStep::Kind::Hold) {
                    const bool until = has("until");
                    if (until == (step.seconds > 0) || until != has("equals")) {
                        error = where + "hold needs either \"seconds\" or \"until\" with \"equals\"";
                        return false;
                    }
                    if (until && step.until.rfind("flow.", 0) != 0 && step.until.rfind("game.", 0) != 0) {
                        error = where + "until names flow.<value> or game.<value>";
                        return false;
                    }
                    if (step.seconds > 0) step.timeout = std::max(step.timeout, step.seconds + 10);
                }
                if (step.kind == FlowStep::Kind::WaitValue) {
                    if (step.target.rfind("flow.", 0) != 0 && step.target.rfind("game.", 0) != 0) {
                        error = where + "wait_value names flow.<value> or game.<value>";
                        return false;
                    }
                    if (!has("equals")) {
                        error = where + "wait_value needs \"equals\"";
                        return false;
                    }
                }
                if (step.kind == FlowStep::Kind::Press && step.target != kPauseAction) {
                    error = where + "press takes \"pause\" (the only action flow tests press yet)";
                    return false;
                }
                TravelPhase phase;
                if (step.kind == FlowStep::Kind::WaitPhase && !phaseFromName(step.target, phase)) {
                    error = where + "\"" + step.target +
                            "\" isn't a travel phase (queued, ..., awaiting_boundary, "
                            "succeeded, failed, cancelled)";
                    return false;
                }
                if (step.kind == FlowStep::Kind::AssertValue) {
                    if (step.target.rfind("flow.", 0) != 0 && step.target.rfind("game.", 0) != 0) {
                        error = where + "assert_value names flow.<value> (the shell's) or game.<value> (the level's)";
                        return false;
                    }
                    bool has = false;
                    for (const auto& [k, f] : s.fields) has = has || k == "equals";
                    if (!has) {
                        error = where + "assert_value needs \"equals\"";
                        return false;
                    }
                }
                scenario.steps.push_back(std::move(step));
            }
        } else {
            error = "a scenario has no \"" + key + "\" (it has format, name, timeout and steps)";
            return false;
        }
    }
    if (!format || !steps) {
        error = "a scenario needs \"format\": 1 and \"steps\"";
        return false;
    }
    out = std::move(scenario);
    return true;
}

// ---- The runner ------------------------------------------------------------------------

FlowTest::FlowTest(FlowScenario scenario, Hooks hooks) : m_scenario(std::move(scenario)), m_hooks(std::move(hooks)) {
    m_started = Clock::now();
    if (m_hooks.flow)
        m_subscription = m_hooks.flow->subscribe([this](const TravelCompletion& c) { m_completions.push_back(c); });
}

FlowTest::~FlowTest() = default;

void FlowTest::follow() {
    if (!m_hooks.flow) return;
    if (m_hooks.shell && m_hooks.shell->lastTicket() > m_ticket) m_ticket = m_hooks.shell->lastTicket();
    if (const uint64_t active = m_hooks.flow->activeTicket(); active > m_ticket) m_ticket = active;
}

std::optional<std::string> FlowTest::value(std::string_view name) const {
    if (name.rfind("flow.", 0) == 0) {
        if (!m_hooks.shell) return std::nullopt;
        const UIValue& v = m_hooks.shell->value(name.substr(5));
        if (v.kind() == UIValue::Kind::Unset) return std::nullopt;
        return v.toText();
    }
    if (name.rfind("game.", 0) == 0 && m_hooks.levelValue) return m_hooks.levelValue(name.substr(5));
    return std::nullopt;
}

void FlowTest::finishStep(bool ok, std::string message) {
    const FlowStep& step = m_scenario.steps[m_step];
    Record r{step.kind,    step.target,
             ok,           std::chrono::duration<double, std::milli>(Clock::now() - m_stepStarted).count(),
             m_stepFrames, message};
    m_records.push_back(std::move(r));
    release();   // a hold that timed out lets go too
    m_lookLeft = 0;
    m_stepBegun = false;
    if (!ok) {
        m_state = State::Failed;
        m_message = "step " + std::to_string(m_step + 1) + " (" + flowStepKindName(step.kind) + " " + step.target +
                    "): " + message;
        return;
    }
    ++m_step;
}

bool FlowTest::run(const FlowStep& step, std::string& failure) {
    GameFlow& flow = *m_hooks.flow;
    switch (step.kind) {
    case FlowStep::Kind::Travel: {
        TravelRequest r;
        r.level = step.target;
        r.spawn = step.spawn;
        const TravelTicket t = flow.request(std::move(r));
        if (t) m_ticket = t.id;
        if (!step.expect.empty()) {
            if (t) {
                failure = "it was accepted (ticket " + std::to_string(t.id) + "), not refused as " + step.expect;
                flow.cancel(t.id);
            } else if (step.expect != travelErrorName(t.error)) {
                failure =
                    "refused as " + std::string(travelErrorName(t.error)) + ", not " + step.expect + ": " + t.message;
            }
            return true;
        }
        if (!t) failure = std::string("refused: ") + travelErrorName(t.error) + ": " + t.message;
        return true;
    }
    case FlowStep::Kind::Click: {
        // Waits for the element to show: screens appear a frame after what opens them.
        const bool clicked = step.document.empty()
                                 ? m_hooks.shell && m_hooks.shell->click(step.target)
                                 : m_hooks.clickLevel && m_hooks.clickLevel(step.document, step.target);
        if (!clicked) {
            if (step.document.empty() && !m_hooks.shell) failure = "there's no application shell (app_shell off)";
            return !failure.empty();
        }
        follow();
        return true;
    }
    case FlowStep::Kind::Press: {
        // What the pause key does: open the pause screen, or close it.
        if (!m_hooks.shell) {
            failure = "there's no application shell (app_shell off)";
        } else if (!(m_hooks.shell->paused() ? m_hooks.shell->resume() : m_hooks.shell->pause())) {
            failure = "the pause screen didn't open (app_shell has no pause, or nothing can pause now)";
        }
        return true;
    }
    case FlowStep::Kind::WaitPhase: {
        follow();
        TravelPhase wanted = TravelPhase::Succeeded;
        phaseFromName(step.target, wanted);
        if (!m_ticket) return false;   // nothing asked yet: a level may still ask
        const auto status = flow.status(m_ticket);
        if (!status) {
            failure = "ticket " + std::to_string(m_ticket) + " is unknown";
            return true;
        }
        if (status->phase == wanted) return true;
        if (travelPhaseTerminal(status->phase)) {
            // A phase passed within one frame still counts once the travel succeeded.
            if (status->phase == TravelPhase::Succeeded && !travelPhaseTerminal(wanted)) return true;
            failure = "ticket " + std::to_string(m_ticket) + " to \"" + status->request.level + "\" ended " +
                      travelPhaseName(status->phase) + " (" + travelErrorName(status->code) + ": " + status->message +
                      ")";
            return true;
        }
        return false;
    }
    case FlowStep::Kind::AssertLevel: {
        const bool playing = m_hooks.playing ? m_hooks.playing() : true;
        const std::string& level = flow.currentLevel().level;
        if (!playing || level != step.target)
            failure = playing ? "level \"" + level + "\" is playing" : "nothing is playing (last: \"" + level + "\")";
        return true;
    }
    case FlowStep::Kind::AssertValue: {
        const auto v = value(step.target);
        if (!v) {
            failure = step.target + " isn't set";
        } else if (*v != step.equals) {
            failure = step.target + " is \"" + *v + "\"";
        }
        return true;
    }
    case FlowStep::Kind::Save:
    case FlowStep::Kind::Load: {
        const auto& hook = step.kind == FlowStep::Kind::Save ? m_hooks.save : m_hooks.load;
        std::string error;
        if (!hook) {
            failure = "this run can't save or load";
        } else if (!hook(step.target, error)) {
            failure = error.empty() ? std::string("it failed") : error;
        }
        return true;
    }
    case FlowStep::Kind::Hold:
    case FlowStep::Kind::Tap: {
        if (!m_hooks.inject) {
            failure = "this run can't press anything";
            return true;
        }
        if (!m_holding) {
            m_hooks.inject(step.binding, step.kind == FlowStep::Kind::Hold ? step.value : 1.0f);
            m_holding = true;
            return false;   // held from the next update on
        }
        bool done = step.kind == FlowStep::Kind::Tap;
        if (step.kind == FlowStep::Kind::Hold)
            done = step.seconds > 0 ? m_stepTime >= step.seconds : value(step.until) == step.equals;
        if (done) release();
        return done;
    }
    case FlowStep::Kind::Look: {
        if (!m_hooks.look) {
            failure = "this run can't move the mouse";
            return true;
        }
        if (m_lookLeft == 0 && m_stepFrames == 0) m_lookLeft = step.frames;
        m_hooks.look(step.lookX / float(step.frames), step.lookY / float(step.frames));
        return --m_lookLeft <= 0;
    }
    case FlowStep::Kind::Wait: return m_stepTime >= step.seconds;
    case FlowStep::Kind::WaitValue: return value(step.target) == step.equals;
    case FlowStep::Kind::Quit: return true;
    }
    return true;
}

void FlowTest::release() {
    if (!m_holding) return;
    m_holding = false;
    if (m_step < m_scenario.steps.size() && m_hooks.inject) m_hooks.inject(m_scenario.steps[m_step].binding, 0.0f);
}

FlowTest::State FlowTest::frame(double frameDt) {
    if (m_state != State::Running) return m_state;
    m_frameDt = std::isfinite(frameDt) && frameDt > 0 ? frameDt : 0;
    if (!m_hooks.flow) {
        m_state = State::Failed;
        m_message = "nothing to test: no game flow";
        return m_state;
    }
    follow();
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - m_started).count() > m_scenario.timeout) {
        if (m_step < m_scenario.steps.size()) {
            finishStep(false, "the scenario's " + number(m_scenario.timeout) + " s ran out");
        } else {
            m_state = State::Failed;
            m_message = "the scenario's time ran out";
        }
        return m_state;
    }
    while (m_state == State::Running && m_step < m_scenario.steps.size()) {
        const FlowStep& step = m_scenario.steps[m_step];
        if (!m_stepBegun) {
            m_stepBegun = true;
            m_stepStarted = Clock::now();
            m_stepFrames = 0;
            m_stepTime = 0;
        } else {
            m_stepTime += m_frameDt;   // this step waited through a frame
        }
        std::string failure;
        const bool done = run(step, failure);
        if (!done) {
            ++m_stepFrames;
            if (std::chrono::duration<double>(Clock::now() - m_stepStarted).count() > step.timeout)
                finishStep(
                    false, "it didn't happen within " + number(step.timeout) + " s" +
                               (step.kind == FlowStep::Kind::Click ? " (the element never showed)" : ""));
            return m_state;
        }
        finishStep(failure.empty(), failure);
        if (m_state == State::Running && step.kind == FlowStep::Kind::Quit) {
            m_state = State::Passed;
            return m_state;
        }
        // A step that changed something lets a frame run before the next: travel happens
        // after the frame, and screens show a frame after what opens them.
        if (step.kind == FlowStep::Kind::Travel || step.kind == FlowStep::Kind::Click ||
            step.kind == FlowStep::Kind::Press || step.kind == FlowStep::Kind::Load ||
            step.kind == FlowStep::Kind::Hold || step.kind == FlowStep::Kind::Tap || step.kind == FlowStep::Kind::Look)
            return m_state;
    }
    if (m_state == State::Running && m_step >= m_scenario.steps.size()) m_state = State::Passed;
    return m_state;
}

std::string FlowTest::reportJson() const {
    std::string out = "{\n  \"format\": 1,\n  \"mode\": \"flow_test\",\n";
    out += "  \"scenario\": " + quote(m_scenario.name) + ",\n";
    out += std::string("  \"ok\": ") + (m_state == State::Passed ? "true" : "false") + ",\n";
    out += std::string("  \"state\": ") +
           quote(
               m_state == State::Passed   ? "passed"
               : m_state == State::Failed ? "failed"
                                          : "running") +
           ",\n";
    out += "  \"failed_step\": " +
           (m_state == State::Failed && m_step < m_scenario.steps.size() ? std::to_string(m_step + 1)
                                                                         : std::string("null")) +
           ",\n";
    out += "  \"message\": " + quote(m_message) + ",\n";
    out += "  \"elapsed_ms\": " + number(std::chrono::duration<double, std::milli>(Clock::now() - m_started).count()) +
           ",\n";
    out += "  \"steps\": [";
    for (size_t i = 0; i < m_records.size(); ++i) {
        const Record& r = m_records[i];
        out += std::string(i ? ",\n    " : "\n    ") + "{\"step\": " + std::to_string(i + 1) +
               ", \"kind\": " + quote(flowStepKindName(r.kind)) + ", \"target\": " + quote(r.target) +
               ", \"ok\": " + (r.ok ? "true" : "false") + ", \"ms\": " + number(r.ms) +
               ", \"frames\": " + std::to_string(r.frames) + ", \"message\": " + quote(r.message) + "}";
    }
    out += m_records.empty() ? "],\n" : "\n  ],\n";
    // Where things stood when it ended.
    out += "  \"now\": {";
    if (m_hooks.flow) {
        const GameFlow& flow = *m_hooks.flow;
        const uint64_t active = flow.activeTicket();
        const auto status = active ? flow.status(active) : std::nullopt;
        out += "\"level\": " + quote(flow.currentLevel().level) +
               ", \"epoch\": " + std::to_string(flow.currentEpoch()) +
               ", \"playing\": " + ((m_hooks.playing ? m_hooks.playing() : true) ? "true" : "false") +
               ", \"active_ticket\": " + std::to_string(active) +
               ", \"phase\": " + (status ? quote(travelPhaseName(status->phase)) : std::string("null"));
        if (m_hooks.shell) {
            out += ", \"screen\": " + quote(shellScreenName(m_hooks.shell->screen())) +
                   ", \"page\": " + quote(m_hooks.shell->page());
            if (const auto& f = m_hooks.shell->failure())
                out += ", \"error\": {\"code\": " + quote(travelErrorName(f->code)) +
                       ", \"level\": " + quote(f->request.level) + ", \"message\": " + quote(f->message) + "}";
        }
    }
    out += "},\n";
    out += "  \"travel\": [";
    for (size_t i = 0; i < m_completions.size(); ++i) {
        const TravelCompletion& c = m_completions[i];
        out += std::string(i ? ",\n    " : "\n    ") + "{\"ticket\": " + std::to_string(c.ticket) +
               ", \"level\": " + quote(c.request.level) + ", \"from\": " + quote(c.from) + ", \"to\": " + quote(c.to) +
               ", \"status\": " +
               quote(
                   c.status == TravelStatus::Succeeded   ? "succeeded"
                   : c.status == TravelStatus::Cancelled ? "cancelled"
                                                         : "failed") +
               ", \"code\": " + quote(travelErrorName(c.code)) + ", \"message\": " + quote(c.message) +
               ", \"ms\": " + number(c.durationMs) + "}";
    }
    out += m_completions.empty() ? "]\n" : "\n  ]\n";
    return out + "}\n";
}

}   // namespace shockwave

#include <GameUI/UIWorld.h>

#include <Engine/Components.h>

#include <algorithm>
#include <filesystem>

namespace shockwave {

UIWorld::UIWorld(World& world, GameUI* ui) : m_world(world), m_ui(ui) {
    if (m_ui) m_ui->setEventListener([this](const UIEvent& event) { dispatch(event); });
}

UIWorld::~UIWorld() {
    if (!m_ui) return;
    // Nothing done while playing sticks: handlers, values and changed elements go.
    m_ui->setEventListener({});
    m_ui->clearValues();
    m_ui->reloadAll();
}

Entity UIWorld::find(std::string_view document) const {
    if (document.empty()) return {};
    Entity byName, byPath, byStem;
    m_world.each<const UIDocument>([&](Entity e, const UIDocument& c) {
        const Name* name = m_world.get<Name>(e);
        if (name && name->value == document && !byName.valid()) byName = e;
        if (c.document == document && !byPath.valid()) byPath = e;
        if (std::filesystem::path(c.document).stem().string() == document && !byStem.valid()) byStem = e;
    });
    return byName.valid() ? byName : byPath.valid() ? byPath : byStem;
}

bool UIWorld::show(std::string_view document, bool visible) {
    const Entity e = find(document);
    UIDocument* c = e.valid() ? m_world.get<UIDocument>(e) : nullptr;
    if (!c) return false;
    c->visible = visible;
    return true;
}

bool UIWorld::toggle(std::string_view document) { return show(document, !visible(document)); }

bool UIWorld::visible(std::string_view document) const {
    const Entity e = find(document);
    const UIDocument* c = e.valid() ? m_world.get<UIDocument>(e) : nullptr;
    return c && c->visible;
}

// Element calls load the document first, so they work in beginPlay too.
#define SW_UI_ELEMENT(call)                                                                                            \
    const Entity e = find(document);                                                                                   \
    if (!m_ui || !e.valid() || !m_ui->loaded(m_world, e)) return false;                                                \
    return m_ui->call

bool UIWorld::setText(std::string_view document, std::string_view element, std::string_view text) {
    SW_UI_ELEMENT(setText(e, element, text));
}

std::string UIWorld::text(std::string_view document, std::string_view element) const {
    const Entity e = find(document);
    if (!m_ui || !e.valid() || !m_ui->loaded(m_world, e)) return {};
    return m_ui->text(e, element).value_or(std::string());
}

bool UIWorld::setRml(std::string_view document, std::string_view element, std::string_view rml) {
    SW_UI_ELEMENT(setRml(e, element, rml));
}

bool UIWorld::setClass(std::string_view document, std::string_view element, std::string_view name, bool on) {
    SW_UI_ELEMENT(setClass(e, element, name, on));
}

bool UIWorld::setAttribute(
    std::string_view document, std::string_view element, std::string_view name, std::string_view value) {
    SW_UI_ELEMENT(setAttribute(e, element, name, value));
}

bool UIWorld::setProperty(
    std::string_view document, std::string_view element, std::string_view name, std::string_view value) {
    SW_UI_ELEMENT(setProperty(e, element, name, value));
}

bool UIWorld::focus(std::string_view document, std::string_view element) { SW_UI_ELEMENT(focus(e, element)); }

bool UIWorld::click(std::string_view document, std::string_view element) { SW_UI_ELEMENT(click(e, element)); }

#undef SW_UI_ELEMENT

void UIWorld::set(std::string_view name, double value) {
    if (m_ui) m_ui->setValue(name, value);
}
void UIWorld::set(std::string_view name, bool value) {
    if (m_ui) m_ui->setValue(name, value);
}
void UIWorld::set(std::string_view name, std::string_view value) {
    if (m_ui) m_ui->setValue(name, value);
}
void UIWorld::set(std::string_view name, UIValue value) {
    if (m_ui) m_ui->setValue(name, std::move(value));
}
const UIValue& UIWorld::value(std::string_view name) const {
    static const UIValue unset;
    return m_ui ? m_ui->value(name) : unset;
}
std::string UIWorld::get(std::string_view name) const { return m_ui ? m_ui->valueText(name) : std::string(); }
double UIWorld::number(std::string_view name) const { return m_ui ? m_ui->valueNumber(name) : 0.0; }

void UIWorld::on(std::string_view command, Handler handler) {
    auto it = m_commands.find(command);
    if (it == m_commands.end()) it = m_commands.emplace(std::string(command), std::vector<Handler>{}).first;
    it->second.push_back(std::move(handler));
}

void UIWorld::onClick(std::string_view document, std::string_view element, Handler handler) {
    onEvent(document, element, "click", std::move(handler));
}

void UIWorld::onEvent(std::string_view document, std::string_view element, std::string_view type, Handler handler) {
    m_elementHandlers.push_back({std::string(document), std::string(element), std::string(type), std::move(handler)});
}

bool UIWorld::navigate(UINav nav) { return m_ui && m_ui->navigate(nav); }

std::string UIWorld::focused(std::string_view document) const {
    const auto focus = m_ui ? m_ui->focused() : std::nullopt;
    return focus && focus->first == find(document) ? focus->second : std::string();
}

bool UIWorld::capturesInput() const { return m_ui && (m_ui->wantsMouse() || m_ui->wantsKeyboard()); }

void UIWorld::dispatch(const UIEvent& event) {
    if (!event.command.empty()) {
        if (const auto it = m_commands.find(event.command); it != m_commands.end()) {
            const std::vector<Handler> handlers = it->second;   // a handler may add more
            for (const Handler& h : handlers) h(event);
        }
        return;
    }
    const size_t count = m_elementHandlers.size();
    for (size_t i = 0; i < count && i < m_elementHandlers.size(); ++i) {
        const ElementHandler& h = m_elementHandlers[i];
        if (h.type != event.type) continue;
        if (std::find(event.ids.begin(), event.ids.end(), h.element) == event.ids.end()) continue;
        if (!(find(h.document) == event.entity)) continue;
        const Handler handler = h.handler;   // a handler may add more, moving the vector
        handler(event);
    }
}

}   // namespace shockwave

#pragma once

// Reading documents' text without RmlUi: the {{names}} a document's data bindings use.

#include <string>
#include <string_view>
#include <vector>

namespace shockwave::ui {

// The root value names an .rml's data bindings refer to: {{score}}, data-if="health < 25",
// data-for="item : items" (items, not item). Approximate, but it never misses a plain
// {{name}}; a name it adds by mistake is only bound and left unset.
std::vector<std::string> referencedNames(std::string_view rml);

}   // namespace shockwave::ui

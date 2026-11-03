#pragma once

// A finding from a project check, for people and for tools. `code` never changes once
// released, so scripts and the editor can match it; the message may be reworded. Plain
// output keeps the familiar "file:line: message" shape (text()); JSON carries every field.

#include <span>
#include <string>
#include <string_view>

namespace shockwave {

enum class DiagnosticSeverity { Error, Warning };

struct Diagnostic {
    std::string code;   // stable, like "ReleaseMissingKey" (docs/ReleaseSchema.md lists them)
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string file;   // as the project names it ("Config/Release.swrelease"); may be empty
    int line = 0;   // 1-based; 0 when the finding isn't about one line
    std::string object;   // what it's about inside the file (a key, an entity ID); may be empty
    std::string message;

    bool error() const { return severity == DiagnosticSeverity::Error; }
    // "file:line: message", leaving out what's unknown.
    std::string text() const;
};

const char* diagnosticSeverityName(DiagnosticSeverity severity);   // "error", "warning"
bool hasErrors(std::span<const Diagnostic> diagnostics);

// A JSON string literal, quotes included, with control characters escaped.
std::string diagnosticJsonString(std::string_view text);
// One diagnostic as a JSON object: {"code": ..., "severity": ..., "file": ..., "line": ...,
// "object": ..., "message": ...}; an unknown file, line or object is null.
std::string diagnosticJson(const Diagnostic& diagnostic);

}   // namespace shockwave

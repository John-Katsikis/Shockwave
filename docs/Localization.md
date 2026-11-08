# Localization

The game's text in each language: one catalog per language, of stable keys and UTF-8
text, in `Content/Localization`.

```
Content/Localization/en.swstrings
  swstrings 1
  # The HUD.
  hud.objective_caption "OBJECTIVE {done} / {total}"
  hud.find_exit "Find the way out"

Content/Localization/fr.swstrings
  swstrings 1
  hud.objective_caption "OBJECTIF {done} / {total}"
```

- A **key** is lowercase letters, digits and `_`, in parts joined by `.`: `hud.find_exit`.
- A **value** is quoted UTF-8 (`\"` and `\\` inside). `{name}` is a placeholder that
  `format()` fills; `{{` and `}}` are literal braces.
- The **file name** is the locale: `en`, `fr`, `pt-BR`.
- The **default language** is the project's `default_language` key (`en` when left out).

## Which text shows

The player's Language preference ([Preferences](Preferences.md)) picks the locale; empty is
the default language. A key is looked up in order:

1. the locale (`pt-BR`)
2. its language (`pt`)
3. the default language
4. the engine's own English, for the shell's screens (`shell.*` keys)
5. a visible `[missing: hud.find_exit]`, so a gap is seen rather than silently blank

So a partial catalog works: what it leaves out shows in the default language. Reactor
Run's `fr.swstrings` is partial on purpose.

The language can change while the game runs (Settings > Language): documents update on the
next frame.

## In documents

Every document's data model has a `text` value with the catalog's keys:

```html
<span class="caption">{{text.hud.health}}</span>
```

Values are text, never markup: GameUI escapes them like any other value.

## In game code

```cpp
game.text("hud.find_exit");
game.format("hud.objective_caption", {{"done", "2"}, {"total", "4"}});
```

Format whole sentences, never join translated pieces: word order differs between
languages, so `"OBJECTIVE " + done + " / " + total` can't be translated and
`format("hud.objective_caption", ...)` can. A placeholder with no value stays as it is, so
the gap shows.

Without an engine (tests, tools) only the engine's own text is there: the game's keys
show as `[missing: key]`.

## The shell's text

The shell's screens use `shell.*` keys (`AppShell::builtinText()` lists them, in English).
A catalog translates them like the game's own: `shell.new_game "Nouvelle partie"` in
`fr.swstrings`.

## Checks

`player --check` reads every catalog and reports:

- a file that doesn't read, with the line;
- a key a locale has that neither the default language nor the engine has;
- a translation whose placeholders differ from the default's;
- a document's `{{text.key}}` that no catalog has;
- keys a locale hasn't translated yet, as a warning;
- text with characters no font draws: they'd show blank. The engine's Noto Sans has
  Latin, Greek and Cyrillic; for Chinese, Japanese, Korean, Arabic and the rest, put a
  font that has them (`.ttf`, `.otf`) anywhere in `Content`, and every document can use it.

### Does it fit

Translations run longer than English (German by a third or more) and some words don't
break. `GameUI::layoutProblems()` lists text that runs past its box, a box that runs out
of the one it's in, and text partly off the screen, in the documents shown. The shell's
screens are tested with it in English, a long pseudo-German and Greek letters, at 720p,
1080p, 1440p and 4K at 200%, and at Interface sizes 1.5 and 2. In the editor, Player
Settings previews a language and a size, and **Check layout** runs the same check on the
game's own documents ([Preferences](Preferences.md#in-the-editor)).

## Tests

`player_preferences` (keys, placeholders, the file, the fallback order, `format()`,
`check()`), `game_flow` (the shell in French, falling back to English; the shell's layout
in long and Greek text at every size above, and every character having a font),
`localization_glyphs` (`--check` failing on a Japanese catalog with no font for it),
`editor_player_settings` (the editor previewing French) and
`shipping_demo_flow` (Reactor Run switched to French on its Settings page, its HUD in
French, from the source tree and from a package).

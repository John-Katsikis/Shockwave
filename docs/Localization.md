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
- keys a locale hasn't translated yet, as a warning.

## Tests

`player_preferences` (keys, placeholders, the file, the fallback order, `format()`,
`check()`), `game_flow` (the shell in French, falling back to English) and
`shipping_demo_flow` (Reactor Run switched to French on its Settings page, its HUD in
French, from the source tree and from a package).

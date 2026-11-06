# RCK — the RockOS scripting language (v1)

RCK is the UI/event scripting language of RockOS (`github.com/SquigglyCat-tech/RockOS`).
It describes windows, positioned widgets and click actions in a forgiving,
flow-style grammar. The canonical example is `Credits.rck` at the repo root:

```
window(
txt.positioning = middle;
 txt(still being built);
  btn.positioning = middle_bottom
  wrp.btn1{
   btn.txt (exit)
    l
   on click
    on btn1 do "exit_process"
     then wait
    until done
    )
```

That file parses exactly as written — including the stray `)` and the free
floating words (`l`, `then wait`, `until done`) — because RCK v1 treats
unknown words as documentation-flow material and only really interprets a
small core.

## Grammar (v1 semantics)

A script is a stream of statements; whitespace/newlines are not significant.

```
script    -> statement*
statement -> window_head | on_stmt | attr_stmt | wrapper_stmt
            | widget_stmt | stray_words
window_head -> "window" "("                     (prelude; optional)
attr_stmt   -> class_name "." attr_name "=" value [";"]
wrapper_stmt-> wrapper_name "." id "{" statement* "}"
widget_stmt -> class_name ["." attr] "(" text ")"
on_stmt     -> "on" event                       ("click" = click stream)
             event_name ["do"] action_string
             ["then" "wait" ... "until" "done"]
```

- `class_name`: `txt` | `txt.text` | `btn` | `button` | `wrp` (wrapper).
- `attr`: `positioning` with values `middle`, `bottom`, `middle_bottom`.
- A widget's positional default comes from the most recent class-level
  `class_name.positioning = value;` immediately before it is created.
- `wrp.NAME{...}` names the group's widgets so `on NAME do "..."` targets
  them by name.
- Quoted strings: actions (`on btn1 do "exit_process"`) and quoted
  widget text both work.
- Unknown/solo words are skipped and cannot change state.

## Runtime v1

Engine: `src/kernel/rck.c`, header `include/rck.h`:

```c
bool rck_run_source(const char* source);  // parse + render + dispatch
bool rck_run_path(const char* path);      // read via filesystem, then run
void rck_register_action(const char* name, rck_action_fn fn);
bool rck_self_test(void);                 // parses Credits' canonical sample
```

- Rendering draws a centered credits panel using display primitives
  (the engine produces text/buttons from the parsed tree).
- Actions ("then wait until done") run synchronously at the end of a
  script (v1), through the registry; `exit_process` is a built-in no-op.
- Interactivity: in v1 events dispatch at the end of a run; live mouse
  click routing lands in v2 alongside the desktop integration.
- Hard limits: 96 parse nodes, 8 KiB source per file, no heap — all
  engine storage is static, matching the rest of RockOS.

## Roadmap

- v2: input loop hooks into desktop.c and windowing integration
- v3: user actions with variables and expressions

/*
 * rck.c - RCK (v1) engine for RockOS.
 *
 * RCK is the UI/event scripting language of RockOS. Credits.rck is the
 * canonical example of what a script looks like:
 *
 *   window(
 *   txt.positioning = middle;
 *    txt(still being built);
 *     btn.positioning = middle_bottom
 *     wrp.btn1{
 *      btn.txt (exit)
 *       l
 *      on click
 *       on btn1 do "exit_process"
 *        then wait
 *       until done
 *       )
 *
 * The grammar is intentionally forgiving (see docs/RCK.md): the example
 * above contains a stray `)` and event clauses spread across bare words.
 * This engine parses exactly that shape with no heap: a fixed node pool,
 * stack tokenizer buffers, and a synchronous action registry.
 */

#include "rck.h"

#include "filesystem.h"
#include "display.h"

#include <stddef.h>
#include <stdint.h>

/* ================================================================== */
/* Fixed storage (no malloc)                                           */
/* ================================================================== */

#define RCK_NULL            0xFFFFu
#define RCK_MAX_NODES       96
#define RCK_MAX_NAME        15
#define RCK_MAX_TEXT        79
#define RCK_MAX_ACTIONS     16
#define RCK_MAX_SOURCE      8192

#define RCK_POS_UNSPEC          0
#define RCK_POS_MIDDLE          1
#define RCK_POS_BOTTOM          2
#define RCK_POS_MIDDLE_BOTTOM   (RCK_POS_MIDDLE | RCK_POS_BOTTOM)

typedef enum {
    RCK_WINDOW = 1,
    RCK_TXT,
    RCK_BTN,
    RCK_WRAPPER,
    RCK_EVENT,
} rck_kind_t;

typedef struct {
    uint16_t kind;
    uint16_t first_child;
    uint16_t next_sibling;
    char     name[RCK_MAX_NAME + 1];
    char     text[RCK_MAX_TEXT + 1];
    uint8_t  positioning;
    bool     has_action;
} rck_node_t;

typedef void (*rck_action_fn)(void);

typedef struct {
    char          name[RCK_MAX_NAME + 1];
    rck_action_fn fn;
} rck_action_t;

static rck_node_t   g_nodes[RCK_MAX_NODES];
static uint16_t     g_node_count;
static rck_action_t g_actions[RCK_MAX_ACTIONS];
static uint16_t     g_action_count;
static uint32_t     rck_action_dispatches;

/* Default class-property values, set by `txt.positioning = middle;`. */
#define RCK_CLASS_COUNT 4                /* window, txt, btn, wrapper   */
static uint8_t g_class_positioning[RCK_CLASS_COUNT];

/* ================================================================== */
/* Tiny string helpers (freestanding)                                  */
/* ================================================================== */

static size_t rck_len(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool rck_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void rck_copy(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool rck_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}

/* ================================================================== */
/* Tokenizer                                                           */
/* ================================================================== */

typedef enum {
    T_END, T_IDENT, T_STRING, T_LPAREN, T_RPAREN, T_LBRACE,
    T_RBRACE, T_DOT, T_ASSIGN, T_SEMI,
} rck_token_kind_t;

typedef struct {
    rck_token_kind_t kind;
    char text[RCK_MAX_NAME + 1];
} rck_token_t;

typedef struct {
    const char* source;
    size_t position;
} rck_lexer_t;

static rck_token_t rck_token_clear(void) {
    rck_token_t token;
    token.kind = T_END;
    token.text[0] = '\0';
    return token;
}

static rck_token_t rck_next(rck_lexer_t* lx) {
    rck_token_t token = rck_token_clear();
    size_t i = 0;
    char c;

    for (;;) {
        c = lx->source[lx->position];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->position++;
            continue;
        }
        break;
    }
    c = lx->source[lx->position];
    if (c == '\0') { token.kind = T_END; return token; }
    if (rck_ident_char(c)) {
        while (rck_ident_char(lx->source[lx->position]) &&
               i + 1 < sizeof(token.text)) {
            token.text[i++] = lx->source[lx->position++];
        }
        token.text[i] = '\0';
        token.kind = T_IDENT;
        return token;
    }
    lx->position++;
    switch (c) {
        case '(': token.kind = T_LPAREN;  return token;
        case ')': token.kind = T_RPAREN;  return token;
        case '{': token.kind = T_LBRACE;  return token;
        case '}': token.kind = T_RBRACE;  return token;
        case '.': token.kind = T_DOT;     return token;
        case '=': token.kind = T_ASSIGN;  return token;
        case ';': token.kind = T_SEMI;    return token;
        case '"': {
            while (lx->source[lx->position] != '"' &&
                   lx->source[lx->position] != '\0' &&
                   i + 1 < sizeof(token.text)) {
                token.text[i++] = lx->source[lx->position++];
            }
            if (lx->source[lx->position] == '"') lx->position++;
            token.text[i] = '\0';
            token.kind = T_STRING;
            return token;
        }
        default:
            /* Unknown punctuation: skip and report a soft stop so the
             * parser stays as lenient as the canonical example. */
            token.kind = T_SEMI;
            return token;
    }
}

/* ================================================================== */
/* Parser                                                              */
/* ================================================================== */

typedef struct {
    rck_lexer_t lexer;
    rck_token_t token;
    uint16_t    last_widget;
    uint16_t    last_wrapper;
    const char* error;
} rck_parser_t;

static uint8_t positioning_from_text(const char* value) {
    if (rck_streq(value, "middle")) return RCK_POS_MIDDLE;
    if (rck_streq(value, "bottom")) return RCK_POS_BOTTOM;
    if (rck_streq(value, "middle_bottom")) return RCK_POS_MIDDLE_BOTTOM;
    return RCK_POS_UNSPEC;
}

static uint8_t class_index_from_name(const char* class_name) {
    if (rck_streq(class_name, "btn") || rck_streq(class_name, "button")) {
        return 2;
    }
    if (rck_streq(class_name, "wrapper") || rck_streq(class_name, "wrp")) {
        return 3;
    }
    return 1;                                  /* txt/text default */
}

static rck_kind_t widget_kind_from_class(const char* class_name) {
    if (rck_streq(class_name, "btn") || rck_streq(class_name, "button")) {
        return RCK_BTN;
    }
    return RCK_TXT;
}

/* Set a class default and apply it retroactively to the most recent
 * widget of that class (txt.positioning = middle;). */
static void set_class_positioning(const char* class_name, const char* value) {
    uint8_t index = class_index_from_name(class_name);
    g_class_positioning[index] = positioning_from_text(value);
    for (uint16_t i = g_node_count; i > 0; i--) {
        rck_node_t* n = &g_nodes[i - 1];
        if (index == 1 && n->kind == RCK_TXT) {
            n->positioning = g_class_positioning[index];
            return;
        }
        if (index == 2 && n->kind == RCK_BTN) {
            n->positioning = g_class_positioning[index];
            return;
        }
    }
}

static rck_node_t* node_new(rck_kind_t kind) {
    rck_node_t* n;
    if (g_node_count >= RCK_MAX_NODES) return 0;
    n = &g_nodes[g_node_count];
    n->kind = kind;
    n->first_child = RCK_NULL;
    n->next_sibling = RCK_NULL;
    n->name[0] = '\0';
    n->text[0] = '\0';
    n->positioning = RCK_POS_UNSPEC;
    n->has_action = false;
    g_node_count++;
    return n;
}

static rck_node_t* node_at(uint16_t index) {
    if (index == RCK_NULL || index >= g_node_count) return 0;
    return &g_nodes[index];
}

static void node_append(uint16_t parent, uint16_t child) {
    rck_node_t* p = node_at(parent);
    if (!p) return;
    if (p->first_child == RCK_NULL) {
        p->first_child = child;
        return;
    }
    uint16_t c = p->first_child;
    while (g_nodes[c].next_sibling != RCK_NULL) c = g_nodes[c].next_sibling;
    g_nodes[c].next_sibling = child;
}

/* Skip a run of bare words (stray clauses like `l`, `then wait`,
 * `until done`; RCK v1 treats those as documentation-flow words). */
static void parse_eat_words(rck_parser_t* p) {
    for (;;) {
        if (p->token.kind == T_SEMI) {
            p->token = rck_next(&p->lexer);
            continue;
        }
        if (p->token.kind == T_IDENT && !rck_streq(p->token.text, "on")) {
            p->token = rck_next(&p->lexer);
            continue;
        }
        break;
    }
}

/* txt(...) / txt("...") / btn.txt (exit): widget with inline content.
 * All widgets attach flat to the window node; a wrapper id (from
 * `wrp.btn1{...}`) names the widget so `on btn1 do "x"` targets it. */
static void parse_widget(rck_parser_t* p, const char* class_name,
    const char* inline_attr) {
    char text[RCK_MAX_TEXT + 1];
    size_t wrote = 0;

    rck_node_t* widget = node_new(widget_kind_from_class(class_name));
    if (!widget) { p->error = "rck: node pool exhausted"; return; }

    const rck_node_t* wrapper = node_at(p->last_wrapper);
    if (wrapper && widget->name[0] == '\0') {
        rck_copy(widget->name, wrapper->name, sizeof(widget->name));
    }
    if (rck_streq(class_name, "txt") || rck_streq(class_name, "text")) {
        widget->positioning = g_class_positioning[1];
    } else {
        widget->positioning = g_class_positioning[2];
    }
    if (inline_attr && rck_streq(inline_attr, "txt")) {
        widget->positioning = g_class_positioning[2];
    }

    node_append(0, (uint16_t)(widget - g_nodes));
    p->last_widget = (uint16_t)(widget - g_nodes);

    if (p->token.kind != T_LPAREN) {
        parse_eat_words(p);
        return;
    }
    for (;;) {
        p->token = rck_next(&p->lexer);
        if (p->token.kind == T_RPAREN || p->token.kind == T_END) break;
        if (p->token.kind == T_IDENT) {
            if (wrote && wrote + 1 < sizeof(text)) text[wrote++] = ' ';
            size_t i = 0;
            while (p->token.text[i] && wrote + 1 < sizeof(text)) {
                text[wrote++] = p->token.text[i++];
            }
        } else if (p->token.kind == T_STRING) {
            size_t i = 0;
            while (p->token.text[i] && wrote + 1 < sizeof(text)) {
                text[wrote++] = p->token.text[i++];
            }
        }
    }
    text[wrote] = '\0';
    rck_copy(widget->text, text, sizeof(widget->text));
    /* The closing ')' was consumed as a token: swap it for the next
     * token so the statement loop does not mistake it for a stray
     * window close and abort parsing. */
    if (p->token.kind == T_RPAREN) {
        p->token = rck_next(&p->lexer);
    }
}

/* on click / on btn1 do "exit_process" then wait until done */
static void parse_event(rck_parser_t* p) {
    rck_node_t* event = node_new(RCK_EVENT);
    if (!event) { p->error = "rck: node pool exhausted"; return; }

    p->token = rck_next(&p->lexer);
    if (p->token.kind == T_IDENT) {
        rck_copy(event->name, p->token.text, sizeof(event->name));
        p->token = rck_next(&p->lexer);
    }
    for (;;) {                                  /* collect the clause    */
        if (p->token.kind == T_END || p->token.kind == T_RBRACE ||
            p->token.kind == T_LPAREN) {
            break;
        }
        if (p->token.kind == T_IDENT) {
            p->token = rck_next(&p->lexer);
            continue;
        }
        if (p->token.kind == T_STRING) {
            rck_copy(event->text, p->token.text, sizeof(event->text));
            event->has_action = true;
            p->token = rck_next(&p->lexer);
            continue;
        }
        break;
    }
    parse_eat_words(p);
    node_append(0, (uint16_t)(event - g_nodes));
}

static void parse_statements(rck_parser_t* p, bool inside_window) {
    char first_ident[RCK_MAX_NAME + 1];

    for (;;) {
        if (p->token.kind == T_END) return;
        if (p->token.kind == T_RPAREN) {
            p->token = rck_next(&p->lexer);    /* stray close tolerated */
            if (inside_window) return;
            continue;
        }
        if (p->token.kind == T_RBRACE) {
            p->token = rck_next(&p->lexer);
            p->last_wrapper = RCK_NULL;        /* pop the named wrapper */
            continue;
        }
        if (p->token.kind != T_IDENT) {
            p->token = rck_next(&p->lexer);
            continue;
        }
        if (rck_streq(p->token.text, "window")) {
            p->token = rck_next(&p->lexer);
            if (p->token.kind == T_LPAREN) p->token = rck_next(&p->lexer);
            continue;
        }
        if (rck_streq(p->token.text, "on")) {
            parse_event(p);
            continue;
        }

        rck_copy(first_ident, p->token.text, sizeof(first_ident));
        p->token = rck_next(&p->lexer);

        if (p->token.kind != T_DOT) {
            if (p->token.kind == T_LPAREN) {
                parse_widget(p, first_ident, 0);
                continue;
            }
            parse_eat_words(p);
            continue;
        }

        p->token = rck_next(&p->lexer);
        char attr[RCK_MAX_NAME + 1];
        attr[0] = '\0';
        if (p->token.kind == T_IDENT) {
            rck_copy(attr, p->token.text, sizeof(attr));
            p->token = rck_next(&p->lexer);
        }

        if (p->token.kind == T_LBRACE) {
            /* wrp.btn1{ ... }: a named wrapper gives widgets an id. */
            rck_node_t* wrapper = node_new(RCK_WRAPPER);
            if (!wrapper) { p->error = "rck: node pool exhausted"; return; }
            rck_copy(wrapper->name, attr, sizeof(wrapper->name));
            node_append(0, (uint16_t)(wrapper - g_nodes));
            p->last_wrapper = (uint16_t)(wrapper - g_nodes);
            p->token = rck_next(&p->lexer);
            continue;
        }
        if (p->token.kind == T_LPAREN) {
            /* btn.txt (exit): inline label attribute + content. */
            parse_widget(p, first_ident, attr);
            continue;
        }
        if (p->token.kind == T_ASSIGN) {
            p->token = rck_next(&p->lexer);
            char value[RCK_MAX_NAME + 1];
            value[0] = '\0';
            if (p->token.kind == T_IDENT || p->token.kind == T_STRING) {
                rck_copy(value, p->token.text, sizeof(value));
                p->token = rck_next(&p->lexer);
            }
            if (rck_streq(attr, "positioning")) {
                set_class_positioning(first_ident, value);
            }
            if (p->token.kind == T_SEMI) p->token = rck_next(&p->lexer);
            continue;
        }
        parse_eat_words(p);
    }
}

/* ================================================================== */
/* Evaluation / rendering                                              */
/* ================================================================== */

/* One RCK "virtual screen slot": a text block or a button on the panel. */
static void render_slot_text(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background) {
    display_fill_rect(x, y,
        (uint32_t)(rck_len(text) * 5U) + 16U, 18U, background);
    display_draw_text(x + 8U, y + 3U, text, foreground, background);
}

static void run_events(void) {
    for (uint16_t i = 0; i < g_node_count; i++) {
        rck_node_t* n = &g_nodes[i];
        if (n->kind != RCK_EVENT || !n->has_action) continue;
        for (uint16_t a = 0; a < g_action_count; a++) {
            if (rck_streq(g_actions[a].name, n->text)) {
                /* "then wait until done": v1 actions run synchronously. */
                g_actions[a].fn();
                rck_action_dispatches++;
            }
        }
    }
}

static void render_window(uint32_t screen_w, uint32_t screen_h) {
    const uint32_t W = 264U, H = 120U;
    uint32_t x0 = screen_w > W ? (screen_w - W) / 2U : 0U;
    uint32_t y0 = screen_h > H ? (screen_h - H) / 2U : 0U;

    display_fill_rect(x0, y0, W, H, COLOR_DARK_GRAY);
    display_fill_rect(x0 + 2U, y0 + 2U, W - 4U, H - 4U, COLOR_BLUE);

    for (uint16_t i = 0; i < g_node_count; i++) {
        rck_node_t* n = &g_nodes[i];
        if (n->kind != RCK_TXT && n->kind != RCK_BTN) continue;
        if (n->text[0] == '\0') continue;
        uint8_t pos = n->positioning;
        if (n->kind == RCK_TXT) {
            if (pos == RCK_POS_UNSPEC) pos = RCK_POS_MIDDLE;
            if (pos & RCK_POS_MIDDLE) {
                render_slot_text(x0 + 8U, y0 + H / 2U - 9U, n->text,
                    COLOR_WHITE, COLOR_BLUE);
            } else if (pos & RCK_POS_BOTTOM) {
                render_slot_text(x0 + 8U, y0 + H - 34U, n->text,
                    COLOR_WHITE, COLOR_BLUE);
            } else {
                render_slot_text(x0 + 8U, y0 + 8U, n->text,
                    COLOR_WHITE, COLOR_BLUE);
            }
        } else {
            if (pos == RCK_POS_UNSPEC) pos = RCK_POS_MIDDLE_BOTTOM;
            if (pos == RCK_POS_MIDDLE_BOTTOM) {
                display_fill_rect(x0 + W / 2U - 32U,
                    y0 + H - 34U, 64U, 24U, COLOR_LIGHT_GRAY);
                display_draw_text(x0 + W / 2U - 16U,
                    y0 + H - 34U + 6U, n->text, COLOR_BLACK,
                    COLOR_LIGHT_GRAY);
            }
        }
    }
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

static bool g_defaults_registered;

/* Built-in registry: hosts may override by registering their own. */
static void rck_default_exit_process(void) {
    /* v1: the runner returns right away; the caller redraws its UI. */
}

static void rck_register_defaults(void) {
    if (g_defaults_registered) return;
    g_defaults_registered = true;
    rck_register_action("exit_process", rck_default_exit_process);
}

bool rck_run_source(const char* source) {
    rck_parser_t parser;
    rck_node_t* window;

    if (!source) return false;

    rck_register_defaults();
    g_node_count = 0;
    rck_action_dispatches = 0;
    for (uint16_t i = 0; i < RCK_CLASS_COUNT; i++) {
        g_class_positioning[i] = RCK_POS_UNSPEC;
    }

    parser.lexer.source = source;
    parser.lexer.position = 0;
    parser.token = rck_next(&parser.lexer);
    parser.last_widget = RCK_NULL;
    parser.last_wrapper = RCK_NULL;
    parser.error = 0;

    window = node_new(RCK_WINDOW);
    if (!window) return false;

    parse_statements(&parser, true);

    if (parser.error) {
        display_puts(parser.error);
        display_putchar('\n');
        return false;
    }

    uint32_t w = 0, h = 0;
    (void)display_get_framebuffer_size(&w, &h);
    render_window(w ? w : 640U, h ? h : 480U);
    display_present();

    run_events();          /* v1: all bound events dispatch at script end */
    return g_node_count != 1;
}

bool rck_run_path(const char* path) {
    static char rck_source_buffer[RCK_MAX_SOURCE];
    uint32_t bytes_read = 0;
    int r = filesystem_read(path, rck_source_buffer,
        (uint32_t)sizeof(rck_source_buffer) - 1U, &bytes_read);
    if (r != FS_OK) {
        display_puts("rck: cannot read path (is the fs mounted?)\n");
        return false;
    }
    rck_source_buffer[bytes_read] = '\0';
    return rck_run_source(rck_source_buffer);
}

void rck_register_action(const char* name, rck_action_fn fn) {
    if (!name || !fn || !*name || g_action_count >= RCK_MAX_ACTIONS) return;
    rck_copy(g_actions[g_action_count].name, name,
        sizeof(g_actions[g_action_count].name));
    g_actions[g_action_count].fn = fn;
    g_action_count++;
}

/* ================================================================== */
/* Self-test: parse the exact Credits.rck canonical sample             */
/* ================================================================== */


static const char* RCK_SELF_TEST_SOURCE =
    "window(\n"
    "txt.positioning = middle;\n"
    " txt(still being built);\n"
    "  btn.positioning = middle_bottom\n"
    "  wrp.btn1{\n"
    "   btn.txt (exit)\n"
    "    l\n"
    "   on click\n"
    "    on btn1 do \"exit_process\"\n"
    "     then wait\n"
    "    until done\n"
    "    )\n";

static void self_test_action(void) { /* observable via engine count */ }

bool rck_self_test(void) {
    bool ok = true;
    rck_node_t* found_txt = 0;
    rck_node_t* found_btn = 0;
    rck_node_t* found_event = 0;
    rck_action_t saved[RCK_MAX_ACTIONS];
    uint16_t saved_count = g_action_count;
    rck_register_defaults();           /* so run_source is a no-op here */
    for (uint16_t i = 0; i < saved_count; i++) saved[i] = g_actions[i];
    for (uint16_t i = 0; i < saved_count; i++) saved[i] = g_actions[i];
    g_action_count = 0;
    rck_action_dispatches = 0;
    rck_register_action("exit_process", self_test_action);

    rck_run_source(RCK_SELF_TEST_SOURCE);

    for (uint16_t i = 0; i < g_node_count; i++) {
        rck_node_t* n = &g_nodes[i];
        if (n->kind == RCK_TXT && rck_streq(n->text, "still being built")) {
            found_txt = n;
        }
        if (n->kind == RCK_BTN && rck_streq(n->text, "exit")) {
            found_btn = n;
        }
        if (n->kind == RCK_EVENT && n->has_action &&
            rck_streq(n->text, "exit_process")) {
            found_event = n;
        }
    }
    if (!found_txt) { ok = false; display_puts("[txt]"); }
    if (!found_btn) { ok = false; display_puts("[btn]"); }
    if (!found_event) { ok = false; display_puts("[event]"); }
    if (found_btn && found_btn->positioning != RCK_POS_MIDDLE_BOTTOM) { ok = false; display_puts("[btnpos]"); }
    if (found_txt && found_txt->positioning != RCK_POS_MIDDLE) { ok = false; display_puts("[txtpos]"); }
    if (rck_action_dispatches != 1) {
        ok = false;
        display_puts("[disp]");
    }
    if (node_at(g_nodes[0].first_child) == 0) { ok = false; display_puts("[kids]"); }

    g_action_count = saved_count;
    for (uint16_t i = 0; i < saved_count; i++) g_actions[i] = saved[i];

    if (!ok) {
        display_puts("rck: text=");
        display_puts(found_txt ? found_txt->text : "-");
        display_puts(" btn=");
        display_puts(found_btn ? found_btn->text : "-");
        display_puts(" ev=");
        display_puts(found_event ? found_event->text : "-");
        display_puts(" pos=");
        static const char posnames[4][8] = { "uns", "mid", "bot", "midb" };
        display_puts(found_txt ? posnames[found_txt->positioning & 3] : "-");
        display_puts("/");
        display_puts(found_btn ? posnames[found_btn->positioning & 3] : "-");
        display_puts(" disp=");
        display_puts(rck_action_dispatches ? "1" : "0");
        display_putchar('\n');
    }
    display_puts(ok ? "rck self-test: PASS\n" : "rck self-test: FAIL\n");
    return ok;
}

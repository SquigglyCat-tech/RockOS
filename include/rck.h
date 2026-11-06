#ifndef ROCKOS_RCK_H
#define ROCKOS_RCK_H

/* RCK - the RockOS UI/event scripting language (v1).
 * See docs/RCK.md for the grammar. Credits.rck is the canonical example. */

#include <stdbool.h>
#include <stddef.h>

/* Parse + execute an RCK source string. Renders the described window with
 * display primitives and dispatches bound right away ("then wait until
 * done" actions are synchronous in v1). Returns false on parse error,
 * with a diagnostic printed through display_puts. */
bool rck_run_source(const char* source);

/* Read a .rck script from the mounted filesystem and run it. */
bool rck_run_path(const char* path);

/* Register a builtin for `on <widget> do "<name>"`. */
typedef void (*rck_action_fn)(void);
void rck_register_action(const char* name, rck_action_fn fn);

/* Self-test: parses the canonical Credits.rck sample, checks the widget
 * tree, event binding and action dispatch, results via display_puts. */
bool rck_self_test(void);

#endif // ROCKOS_RCK_H

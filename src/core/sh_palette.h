// The accent colour: from any seed colour to the roles the interface uses, with every pair meeting its contrast target. Pure
// functions, compiled into the window too. Neutrals and the semantic amber, lime and red never change.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char action[8], action_ink[8];  // the primary button
    char mark[8];                   // ports, selection marks, lit wires: 3:1 on surfaces
    char mark_text[8];              // the accent as text: 4.5:1 on surfaces and on mark_low
    char mark_low[8];               // a quiet fill
    char focus[8];
    char selected[8];               // the selected row
    char ink_ok[8];                 // the accent on the inverted surface of toasts
    const char *clash;              // "", or the semantic colour the seed sits close to: "amber", "red", "lime"
} sh_palette;

bool sh_palette_derive(const char *seed, bool dark, sh_palette *out);  // seed: #RRGGBB
double sh_palette_contrast(const char *a, const char *b);              // WCAG ratio of two #RRGGBB colours
bool sh_oklch_from_hex(const char *hex, double *l, double *c, double *h);
void sh_oklch_to_hex(double l, double c, double h, char out[8]);       // out of gamut: chroma comes down until it fits

#ifdef __cplusplus
}
#endif

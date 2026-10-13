#ifndef INJECT_H
#define INJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "fb.h"
#include "wm.h"

/* Scripted input for headless runs and tests (cwfa --inject). A script is a list of steps separated by ';':
 *
 *   wait:N                 do nothing for N frames
 *   waitms:N               do nothing until N milliseconds of the program's clock have passed (speech, animations)
 *   key:SPEC               press and release a key; SPEC is [ctrl+|alt+|shift+]NAME, NAME a letter, a digit,
 *                          F1..F12, RETURN, ESCAPE, TAB, SPACE, BACK, DELETE, INSERT, HOME, END, PRIOR, NEXT,
 *                          LEFT, UP, RIGHT, DOWN
 *   keydown:SPEC keyup:SPEC
 *   type:TEXT              the keys of each character of TEXT (letters, digits, space and common punctuation)
 *   move:X,Y click:X,Y dblclick:X,Y rclick:X,Y down:X,Y up:X,Y      screen coordinates, left button unless rclick
 *   quit                   the window's close request
 *   stop                   end the run (cwfa stops at the next frame)
 *   shot:PATH              write the screen as a PPM; does not use up a frame
 *
 * Every other step is delivered on its own frame (inject_run is called once per idle step of the host), so the
 * program has a chance to react between steps. */

typedef struct inject inject_t;

/* NULL with a message in err when the script cannot be parsed. */
inject_t *inject_parse(const char *spec, char *err, size_t errcap);
/* Delivers the next step to the window manager. False when the script is finished. */
bool inject_run(inject_t *in, wm_t *w, const fb_t *fb);
bool inject_done(const inject_t *in);
/* True once a stop step was reached: the host ends the run. */
bool inject_stopped(const inject_t *in);
size_t inject_steps_left(const inject_t *in);
void inject_free(inject_t *in);

#endif

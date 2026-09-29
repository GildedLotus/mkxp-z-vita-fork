// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * erroroverlay.h -- the one on-screen panel for script errors and msgbox
 * text.
 *
 * There is no OS message box on a Vita: SDL's Vita backend has no
 * ShowMessageBox implementation, so every "Unable to load scripts from ...",
 * every `print` and every unrescued exception used to leave the player
 * looking at the last frame the game drew, or at nothing. The fatal-report path now puts all
 * of it in the log and in last-error.txt; this is the half a person holding
 * the device can read.
 *
 * ONE ENTRY POINT, ONE THREAD
 * ---------------------------
 * vitaShowPanel() lays the text out and rasterises it on the CPU into a
 * single 960x544 RGBA image (vita/textpanel + vita/swraster), uploads that
 * image to ONE texture, and draws it over the finished frame with the shader
 * program, blend state and scratch quad boot already warmed -- then waits for
 * a pad button and gives every byte of it back. The GPU budget is why
 * it is shaped that way: after GPUBudget::seal() a new render surface, shader
 * program or VAO can fail hard, and a texture is the one object
 * class that still works.
 *
 * It must be called from the RGSS thread and from nowhere else. That thread
 * owns the GL context (main.cpp creates it inside rgssThreadFun under
 * -DMKXPZ_INIT_GL_LATER), and it is the thread the bindings raise on.
 *
 * It never throws -- not a Ruby exception, not a C++ one -- and it never
 * fails twice: anything that does not work is one `error-overlay: skipped
 * (...)` line in the log and a normal return, because the caller is already
 * reporting a failure and a panel that raises inside a rescue handler would
 * take the report with it.
 *
 * Off a Vita, or in a build without the CPU-authoritative Bitmap backend,
 * this is an empty function: desktop mkxp-z keeps its real message box.
 */

#ifndef ERROROVERLAY_H
#define ERROROVERLAY_H

#include <string>

/*
 * Draw `heading` over `body` and block until the player presses a pad button.
 *
 * `fatal` selects the footer ("quit" vs "continue") and is reported in the
 * log; it changes nothing else -- the caller decides what happens next.
 * `heading` may be NULL. `body` may be anything at all, including invalid
 * UTF-8, Shift_JIS bytes, embedded NULs and megabytes of backtrace: it is
 * sanitised and bounded here (textpanel::sanitizeUtf8).
 */
void vitaShowPanel(const char *heading, const std::string &body, bool fatal);

#endif /* ERROROVERLAY_H */

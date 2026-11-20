// diag: on-device network diagnostics. Runs low-level socket tests against
// one speaker IP and reports raw results (return codes, errno, poll flags),
// so platform quirks in the 3DS socket service can be seen instead of guessed.
#pragma once

typedef void (*diag_log_fn)(const char *line);

// Runs on the worker thread (blocking, ~10 s worst case).
void diag_run(const char *ip, diag_log_fn log);

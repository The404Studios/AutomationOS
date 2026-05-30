#ifndef SH_GIT_H
#define SH_GIT_H

/*
 * Native git-like VCS for the terminal shell.
 *
 * git_run() executes a single `git` subcommand and prints all output via the
 * supplied callback (the terminal passes its own grid printer). It is fully
 * self-contained: it talks to the kernel only through syscalls and uses its own
 * string/file helpers (no libc, no malloc).
 *
 *   argline : text after "git " with leading spaces trimmed
 *             (e.g. "commit -m hello", "add foo.txt", "log"); never NULL, may be "".
 *   cwd     : absolute current working directory of the shell
 *             (e.g. "/", "/tmp"); never NULL. The repo lives at <cwd>/.git.
 *   out     : print callback. Call with NUL-terminated strings; embed '\n'
 *             yourself for line breaks. Never NULL.
 *
 * Returns 0 on success, non-zero on error (a human-readable message is printed
 * through `out` before returning).
 */
int git_run(const char* argline, const char* cwd, void (*out)(const char*));

#endif /* SH_GIT_H */

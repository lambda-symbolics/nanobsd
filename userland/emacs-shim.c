/* emacs-shim: re-exec wrapper installed as /usr/local/bin/emacs.
 *
 * On NetBSD, GNU Emacs starts with a corrupted runtime (e.g.
 * file-name-sans-extension appears void at the first idle timer) when it is
 * the *direct* posix_spawn(3) child of a threaded process such as the cclsh
 * Lisp shell.  Interposing a single ordinary execve(2) launders the process
 * state and Emacs runs correctly.  We also restore the job-control signal
 * dispositions to their defaults, which a foreground program expects.
 *
 * Under Wayland (WAYLAND_DISPLAY set) it runs the pure-GTK build in
 * /usr/local/emacs-pgtk, which draws natively there; under X the pkgsrc
 * X11 build.  EMACS_X11=1 forces the X11 build.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define X11_EMACS "/usr/pkg/bin/emacs"
#define PGTK_EMACS "/usr/local/emacs-pgtk/bin/emacs"

int main(int argc, char **argv)
{
    const char *emacs = X11_EMACS;

    (void)argc;
    if (getenv("WAYLAND_DISPLAY") != NULL && getenv("EMACS_X11") == NULL &&
        access(PGTK_EMACS, X_OK) == 0)
        emacs = PGTK_EMACS;
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    argv[0] = (char *)emacs;
    execv(emacs, argv);
    _exit(127);
}

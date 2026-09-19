;;;; Local cclsh startup file (NetBSD, SBCL-hosted cclsh)

(in-package #:cclsh-user)

(defun startup--set-default (name value)
  "Set environment variable NAME to VALUE when it is empty or absent."
  (let ((current (getenv name)))
    (unless (and current (plusp (length current)))
      (setenv name value))))

(defun startup--prepend-path (&rest directories)
  "Prepend DIRECTORIES to PATH while retaining the first occurrence."
  (let* ((current
           (uiop:split-string (or (getenv "PATH") "")
                              :separator '(#\:)))
         (paths
           (remove-duplicates
            (remove "" (append directories current) :test #'string=)
            :test     #'string=
            :from-end t)))
    (setenv "PATH" (format nil "~{~a~^:~}" paths))))

(let ((home
        (string-right-trim
         "/" (or (getenv "HOME")
                  (namestring (user-homedir-pathname))))))
  (startup--set-default "XDG_CONFIG_HOME"
                        (format nil "~a/.config" home))
  (startup--set-default "XDG_DATA_HOME"
                        (format nil "~a/.local/share" home))
  (startup--set-default "XDG_STATE_HOME"
                        (format nil "~a/.local/state" home))
  (startup--set-default "XDG_CACHE_HOME"
                        (format nil "~a/.cache" home))
  (startup--prepend-path
   (format nil "~a/.local/bin" home)
   (format nil "~a/.cargo/bin" home)
   "/usr/local/bin"
   "/usr/pkg/bin"
   "/usr/X11R7/bin"
   "/usr/bin"
   "/bin"
   "/usr/local/sbin"
   "/usr/pkg/sbin"
   "/usr/sbin"
   "/sbin"))

(startup--set-default "LANG" "C.UTF-8")
(startup--set-default "LC_CTYPE" "C.UTF-8")
(startup--set-default "EDITOR" "emacs")
(startup--set-default "VISUAL" (getenv "EDITOR"))
(startup--set-default "PAGER" "less -R")
(startup--set-default "MANPAGER" "sh -c 'col -bx | bat -l man -p'")

(defun startup--starship-prompt
    (&key status duration-milliseconds columns job-count
     &allow-other-keys)
  "Render this account's prompt with Starship, or request the default."
  (handler-case
      (let ((prompt
              (uiop:run-program
               (list "/usr/bin/env"
                     "STARSHIP_SHELL="
                     "starship"
                     "prompt"
                     (format nil "--status=~d" status)
                     (format nil "--cmd-duration=~d" duration-milliseconds)
                     (format nil "--terminal-width=~d" columns)
                     (format nil "--jobs=~d" job-count))
               :input               nil
               :output              '(:string :stripped nil)
               :error-output        nil
               :ignore-error-status nil
               :external-format     ':utf-8)))
        (and (stringp prompt)
             (plusp (length prompt))
             prompt))
    (error () nil)))

(setf *prompt-function* 'startup--starship-prompt)

(defcommand ls (&rest arguments)
  "List directory contents with eza."
  (apply #'run "eza" arguments))

(defcommand la (&rest arguments)
  "List all files in long, human-readable form."
  (apply #'run "eza" "-la" arguments))

(defcommand ll (&rest arguments)
  "List files in long, human-readable form."
  (apply #'run "eza" "-l" arguments))

(defcommand gd (&rest arguments)
  "Show the current Git diff."
  (apply #'run "git" "diff" arguments))

(zoxide-setup)

;;; Console autologin: when this is the autologin session on the console and
;;; no X server is running yet, start X with StumpWM right away. The prompt
;;; is reached again when the X session ends.
;;; Under the shared session server (cclshd) this file is evaluated inside the
;;; detached daemon, whose descriptor 0 is /dev/null, so ask cclsh for the
;;; session's terminal; fall back to ttyname(0) on a standalone image that
;;; predates TERMINAL-NAME.
(let ((tty (or (ignore-errors
                 (let ((terminal-name (find-symbol "TERMINAL-NAME" :cclsh)))
                   (and terminal-name (fboundp terminal-name)
                        (funcall terminal-name))))
               (ignore-errors
                 (sb-alien:alien-funcall
                  (sb-alien:extern-alien "ttyname"
                                         (function sb-alien:c-string sb-alien:int))
                  0))
               "")))
  (when (and (null (getenv "DISPLAY"))
             (or (search "constty" tty) (search "ttyE0" tty))
             ;; A stale /tmp/.X0-lock from a killed server must not block us.
             (not (ignore-errors
                   (zerop (nth-value 2 (uiop:run-program "pgrep -x Xorg"
                                                         :ignore-error-status t))))))
    (run "startx")))

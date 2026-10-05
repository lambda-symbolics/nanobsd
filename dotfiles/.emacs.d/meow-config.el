;;; meow-config.el --- Meow as on the workstation's Doom -*- lexical-binding: t; -*-

;; A port of the workstation's lisp/init-meow.el (Doom, `(meow +override
;; +leader)', no stock layout).  Doom-only commands are replaced by their
;; plain Emacs counterparts: +fold -> hideshow, flycheck -> flymake,
;; +format/buffer -> eglot or rustfmt, lsp-ui-flycheck-list -> flymake's
;; diagnostics buffer.  SPC is a small Doom-style leader map.

(require 'meow)
(require 'subr-x)

;;; Helpers

(defvar mag/meow-g-map (make-sparse-keymap)
  "Prefix keymap for `g' commands in normal state.")

(defvar mag/meow-G-map (make-sparse-keymap)
  "Prefix keymap for `G' commands in normal state.")

(defun mag/meow-line-dwim ()
  "Select the current line, or extend linewise if a region is active."
  (interactive)
  (if (region-active-p)
      (progn
        (meow-next 1)
        (meow-line 1))
    (meow-line 1)))

(defun mag/meow-word-dwim ()
  "Select the current word, or extend forward by word."
  (interactive)
  (if (region-active-p)
      (progn
        (meow-next-word 1)
        (meow-mark-word 1))
    (meow-mark-word 1)))

(defun mag/meow-word-next-dwim ()
  "Move forward by word when active, otherwise select one."
  (interactive)
  (if (region-active-p)
      (meow-next-word 1)
    (meow-mark-word 1)))

(defun mag/meow-buffer-start ()
  "Jump to the start of the buffer and clear selection."
  (interactive)
  (goto-char (point-min))
  (meow-cancel-selection))

(defun mag/meow-buffer-end ()
  "Jump to the end of the buffer and clear selection."
  (interactive)
  (goto-char (point-max))
  (meow-cancel-selection))

(defun mag/meow-select-to-buffer-start ()
  "Select from point to the start of the buffer."
  (interactive)
  (unless (region-active-p)
    (push-mark (point) t t))
  (goto-char (point-min)))

(defun mag/meow-select-to-buffer-end ()
  "Select from point to the end of the buffer."
  (interactive)
  (unless (region-active-p)
    (push-mark (point) t t))
  (goto-char (point-max)))

(defun mag/meow--dispatch (map prompt)
  (let* ((key (read-key prompt))
         (command (lookup-key map (vector key))))
    (if (commandp command)
        (call-interactively command)
      (message "No such %s command: %s" prompt (key-description (vector key))))))

(defun mag/meow-g-prefix ()
  "Dispatch a `g' prefixed command."
  (interactive)
  (mag/meow--dispatch mag/meow-g-map "g-"))

(defun mag/meow-G-prefix ()
  "Dispatch a `G' prefixed command."
  (interactive)
  (mag/meow--dispatch mag/meow-G-map "G-"))

(defun mag/meow-goto-line-and-center ()
  "Jump to a line and recenter."
  (interactive)
  (call-interactively #'meow-goto-line)
  (recenter))

(defun mag/force-kill-current-buffer ()
  "Kill the current buffer without confirmation."
  (interactive)
  (set-buffer-modified-p nil)
  (kill-buffer (current-buffer)))

(defun mag/save-buffer-or-file (file)
  "Save the current buffer, or write it to FILE when given."
  (if file
      (write-file file)
    (save-buffer)))

(defun mag/meow-ex-command ()
  "Run a small Vim-like Ex command."
  (interactive)
  (let* ((parts (split-string (read-string ":") " " t))
         (cmd (car parts))
         (args (cdr parts)))
    (pcase cmd
      ("e" (if-let ((file (car args)))
               (find-file file)
             (call-interactively #'find-file)))
      ("b" (if-let ((buffer (car args)))
               (switch-to-buffer buffer)
             (call-interactively #'consult-buffer)))
      ("db" (call-interactively #'kill-current-buffer))
      ("db!" (mag/force-kill-current-buffer))
      ((or "q" "Q") (save-buffers-kill-terminal))
      ("q!" (kill-emacs))
      ((or "w" "W") (mag/save-buffer-or-file (car args)))
      ("wq" (save-buffer)
       (save-buffers-kill-terminal))
      (_ (message "Unknown command: %s" cmd)))))

(defun mag/meow-select-buffer ()
  "Select the whole buffer."
  (interactive)
  (meow-bounds-of-thing ?b))

(defun mag/indent-selection ()
  "Indent the active region or current line by four spaces."
  (interactive)
  (let ((deactivate-mark nil))
    (if (region-active-p)
        (indent-rigidly (region-beginning) (region-end) 4)
      (indent-rigidly (line-beginning-position) (line-end-position) 4))))

(defun mag/unindent-selection ()
  "Unindent the active region or current line by four spaces."
  (interactive)
  (let ((deactivate-mark nil))
    (if (region-active-p)
        (indent-rigidly (region-beginning) (region-end) -4)
      (indent-rigidly (line-beginning-position) (line-end-position) -4))))

(defun mag/align-dotted-pairs ()
  "Align dotted pairs in the active region (refuses without one)."
  (interactive)
  (unless (region-active-p)
    (user-error "Select a region first"))
  (align-regexp (region-beginning) (region-end) "\\(\\s-*\\)\\." 1 1 nil))

(defun mag/move-and-cancel (fn)
  (lambda ()
    (interactive)
    (funcall fn 1)
    (meow-cancel-selection)))

;; Doom's +fold, through hideshow.
(defun mag/fold-toggle ()
  "Toggle the fold at point."
  (interactive)
  (require 'hideshow)
  (unless hs-minor-mode
    (hs-minor-mode 1))
  (save-excursion
    (back-to-indentation)
    (hs-toggle-hiding)))

(defun mag/fold-close-all ()
  "Fold every block in the buffer."
  (interactive)
  (require 'hideshow)
  (unless hs-minor-mode
    (hs-minor-mode 1))
  (hs-hide-all))

;; Doom's +format/buffer.
(defun mag/format-buffer ()
  "Format with the language server, rustfmt, or plain indentation."
  (interactive)
  (cond
   ((and (fboundp 'eglot-managed-p) (eglot-managed-p))
    (eglot-format-buffer))
   ((fboundp 'rust-format-buffer)
    (if (derived-mode-p 'rust-mode 'rust-ts-mode)
        (rust-format-buffer)
      (indent-region (point-min) (point-max))))
   (t (indent-region (point-min) (point-max)))))

;; flycheck on the workstation, flymake (what eglot uses) here.
(defun mag/diagnostics ()
  "List the buffer's diagnostics."
  (interactive)
  (if (bound-and-true-p flymake-mode)
      (flymake-show-buffer-diagnostics)
    (user-error "No diagnostics in this buffer")))

(defun mag/next-error ()
  (interactive)
  (if (bound-and-true-p flymake-mode)
      (flymake-goto-next-error)
    (next-error)))

(defun mag/previous-error ()
  (interactive)
  (if (bound-and-true-p flymake-mode)
      (flymake-goto-prev-error)
    (previous-error)))

(defun mag/compile-defun ()
  "sly-compile-defun in Lisp, eval-defun in Emacs Lisp."
  (interactive)
  (if (derived-mode-p 'emacs-lisp-mode 'lisp-interaction-mode)
      (call-interactively #'eval-defun)
    (call-interactively #'sly-compile-defun)))

(defun mag/compile-file ()
  "sly-compile-file in Lisp, project compile elsewhere."
  (interactive)
  (if (derived-mode-p 'lisp-mode)
      (call-interactively #'sly-compile-file)
    (call-interactively #'project-compile)))

;;; State and keys

;; C-j / C-k / C-l (barf, slurp, SLY REPL) are bound in init.el.
(setq meow-use-clipboard t
      meow-visit-sanitize-completion nil
      meow-cheatsheet-layout meow-cheatsheet-layout-qwerty)
(meow-setup-indicator)

(define-key mag/meow-g-map (kbd "g") #'mag/meow-goto-line-and-center)
(define-key mag/meow-g-map (kbd "j") #'mag/meow-buffer-end)
(define-key mag/meow-g-map (kbd "k") #'mag/meow-buffer-start)
(define-key mag/meow-G-map (kbd "j") #'mag/meow-select-to-buffer-end)
(define-key mag/meow-G-map (kbd "k") #'mag/meow-select-to-buffer-start)

(meow-motion-overwrite-define-key
 '("j" . meow-next)
 '("k" . meow-prev)
 '("h" . meow-left)
 '("l" . meow-right))

(meow-normal-define-key
 '("0" . meow-expand-0)
 '("1" . meow-expand-1)
 '("2" . meow-expand-2)
 '("3" . meow-expand-3)
 '("4" . meow-expand-4)
 '("5" . meow-expand-5)
 '("6" . meow-expand-6)
 '("7" . meow-expand-7)
 '("8" . meow-expand-8)
 '("9" . meow-expand-9)
 '("-" . negative-argument)
 '("=" . mag/align-dotted-pairs)
 '(";" . meow-reverse)
 '("," . meow-inner-of-thing)
 '("." . meow-bounds-of-thing)
 '("[" . meow-beginning-of-thing)
 '("]" . meow-end-of-thing)
 '("/" . meow-visit)
 '("a" . meow-append)
 '("A" . meow-open-below)
 '("b" . meow-grab)
 '("B" . meow-pop-selection)
 '("c" . meow-change)
 '("C" . mag/diagnostics)
 '("d" . meow-kill)
 '("e" . mag/fold-toggle)
 '("E" . mag/fold-close-all)
 '("<C-e>" . mag/fold-close-all)
 '("f" . meow-find)
 '("F" . split-window-below)
 '("g" . mag/meow-g-prefix)
 '("G" . mag/meow-G-prefix)
 '("h" . mag/compile-defun)
 '("H" . mag/compile-file)
 '("i" . meow-insert)
 '("j" . sp-backward-sexp)
 '("J" . sp-up-sexp)
 '("k" . sp-forward-sexp)
 '("K" . sp-down-sexp)
 '("L" . mag/align-dotted-pairs)
 '("m" . meow-join)
 '("M" . delete-window)
 '("n" . meow-search)
 '("o" . meow-block)
 '("O" . meow-to-block)
 '("p" . meow-yank)
 '("q" . mag/format-buffer)
 '("Q" . split-window-right)
 '("r" . meow-replace)
 '("R" . delete-other-windows)
 '("s" . (lambda () (interactive) (point-to-register ?a)))
 '("S" . (lambda () (interactive) (jump-to-register ?a)))
 '("t" . meow-till)
 '("u" . meow-undo)
 '("U" . undo-redo)
 '("v" . mag/next-error)
 '("V" . mag/previous-error)
 '("w" . mag/meow-word-dwim)
 '("W" . mag/meow-word-next-dwim)
 '("x" . mag/meow-line-dwim)
 '("X" . meow-line-expand)
 '("y" . meow-save)
 '("Y" . meow-sync-grab)
 '("z" . other-window)
 '("Z" . xref-find-definitions-other-window)
 '("<" . mag/unindent-selection)
 '(">" . mag/indent-selection)
 '("(" . shrink-window-horizontally)
 '(")" . enlarge-window-horizontally)
 '("%" . mag/meow-select-buffer)
 '("'" . repeat)
 '(":" . mag/meow-ex-command)
 '("<escape>" . meow-cancel-selection)
 `("<up>" . ,(mag/move-and-cancel #'previous-line))
 `("<down>" . ,(mag/move-and-cancel #'next-line))
 `("<left>" . ,(mag/move-and-cancel #'left-char))
 `("<right>" . ,(mag/move-and-cancel #'right-char))
 '("S-<up>" . meow-prev-expand)
 '("S-<down>" . meow-next-expand)
 '("S-<left>" . meow-left-expand)
 '("S-<right>" . meow-right-expand))

(dolist (mode '(eat-mode vterm-mode term-mode eshell-mode shell-mode
                message-mode))
  (add-to-list 'meow-mode-state-list `(,mode . insert)))

;;; Leader (Doom-style SPC map)

(defun mag/search-project ()
  "Search the project with ripgrep when available, grep otherwise."
  (interactive)
  (if (executable-find "rg")
      (call-interactively #'consult-ripgrep)
    (call-interactively #'consult-grep)))

(defun mag/open-init ()
  (interactive)
  (find-file user-init-file))

(defvar-keymap mag/leader-file-map
  "f" #'find-file
  "r" #'consult-recent-file
  "s" #'save-buffer
  "S" #'write-file
  "p" #'mag/open-init)

(defvar-keymap mag/leader-buffer-map
  "b" #'consult-buffer
  "k" #'kill-current-buffer
  "K" #'mag/force-kill-current-buffer
  "n" #'next-buffer
  "p" #'previous-buffer
  "s" #'save-buffer
  "i" #'ibuffer)

(defvar-keymap mag/leader-project-map
  "p" #'project-switch-project
  "f" #'project-find-file
  "b" #'project-switch-to-buffer
  "s" #'mag/search-project
  "c" #'project-compile
  "k" #'project-kill-buffers)

(defvar-keymap mag/leader-search-map
  "s" #'consult-line
  "p" #'mag/search-project
  "i" #'consult-imenu
  "o" #'consult-outline)

(defvar-keymap mag/leader-window-map
  "w" #'other-window
  "s" #'split-window-below
  "v" #'split-window-right
  "d" #'delete-window
  "q" #'delete-window
  "m" #'delete-other-windows
  "=" #'balance-windows)

(defvar-keymap mag/leader-code-map
  "a" #'eglot-code-actions
  "r" #'eglot-rename
  "f" #'mag/format-buffer
  "d" #'xref-find-definitions
  "D" #'xref-find-references
  "x" #'mag/diagnostics
  "c" #'compile)

(defvar-keymap mag/leader-toggle-map
  "t" #'mag/toggle-theme
  "l" #'display-line-numbers-mode
  "w" #'visual-line-mode)

(defvar-keymap mag/leader-map
  "SPC" #'project-find-file
  "." #'find-file
  "," #'consult-buffer
  ":" #'execute-extended-command
  "/" #'mag/search-project
  "?" #'meow-cheatsheet
  "x" #'scratch-buffer
  "f" mag/leader-file-map
  "b" mag/leader-buffer-map
  "p" mag/leader-project-map
  "s" mag/leader-search-map
  "w" mag/leader-window-map
  "c" mag/leader-code-map
  "t" mag/leader-toggle-map
  "g" #'vc-dir
  "h" help-map
  "q q" #'save-buffers-kill-terminal)

(which-key-add-keymap-based-replacements mag/leader-map
  "f" "file" "b" "buffer" "p" "project" "s" "search" "w" "window"
  "c" "code" "t" "toggle" "h" "help" "q" "quit")

(define-key meow-normal-state-keymap (kbd "SPC") mag/leader-map)
(define-key meow-motion-state-keymap (kbd "SPC") mag/leader-map)

(meow-global-mode 1)

;;; meow-config.el ends here

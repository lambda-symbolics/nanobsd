;;; init.el --- Small Emacs for the X1 Nano -*- lexical-binding: t; -*-

;; Meow (the bindings of the Doom setup on the workstation, see meow-config.el),
;; Rust through eglot and rust-analyzer, Org, Common Lisp through SLY.  No
;; Doom, no EXWM.  Packages come from GNU ELPA and MELPA on first start.

;;; Packages

(require 'package)
(add-to-list 'package-archives '("melpa" . "https://melpa.org/packages/") t)
(package-initialize)
(require 'use-package)
(setq use-package-always-ensure t
      use-package-expand-minimally t)

;; A stale archive index names package versions MELPA no longer serves, so
;; refresh it once whenever something below still has to be installed.
(defconst mag/packages
  '(meow vertico orderless marginalia consult corfu smartparens drag-stuff
    rust-mode sly org-appear org-auto-tangle eat))
(unless (seq-every-p #'package-installed-p mag/packages)
  (package-refresh-contents))

(add-hook 'emacs-startup-hook
          (lambda () (setq gc-cons-threshold (* 32 1024 1024))))

;;; Basics

(setq ring-bell-function 'ignore
      use-dialog-box nil
      use-short-answers t
      make-backup-files nil
      auto-save-default nil
      create-lockfiles nil
      sentence-end-double-space nil
      tab-always-indent 'complete
      indent-tabs-mode nil
      focus-follows-mouse t
      mouse-autoselect-window t
      scroll-conservatively 101
      custom-file (locate-user-emacs-file "custom.el"))
(setq-default indent-tabs-mode nil)
(when (file-exists-p custom-file)
  (load custom-file nil t))

(column-number-mode 1)
(global-auto-revert-mode 1)
(recentf-mode 1)
(savehist-mode 1)
(save-place-mode 1)
(show-paren-mode 1)
(delete-selection-mode 1)
(which-key-mode 1)

(require 'server)
(unless (server-running-p)
  (server-start))

;;; Look

;; ProFontExtended 9 pt is also set in early-init.el for the first frame;
;; this covers frames made later (emacsclient -c).  C-= C-- C-0 zoom every
;; buffer at once.
(defun mag/set-font (&optional frame)
  (when (display-graphic-p frame)
    (set-face-attribute 'default frame :family "ProFontExtended" :height 90)))
(mag/set-font)
(add-hook 'after-make-frame-functions #'mag/set-font)
(global-set-key (kbd "C-=") #'global-text-scale-adjust)
(global-set-key (kbd "C--") #'global-text-scale-adjust)
(global-set-key (kbd "C-0") #'global-text-scale-adjust)

(defconst mag/light-theme 'acme)
(defconst mag/dark-theme 'kanagawa-dragon)
(defconst mag/theme-file (locate-user-emacs-file "theme-choice"))
(add-to-list 'custom-theme-load-path (locate-user-emacs-file "themes/"))

(defun mag/load-theme (theme)
  (mapc #'disable-theme custom-enabled-themes)
  (load-theme theme t)
  (with-temp-file mag/theme-file
    (insert (symbol-name theme))))

(defun mag/toggle-theme ()
  "Switch between the light and the dark theme; the choice is remembered."
  (interactive)
  (mag/load-theme (if (memq mag/dark-theme custom-enabled-themes)
                      mag/light-theme
                    mag/dark-theme)))

(mag/load-theme
 (or (and (file-readable-p mag/theme-file)
          (let ((name (intern (string-trim
                               (with-temp-buffer
                                 (insert-file-contents mag/theme-file)
                                 (buffer-string))))))
            (and (memq name (list mag/light-theme mag/dark-theme)) name)))
     mag/light-theme))
(global-set-key (kbd "<f9>") #'mag/toggle-theme)

;;; Completion

(use-package vertico
  :init (vertico-mode 1))

(use-package orderless
  :custom
  (completion-styles '(orderless basic))
  (completion-category-overrides '((file (styles basic partial-completion)))))

(use-package marginalia
  :init (marginalia-mode 1))

(use-package consult
  :bind (("C-x b" . consult-buffer)
         ("M-y" . consult-yank-pop)))

(use-package corfu
  :custom
  (corfu-auto t)
  (corfu-auto-delay 0.2)
  (corfu-auto-prefix 2)
  :init (global-corfu-mode 1))

;;; Editing

(use-package smartparens
  :hook ((prog-mode . smartparens-mode)
         (sly-mrepl-mode . smartparens-mode))
  :config
  (require 'smartparens-config))

(electric-pair-mode 1)
(add-hook 'smartparens-mode-hook (lambda () (electric-pair-local-mode -1)))

(use-package drag-stuff
  :bind (("<C-up>" . drag-stuff-up)
         ("<C-down>" . drag-stuff-down)))

(global-set-key (kbd "M-<left>") #'previous-buffer)
(global-set-key (kbd "M-<right>") #'next-buffer)

;;; Rust

;; rust-ts-mode when the tree-sitter grammar loads (pkgsrc tree-sitter-rust
;; puts libtree-sitter-rust.so in /usr/pkg/lib), rust-mode otherwise.
(use-package rust-mode
  :init
  (when (treesit-language-available-p 'rust)
    (setq rust-mode-treesitter-derive t)))

(defun mag/rust-setup ()
  (when (executable-find "rust-analyzer")
    (eglot-ensure)
    (add-hook 'before-save-hook #'eglot-format-buffer nil t)))
(add-hook 'rust-mode-hook #'mag/rust-setup)
(add-hook 'rust-ts-mode-hook #'mag/rust-setup)

(with-eval-after-load 'eglot
  ;; As on the workstation: inlay hints on, but not for parameters or
  ;; closure return types.
  (setq-default eglot-workspace-configuration
                '(:rust-analyzer
                  (:inlayHints
                   (:parameterHints (:enable :json-false)
                    :closureReturnTypeHints (:enable "never")
                    :lifetimeElisionHints (:enable "always"
                                           :useParameterNames :json-false)))))
  (setq eglot-autoshutdown t))

;;; Common Lisp

(use-package sly
  :init
  (setq inferior-lisp-program (if (file-executable-p "/usr/local/bin/sbcl")
                                  "/usr/local/bin/sbcl"
                                "sbcl")))

(defun mag/find-file-upward (filename)
  "Find FILENAME in the current directory or one of its parents."
  (when-let ((dir (locate-dominating-file
                   (or buffer-file-name default-directory) filename)))
    (expand-file-name filename dir)))

(defun mag/sly-mrepl-here ()
  "Open a SLY REPL below, and feed it `.slyrc.lisp' if one is found upward."
  (interactive)
  (let* ((rc-path (mag/find-file-upward ".slyrc.lisp"))
         (rc (if rc-path
                 (with-temp-buffer
                   (insert-file-contents rc-path)
                   (buffer-string))
               ""))
         (window (split-window-below)))
    (with-selected-window window
      (set-window-text-height window 20)
      (call-interactively #'sly-mrepl)
      (with-current-buffer (sly-mrepl--find-buffer)
        (goto-char (point-max))
        (insert rc)
        (sly-mrepl-return)))))

(global-set-key (kbd "C-j") #'sp-forward-barf-sexp)
(global-set-key (kbd "C-k") #'sp-forward-slurp-sexp)
(global-set-key (kbd "C-l") #'mag/sly-mrepl-here)

;;; Org

(setq org-directory (expand-file-name "~/org/")
      org-log-done 'time
      org-startup-indented t
      org-hide-emphasis-markers t
      org-link-descriptive t
      org-export-with-broken-links t)
(with-eval-after-load 'org
  (when (file-directory-p org-directory)
    (setq org-agenda-files
          (directory-files-recursively org-directory "\\.org\\'")))
  (define-key org-mode-map (kbd "M-<left>") #'previous-buffer)
  (define-key org-mode-map (kbd "M-<right>") #'next-buffer)
  (define-key org-mode-map (kbd "C-c o .") #'org-time-stamp))

(use-package org-appear
  :hook (org-mode . org-appear-mode)
  :custom
  (org-appear-trigger 'manual)
  (org-appear-autolinks t)
  (org-appear-autoemphasis t)
  (org-appear-autosubmarkers t)
  (org-appear-autoentities t)
  (org-appear-autokeywords t)
  (org-appear-delay 0.0)
  :config
  ;; Show the hidden markup while in Meow insert state.
  (add-hook 'org-mode-hook
            (lambda ()
              (add-hook 'meow-insert-enter-hook #'org-appear-manual-start nil t)
              (add-hook 'meow-insert-exit-hook #'org-appear-manual-stop nil t))))

(use-package org-auto-tangle
  :hook (org-mode . org-auto-tangle-mode))

(defun mag/insert-org-src-block (language)
  "Insert an Org source block for LANGUAGE and enter insert state."
  (interactive "sLanguage: ")
  (insert (format "#+BEGIN_SRC %s\n\n#+END_SRC" language))
  (forward-line -1)
  (when (fboundp 'meow-insert)
    (meow-insert)))

(global-set-key (kbd "C-s-1") (lambda () (interactive) (mag/insert-org-src-block "rust")))
(global-set-key (kbd "C-s-2") (lambda () (interactive) (mag/insert-org-src-block "toml")))
(global-set-key (kbd "C-s-3") (lambda () (interactive) (mag/insert-org-src-block "asm")))
(global-set-key (kbd "C-s-4") (lambda () (interactive) (mag/insert-org-src-block "sh")))
(global-set-key (kbd "C-s-b") #'mag/insert-org-src-block)

;;; Terminal

(use-package eat
  :bind ("C-c t" . eat))

;;; Meow

(use-package meow
  :demand t
  :config
  (load (locate-user-emacs-file "meow-config.el") nil t))

;;; Mode line

(setq-default mode-line-format
              '("%e" mode-line-front-space
                (:eval (when (bound-and-true-p meow-mode) (meow-indicator)))
                " " mode-line-modified " %b  %l:%c  "
                mode-name
                (:eval (when (bound-and-true-p flymake-mode)
                         (concat "  " (format-mode-line flymake-mode-line-counters))))
                mode-line-end-spaces))

;;; init.el ends here

;;; kanagawa-dragon-theme.el --- Kanagawa Dragon theme -*- lexical-binding: t; -*-

;; Imported from root@10.0.1.6:/root/project/dots/doom/themes/kanagawa-dragon-theme.el
;; and adapted from Doom's `def-doom-theme' form to a plain custom theme.

(require 'cl-lib)
(require 'color)

(deftheme kanagawa-dragon "A dark theme based on the Kanagawa Dragon color scheme.")

(defgroup kanagawa-dragon-theme nil
  "Options for the `kanagawa-dragon' theme."
  :group 'faces)

(defcustom kanagawa-dragon-comment-bg nil
  "If non-nil, comments will have a subtle, darker background."
  :type 'boolean
  :group 'kanagawa-dragon-theme)

(defun kanagawa-dragon--blend (color1 color2 alpha)
  "Blend COLOR1 and COLOR2 by ALPHA."
  (when (and color1 color2)
    (let ((rgb1 (color-name-to-rgb color1))
          (rgb2 (color-name-to-rgb color2)))
      (apply #'color-rgb-to-hex
             (append
              (mapcar (lambda (pair)
                        (+ (* alpha (car pair))
                           (* (- 1 alpha) (cdr pair))))
                      (cl-mapcar #'cons rgb1 rgb2))
              '(2))))))

(defun kanagawa-dragon--darken (color alpha)
  "Darken COLOR by ALPHA."
  (kanagawa-dragon--blend color "#000000" (- 1 alpha)))

(defun kanagawa-dragon--lighten (color alpha)
  "Lighten COLOR by ALPHA."
  (kanagawa-dragon--blend color "#ffffff" (- 1 alpha)))

(let* ((bg "#080606")
       (bg-alt "#181616")
       (base0 "#0d0c0c")
       (base1 "#181616")
       (base2 "#2D4F67")
       (base3 "#1e3f52")
       (base4 "#a6a69c")
       (base5 "#c5c9c5")
       (base8 "#c5c9c5")
       (fg "#c5c9c5")
       (fg-alt "#a6a69c")
       (red "#c4746e")
       (orange "#b6927b")
       (green "#8a9a7b")
       (yellow "#c4b28a")
       (blue "#8ba4b0")
       (magenta "#a292a3")
       (violet "#938AA9")
       (cyan "#7AA89F")
       (highlight blue)
       (selection base2)
       (region (kanagawa-dragon--lighten bg-alt 0.15))
       (modeline-pad 4)
       (modeline-bg (kanagawa-dragon--darken blue 0.45))
       (modeline-bg-l (kanagawa-dragon--darken blue 0.475))
       (modeline-bg-inactive bg-alt)
       (modeline-bg-inactive-l (kanagawa-dragon--darken bg-alt 0.1))
       (comment-bg (when kanagawa-dragon-comment-bg
                     (kanagawa-dragon--lighten bg 0.05))))
  (custom-theme-set-faces
   'kanagawa-dragon
   `(default ((t (:background ,bg :foreground ,fg))))
   `(cursor ((t (:background ,highlight))))
   `(fringe ((t (:background ,bg :foreground ,base4))))
   `(region ((t (:background ,region :foreground ,fg :extend t))))
   `(highlight ((t (:background ,highlight :foreground ,base0))))
   `(shadow ((t (:foreground ,base5))))
   `(minibuffer-prompt ((t (:foreground ,highlight))))
   `(tooltip ((t (:background ,bg-alt :foreground ,fg))))
   `(secondary-selection ((t (:background ,selection :extend t))))
   `(trailing-whitespace ((t (:background ,red))))
   `(vertical-border ((t (:background ,(kanagawa-dragon--darken base1 0.1)
                          :foreground ,(kanagawa-dragon--darken base1 0.1)))))
   `(link ((t (:foreground ,highlight :underline t :weight bold))))
   `(error ((t (:foreground ,red))))
   `(warning ((t (:foreground ,yellow))))
   `(success ((t (:foreground ,green))))

   `(font-lock-builtin-face ((t (:foreground ,magenta))))
   `(font-lock-comment-face ((t (:foreground ,base4 :background ,comment-bg))))
   `(font-lock-comment-delimiter-face ((t (:inherit font-lock-comment-face))))
   `(font-lock-doc-face ((t (:foreground ,(kanagawa-dragon--lighten base4 0.15)))))
   `(font-lock-constant-face ((t (:foreground ,violet))))
   `(font-lock-function-name-face ((t (:foreground ,blue))))
   `(font-lock-keyword-face ((t (:foreground ,magenta))))
   `(font-lock-string-face ((t (:foreground ,red))))
   `(font-lock-type-face ((t (:foreground ,yellow))))
   `(font-lock-variable-name-face ((t (:foreground ,blue))))
   `(font-lock-number-face ((t (:foreground ,orange))))
   `(font-lock-warning-face ((t (:inherit warning))))
   `(font-lock-negation-char-face ((t (:foreground ,blue :weight bold))))
   `(font-lock-preprocessor-face ((t (:foreground ,blue :weight bold))))
   `(font-lock-regexp-grouping-backslash ((t (:foreground ,blue :weight bold))))
   `(font-lock-regexp-grouping-construct ((t (:foreground ,blue :weight bold))))

   `(line-number ((t (:inherit default :foreground ,base4 :weight normal))))
   `(line-number-current-line ((t (:inherit default :foreground ,fg :weight normal))))

   `(mode-line ((t (:background ,modeline-bg
                    :foreground ,fg
                    :box (:line-width ,modeline-pad :color ,modeline-bg)))))
   `(mode-line-active ((t (:inherit mode-line))))
   `(mode-line-inactive ((t (:background ,modeline-bg-inactive
                             :foreground ,base5
                             :box (:line-width ,modeline-pad :color ,modeline-bg-inactive)))))
   `(mode-line-emphasis ((t (:foreground ,base8))))
   `(mode-line-highlight ((t (:inherit highlight))))
   `(mode-line-buffer-id ((t (:weight bold))))
   `(header-line ((t (:inherit mode-line))))

   `(isearch ((t (:background ,yellow :foreground ,base0 :weight bold))))
   `(isearch-fail ((t (:background ,red :foreground ,base0 :weight bold))))
   `(lazy-highlight ((t (:background ,(kanagawa-dragon--darken highlight 0.3)
                         :foreground ,base8 :weight bold))))
   `(match ((t (:background ,base0 :foreground ,green :weight bold))))
   `(show-paren-match ((t (:foreground ,red :weight bold :underline t))))
   `(show-paren-mismatch ((t (:background ,red :foreground ,bg))))

   `(ansi-color-black ((t (:foreground ,bg :background ,bg))))
   `(ansi-color-red ((t (:foreground ,red :background ,red))))
   `(ansi-color-green ((t (:foreground ,green :background ,green))))
   `(ansi-color-yellow ((t (:foreground ,yellow :background ,yellow))))
   `(ansi-color-blue ((t (:foreground ,blue :background ,blue))))
   `(ansi-color-magenta ((t (:foreground ,magenta :background ,magenta))))
   `(ansi-color-cyan ((t (:foreground ,cyan :background ,cyan))))
   `(ansi-color-white ((t (:foreground ,fg :background ,fg))))
   `(term-color-black ((t (:foreground ,bg :background ,bg))))
   `(term-color-red ((t (:foreground ,red :background ,red))))
   `(term-color-green ((t (:foreground ,green :background ,green))))
   `(term-color-yellow ((t (:foreground ,yellow :background ,yellow))))
   `(term-color-blue ((t (:foreground ,blue :background ,blue))))
   `(term-color-magenta ((t (:foreground ,magenta :background ,magenta))))
   `(term-color-cyan ((t (:foreground ,cyan :background ,cyan))))
   `(term-color-white ((t (:foreground ,fg :background ,fg))))

   `(css-proprietary-property ((t (:foreground ,orange))))
   `(css-property ((t (:foreground ,green))))
   `(css-selector ((t (:foreground ,blue))))
   `(font-latex-math-face ((t (:foreground ,green))))
   `(markdown-markup-face ((t (:foreground ,base5))))
   `(markdown-header-face ((t (:inherit bold :foreground ,red))))
   `(markdown-code-face ((t (:background ,(kanagawa-dragon--lighten base3 0.05)))))
   `(rjsx-tag ((t (:foreground ,red))))
   `(rjsx-attr ((t (:foreground ,orange))))
   `(ivy-current-match ((t (:background ,base2 :distant-foreground ,base0 :weight normal))))

   `(doom-modeline-bar ((t (:background ,modeline-bg))))
   `(doom-modeline-buffer-file ((t (:inherit mode-line-buffer-id :weight bold))))
   `(doom-modeline-buffer-path ((t (:inherit mode-line-emphasis :weight bold))))
   `(doom-modeline-buffer-project-root ((t (:foreground ,green :weight bold))))
   `(solaire-mode-line-face ((t (:inherit mode-line
                              :background ,modeline-bg-l
                              :box (:line-width ,modeline-pad :color ,modeline-bg-l)))))
   `(solaire-mode-line-inactive-face ((t (:inherit mode-line-inactive
                                       :background ,modeline-bg-inactive-l
                                       :box (:line-width ,modeline-pad :color ,modeline-bg-inactive-l)))))

   `(diff-added ((t (:background ,(kanagawa-dragon--blend green bg 0.2)
                      :foreground ,green))))
   `(diff-removed ((t (:background ,(kanagawa-dragon--blend red bg 0.2)
                        :foreground ,red))))
   `(diff-changed ((t (:foreground ,yellow))))
   `(diff-header ((t (:background ,bg-alt :foreground ,fg-alt))))
   `(diff-file-header ((t (:background ,bg-alt :foreground ,blue :weight bold))))
   `(magit-section-heading ((t (:foreground ,cyan :weight bold))))
   `(magit-branch-local ((t (:foreground ,blue :weight bold))))
   `(magit-branch-remote ((t (:foreground ,green :weight bold))))
   `(magit-diff-added ((t (:foreground ,green :background ,(kanagawa-dragon--blend green bg 0.12)))))
   `(magit-diff-removed ((t (:foreground ,red :background ,(kanagawa-dragon--blend red bg 0.12)))))

   `(org-document-title ((t (:foreground ,blue :weight bold :height 1.2))))
   `(org-level-1 ((t (:foreground ,blue :weight bold))))
   `(org-level-2 ((t (:foreground ,cyan :weight bold))))
   `(org-level-3 ((t (:foreground ,yellow :weight bold))))
   `(org-level-4 ((t (:foreground ,green))))
   `(org-level-5 ((t (:foreground ,magenta))))
   `(org-level-6 ((t (:foreground ,violet))))
   `(org-level-7 ((t (:foreground ,orange))))
   `(org-level-8 ((t (:foreground ,fg-alt))))
   `(org-code ((t (:foreground ,red :background ,bg-alt))))
   `(org-block ((t (:background ,bg-alt :extend t))))
   `(org-block-begin-line ((t (:foreground ,base4 :background ,bg-alt))))
   `(org-block-end-line ((t (:foreground ,base4 :background ,bg-alt))))
   `(org-date ((t (:foreground ,violet))))
   `(org-done ((t (:foreground ,green :weight bold))))
   `(org-todo ((t (:foreground ,yellow :weight bold))))
   `(org-table ((t (:foreground ,violet))))
   `(org-verbatim ((t (:foreground ,yellow :background ,bg-alt))))))

(provide-theme 'kanagawa-dragon)

;;; kanagawa-dragon-theme.el ends here

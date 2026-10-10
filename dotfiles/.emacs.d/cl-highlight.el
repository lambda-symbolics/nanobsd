;;; cl-highlight.el --- Common Lisp highlighting from the workstation -*- lexical-binding: t; -*-

;; The custom Common Lisp font-lock rules of the Doom config on the
;; workstation (~/.config/doom/lisp/init-lisp.el), kept under the same luk/
;; names so the two stay diffable.  Differences: the place operators (setf,
;; setq, push, ...) use the Acme theme's red, the list also covers the
;; destructive standard functions, and the palette is re-applied from
;; `enable-theme-functions' instead of Doom's theme hook.  The Scheme,
;; Gerbil and Nyxt parts were not brought over.

(defface luk/lisp-mutation-face
  '((t :foreground "#da908b" :weight bold))
  "Face for mutating operations.")

(defface luk/lisp-definition-face
  '((default (:weight bold))
    (((class color) (min-colors 88) (background dark))
     (:foreground "gold1" :background "gray20"))
    (((class color) (min-colors 88) (background light))
     (:foreground "DarkBlue" :background "SlateGray1"))
    (((class color) (min-colors 16) (background dark))
     (:foreground "yellow"))
    (((class color) (min-colors 16) (background light))
     (:foreground "blue"))
    (t (:inverse-video t)))
  "Face for important Lisp definitions.")

;; Tinker zone for custom Common Lisp highlighting.
(defface luk/cl-primitive-control-face
  '((t :inherit default))
  "Face for primitive Common Lisp control operators.")

(defface luk/cl-call-operator-face
  '((t :inherit default))
  "Face for Common Lisp call operators.")

(defface luk/cl-sequence-operator-face
  '((t :inherit default))
  "Face for Common Lisp sequence and mapping operators.")

(defface luk/cl-place-operator-face
  '((t :inherit default))
  "Face for Common Lisp place and mutation operators.")

(defface luk/cl-test-declaration-face
  '((t :inherit default))
  "Face for Hiisi test declaration macros.")

(defface luk/cl-test-assertion-face
  '((t :inherit default))
  "Face for Hiisi test assertion macros.")

(defface luk/cl-keyword-symbol-face
  '((t :inherit default))
  "Face for keyword symbols.")

(defface luk/cl-binding-introducer-face
  '((t :inherit default))
  "Face for binding-introducing operators.")

(defface luk/cl-bound-name-face
  '((t :inherit default))
  "Face for names introduced by binders.")

(defface luk/cl-serapeum-introducer-face
  '((t :inherit default))
  "Face for Serapeum declaration introducers.")

(defface luk/cl-serapeum-declared-name-face
  '((t :inherit default))
  "Face for names declared by Serapeum's `->'.")

(defface luk/cl-serapeum-declaration-syntax-face
  '((t :inherit default))
  "Face for Serapeum declaration syntax like `&key'.")

(defface luk/cl-serapeum-type-constructor-face
  '((t :inherit default))
  "Face for Serapeum type constructors.")

(defface luk/cl-serapeum-type-atom-face
  '((t :inherit default))
  "Face for Serapeum type atoms.")

(defface luk/cl-hsx-element-face
  '((t :inherit default))
  "Face for Shoelace HSX element names.")

(defface luk/cl-number-face
  '((t :inherit default))
  "Face for numeric literals.")

(defface luk/cl-predicate-face
  '((t :inherit default))
  "Face for Common Lisp predicate operators.")

(defconst luk/cl-highlight-face-alist
  '((primitive-control        . luk/cl-primitive-control-face)
    (call-operator            . luk/cl-call-operator-face)
    (sequence-operator        . luk/cl-sequence-operator-face)
    (place-operator           . luk/cl-place-operator-face)
    (test-declaration         . luk/cl-test-declaration-face)
    (test-assertion           . luk/cl-test-assertion-face)
    (keyword-symbol           . luk/cl-keyword-symbol-face)
    (binding-introducer       . luk/cl-binding-introducer-face)
    (bound-name               . luk/cl-bound-name-face)
    (serapeum-introducer      . luk/cl-serapeum-introducer-face)
    (serapeum-declared-name   . luk/cl-serapeum-declared-name-face)
    (serapeum-declaration-syntax . luk/cl-serapeum-declaration-syntax-face)
    (serapeum-type-constructor . luk/cl-serapeum-type-constructor-face)
    (serapeum-type-atom       . luk/cl-serapeum-type-atom-face)
    (hsx-element              . luk/cl-hsx-element-face)
    (numeric-literal          . luk/cl-number-face)
    (predicate                . luk/cl-predicate-face))
  "Mapping of Common Lisp highlight groups to Emacs faces.")

(defun luk/cl--set-highlight-style-alist (symbol value)
  "Persist SYMBOL as VALUE and refresh the Common Lisp palette."
  (set-default symbol value)
  (when (fboundp 'luk/cl-apply-highlight-palette)
    (luk/cl-apply-highlight-palette)))

(defgroup luk/cl-highlighting nil
  "Custom Common Lisp highlighting."
  :group 'faces)

(defcustom luk/cl-highlight-style-alist
  '((primitive-control :foreground "#1054AF" :weight bold)
    (call-operator :foreground "#555599" :weight bold)
    (sequence-operator :foreground "#006680" :weight bold)
    (place-operator :foreground "#880000" :weight bold) ; acme-red
    (test-declaration :foreground "#0024AF" :background "#E1FAFF" :weight bold)
    (test-assertion :foreground "#006666" :background "#E8FCE8" :weight bold)
    (keyword-symbol :foreground "#562356" :weight bold)
    (binding-introducer :foreground "#1054AF" :weight bold)
    (bound-name :foreground "#005F5F" :background "#E3FAF4" :weight bold)
    (serapeum-introducer :foreground "#007700" :weight bold)
    (serapeum-declared-name :foreground "#0024AF" :background "#E1FAFF" :weight bold)
    (serapeum-declaration-syntax :foreground "#562356" :weight bold)
    (serapeum-type-constructor :foreground "#888838" :weight bold)
    (serapeum-type-atom :foreground "#888838" :weight bold :slant italic)
    (hsx-element :foreground "#0024AF" :weight bold)
    (numeric-literal :foreground "#000000" :weight bold)
    (predicate :foreground "#006666" :weight bold))
  "Style alist for the custom Common Lisp highlighting groups.

Each entry is `(GROUP . FACE-PLIST)` without the dot, for example:

  (numeric-literal :foreground \"#000000\" :weight bold)

Edit this and reload the config to tinker with the palette."
  :group 'luk/cl-highlighting
  :type 'sexp
  :set #'luk/cl--set-highlight-style-alist)

(defcustom luk/cl-font-lock-bisect 'all
  "Select which half of the custom Common Lisp font-lock rules to enable.

This is a debugging switch for isolating runaway font-lock behavior:

- `all' enables the full custom ruleset.
- `first-half' enables only the first half of the custom rules.
- `second-half' enables only the second half of the custom rules."
  :group 'luk/cl-highlighting
  :type '(choice (const :tag "All rules" all)
                 (const :tag "First half only" first-half)
                 (const :tag "Second half only" second-half)))

(defcustom luk/cl-font-lock-debug-range nil
  "Optional 1-based inclusive range of custom Common Lisp rules to enable.

When non-nil, this takes precedence over `luk/cl-font-lock-bisect' and is used
to bisect the ruleset more precisely while debugging font-lock stalls."
  :group 'luk/cl-highlighting
  :type '(choice (const :tag "Use bisect setting" nil)
                 (cons :tag "Rule range" integer integer)))

(defcustom luk/cl-primitive-control-operators
  '(and or not)
  "Operators that should use the primitive-control face."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-call-operators
  '(funcall apply)
  "Operators that dispatch function calls explicitly."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-sequence-operators
  '(map map-into
    mapc mapcar mapcan mapcon mapl maplist
    reduce
    some every notany notevery)
  "Sequence-oriented standard operators to highlight specially."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-place-operators
  '(setf setq psetf psetq getf remf incf decf push pushnew pop shiftf rotatef
    ;; destructive standard functions
    rplaca rplacd nconc nreconc nreverse nsubstitute nsubstitute-if
    nsublis nsubst nunion nintersection nset-difference nset-exclusive-or
    delete delete-if delete-if-not delete-duplicates sort stable-sort merge
    fill replace vector-push vector-push-extend vector-pop
    remhash clrhash setq-default)
  "Operators that mutate places or interact with generalized variables."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-binding-introducers
  '(let let*
    when-let when-let*
    if-let if-let*
    destructuring-bind
    multiple-value-bind
    flet labels macrolet symbol-macrolet
    with-slots with-accessors)
  "Binders whose introduced names should receive special highlighting."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-serapeum-declaration-operators
  '(->)
  "Serapeum declaration operators that describe typed functions."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-serapeum-declaration-syntax
  '(&optional &key &rest)
  "Lambda-list markers inside Serapeum type declarations."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-test-declaration-macros
  '(deftest deftest-group deftest-cases deftest-route-smoke deftest-manual-route-smoke)
  "Hiisi test macros that declare tests or suites."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-test-assertion-macros
  '(is is-null is-eq is-equal is= is-string= signals)
  "Hiisi test assertion macros."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-hsx-element-prefixes
  '("sl-")
  "Prefixes that identify Shoelace HSX element names."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-hsx-root-forms
  '(shoelace-hsx
    hsx
    with-hx-hidden-fields
    hx-post-button-with-fields
    with-base-page
    with-public-page
    with-app-page
    with-admin-page)
  "Forms whose bodies should be treated as Shoelace HSX for highlighting."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-hsx-introducers
  '(shoelace-hsx hsx)
  "Wrapper macros that should get explicit HSX highlighting."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-hsx-element-names
  '("<>" "a" "article" "aside" "body" "br" "button" "code" "dd" "details" "dialog"
    "div" "dl" "dt" "em" "fieldset" "footer" "form" "h1" "h2" "h3" "h4" "h5" "h6"
    "head" "header" "hr" "html" "img" "input" "label" "li" "link" "main" "meta"
    "nav" "ol" "option" "p" "pre" "raw!" "script" "section" "select" "small" "span" "strong"
    "style" "summary" "table" "tbody" "td" "textarea" "tfoot" "th" "thead" "title"
    "tr" "ul")
  "Head symbols that should count as Shoelace HSX elements."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-predicate-symbols
  '(activep alistp arrayp boundp consp endp evenp fboundp functionp integerp
    keywordp listp minusp numberp oddp packagep pathnamep plistp plusp
    rationalp realp streamp stringp symbolp typep vectorp zerop)
  "Explicit predicate symbols to highlight specially."
  :group 'luk/cl-highlighting
  :type 'sexp)

(defcustom luk/cl-predicate-suffix-regexp "[[:alnum:]-]+-p"
  "Regexp for hyphenated predicate symbols to highlight specially."
  :group 'luk/cl-highlighting
  :type 'regexp)

(defvar-local luk/cl--font-lock-queues nil
  "Per-buffer queue of structural font-lock matches for Common Lisp.")

(defvar-local luk/cl--font-lock-keywords nil
  "Per-buffer font-lock keyword list installed by `luk/common-lisp-font-lock-setup'.")

(defun luk/cl-apply-highlight-palette ()
  "Apply `luk/cl-highlight-style-alist' to the Common Lisp faces."
  (dolist (entry luk/cl-highlight-style-alist)
    (let* ((group (car entry))
           (style (cdr entry))
           (face  (alist-get group luk/cl-highlight-face-alist)))
      (when face
        (face-spec-set face `((t (:inherit default ,@style))))))))

;; Re-apply the palette whenever a theme is enabled (F9 toggles them).
(add-hook 'enable-theme-functions
          (lambda (_theme) (luk/cl-apply-highlight-palette)))

(defconst luk/cl-number-token-regexp
  "\\(?:[+-]?[0-9]+\\(?:/[0-9]+\\)?\\(?:\\.[0-9]+\\)?\\(?:[eEdDfFsSlL][+-]?[0-9]+\\)?\\|#[xX][0-9A-Fa-f]+\\|#[oO][0-7]+\\|#[bB][01]+\\)"
  "Regexp for a practical subset of Common Lisp numeric literal tokens.")

(defconst luk/cl-number-literal-regexp
  (format "\\(?:\\_<[+-]?[0-9]+\\(?:/[0-9]+\\)?\\(?:\\.[0-9]+\\)?\\(?:[eEdDfFsSlL][+-]?[0-9]+\\)?\\_>\\|#[xX][0-9A-Fa-f]+\\|#[oO][0-7]+\\|#[bB][01]+\\)")
  "Regexp for a practical subset of Common Lisp numeric literals in buffers.")

(defconst luk/cl-keyword-symbol-regexp
  "\\(?:\\`\\|[ \t\n('`#,]\\)\\(:\\(?:\\(?:\\sw\\|\\s_\\)+\\||[^|]+|\\)\\)"
  "Regexp for keyword symbols without matching package-qualified names.")

(defun luk/cl--normalized-name-list (items)
  "Return ITEMS converted to lowercase strings."
  (mapcar (lambda (item)
            (downcase (if (symbolp item) (symbol-name item) item)))
          items))

(defun luk/cl--list-take (count items)
  "Return the first COUNT ITEMS."
  (let (result)
    (while (and items (> count 0))
      (push (car items) result)
      (setq items (cdr items)
            count (1- count)))
    (nreverse result)))

(defun luk/cl--list-drop (count items)
  "Return ITEMS without the first COUNT elements."
  (while (and items (> count 0))
    (setq items (cdr items)
          count (1- count)))
  items)

(defun luk/cl--list-slice (start end items)
  "Return ITEMS from START through END, using 1-based inclusive indices."
  (let ((index 1)
        result)
    (dolist (item items (nreverse result))
      (when (and (<= start index)
                 (<= index end))
        (push item result))
      (setq index (1+ index)))))

(defun luk/cl--select-font-lock-keywords (keywords)
  "Return KEYWORDS filtered according to `luk/cl-font-lock-bisect'."
  (if-let* ((range luk/cl-font-lock-debug-range)
            (start (car-safe range))
            (end (cdr-safe range)))
      (luk/cl--list-slice start end keywords)
    (let ((split (/ (1+ (length keywords)) 2)))
      (pcase luk/cl-font-lock-bisect
        ('first-half (luk/cl--list-take split keywords))
        ('second-half (luk/cl--list-drop split keywords))
        (_ keywords)))))

(defun luk/cl--head-form-regexp (symbols)
  "Build a regexp that matches a list head in SYMBOLS."
  (when symbols
    (format "(\\(%s\\)\\_>" (regexp-opt (luk/cl--normalized-name-list symbols)))))

(defun luk/cl--predicate-symbol-regexp ()
  "Build a regexp for predicate symbols."
  (let ((parts (delq nil
                     (list (when luk/cl-predicate-symbols
                             (regexp-opt (luk/cl--normalized-name-list luk/cl-predicate-symbols)))
                           luk/cl-predicate-suffix-regexp))))
    (when parts
      (format "\\_<\\(%s\\)\\_>" (mapconcat #'identity parts "\\|")))))

(defun luk/cl--in-code-p (pos)
  "Return non-nil when POS is not in a string or comment."
  (let ((state (syntax-ppss pos)))
    (not (or (nth 3 state)
             (nth 4 state)))))

(defun luk/cl--token-name (bounds)
  "Return the downcased token text for BOUNDS."
  (downcase (buffer-substring-no-properties (car bounds) (cdr bounds))))

(defun luk/cl--number-token-p (token)
  "Return non-nil when TOKEN looks like a number literal."
  (string-match-p (concat "\\`" luk/cl-number-token-regexp "\\'") token))

(defun luk/cl--current-token-bounds ()
  "Return bounds for the token at point, or nil."
  (let* ((start (point))
         (end   (ignore-errors (scan-sexps start 1))))
    (when (and end
               (> end start)
               (luk/cl--in-code-p start)
               (not (memq (char-after start) '(?\( ?\) ?\" ?\;))))
      (cons start end))))

(defun luk/cl--structural-scan-start ()
  "Return a stable scan start for context-sensitive Common Lisp matchers."
  (save-excursion
    (or (ignore-errors
          (beginning-of-defun)
          (point))
        (point-min))))

(defun luk/cl--structural-scan-origin ()
  "Return an efficient scan origin for structural Common Lisp matchers."
  (max (point) (luk/cl--structural-scan-start)))

(defun luk/cl--queue-get (group)
  "Return the queued matches for GROUP."
  (alist-get group luk/cl--font-lock-queues))

(defun luk/cl--queue-put (group queue)
  "Store QUEUE for GROUP."
  (setf (alist-get group luk/cl--font-lock-queues) queue))

(defun luk/cl--queue-match (group start end)
  "Queue GROUP match from START to END."
  (when (and start end (< start end))
    (luk/cl--queue-put group
                       (nconc (luk/cl--queue-get group)
                              (list (cons start end))))))

(defun luk/cl--pop-match (group limit)
  "Pop one queued match for GROUP before LIMIT and expose it to font-lock."
  (let ((queue (luk/cl--queue-get group)))
    (while (and queue
                (< (caar queue) (point)))
      (setq queue (cdr queue)))
    (luk/cl--queue-put group queue)
    (when-let* ((range (car queue)))
      (when (< (car range) limit)
        (luk/cl--queue-put group (cdr queue))
        (set-match-data (list (car range) (cdr range)))
        (goto-char (cdr range))
        t))))

(defun luk/cl--clear-font-lock-queues (&rest _)
  "Clear queued Common Lisp font-lock ranges."
  (setq luk/cl--font-lock-queues nil))

(defun luk/cl--extend-font-lock-region-to-defun ()
  "Extend Common Lisp font-lock regions back to the containing top-level form.

Only extending the beginning keeps binding-site highlighting correct without
forcing jit-lock to refontify the rest of a large defun or ASDF form."
  (let ((changed nil))
    (save-excursion
      (goto-char font-lock-beg)
      (when (ignore-errors (beginning-of-defun) t)
        (when (< (point) font-lock-beg)
          (setq font-lock-beg (point)
                changed t))))
    changed))

(defun luk/cl--queued-matcher (group scanner limit)
  "Return the next queued GROUP match, using SCANNER up to LIMIT when needed."
  (or (luk/cl--pop-match group limit)
      (progn
        (save-excursion
          (funcall scanner limit))
        (luk/cl--pop-match group limit))))

(defun luk/cl--keyword-symbol-matcher (limit)
  "Match keyword symbols up to LIMIT without grabbing package prefixes."
  (when (re-search-forward luk/cl-keyword-symbol-regexp limit t)
    (let ((start (match-beginning 1))
          (end (match-end 1)))
      (set-match-data (list start end))
      (goto-char end)
      t)))

(defun luk/cl--queue-let-binding-ranges (bindings-start bindings-end)
  "Queue bound-name matches between BINDINGS-START and BINDINGS-END."
  (save-excursion
    (goto-char (1+ bindings-start))
    (while (< (point) (1- bindings-end))
      (forward-comment (point-max))
      (when (< (point) (1- bindings-end))
        (cond
         ((eq (char-after) ?\()
          (let ((binding-end (ignore-errors (scan-sexps (point) 1))))
            (when binding-end
              (save-excursion
                (forward-char 1)
                (forward-comment (point-max))
                (when-let* ((bounds (luk/cl--current-token-bounds)))
                  (luk/cl--queue-match 'bound-name (car bounds) (cdr bounds))))
              (goto-char binding-end))))
         (t
          (let ((binding-end (ignore-errors (scan-sexps (point) 1))))
            (when-let* ((bounds (luk/cl--current-token-bounds)))
              (luk/cl--queue-match 'bound-name (car bounds) (cdr bounds)))
            (goto-char (or binding-end bindings-end)))))))))

(defun luk/cl--scan-let-binding-names (limit)
  "Queue let-binding names up to LIMIT."
  (let ((regexp (luk/cl--head-form-regexp luk/cl-binding-introducers)))
    (when regexp
      (save-excursion
        (goto-char (luk/cl--structural-scan-origin))
        (while (re-search-forward regexp limit t)
          (when (luk/cl--in-code-p (match-beginning 1))
            (let ((bindings-start (save-excursion
                                    (goto-char (match-end 0))
                                    (forward-comment (point-max))
                                    (point))))
              (when (eq (char-after bindings-start) ?\()
                (when-let* ((bindings-end (ignore-errors (scan-sexps bindings-start 1))))
                  (luk/cl--queue-let-binding-ranges bindings-start bindings-end))))))))))

(defun luk/cl--scan-serapeum-types (start end)
  "Queue Serapeum type ranges found between START and END."
  (save-excursion
    (let ((syntax-names (luk/cl--normalized-name-list luk/cl-serapeum-declaration-syntax))
          (shared-control-names (luk/cl--normalized-name-list luk/cl-primitive-control-operators)))
      (goto-char start)
      (while (< (point) end)
	(forward-comment (point-max))
	(cond
	 ((>= (point) end))
	 ((eq (char-after) ?\()
          (let ((list-start (point))
		(list-end   (ignore-errors (scan-sexps (point) 1))))
            (if (not list-end)
		(goto-char end)
              (forward-char 1)
              (forward-comment (point-max))
              (when-let* ((head (luk/cl--current-token-bounds)))
		(let ((name (luk/cl--token-name head)))
                  (cond
                   ((member name syntax-names)
                    (luk/cl--queue-match 'serapeum-declaration-syntax (car head) (cdr head)))
                   ((string-prefix-p ":" name)
                    nil)
                   ((member name shared-control-names)
                    (luk/cl--queue-match 'primitive-control (car head) (cdr head)))
                   (t
                    (luk/cl--queue-match 'serapeum-type-constructor (car head) (cdr head)))))
		(goto-char (cdr head)))
              (luk/cl--scan-serapeum-types (point) (1- list-end))
              (goto-char list-end))))
	 (t
          (if-let* ((bounds (luk/cl--current-token-bounds)))
              (let ((name (luk/cl--token-name bounds)))
		(unless (luk/cl--number-token-p name)
                  (if (member name syntax-names)
                      (luk/cl--queue-match 'serapeum-declaration-syntax (car bounds) (cdr bounds))
                    (luk/cl--queue-match 'serapeum-type-atom (car bounds) (cdr bounds))))
		(goto-char (cdr bounds)))
            (forward-char 1))))))))

(defun luk/cl--scan-serapeum-declarations (limit)
  "Queue Serapeum declaration matches up to LIMIT."
  (let ((regexp (luk/cl--head-form-regexp luk/cl-serapeum-declaration-operators)))
    (when regexp
      (save-excursion
        (goto-char (luk/cl--structural-scan-origin))
        (while (re-search-forward regexp limit t)
          (when (luk/cl--in-code-p (match-beginning 1))
            (let ((form-start (match-beginning 0)))
              (when-let* ((form-end (ignore-errors (scan-sexps form-start 1))))
                (save-excursion
                  (goto-char (match-end 0))
                  (forward-comment (point-max))
                  (when-let* ((name-bounds (luk/cl--current-token-bounds)))
                    (luk/cl--queue-match 'serapeum-declared-name (car name-bounds) (cdr name-bounds))
                    (goto-char (cdr name-bounds))
                    (forward-comment (point-max))
                    (when-let* ((args-end (ignore-errors (scan-sexps (point) 1))))
                      (luk/cl--scan-serapeum-types (point) args-end)
                      (goto-char args-end)
                      (forward-comment (point-max))
                      (when (and (< (point) form-end)
                                 (ignore-errors (scan-sexps (point) 1)))
                        (luk/cl--scan-serapeum-types (point)
                                                     (ignore-errors (scan-sexps (point) 1)))))))))))))))

(defun luk/cl--hsx-element-token-p (token)
  "Return non-nil when TOKEN should be highlighted as an HSX element."
  (or (member token (luk/cl--normalized-name-list luk/cl-hsx-element-names))
      (catch 'match
        (dolist (prefix luk/cl-hsx-element-prefixes)
          (when (string-prefix-p (downcase prefix) token)
            (throw 'match t)))
        nil)))

(defun luk/cl--hsx-element-head-regexp ()
  "Build a regexp that matches likely HSX element list heads."
  (let* ((names (regexp-opt (luk/cl--normalized-name-list luk/cl-hsx-element-names)))
         (prefixes (mapcar (lambda (prefix)
                             (format "%s[[:alnum:]-]+" (regexp-quote (downcase prefix))))
                           luk/cl-hsx-element-prefixes))
         (pattern (if prefixes
                      (mapconcat #'identity (cons names prefixes) "\\|")
                    names)))
    (format "(\\(%s\\)\\(?:\\_>\\|\\s-\\|)\\)" pattern)))

(defun luk/cl--match-bound-names (limit)
  "Match Common Lisp binding names up to LIMIT."
  (luk/cl--queued-matcher 'bound-name #'luk/cl--scan-let-binding-names limit))

(defun luk/cl--match-serapeum-declared-names (limit)
  "Match Serapeum declared names up to LIMIT."
  (luk/cl--queued-matcher 'serapeum-declared-name #'luk/cl--scan-serapeum-declarations limit))

(defun luk/cl--match-serapeum-declaration-syntax (limit)
  "Match Serapeum declaration syntax up to LIMIT."
  (luk/cl--queued-matcher 'serapeum-declaration-syntax #'luk/cl--scan-serapeum-declarations limit))

(defun luk/cl--match-serapeum-primitive-controls (limit)
  "Match primitive control operators used inside Serapeum type syntax."
  (luk/cl--queued-matcher 'primitive-control #'luk/cl--scan-serapeum-declarations limit))

(defun luk/cl--match-serapeum-type-constructors (limit)
  "Match Serapeum type constructors up to LIMIT."
  (luk/cl--queued-matcher 'serapeum-type-constructor #'luk/cl--scan-serapeum-declarations limit))

(defun luk/cl--match-serapeum-type-atoms (limit)
  "Match Serapeum type atoms up to LIMIT."
  (luk/cl--queued-matcher 'serapeum-type-atom #'luk/cl--scan-serapeum-declarations limit))

(defun luk/cl--build-font-lock-keywords ()
  "Return the custom Common Lisp font-lock keywords in precedence order."
  (luk/cl--select-font-lock-keywords
   (delq
    nil
    (list
     '("\\<[[:alnum:]-]+!\\>" 0 'luk/lisp-mutation-face t)
     '("\\<[[:alnum:]-]+->+[[:alnum:]-]+\\>" . font-lock-type-face)
     '("(\\([[:alnum:]-]+\\)\\>[[:space:]]" 1 font-lock-function-name-face)
     '("(\\(defun\\|defmacro\\|defmethod\\)\\s-+\\([[:alnum:]><-]+\\)"
       (1 font-lock-keyword-face)
       (2 'luk/lisp-definition-face t))
     '("(\\(defvar\\|defparameter\\|defconstant\\)\\s-+\\([[:alnum:]><-+*]+\\)"
       (1 font-lock-keyword-face)
       (2 font-lock-variable-name-face t))
     '("(\\(defstruct\\|mit:deftable\\|defroute\\|defclass\\|deftype\\|defpackage\\)\\s-+\\([[:alnum:]-]+\\)"
       (1 font-lock-keyword-face)
       (2 'luk/lisp-definition-face t))
     '("(lambda\\s-+\\((.*?)\\)" 1 font-lock-variable-name-face t)
     '("(\\(if\\|cond\\|when\\|unless\\|case\\|ecase\\|prog\\|block\\|return\\|return-from\\|do\\|dotimes\\|dolist\\|loop\\)\\>"
       1 'luk/lisp-mutation-face t)
     '("#\\\\\\w+" . font-lock-string-face)
     '("\\<:\\(for\\|while\\|collect\\|append\\|sum\\|into\\|finally\\|from\\|to\\|by\\|across\\|in\\)\\>"
       0 font-lock-type-face t)
     `(,luk/cl-keyword-symbol-regexp
       (1 'luk/cl-keyword-symbol-face t))
     `(,luk/cl-number-literal-regexp
       (0 'luk/cl-number-face t))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-primitive-control-operators)))
       `(,regexp (1 'luk/cl-primitive-control-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-call-operators)))
       `(,regexp (1 'luk/cl-call-operator-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-sequence-operators)))
       `(,regexp (1 'luk/cl-sequence-operator-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-place-operators)))
       `(,regexp (1 'luk/cl-place-operator-face t)))
     (when-let* ((regexp (luk/cl--predicate-symbol-regexp)))
       `(,regexp (1 'luk/cl-predicate-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-binding-introducers)))
       `(,regexp (1 'luk/cl-binding-introducer-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-test-assertion-macros)))
       `(,regexp (1 'luk/cl-test-assertion-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-test-declaration-macros)))
       `(,regexp (1 'luk/cl-test-declaration-face t)))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-serapeum-declaration-operators)))
       `(,regexp (1 'luk/cl-serapeum-introducer-face t)))
     '(luk/cl--match-serapeum-declaration-syntax
       (0 'luk/cl-serapeum-declaration-syntax-face t))
     '(luk/cl--match-serapeum-primitive-controls
       (0 'luk/cl-primitive-control-face t))
     '(luk/cl--match-serapeum-type-atoms
       (0 'luk/cl-serapeum-type-atom-face t))
     '(luk/cl--match-serapeum-type-constructors
       (0 'luk/cl-serapeum-type-constructor-face t))
     '(luk/cl--match-serapeum-declared-names
       (0 'luk/cl-serapeum-declared-name-face t))
     (when-let* ((regexp (luk/cl--head-form-regexp luk/cl-hsx-introducers)))
       `(,regexp (1 'luk/cl-hsx-element-face t)))
     `(,(luk/cl--hsx-element-head-regexp)
       (1 'luk/cl-hsx-element-face t))
     '(luk/cl--match-bound-names
       (0 'luk/cl-bound-name-face t))))))

(defun luk/common-lisp-font-lock-setup ()
  "Install the custom Common Lisp highlighting rules."
  (luk/cl-apply-highlight-palette)
  (remove-hook 'before-change-functions #'luk/cl--clear-font-lock-queues t)
  (add-hook 'before-change-functions #'luk/cl--clear-font-lock-queues nil t)
  (remove-hook 'font-lock-extend-region-functions
               #'luk/cl--extend-font-lock-region-to-defun t)
  (add-hook 'font-lock-extend-region-functions
            #'luk/cl--extend-font-lock-region-to-defun nil t)
  (luk/cl--clear-font-lock-queues)
  (when luk/cl--font-lock-keywords
    (font-lock-remove-keywords nil luk/cl--font-lock-keywords))
  (setq-local luk/cl--font-lock-keywords (luk/cl--build-font-lock-keywords))
  (font-lock-add-keywords nil luk/cl--font-lock-keywords 'prepend)
  (when font-lock-mode
    (font-lock-flush)))

(defun luk/lisp-prettify-setup ()
  "Enable pretty lambda in Lisp buffers."
  (setq-local prettify-symbols-alist
              (cons '("lambda" . ?λ) prettify-symbols-alist))
  (prettify-symbols-mode 1))

(defun luk/common-lisp-outline-setup ()
  "Enable outline navigation in Common Lisp comments."
  (setq-local outline-regexp ";;;;\\(;*\\)")
  (outline-minor-mode 1))

(add-hook 'lisp-mode-hook #'luk/common-lisp-font-lock-setup)
(add-hook 'lisp-mode-hook #'luk/lisp-prettify-setup)
(add-hook 'lisp-mode-hook #'luk/common-lisp-outline-setup)

(setq outline-minor-mode-cycle t)

(provide 'cl-highlight)
;;; cl-highlight.el ends here

;;;; The look: a palette of named styles, styled text, bars.
;;;;
;;;; A line of output is a list of spans; a span is a string (plain) or
;;;; (ROLE . STRING), drawn with the style ROLE has in *PALETTE*.  Repaint from
;;;; init.lisp with (setf (style :label) (c:make-style ...)).
(in-package #:lfetch)

(defun indexed (index fallback &rest attributes)
  (apply #'c:make-style :foreground (c:indexed-color index :fallback fallback) attributes))

(defun on-status (&rest attributes)
  (apply #'c:make-style :background (c:indexed-color 236 :fallback :black) attributes))

(defvar *palette*
  (list :logo-1 (indexed 157 :green :bold t)
        :logo-2 (indexed 121 :green :bold t)
        :logo-3 (indexed 85 :green :bold t)
        :logo-4 (indexed 84 :green :bold t)
        :logo-5 (indexed 78 :green :bold t)
        :logo-6 (indexed 72 :green :bold t)
        :title (c:make-style :bold t)
        :subtitle (c:make-style :faint t)
        :rule (c:make-style :faint t)
        :herald (c:make-style :italic t)
        :label (c:make-style :faint t)
        :value nil
        :accent (c:make-style :foreground :magenta :bold t)
        :good (c:make-style :foreground :green)
        :warn (c:make-style :foreground :yellow)
        :bad (c:make-style :foreground :red :bold t)
        :empty (c:make-style :faint t)
        :status (on-status :foreground :bright-white)
        :status-dim (on-status :foreground :white)
        :status-accent (on-status :foreground :bright-magenta :bold t))
  "Role keyword -> colorist style, or NIL for plain text.")

(defun style (role) (getf *palette* role))
(defun (setf style) (new role) (setf (getf *palette* role) new))

(defun span-text (span) (if (consp span) (cdr span) span))

(defun line-width (spans)
  (reduce #'+ spans :key (lambda (s) (c:visible-length (span-text s)))))

(defun render-line (spans &optional (stream *standard-output*))
  (dolist (span spans)
    (let ((style (and (consp span) (style (car span)))))
      (write-string (if style (c:colorize (cdr span) style) (span-text span)) stream))))

(defun truncate-line (spans width)
  "SPANS cut to WIDTH visible characters, ending in an ellipsis when cut."
  (if (<= (line-width spans) width)
      spans
      (loop with room = (1- width)
            for span in spans
            for text = (span-text span)
            while (plusp room)
            collect (let ((part (subseq text 0 (min room (length text)))))
                      (decf room (length part))
                      (if (consp span) (cons (car span) part) part))
              into out
            finally (return (append out (list (cons :rule "…")))))))

(defun sep () '(:accent . " ∙ "))

(defun join (items &optional (separator (sep)))
  "Splice span lists ITEMS (NILs dropped) with SEPARATOR between them."
  (loop for (item . more) on (remove nil items)
        append (if (listp item) item (list item))
        when more collect separator))

(defun bar (fraction &key (width 10) (good-when :low))
  "A bar of WIDTH cells filled to FRACTION, coloured by how worrying it is:
GOOD-WHEN :low means a full bar is bad (memory), :high that an empty one is (battery)."
  (let* ((f (max 0 (min 1 fraction)))
         (filled (round (* f width)))
         (worry (if (eq good-when :low) f (- 1 f)))
         (role (cond ((< worry 0.6) :good) ((< worry 0.85) :warn) (t :bad))))
    (list (cons role (make-string filled :initial-element #\▰))
          (cons :empty (make-string (- width filled) :initial-element #\▱)))))

;;; Units.

(defun bytes (n)
  (let ((gib (/ n (expt 1024 3))))
    (if (>= gib 100) (format nil "~D GiB" (round gib)) (format nil "~,1F GiB" gib))))

(defun duration (seconds)
  (let* ((m (floor seconds 60)) (h (floor m 60)) (d (floor h 24)))
    (cond ((plusp d) (format nil "~Dd ~Dh ~2,'0Dm" d (mod h 24) (mod m 60)))
          ((plusp h) (format nil "~Dh ~2,'0Dm" h (mod m 60)))
          (t (format nil "~Dm" m)))))

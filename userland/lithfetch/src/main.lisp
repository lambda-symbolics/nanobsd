;;;; Running: options, the user's init.lisp, layout.
(in-package #:lithfetch)

(defvar *init-file* nil "Overrides the init file path (--init FILE).")
(defvar *max-width* 100 "The herald never grows wider than this.")

(defun init-path ()
  (or *init-file*
      (let ((xdg (env "XDG_CONFIG_HOME")) (home (env "HOME")))
        (cond (xdg (format nil "~A/lithfetch/init.lisp" xdg))
              (home (format nil "~A/.config/lithfetch/init.lisp" home))))))

(defun load-init ()
  "LOAD the user's init file into this package, raw: it may redefine anything.
An error in it is reported and the run goes on with whatever it did define."
  (let ((path (init-path)))
    (when (and path (probe-file path))
      (handler-case
          ;; Interpreted, without compiler chatter: an init file is a handful
          ;; of forms, and redefining things is the point of it.
          (let ((*package* (find-package '#:lithfetch))
                (sb-ext:*evaluator-mode* :interpret))
            (handler-bind ((warning #'muffle-warning))
              (load path)))
        (error (e)
          (format *error-output* "~&lithfetch: ~A: ~A~%" path e))))))

(defun pad-to (spans width)
  (append spans (list (make-string (max 0 (- width (line-width spans))) :initial-element #\Space))))

(defun draw (&optional (stream *standard-output*))
  (let* ((columns (terminal-columns))
         (logo-width (reduce #'max *logo* :key #'line-width :initial-value 0))
         (logo (and *logo* (>= columns (+ logo-width *logo-gap* 40)) *logo*))
         (left (if logo (+ logo-width *logo-gap*) 0))
         (width (max 20 (min *max-width* (- columns left 1))))
         (herald (mapcar (lambda (line) (truncate-line line width)) (herald-lines width)))
         (rows (max (length logo) (length herald)))
         (logo-top (floor (- rows (length logo)) 2))
         (herald-top (floor (- rows (length herald)) 2))
         (block-width (+ left (reduce #'max herald :key #'line-width :initial-value 0))))
    (terpri stream)
    (dotimes (row rows)
      (let ((logo-line (and logo (<= logo-top row) (nth (- row logo-top) logo)))
            (herald-line (and (<= herald-top row) (nth (- row herald-top) herald))))
        (render-line (append (and logo (pad-to logo-line left)) herald-line) stream)
        (terpri stream)))
    (when *status*
      (terpri stream)
      (render-line (status-line (min block-width (1- columns))) stream)
      (terpri stream))
    (terpri stream)))

(defun facts-sexp ()
  "Every fact but the raw ones, as one property list."
  (cons 'lithfetch
        (loop for name in (sort (loop for k being the hash-keys of *facts* collect k) #'string<)
              unless (member name '(:sysctl :dmesg :envstat))
                append (list name (fact name)))))

(defun usage ()
  (format t "usage: lithfetch [--sexp] [--no-init] [--init FILE] [--no-logo] [--no-status]

  --sexp       print what lithfetch knows as a Lisp property list
  --no-init    do not load ~~/.config/lithfetch/init.lisp
  --init FILE  load FILE instead
  --no-logo    leave the λ out
  --no-status  leave the status line out

init.lisp is LOADed into the LITHFETCH package after the defaults, so it can
change anything: DEFINE-FACT, DEFINE-ENTRY, *HERALD*, *PALETTE*, *LOGO*.~%"))

(defun main ()
  (clrhash *fact-cache*)
  (setf *start-rapl* (rapl-now))
  (let ((args (rest sb-ext:*posix-argv*)) (init t) (sexp nil))
    (loop while args
          do (let ((a (pop args)))
               (cond ((member a '("-h" "--help") :test #'string=) (usage) (return-from main))
                     ((string= a "--sexp") (setf sexp t))
                     ((string= a "--no-init") (setf init nil))
                     ((string= a "--init") (setf *init-file* (pop args)))
                     ((string= a "--no-logo") (setf *logo* nil))
                     ((string= a "--no-status") (setf *status* nil))
                     (t (format *error-output* "lithfetch: unknown option ~A~%" a)
                        (usage) (sb-ext:exit :code 2)))))
    (when init (load-init))
    (if sexp
        (let ((*print-case* :downcase) (*print-pretty* t) (*print-right-margin* 80)
              (*package* (find-package '#:lithfetch)))
          (prin1 (facts-sexp)) (terpri))
        (draw))
    (finish-output)))

;;;; freezeshot: freeze the screen, drag a rectangle on the frozen frame, and
;;;; save that part of it as PNG.  The capture is taken before anything is
;;;; shown, so whatever was on screen at the key press is what gets cut.
;;;;
;;;;   freezeshot FILE.png   -> prints FILE.png, exit 0; exit 1 if cancelled
;;;;
;;;; Drag with the left button.  A click (no drag) or Return takes the whole
;;;; output under the pointer.  Escape, q or the right button cancel, and so
;;;; does a minute without input.
;;;; The Wayland side (capture, overlay, input, PNG) is libfreezeshot.so.

(defpackage #:freezeshot
  (:use #:cl)
  (:export #:main))

(in-package #:freezeshot)

(sb-alien:define-alien-type nil
  (sb-alien:struct fz-event
    (type sb-alien:int)
    (output sb-alien:int)
    (x sb-alien:double)
    (y sb-alien:double)
    (code sb-alien:unsigned-int)
    (pressed sb-alien:int)))

(sb-alien:define-alien-routine ("fz_open" fz-open) sb-alien:system-area-pointer)
(sb-alien:define-alien-routine ("fz_error" fz-error) sb-alien:c-string
  (fz sb-alien:system-area-pointer))
(sb-alien:define-alien-routine ("fz_noutputs" fz-noutputs) sb-alien:int
  (fz sb-alien:system-area-pointer))
(sb-alien:define-alien-routine ("fz_output_width" fz-output-width) sb-alien:int
  (fz sb-alien:system-area-pointer) (i sb-alien:int))
(sb-alien:define-alien-routine ("fz_output_height" fz-output-height) sb-alien:int
  (fz sb-alien:system-area-pointer) (i sb-alien:int))
(sb-alien:define-alien-routine ("fz_show" fz-show) sb-alien:int
  (fz sb-alien:system-area-pointer))
(sb-alien:define-alien-routine ("fz_next_event" fz-next-event) sb-alien:int
  (fz sb-alien:system-area-pointer) (ev (* (sb-alien:struct fz-event))))
(sb-alien:define-alien-routine ("fz_select" fz-select) sb-alien:void
  (fz sb-alien:system-area-pointer) (i sb-alien:int)
  (x sb-alien:int) (y sb-alien:int) (w sb-alien:int) (h sb-alien:int))
(sb-alien:define-alien-routine ("fz_hide" fz-hide) sb-alien:void
  (fz sb-alien:system-area-pointer))
(sb-alien:define-alien-routine ("fz_save_png" fz-save-png) sb-alien:int
  (fz sb-alien:system-area-pointer) (i sb-alien:int)
  (x sb-alien:int) (y sb-alien:int) (w sb-alien:int) (h sb-alien:int)
  (path sb-alien:c-string))
(sb-alien:define-alien-routine ("fz_close" fz-close) sb-alien:void
  (fz sb-alien:system-area-pointer))

;;; Event types and evdev codes the C side passes through.
(defconstant +motion+ 1)
(defconstant +button+ 2)
(defconstant +key+ 3)
(defconstant +btn-left+ #x110)
(defconstant +btn-right+ #x111)
(defparameter *cancel-keys* '(1 16))    ; Escape, q
(defparameter *whole-keys* '(28 96))    ; Return, keypad Enter

(defun die (fmt &rest args)
  (format *error-output* "freezeshot: ~?~%" fmt args)
  (finish-output *error-output*)
  (sb-ext:exit :code 2 :abort t))

(defun clamp (v lo hi)
  (max lo (min hi v)))

(defun span (fz out ax ay x y)
  "The rectangle between the anchor and (X, Y) on output OUT, inside it."
  (let* ((w (fz-output-width fz out))
         (h (fz-output-height fz out))
         (x0 (clamp (min ax x) 0 w)) (x1 (clamp (max ax x) 0 w))
         (y0 (clamp (min ay y) 0 h)) (y1 (clamp (max ay y) 0 h)))
    (list out x0 y0 (- x1 x0) (- y1 y0))))

(defun whole (fz out)
  (list out 0 0 (fz-output-width fz out) (fz-output-height fz out)))

(defun select-region (fz)
  "Run the drag.  Return (OUTPUT X Y W H) in buffer pixels, or NIL."
  (sb-alien:with-alien ((ev (sb-alien:struct fz-event)))
    (let ((anchor nil)          ; (output x y) while the button is down
          (last nil)            ; the rectangle drawn last
          (pointer 0))          ; output the pointer was last seen on
      (flet ((field (name) (sb-alien:slot ev name)))
        (loop
          (when (minusp (fz-next-event fz (sb-alien:addr ev)))
            (return nil))
          (let ((type (field 'type))
                (out (field 'output))
                (x (round (field 'x)))
                (y (round (field 'y)))
                (code (field 'code))
                (pressed (= 1 (field 'pressed))))
            (when (>= out 0)
              (setf pointer out))
            (cond
              ((= type +motion+)
               (when (and anchor (= out (first anchor)))
                 (setf last (span fz out (second anchor) (third anchor) x y))
                 (apply #'fz-select fz last)))
              ((and (= type +button+) (= code +btn-left+) pressed (>= out 0))
               (setf anchor (list out x y)
                     last nil))
              ((and (= type +button+) (= code +btn-left+) anchor)
               (when (= out (first anchor))
                 (setf last (span fz out (second anchor) (third anchor) x y)))
               ;; A click, not a drag: the whole output.
               (return (if (or (null last)
                               (and (< (fourth last) 3) (< (fifth last) 3)))
                           (whole fz (first anchor))
                           last)))
              ((and (= type +button+) (= code +btn-right+) pressed)
               (return nil))
              ((and (= type +key+) pressed (member code *cancel-keys*))
               (return nil))
              ((and (= type +key+) pressed (member code *whole-keys*))
               (return (whole fz pointer))))))))))

(defun main ()
  ;; Killed while the overlay holds the keyboard: leave at once.
  (sb-sys:enable-interrupt sb-unix:sigterm
                           (lambda (&rest args)
                             (declare (ignore args))
                             (sb-ext:exit :code 1 :abort t)))
  (let ((args (rest sb-ext:*posix-argv*)))
    (when (or (/= (length args) 1)
              (member (first args) '("-h" "--help") :test #'string=))
      (format *error-output* "usage: freezeshot FILE.png~%")
      (sb-ext:exit :code 2 :abort t))
    (let ((path (first args))
          (fz (fz-open)))
      (when (zerop (sb-sys:sap-int fz))
        (die "out of memory"))
      (when (zerop (fz-noutputs fz))
        (die "~a" (fz-error fz)))
      (when (minusp (fz-show fz))
        (die "~a" (fz-error fz)))
      (let ((region (select-region fz)))
        (fz-hide fz)
        (unless region
          (fz-close fz)
          (sb-ext:exit :code 1 :abort t))
        (when (minusp (apply #'fz-save-png fz (append region (list path))))
          (die "~a: ~a" path (fz-error fz)))
        (fz-close fz)
        (format t "~a~%" path)
        (finish-output)
        (sb-ext:exit :code 0 :abort t)))))

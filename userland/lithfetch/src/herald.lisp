;;;; The herald, after the one a Symbolics machine printed when it booted:
;;;; system and boot file, the machine, the software loaded, then one line
;;;; per entry, and a status line at the bottom.
;;;;
;;;; An entry is a label and a body returning spans (or NIL to leave the line
;;;; out).  *HERALD* is the order; add, drop or redefine entries from init.lisp:
;;;;   (define-entry :weather "weather" (list (sh "/usr/local/bin/wttr")))
;;;;   (setf *herald* (remove :audio *herald*))
(in-package #:lithfetch)

(defvar *entries* (make-hash-table) "Entry name -> (label . function).")

(defmacro define-entry (name label &body body)
  "Define the herald line NAME, shown as LABEL; BODY returns spans or NIL."
  `(progn (setf (gethash ,name *entries*) (cons ,label (lambda () ,@body))) ,name))

(defvar *herald*
  '(:host :machine :processor :graphics :memory :storage :power :display
    :network :audio :shell :uptime)
  "The entries shown, in order.")

(defvar *label-width* 12)

(defun v (control &rest args) (cons :value (apply #'format nil control args)))
(defun l (control &rest args) (cons :label (apply #'format nil control args)))

(define-entry :host "host"
  (let ((host (fact :identity :host)) (user (fact :identity :user)))
    (and host (join (list (list (v "~A" host)) (and user (list (v "~A" user))))))))

(define-entry :machine "machine"
  (let ((p (fact :machine :product)))
    (and p (list (v "~@[~A ~]~A" (fact :machine :vendor) p)
                 (l "~@[ (~A)~]" (fact :machine :model))))))

(define-entry :processor "processor"
  (let ((cpu (fact :cpu)))
    (and (getf cpu :model)
         (join (list (list (v "~@[~A ~]~A" (getf cpu :vendor) (getf cpu :model)))
                     (and (getf cpu :threads)
                          (list (v "~@[~D cores, ~]~D threads" (getf cpu :cores) (getf cpu :threads))))
                     (and (getf cpu :ghz) (list (v "~,1F GHz" (getf cpu :ghz))))
                     (and (getf cpu :celsius) (list (v "~D °C" (round (getf cpu :celsius)))))
                     (and (getf cpu :epp)
                          (list (v "EPP ~D" (getf cpu :epp)) (l "~@[ ~A~]" (getf cpu :profile)))))))))

(define-entry :graphics "graphics"
  (let ((g (fact :graphics)))
    (and (getf g :name)
         (join (list (list (v "~A" (getf g :name))) (and (getf g :vaapi) (list (v "VA-API"))))))))

(define-entry :memory "memory"
  (let ((m (fact :memory)))
    (and m (append (bar (/ (getf m :used) (getf m :total)))
                   (list " " (v "~A" (bytes (getf m :used)))
                         (l " of ~A" (bytes (getf m :total))))))))

(define-entry :storage "storage"
  (let ((s (fact :storage)) (r (fact :root)))
    (and s (join (list (append (bar (/ (getf s :used) (getf s :size)))
                               (list " " (v "~A" (bytes (getf s :used)))
                                     (l " of ~A" (bytes (getf s :size)))))
                       (and r (list (v "~A ~A" (getf r :device) (getf r :type))
                                    (l "~@[ (~A)~]" (getf r :options)))))))))

(define-entry :power "power"
  (let ((b (fact :battery)) (w (fact :package-watts)))
    (and b (join (list (append (bar (/ (getf b :percent) 100) :good-when :high)
                               (list " " (v "~D%" (round (getf b :percent)))))
                       (list (v "~(~A~)~@[ ~,1F W~]" (getf b :state)
                                (and (getf b :watts) (plusp (getf b :watts)) (getf b :watts))))
                       (and (getf b :hours)
                            (list (v "~A" (duration (* 3600 (getf b :hours))))
                                  (l (if (eq (getf b :state) :charging) " to full" " left"))))
                       (and w (list (l "package ") (v "~,1F W" w))))))))

(define-entry :display "display"
  (let ((d (fact :display)))
    (and (getf d :compositor)
         (join (list (list (v "~A" (getf d :compositor)))
                     (and (getf d :size) (list (v "~{~D×~D~}" (getf d :size))))
                     (and (getf d :hz) (list (v "~D Hz" (getf d :hz))))
                     (and (getf d :brightness) (list (l "brightness ") (v "~D%" (getf d :brightness))))
                     (and (getf d :groups) (list (v "~D" (getf d :groups)) (l " groups"))))))))

(define-entry :network "network"
  (let ((ifs (fact :network)))
    (and ifs (join (loop for i in ifs
                         collect (list (v "~A" (or (getf i :ssid) (getf i :name)))
                                       (l " ~A" (getf i :inet))))))))

(define-entry :audio "audio"
  (let ((a (fact :audio)))
    (and (getf a :percent)
         (join (list (and (getf a :codec) (list (v "~A" (getf a :codec))))
                     (list (l "volume ") (v "~D%" (getf a :percent))))))))

(define-entry :shell "shell"
  (let ((s (fact :shell)) (term (fact :terminal)))
    (and (getf s :name)
         (join (list (list (v "~A~@[ ~A~]" (getf s :name) (getf s :version)))
                     (and term (list (v "~A" term))))))))

(define-entry :uptime "uptime"
  (let ((u (fact :uptime))) (and u (list (v "~A" (duration u))))))

;;; The heading: system, boot file, machine, software.

(defun software ()
  "The systems loaded, Genera style: a list of name-and-version strings."
  (let ((k (fact :kernel)) (sh (fact :shell)) (lisp (fact :lisp)) (al (fact :autolith))
        (d (fact :display)) (pkgs (fact :packages)))
    (remove nil
            (list (and (getf k :os) (format nil "~A ~A" (getf k :os) (getf k :release)))
                  (and (getf lisp :type) (format nil "~A ~A" (getf lisp :type) (getf lisp :version)))
                  (and (getf sh :version) (format nil "~A ~A" (getf sh :name) (getf sh :version)))
                  (getf d :compositor)
                  (and al (format nil "Autolith ~A" (getf al :version)))
                  (and pkgs (format nil "~D packages" pkgs))))))

(defun wrap-items (items width)
  "Lines of span lists packing the strings ITEMS, separated by (SEP), into WIDTH."
  (let (lines current (used 0))
    (dolist (item items)
      (let ((need (+ (length item) (if current 3 0))))
        (when (and current (> (+ used need) width))
          (push (nreverse current) lines) (setf current nil used 0 need (length item)))
        (when current (push (sep) current))
        (push (cons :value item) current)
        (incf used need)))
    (when current (push (nreverse current) lines))
    (nreverse lines)))

(defun heading (width)
  (let* ((k (fact :kernel)) (root (fact :root)) (m (fact :machine)))
    (append
     (list (list '(:title . "LispBSD System")
                 (cons :subtitle (format nil "~@[, ~A~]~@[:>~A~]"
                                         (getf root :device)
                                         (let ((f (getf k :file))) (and f (string-left-trim "/" f))))))
           (list (cons :herald (format nil "~@[~A ~]Lisp Machine~@[, ~A~]~@[ #~A~]~@[ (~A)~]"
                                       (getf m :product) (getf k :config) (getf k :build) (getf k :date))))
           (list (cons :rule (make-string (min width 46) :initial-element #\─))))
     (wrap-items (software) width))))

(defun herald-lines (width)
  (append (heading width)
          (list nil)
          (loop for name in *herald*
                for entry = (gethash name *entries*)
                for spans = (and entry (handler-case (funcall (cdr entry)) (error () nil)))
                when spans
                  collect (cons (cons :label (format nil "~vA" *label-width* (car entry))) spans))))

;;; The status line, as on a Lisp machine's screen.

(defvar *status* t "Draw the status line under the herald.")

(defun status-line (width)
  (multiple-value-bind (s mi h d mo y dow) (decode-universal-time (get-universal-time))
    (let* ((left (format nil " ~A ~D ~A ~D  ~2,'0D:~2,'0D:~2,'0D"
                         (aref #("Mon" "Tue" "Wed" "Thu" "Fri" "Sat" "Sun") dow) d
                         (aref #("Jan" "Feb" "Mar" "Apr" "May" "Jun" "Jul" "Aug" "Sep" "Oct" "Nov" "Dec")
                               (1- mo))
                         y h mi s))
           (user (format nil "   ~A" (or (fact :identity :user) "")))
           (package "   CL-USER:")
           (state "   User Input")
           (used (+ (length left) (length user) (length package) (length state))))
      (list (cons :status left) (cons :status-dim user) (cons :status-accent package)
            (cons :status state)
            (cons :status (make-string (max 1 (- width used)) :initial-element #\Space))))))

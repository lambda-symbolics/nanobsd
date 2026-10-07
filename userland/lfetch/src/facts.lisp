;;;; Facts: what lfetch knows about the machine.  Each fact is learned at
;;;; most once per run, lazily; one that fails is NIL and its herald line is
;;;; left out.  Redefine any of them from init.lisp with DEFINE-FACT.
(in-package #:lfetch)

(defvar *facts* (make-hash-table) "Fact name -> function of no arguments.")
(defvar *fact-cache* (make-hash-table))

(defmacro define-fact (name &body body)
  "Define how to learn the fact NAME (a keyword).  BODY runs at most once per
run; a signalled error makes the fact NIL."
  `(progn (setf (gethash ,name *facts*) (lambda () ,@body))
          (remhash ,name *fact-cache*)
          ,name))

(defun fact (name &optional key)
  "The fact NAME, or with KEY the value under KEY in its property list."
  (let ((value (multiple-value-bind (v found) (gethash name *fact-cache*)
                 (if found
                     v
                     (setf (gethash name *fact-cache*)
                           (let ((fn (gethash name *facts*)))
                             (and fn (handler-case (funcall fn) (error () nil)))))))))
    (if key (getf value key) value)))

;;; Raw material, one command each.

(defparameter *sysctl-names*
  '("kern.hostname" "kern.version" "kern.ostype" "kern.osrelease" "hw.ncpu"
    "hw.physmem64" "machdep.cpu_brand" "machdep.dmi.system-vendor"
    "machdep.dmi.system-product" "machdep.dmi.system-version" "machdep.booted_kernel"
    "machdep.hwp.epp" "machdep.lpsched.enabled" "machdep.cidle.rapl"
    "hw.acpi.acpiout15.brightness" "hw.acpi.sleep.freeze_keep_net"))

(define-fact :sysctl
  (let ((table (make-hash-table :test 'equal)))
    (dolist (line (lines (apply #'sh "/sbin/sysctl" *sysctl-names*)) table)
      (let ((p (search " = " line)))
        (when p (setf (gethash (subseq line 0 p) table) (subseq line (+ p 3))))))))

(defun sysctl (name) (let ((table (fact :sysctl))) (and table (gethash name table))))

(define-fact :dmesg (lines (file-text "/var/run/dmesg.boot")))

(defun dmesg (prefix)
  "The first boot message line starting with PREFIX."
  (find-if (lambda (line) (starts-with prefix line)) (fact :dmesg)))

(define-fact :envstat
  ;; ((device sensor value-words) ...) from envstat(8)
  (let (device rows)
    (dolist (line (lines (sh "/usr/sbin/envstat")) (nreverse rows))
      (let ((s (trim line)))
        (cond ((and (starts-with "[" s) (find #\] s)) (setf device (subseq s 1 (position #\] s))))
              ((and device (find #\: s))
               (let ((colon (position #\: s)))
                 (push (list device (trim (subseq s 0 colon)) (words (subseq s (1+ colon))))
                       rows))))))))

(defun sensor (device name)
  "Value words of sensor NAME on DEVICE, e.g. (\"17.540\" ... \"Wh\")."
  (third (find-if (lambda (row) (and (string= device (first row)) (string= name (second row))))
                  (fact :envstat))))

(defun sensor-number (device name)
  (let ((v (first (sensor device name)))) (and v (number-in v))))

;;; What the herald shows.

(define-fact :identity
  (list :host (sysctl "kern.hostname") :user (or (env "USER") (env "LOGNAME"))))

(define-fact :machine
  (list :vendor (let ((v (sysctl "machdep.dmi.system-vendor")))
                  (and v (if (every (lambda (c) (or (upper-case-p c) (not (alpha-char-p c)))) v)
                             (string-capitalize v)   ; "LENOVO" -> "Lenovo"
                             v)))
        :product (sysctl "machdep.dmi.system-version")   ; "ThinkPad X1 Nano Gen 1"
        :model (sysctl "machdep.dmi.system-product")))   ; "20UN000EUS"

(define-fact :kernel
  ;; "NetBSD 11.0 (GENERIC) #133: Fri Oct  2 18:25:14 CEST 2026"
  (let* ((v (sysctl "kern.version"))
         (w (and v (words v))))
    (list :os (sysctl "kern.ostype") :release (sysctl "kern.osrelease")
          :config (let ((c (find-if (lambda (x) (starts-with "(" x)) w))) (and c (string-trim "()" c)))
          :build (let ((b (find-if (lambda (x) (starts-with "#" x)) w))) (and b (string-trim "#:" b)))
          :date (and w (>= (length w) 7) (format nil "~A ~A" (nth 5 w) (nth 6 w)))
          :file (sysctl "machdep.booted_kernel"))))

(define-fact :root
  ;; "/dev/dk1 on / type ffs (log, noatime, local)"
  (let ((line (find-if (lambda (l) (search " on / type " l)) (lines (sh "/sbin/mount")))))
    (and line (list :device (subseq (first (words line)) (length "/dev/"))
                    :type (fifth (words line))
                    :options (let ((o (after "(" line))) (and o (string-right-trim ")" o)))))))

(define-fact :uptime
  (let ((boot (number-in (or (sh "/sbin/sysctl" "-n" "kern.boottime") ""))))
    (and boot (- (unix-time) boot))))

(defparameter *epp-profiles*
  '((32 . "perf") (128 . "balanced") (192 . "load") (224 . "batch") (240 . "powersave"))
  "lpschedd's profiles by the EPP each sets.")

(define-fact :cpu
  (let* ((brand (sysctl "machdep.cpu_brand"))
         (model (and brand (or (find-if (lambda (w) (find #\- w)) (words brand)) brand)))
         (cores (remove-duplicates
                 (loop for l in (fact :dmesg)
                       for c = (after ", core " l)
                       when (and (starts-with "cpu" l) c) collect (parse-integer c :junk-allowed t))))
         (max (let ((l (dmesg "cpu0: CPU max freq"))) (and l (number-in (after "freq" l)))))
         (epp (let ((e (sysctl "machdep.hwp.epp"))) (and e (parse-integer e :junk-allowed t))))
         (temps (loop for (dev name vals) in (fact :envstat)
                      when (and (starts-with "coretemp" dev) (search "temperature" name))
                        collect (number-in (first vals)))))
    (list :vendor (cond ((search "Intel" brand) "Intel") ((search "AMD" brand) "AMD"))
          :model model
          :threads (let ((n (sysctl "hw.ncpu"))) (and n (parse-integer n)))
          :cores (and cores (length cores))
          :ghz (and max (/ max 1d9))
          :epp epp
          :profile (cdr (assoc epp *epp-profiles*))
          :celsius (and temps (reduce #'max (remove nil temps))))))

(define-fact :graphics
  (let ((l (dmesg "i915drmkms0 at")))
    (list :name (and l (let ((n (trim (after ": " l))))
                         (trim (subseq n 0 (or (search "(rev." n) (length n))))))
          :vaapi (and (probe-file "/usr/pkg/lib/dri/iHD_drv_video.so") t))))

(define-fact :memory
  (let* ((total (let ((m (sysctl "hw.physmem64"))) (and m (parse-integer m))))
         (vm (lines (sh "/usr/bin/vmstat" "-s")))
         (page (or (loop for l in vm when (search "bytes per page" l) return (number-in l)) 4096)))
    (flet ((pages (what) (or (loop for l in vm when (search what l) return (number-in l)) 0)))
      (and total
           (let ((available (* page (+ (pages "pages free") (pages "cached file pages")))))
             (list :total total :used (max 0 (- total available))))))))

(define-fact :storage
  (let* ((l (second (lines (sh "/bin/df" "-k" "/"))))
         (w (and l (words l))))
    (and w (list :size (* 1024 (parse-integer (second w)))
                 :used (* 1024 (parse-integer (third w)))
                 :disk (let ((d (dmesg "ld0:"))) (and d (number-in (after "ld0:" d))))))))

(defvar *start-rapl* nil "RAPL energy counter and real time when the run began.")

(defun rapl-now ()
  (let ((r (sysctl-fresh "machdep.cidle.rapl")))
    (and r (cons (number-in (after "raw=" r)) (get-internal-real-time)))))

(defun sysctl-fresh (name)
  (let ((s (sh "/sbin/sysctl" "-n" name))) (and s (trim (first (lines s))))))

(define-fact :package-watts
  ;; Package energy (RAPL, 61.035 uJ units) over the run, so far.
  (let ((end (rapl-now)))
    (and *start-rapl* end
         (let ((de (mod (- (car end) (car *start-rapl*)) (expt 2 32)))
               (dt (/ (- (cdr end) (cdr *start-rapl*)) internal-time-units-per-second)))
           (and (> dt 0.05) (/ (* de 61.035d-6) dt))))))

(define-fact :battery
  (let ((charge (sensor-number "acpibat0" "charge"))
        (full (sensor-number "acpibat0" "last full cap"))
        (in (sensor-number "acpibat0" "charge rate"))
        (out (sensor-number "acpibat0" "discharge rate"))
        (charging (equal (first (sensor "acpibat0" "charging")) "TRUE"))
        (ac (equal (first (sensor "acpiacad0" "connected")) "TRUE")))
    (and charge full (plusp full)
         (list :percent (* 100 (/ charge full))
               :state (cond (charging :charging) (ac :full) (t :discharging))
               :watts (if charging in out)
               :hours (cond ((and charging in (plusp in)) (/ (- full charge) in))
                            ((and (not ac) out (plusp out)) (/ charge out)))))))

(defun mahogany (form)
  "Evaluate FORM (a string) in the running Mahogany, or NIL.  mahoganyctl
reads one line, so newlines become spaces."
  (and (or (env "WAYLAND_DISPLAY") (env "XDG_RUNTIME_DIR"))
       (let ((out (sh "/usr/local/bin/mahoganyctl" (substitute #\Space #\Newline form))))
         (and out (ignore-errors (let ((*read-eval* nil)) (read-from-string out)))))))

(define-fact :display
  (let ((m (mahogany "(list mahogany::*bar-refresh-hz*
                             (length (mahogany::state-groups mahogany::*compositor-state*))
                             (ignore-errors
                              (multiple-value-list
                               (hrt:output-resolution
                                (mahogany/tree:output-container-output
                                 (aref (mahogany::state-cur-outputs mahogany::*compositor-state*) 0))))))")))
    (list :compositor (cond (m "Mahogany") ((env "DISPLAY") "X11") ((env "WAYLAND_DISPLAY") "Wayland"))
          :hz (first m) :groups (second m) :size (third m)
          :brightness (let ((b (sysctl "hw.acpi.acpiout15.brightness"))) (and b (parse-integer b :junk-allowed t))))))

(define-fact :network
  ;; Interfaces with an IPv4 address, from ifconfig(8), and the Wi-Fi SSID.
  (let (blocks)
    (dolist (line (lines (sh "/sbin/ifconfig" "-a")))
      (if (and (plusp (length line)) (not (member (char line 0) '(#\Space #\Tab))))
          (push (list line) blocks)
          (when blocks (push line (first blocks)))))
    (loop for block in (reverse blocks)
          for lines = (reverse block)
          for name = (subseq (first lines) 0 (position #\: (first lines)))
          for inet = (loop for l in lines
                           when (starts-with "inet " (trim l))
                             return (let ((a (second (words l)))) (subseq a 0 (position #\/ a))))
          for ssid = (loop for l in lines
                           when (starts-with "ssid " (trim l)) return (second (words l)))
          when (and inet (string/= name "lo0"))
            collect (list :name name :inet inet :ssid ssid))))

(define-fact :audio
  (let ((m (sh "/usr/bin/mixerctl" "-n" "outputs.master"))
        (codec (dmesg "hdafg0 at")))
    (list :percent (and m (round (* 100 (number-in m)) 255))
          :codec (and codec (let ((c (after ": " codec)))
                              (if (search "Realtek product 0287" c) "Realtek ALC287" c))))))

(define-fact :packages
  (length (directory "/usr/pkg/pkgdb/*/")))

(define-fact :shell
  (let* ((path (or (env "SHELL") ""))
         (name (subseq path (1+ (or (position #\/ path :from-end t) -1)))))
    (list :name name
          :version (and (string= name "cclsh")
                        (let ((asd (file-text "/usr/local/lib/cclsh/src/cclsh/cclsh.asd")))
                          (and asd (let ((v (after ":version \"" asd)))
                                     (and v (subseq v 0 (position #\" v))))))))))

(define-fact :lisp
  (list :type (lisp-implementation-type)
        :version (let ((v (lisp-implementation-version)))
                   (subseq v 0 (or (position #\- v) (length v))))))

(define-fact :autolith
  (let* ((home (env "HOME"))
         (releases (and home (directory (format nil "~A/.local/share/autolith/installation/releases/*/" home))))
         (versions (sort (mapcar (lambda (p) (let ((n (car (last (pathname-directory p)))))
                                               (subseq n 0 (or (position #\- n) (length n)))))
                                 releases)
                         #'string> :key (lambda (v) (format nil "~{~5,'0D~}"
                                                           (mapcar (lambda (x) (or (parse-integer x :junk-allowed t) 0))
                                                                   (uiop-split (string-left-trim "v" v) #\.))))))
         (running (length (lines (sh "/usr/bin/pgrep" "-f" "autolith")))))
    (and versions (list :version (string-left-trim "v" (first versions)) :processes running))))

(defun uiop-split (string char)
  (loop with start = 0
        for p = (position char string :start start)
        collect (subseq string start p)
        while p do (setf start (1+ p))))

(define-fact :terminal
  (cond ((env "ALACRITTY_WINDOW_ID") "alacritty")
        ((env "TERM_PROGRAM"))
        ((env "TERM"))))

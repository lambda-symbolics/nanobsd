;;;; lsblk [-p|-c] [disk ...]: the disks and what is on them.  GPT wedges or
;;;; disklabel partitions with sizes in real units (KiB/MiB/GiB), the file
;;;; system as fstyp(8) probes it, the GPT label, mount point with options
;;;; and usage, swap use, and for NVMe drives the model, serial, firmware,
;;;; sector size, temperature, wear, hours and data written.  Colour on a
;;;; terminal; -p plain, -c forces colour, NO_COLOR honoured.
;;;; LispBSD, 2026-10-12.
(defpackage #:lsblk
  (:use #:cl)
  (:local-nicknames (#:c #:colorist))
  (:export #:main))
(in-package #:lsblk)

;;; Running things

(defun sh (program &rest args)
  "Standard output of PROGRAM ARGS as a string, or NIL."
  (handler-case
      (let ((text (with-output-to-string (out)
                    (sb-ext:run-program program args :search t :input nil :output out :error nil))))
        (and (plusp (length text)) text))
    (error () nil)))

(defun lines (text)
  (and text (loop for line in (split text #\Newline) when (plusp (length line)) collect line)))

(defun split (string char)
  (loop with start = 0
        for pos = (position char string :start start)
        collect (subseq string start pos)
        while pos do (setf start (1+ pos))))

(defun words (string)
  (remove "" (split (substitute #\Space #\Tab string) #\Space) :test #'string=))

(defun trim (s) (string-trim '(#\Space #\Tab) s))

(defun int (s) (and s (parse-integer s :junk-allowed t)))

(defun starts-with (prefix s) (and (>= (length s) (length prefix)) (string= prefix s :end2 (length prefix))))

(defun env (name) (sb-ext:posix-getenv name))

;;; Facts about a disk

(defstruct disk name bytes block-size model serial firmware sector temp wear hours written unsafe media-errors parts)
(defstruct part name blocks label type)

(defun geometry (disk)
  "Bytes and block size from dkctl getgeometry."
  (let* ((text (sh "dkctl" disk "getgeometry"))
         (line (find-if (lambda (l) (search " bytes in " l)) (lines text))))
    (when line
      (let ((w (words line)))
        ;; /dev/rld0: 512110190592 bytes in 1000215216 blocks of 512 bytes
        (values (int (second w)) (int (nth 7 w)))))))

(defun wedges (disk)
  "The dkN wedges of DISK: name, blocks, label, type."
  (loop for line in (lines (sh "dkctl" disk "listwedges"))
        for w = (words line)
        when (and (first w) (starts-with "dk" (first w)) (char= #\: (char (first w) (1- (length (first w))))))
          collect (let* ((name (string-right-trim ":" (first w)))
                         (rest (trim (subseq line (1+ (position #\: line)))))
                         (type-at (search ", type: " rest :from-end t))
                         (type (and type-at (subseq rest (+ type-at 8))))
                         (rest (if type-at (subseq rest 0 type-at) rest))
                         (blocks-at (search ", " rest :from-end t))
                         (blocks (and blocks-at (int (subseq rest (+ blocks-at 2)))))
                         (label (if blocks-at (subseq rest 0 blocks-at) rest)))
                    (make-part :name name :blocks (or blocks 0) :type (or type "")
                               :label (if (uuid-p label) "" label)))))

(defun uuid-p (s)
  (and (= (length s) 36) (every (lambda (ch) (or (digit-char-p ch 16) (char= ch #\-))) s)))

(defun label-parts (disk)
  "Disklabel partitions, for disks without wedges."
  (loop for line in (lines (sh "disklabel" disk))
        for w = (words line)
        when (and (>= (length w) 3) (= (length (first w)) 2) (char= #\: (char (first w) 1))
                  (char<= #\a (char (first w) 0) #\p) (int (second w)) (plusp (int (second w)))
                  (not (string= (nth 3 w) "unused")))
          collect (make-part :name (format nil "~A~C" disk (char (first w) 0))
                             :blocks (int (second w)) :type (nth 3 w) :label "")))

(defun nvme-of (disk)
  (or (loop for line in (lines (sh "dmesg"))
            for at = (search (format nil " ~A at nvme" disk) line)
            when at return (subseq line (+ at 1 (length disk) 4)
                                   (position #\Space line :start (+ at 1 (length disk) 4))))
      (format nil "nvme~A" (subseq disk 2))))

(defun field (lines label &optional (take #'last-word))
  (let ((line (find-if (lambda (l) (starts-with label (trim l))) lines)))
    (and line (funcall take (trim (subseq (trim line) (length label)))))))

(defun last-word (s) (car (last (words s))))

(defun nvme-queries (n)
  "The three nvmectl outputs for controller N: identify, namespace, SMART.
nvmectl opens the controller read-write, so an unprivileged run gets them
through doas in one go (the doas rule on this laptop needs no password)."
  (let ((id (sh "nvmectl" "identify" n)))
    (if id
        (list id (sh "nvmectl" "identify" (format nil "~Ans1" n)) (sh "nvmectl" "logpage" "-p" "2" n))
        (let* ((script (format nil "nvmectl identify ~A; echo @@; nvmectl identify ~Ans1; echo @@; nvmectl logpage -p 2 ~A" n n n))
               (text (and (zerop (sb-unix:unix-getuid)) nil))
               (text (or text (and (not (zerop (sb-unix:unix-getuid))) (sh "doas" "sh" "-c" script)))))
          (when text
            (loop with parts = (list "" "" "")
                  with i = 0
                  for line in (split text #\Newline)
                  do (if (string= line "@@") (incf i)
                         (when (< i 3) (setf (nth i parts) (format nil "~A~A~%" (nth i parts) line))))
                  finally (return parts)))))))

(defun nvme-facts (d)
  (let* ((n (nvme-of (disk-name d)))
         (q (nvme-queries n))
         (id (lines (first q)))
         (ns (lines (second q)))
         (smart (lines (third q))))
    (when id
      (setf (disk-model d) (substitute #\Space #\_ (field id "Model Number:" #'identity))
            (disk-serial d) (field id "Serial Number:")
            (disk-firmware d) (field id "Firmware Version:"))
      (let* ((current (field ns "Current LBA Format:"))     ; "LBA Format #00" -> "#00"
             (fmt (and current (find-if (lambda (l) (starts-with (format nil "LBA Format ~A:" current) (trim l))) ns))))
        (when fmt (setf (disk-sector d) (int (nth 5 (words fmt))))))
      (when smart
        (let ((temp (find-if (lambda (l) (and (starts-with "Temperature:" (trim l)) (search " K," l))) smart)))
          ;; "Temperature:  306 K, 32.85 C, 91.13 F"; the indented one above it is a warning flag
          (when temp (setf (disk-temp d) (ignore-errors (round (read-from-string (nth 3 (words temp))))))))
        (setf (disk-wear d) (int (field smart "Percentage used:"))
              (disk-hours d) (int (field smart "Power on hours:"))
              (disk-unsafe d) (int (field smart "Unsafe shutdowns:"))
              (disk-media-errors d) (int (field smart "Media errors:")))
        (let ((w (field smart "Data units written:" #'identity)))   ; "22310447 (10 TB)"
          (when (and w (position #\( w))
            (setf (disk-written d) (subseq w (1+ (position #\( w)) (position #\) w)))))))))

(defun dmesg-model (d)
  ;; "sd0 at scsibus0 target 0 lun 0: <USB, SanDisk 3.2Gen1, 1.00> disk removable"
  (let ((line (find-if (lambda (l) (and (search (format nil "] ~A at " (disk-name d)) l) (position #\< l) (position #\> l)))
                       (reverse (lines (sh "dmesg"))))))
    (when line
      (let ((open (position #\< line)) (close (position #\> line)))
        (when (and open close)
          (setf (disk-model d) (format nil "~{~A~^ ~}" (words (substitute #\Space #\, (subseq line (1+ open) close))))))))))

(defun read-disk (name)
  (let ((d (make-disk :name name)))
    (multiple-value-bind (bytes block-size) (geometry name)
      (setf (disk-bytes d) bytes (disk-block-size d) (or block-size 512)))
    (setf (disk-parts d) (or (wedges name) (label-parts name)))
    (if (starts-with "ld" name) (nvme-facts d) (dmesg-model d))
    d))

;;; File systems, mounts, usage

(defvar *mounts* (make-hash-table :test 'equal) "device -> (mountpoint type options)")
(defvar *usage* (make-hash-table :test 'equal) "device -> (used-kib avail-kib percent)")
(defvar *swap* (make-hash-table :test 'equal) "device -> percent used")

(defun read-mounts ()
  (dolist (line (lines (sh "mount")))
    (let ((w (words line)))
      (when (and (>= (length w) 6) (starts-with "/dev/" (first w)))
        (let* ((open (position #\( line))
               (opts (and open (remove "local" (words (substitute #\Space #\, (subseq line (1+ open) (position #\) line))))
                                       :test #'string=))))
          (setf (gethash (first w) *mounts*) (list (third w) (nth 4 w) opts))))))
  (dolist (line (lines (sh "df" "-kP")))
    (let ((w (words line)))
      (when (and (>= (length w) 6) (starts-with "/dev/" (first w)))
        (setf (gethash (first w) *usage*) (list (int (third w)) (int (fourth w)) (int (fifth w)))))))
  (dolist (line (lines (sh "swapctl" "-l")))
    (let ((w (words line)))
      (when (and (>= (length w) 5) (starts-with "/dev/" (first w)))
        (setf (gethash (first w) *swap*) (int (fifth w)))))))

(defun probe (part)
  "fstyp's verdict on the partition: (type label) or NIL."
  (let ((w (words (or (first (lines (sh "fstyp" "-l" (format nil "/dev/r~A" (part-name part))))) ""))))
    (and w (list (first w) (format nil "~{~A~^ ~}" (rest w))))))

;;; Look

(defun hum (bytes)
  (if (null bytes) "?"
      (loop with units = '("B" "KiB" "MiB" "GiB" "TiB" "PiB")
            for unit in units
            for size = bytes then (/ size 1024)
            when (or (< size 1024) (null (cdr units)))
              return (if (string= unit "B") (format nil "~D ~A" (round size) unit) (format nil "~,1F ~A" size unit))
            do (pop units))))

(defvar *color* nil)
(defvar *palette*
  (list :header (c:make-style :bold t)
        :disk (c:make-style :foreground :bright-white :bold t)
        :model (c:make-style :bold t)
        :part (c:make-style :foreground :bright-cyan :bold t)
        :size (c:make-style :foreground :yellow)
        :disk-size (c:make-style :foreground :bright-yellow :bold t)
        :fs (c:make-style :foreground :magenta)
        :label (c:make-style :faint t)
        :mount (c:make-style :foreground :bright-green :bold t)
        :tree (c:make-style :faint t)
        :dim (c:make-style :faint t)
        :swap (c:make-style :foreground :cyan)
        :good (c:make-style :foreground :green)
        :warn (c:make-style :foreground :yellow)
        :bad (c:make-style :foreground :red :bold t)))

(defun paint (role text)
  (let ((style (getf *palette* role)))
    (if (and *color* style) (c:colorize text style) text)))

(defun pad (text width &optional right)
  "TEXT padded with spaces to WIDTH visible columns."
  (let ((fill (make-string (max 0 (- width (c:visible-length text))) :initial-element #\Space)))
    (if right (concatenate 'string fill text) (concatenate 'string text fill))))

(defun percent (p)
  (paint (cond ((null p) :dim) ((>= p 90) :bad) ((>= p 70) :warn) (t :good)) (format nil "~D %" (or p 0))))

(defun fs-info (dev)
  (let ((mount (gethash dev *mounts*)) (usage (gethash dev *usage*)) (swap (gethash dev *swap*)))
    (cond (swap (format nil "~A  ~A used" (paint :swap "[SWAP]") (percent swap)))
          (mount
           (format nil "~A~A~A" (paint :mount (first mount))
                   (if (third mount) (format nil " ~A" (paint :dim (format nil "(~{~A~^,~})" (third mount)))) "")
                   (if usage (format nil "  ~A used, ~A free, ~A" (hum (* 1024 (first usage)))
                                     (hum (* 1024 (second usage))) (percent (third usage)))
                       "")))
          (t ""))))

(defun health-line (d)
  (when (disk-serial d)
    (let ((bits (list (format nil "fw ~A" (disk-firmware d)) (format nil "serial ~A" (disk-serial d)))))
      (when (disk-sector d) (push (format nil "~D B sectors" (disk-sector d)) bits))
      (when (disk-temp d)
        (push (paint (cond ((>= (disk-temp d) 60) :bad) ((>= (disk-temp d) 50) :warn) (t :dim))
                     (format nil "~D °C" (disk-temp d))) bits))
      (when (disk-wear d) (push (format nil "wear ~D %" (disk-wear d)) bits))
      (when (disk-hours d) (push (format nil "~D h on" (disk-hours d)) bits))
      (when (disk-written d) (push (format nil "~A written" (disk-written d)) bits))
      (when (and (disk-unsafe d) (plusp (disk-unsafe d))) (push (format nil "~D unsafe shutdowns" (disk-unsafe d)) bits))
      (when (and (disk-media-errors d) (plusp (disk-media-errors d)))
        (push (paint :bad (format nil "~D media errors" (disk-media-errors d))) bits))
      (format nil "  ~A ~A" (paint :tree "│") (paint :dim (format nil "~{~A~^  ~}" (reverse bits)))))))

(defun print-disk (d)
  (format t "~A ~A  ~A ~A ~A~%"
          (pad (paint :disk (disk-name d)) 10)
          (pad (paint :disk-size (hum (disk-bytes d))) 12 t)
          (pad (cond ((starts-with "ld" (disk-name d)) "nvme") ((starts-with "sd" (disk-name d)) "usb")
                     ((starts-with "cd" (disk-name d)) "cd") (t "disk")) 5)
          (pad "" 8)
          (paint :model (or (disk-model d) "")))
  (let ((health (health-line d))) (when health (write-line health)))
  (let ((parts (disk-parts d)))
    (if (null parts)
        (format t "  ~A~%" (paint :tree "└─(no partitions)"))
        (loop for (p . more) on parts
              for probed = (probe p)
              for dev = (format nil "/dev/~A" (part-name p))
              for type = (or (second (gethash dev *mounts*)) (first probed) (part-type p))
              for label = (if (string= (part-label p) "") (or (second probed) "") (part-label p))
              do (format t "  ~A~A ~A  ~A ~A ~A  ~A~%"
                         (paint :tree (if more "├─" "└─"))
                         (pad (paint :part (part-name p)) 8)
                         (pad (paint :size (hum (* (part-blocks p) (disk-block-size d)))) 12 t)
                         (pad "part" 5)
                         (pad (paint :fs type) 8)
                         (paint :label label)
                         (fs-info dev))))))

;;; Main

(defun disk-names ()
  (remove-if (lambda (n) (starts-with "dk" n)) (words (or (sh "sysctl" "-n" "hw.disknames") ""))))

(defun tty-p () (ignore-errors (plusp (sb-unix:unix-isatty 1))))

(defun main ()
  (handler-case
      (let ((args (rest sb-ext:*posix-argv*)))
        (setf *color* (and (tty-p) (not (env "NO_COLOR"))))
        (loop while (and args (starts-with "-" (first args)))
              do (let ((a (pop args)))
                   (cond ((string= a "-p") (setf *color* nil))
                         ((string= a "-c") (setf *color* t))
                         (t (format *error-output* "usage: lsblk [-p|-c] [disk ...]~%") (sb-ext:exit :code 2)))))
        (read-mounts)
        (format t "~A~%" (paint :header (format nil "~A ~A  ~A ~A ~A" (pad "NAME" 10) (pad "SIZE" 12 t)
                                                (pad "TYPE" 5) (pad "FS" 8) "LABEL / MOUNT")))
        (dolist (name (or args (disk-names)))
          (print-disk (read-disk name)))
        (finish-output))
    (sb-int:broken-pipe () (sb-ext:exit :code 0 :abort t))))

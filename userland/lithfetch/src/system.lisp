;;;; Talking to NetBSD: commands with a deadline, sysctls, small parsers.
(in-package #:lithfetch)

(defvar *command-timeout* 1.5
  "Seconds a probe command may run before it is killed and its fact is NIL.")

(defun sh (program &rest args)
  "Run PROGRAM with ARGS and return its standard output, or NIL if it could not
run, printed nothing, or outlived *COMMAND-TIMEOUT*."
  (handler-case
      (let ((process (sb-ext:run-program program args
                                         :search t :wait nil :input nil
                                         :output :stream :error nil)))
        (unwind-protect
             (handler-case
                 (sb-sys:with-deadline (:seconds *command-timeout*)
                   (let ((text (with-output-to-string (out)
                                 (loop with in = (sb-ext:process-output process)
                                       for line = (read-line in nil)
                                       while line do (write-line line out)))))
                     (sb-ext:process-wait process)
                     (and (plusp (length text)) text)))
               (sb-sys:deadline-timeout ()
                 (ignore-errors (sb-ext:process-kill process 9))
                 nil))
          (ignore-errors (sb-ext:process-close process))))
    (error () nil)))

(defun env (name) (sb-ext:posix-getenv name))

(defun lines (text)
  (and text (with-input-from-string (in text)
              (loop for line = (read-line in nil) while line collect line))))

(defun words (string)
  (loop with start = nil
        for i from 0 to (length string)
        for ch = (if (< i (length string)) (char string i) #\Space)
        if (member ch '(#\Space #\Tab))
          when start collect (subseq string start i) and do (setf start nil) end
        else do (unless start (setf start i))))

(defun trim (string) (string-trim '(#\Space #\Tab) string))

(defun starts-with (prefix string)
  (and (>= (length string) (length prefix)) (string= prefix string :end2 (length prefix))))

(defun after (marker string)
  "The text after the first MARKER in STRING, or NIL."
  (let ((p (search marker string))) (and p (subseq string (+ p (length marker))))))

(defun number-in (string)
  "The first decimal number in STRING, as a real, or NIL."
  (let* ((start (position-if (lambda (c) (or (digit-char-p c) (char= c #\.))) string))
         (end (and start (position-if-not (lambda (c) (or (digit-char-p c) (char= c #\.)))
                                          string :start start))))
    (and start (ignore-errors
                (let ((*read-default-float-format* 'double-float))
                  (let ((n (read-from-string (subseq string start end))))
                    (and (realp n) n)))))))

(defun file-text (path)
  (ignore-errors (with-open-file (in path :external-format :utf-8)
                   (let ((s (make-string (file-length in))))
                     (subseq s 0 (read-sequence s in))))))

(defun unix-time ()
  (- (get-universal-time) (encode-universal-time 0 0 0 1 1 1970 0)))

(defun terminal-columns ()
  "Width of the terminal on standard output: COLUMNS, then TIOCGWINSZ, then 100."
  (or (ignore-errors (let ((n (parse-integer (env "COLUMNS")))) (and (plusp n) n)))
      (ignore-errors
       (sb-alien:with-alien ((ws (array (sb-alien:unsigned 16) 4)))
         (and (zerop (sb-alien:alien-funcall
                      (sb-alien:extern-alien "ioctl" (function sb-alien:int sb-alien:int
                                                               sb-alien:unsigned-long
                                                               sb-alien:system-area-pointer))
                      1 #x40087468 (sb-alien:alien-sap ws))) ; TIOCGWINSZ on NetBSD
              (plusp (sb-alien:deref ws 1))
              (sb-alien:deref ws 1))))
      100))

;;;; Run with sbcl --script tests/statusbar.lisp. Requires a POSIX awk.
(require :asdf)

(let* ((source (uiop:read-file-string
                (merge-pathnames "../userland/statusbar"
                                 (uiop:pathname-directory-pathname *load-truename*))))
       (start (position #\' source :start (search "} | awk " source)))
       (end (position #\' source :from-end t))
       (parser (subseq source (1+ start) end)))
  (uiop:with-temporary-file (:pathname state :stream stream)
    (close stream)
    (labels ((sample (text)
               (uiop:run-program
                (list "awk" "-v" (format nil "state=~a" state) parser)
                :input (make-string-input-stream text) :output :string))
             (expect-fields (text fields)
               (let ((result (sample text)))
                 (dolist (field fields)
                   (assert (member field (uiop:split-string result
                                        :separator '(#\Space #\Newline))
                                   :test #'string=)
                           () "Missing ~s in ~s" field result)))))
      (expect-fields
       "@ram
1000 pages managed
200 pages free
100 cached file pages
50 cached executable pages
@cpu
user = 100, nice = 0, sys = 50, intr = 0, idle = 850
@sensors
[coretemp0]
 cpu0 temperature: 47.900 degC
[coretemp1]
 cpu1 temperature: 53.200 degC
[thinkpad0]
 temperature 0: 90.000 degC
[acpiacad0]
 connected: TRUE
[acpibat0]
 present: TRUE
 charge: 44.420 5.000% 0.441% Wh (98.01%)
 charging: TRUE
@audio
outputs.master=174,174
outputs.master3.mute=off
record.mic=0,0
record.mic2.mute=on
@wifi
 ssid TestNet nwkey ***
@clock
12:34
"
       '("ram=65" "cpu=?" "temp=53" "vol=68" "volm=off" "mic=0"
         "micm=on" "bat=98" "bats=chg" "ssid=TestNet" "clk=12:34"))
      (expect-fields
       "@cpu
user = 120, nice = 0, sys = 60, intr = 0, idle = 920
@sensors
[acpibat0]
 charging: FALSE
[acpiacad0]
 connected: TRUE
@audio
outputs.master.mute=on
outputs.master3.mute=off
record.mic.mute=off
record.mic2.mute=on
"
       '("cpu=30" "bats=plug" "volm=on" "micm=on"))
      (expect-fields "@sensors
[acpibat0]
 charge: 45.000 Wh (100.00%)
 charging: FALSE
[acpiacad0]
 connected: TRUE
" '("bat=100" "bats=full"))
      (expect-fields "@sensors
[acpibat0]
 present: FALSE
 charging: FALSE
" '("bat=?" "bats=none"))
      (expect-fields "" '("ram=?" "cpu=?" "temp=?" "bat=?" "ssid="))
      (expect-fields "@cpu
user = 1, nice = 0, sys = 0, intr = 0, idle = 1
" '("cpu=?"))
      (format t "Status parser checks passed.~%"))))

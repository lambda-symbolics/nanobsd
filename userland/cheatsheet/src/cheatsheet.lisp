;;;; cheatsheet [pattern]: the commands made for this laptop, by subject, one
;;;; line each; with a pattern only the matching lines.  Sources live in
;;;; nanobsd/userland (scripts, C, Lisp) and ~/common-lisp (Lisp programs).
;;;; Add a line here when a tool is born.  LispBSD, 2026-10-12.
(defpackage #:cheatsheet
  (:use #:cl)
  (:local-nicknames (#:c #:colorist))
  (:export #:main))
(in-package #:cheatsheet)

(defparameter *sheet*
  '(("Radios, USB, camera"
     ("usb on|off|status" "suspend/resume both xHCI controllers; rc.local turns them off at boot, run 'on' before plugging anything")
     ("bluetooth on|off|status" "attach/detach the Intel AX201 (ubt0); 'on' resumes USB first")
     ("bt-headphones on|off" "A2DP to the Denon PerL Pro: firmware, bthcid, bta2dpd on pad0, the pad as default audio")
     ("hcisniff [ubt0]" "every HCI command, event and ACL packet with direction and decoded headers (root)")
     ("btconfig ubt0 up" "base: brings the radio up, loading the Intel firmware on the first up")
     ("wifi" "Common Lisp Wi-Fi TUI: scan, join, forget (never scan on hotel Wi-Fi remotely)")
     ("wifi-powersave on|off" "iwx power save, about 1.5 W; live only")
     ("webcam on|off|status" "attach/detach the internal camera (uvideo)"))
    ("Power"
     ("battery" "charge, state, time left")
     ("s0ix" "kernel-coordinated s2idle (freeze); the lid does it on its own")
     ("brightness [N|+N|-N|...]" "panel backlight: levels, dim/undim, restore ac|bat")
     ("pcsample [-d S] LABEL" "package C-state residency sample for A/B runs")
     ("apst-ab, lpsched-ab, rr30/*" "A/B harnesses: NVMe APST, lpsched mechanisms, 30 Hz panel modes"))
    ("Audio"
     ("volume up|down|mute|get" "every output path at once (hdafg levels mapped onto the DAC steps)")
     ("hdafg-hp-fix" "makes the headphone jack usable on the ALC287 (rc.local)")
     ("bta2dpd -m encrypt ..." "base A2DP source; -m encrypt is what makes a modern headset pair"))
    ("Session and screen"
     ("startmh" "the Mahogany session the console autologin starts (X and StumpWM are gone)")
     ("super+m / super+shift+m" "mirror the panel onto the projector / cycle the fit; plug in after 'usb on'")
     ("mhcu mirror status|shot" "what the mirror does / what the projector shows (PNG)")
     ("mahoganyctl FORM" "evaluate Lisp in the running Mahogany (never after a defstruct change)")
     ("screenshot [area|full]" "PrtSc: freeze, select a region, save timestamped, copy")
     ("freezeshot FILE.png" "the freeze-and-drag part on its own (Esc/q/right button cancel)")
     ("feh PATH... (imv)" "image viewer: files or folders, pan and zoom")
     ("screentemp [K] [-d]" "colour temperature of the panel")
     ("statusbar / statusbard" "a status snapshot / the daemon publishing it to the bar")
     ("waytemp" "temperatures under Wayland")
     ("wmenu-run" "launcher"))
    ("System"
     ("lsblk [-p|-c] [disk]" "disks, wedges, probed file systems, mounts and usage in real units, NVMe health")
     ("lfetch" "the fetch, Genera style")
     ("tickrate" "measure the timer tick rate")
     ("cheatsheet [pattern]" "this list"))
    ("Daemons (rc.local or the scripts above start them)"
     ("lpschedd" "userland half of the power governor (machdep.lpsched)")
     ("brightnessd" "lets the session user change the backlight")
     ("bthcid (/usr/local/sbin)" "pairing daemon with Secure Simple Pairing, just works")
     ("ndevd" "device hotplug events for Mahogany"))))

(defvar *color* nil)
(defvar *heading* (c:make-style :foreground :bright-yellow :bold t))
(defvar *command* (c:make-style :foreground :bright-cyan :bold t))

(defun paint (style text) (if *color* (c:colorize text style) text))

(defun matches (pattern &rest strings)
  (or (null pattern) (some (lambda (s) (search pattern (string-downcase s))) strings)))

(defun main ()
  (handler-case
      (let* ((args (rest sb-ext:*posix-argv*))
             (pattern (and args (string-downcase (first args)))))
        (setf *color* (and (ignore-errors (plusp (sb-unix:unix-isatty 1))) (not (sb-ext:posix-getenv "NO_COLOR"))))
        (dolist (section *sheet*)
          (let ((rows (remove-if-not (lambda (row) (matches pattern (first row) (second row))) (rest section))))
            (when rows
              (format t "~%~A~%" (paint *heading* (if *color* (first section) (format nil "== ~A ==" (first section)))))
              (dolist (row rows)
                (let ((name (first row)))
                  (format t "  ~A~A ~A~%" (paint *command* name)
                          (make-string (max 1 (- 26 (length name))) :initial-element #\Space)
                          (second row)))))))
        (finish-output))
    (sb-int:broken-pipe () (sb-ext:exit :code 0 :abort t))))

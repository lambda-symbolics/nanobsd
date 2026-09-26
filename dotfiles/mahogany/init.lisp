;;;; Mahogany configuration for lispbsd. Installed as
;;;; /home/mag/.config/mahogany/init.lisp. Mirrors .stumpwmrc: Super is the
;;;; modifier, six workspaces, scrolling columns, the same bar fed by
;;;; statusbard, alacritty as the terminal.
(in-package #:mahogany)

;;;; -- Keyboard, pointer --

(when *initializing*
  (handler-case
      (xkb:with-xkb-rule-names (rules (:layout "cz,us" :options "grp:alt_shift_toggle"))
        (mahogany-set-keymap *compositor-state* :rules rules))
    (error (c) (log-string :error "keymap: ~A" c))))

(config-system:set-config keyboard-repeat-rate 40
                          keyboard-repeat-delay 300)
(config-system:set-config touchpad-tap-to-click t)
(config-system:set-config keyboard-focus-type :click-and-wheel)

;; C-t belongs to Firefox; the prefix map is hardly used anyway.
(setf (state-prefix-key *compositor-state*) (kbd "s-z"))

;;;; -- Appearance --

(hrt:border-box-style-update tree::*frame-focus-border-style*
                             :hrt-border-solid (colors:as-rgb "#ffffff") 1d0)
(hrt:border-box-style-update tree::*frame-unfocus-border-style*
                             :hrt-border-solid (colors:as-rgb "#505050") 2d0)
(setf tree:*strip-gap* 16)

(setf *message-theme*
      (mh/theme:make-theme :font "ProFontExtended 9"
                           :font-color (colors:as-rgb "#c5c9c5")
                           :background-color (colors:as-rgb "#080606")
                           :border-color (colors:as-rgb "#505050")))
(setf *message-error-theme*
      (mh/theme:augment-theme *message-theme*
                              :font-color (colors:as-rgb "#f53c3c")
                              :border-color (colors:as-rgb "#f53c3c")))
(setf *show-group-name* nil)

;;;; -- Workspaces --

(when *initializing*
  (setf (mahogany-group-name (state-current-group *compositor-state*)) "1")
  (dolist (name '("2" "3" "4" "5" "6"))
    (mahogany-state-group-add *compositor-state* :group-name name :make-current nil)))

;;;; -- Commands --

(defun run-shell (command)
  (uiop:launch-program (list "/bin/sh" "-c" command)))

(defcommand terminal ()
  (:documentation "Open a terminal window, reusing the alacritty daemon when it runs.")
  (:method ()
    (run-shell "alacritty msg --socket /tmp/alacritty.sock create-window 2>/dev/null || alacritty")))

(defcommand terminal-dfly ()
  (:method ()
    (run-shell "if command -v dfly >/dev/null 2>&1; then alacritty -e dfly; else alacritty; fi")))

(defcommand editor ()
  (:method ()
    (run-shell "if command -v e >/dev/null 2>&1; then e; else emacsclient -c -a emacs; fi")))

(defcommand launcher ()
  (:documentation "rofi in the niri wmenu look.")
  (:method ()
    (run-shell "rofi -show run -font 'ProFontExtended 9' -no-show-icons")))

(defcommand screenshot ()
  (:method () (run-shell "screenshot area")))

(defcommand screenshot-full ()
  (:method () (run-shell "screenshot full")))

(defcommand volume-up () (:method () (run-shell "volume up")))
(defcommand volume-down () (:method () (run-shell "volume down")))
(defcommand volume-mute () (:method () (run-shell "volume mute")))
(defcommand microphone-mute ()
  (:method ()
    (run-shell "mixerctl -w record.mic2.mute=toggle 2>/dev/null || mixerctl -w record.mic.mute=toggle")))
(defcommand brightness-up () (:method () (run-shell "brightness +10")))
(defcommand brightness-down () (:method () (run-shell "brightness -10")))

;;;; -- Key bindings (niri layout, Super instead of Mod) --

(defmacro bind-top (&rest pairs)
  `(progn
     ,@(loop for (key command) on pairs by #'cddr
             collect `(define-key *top-map* (kbd ,key) ,command))))

(bind-top
 "s-q"              #'close-current-view
 "s-Left"           #'strip-focus-left
 "s-Right"          #'strip-focus-right
 "s-Up"             #'strip-focus-up
 "s-Down"           #'strip-focus-down
 "s-C-Left"         #'strip-move-left
 "s-C-Right"        #'strip-move-right
 "s-C-Up"           #'strip-move-up
 "s-C-Down"         #'strip-move-down
 "s-Home"           #'strip-focus-first
 "s-End"            #'strip-focus-last
 "s-C-Home"         #'strip-move-first
 "s-C-End"          #'strip-move-last
 "s-c"              #'group-select-1
 "s-i"              #'group-select-2
 "s-e"              #'group-select-3
 "s-a"              #'group-select-4
 "s-h"              #'group-select-5
 "s-t"              #'group-select-6
 "s-C-c"            #'strip-workspace-1
 "s-C-i"            #'strip-workspace-2
 "s-C-e"            #'strip-workspace-3
 "s-C-a"            #'strip-workspace-4
 "s-C-h"            #'strip-workspace-5
 "s-C-t"            #'strip-workspace-6
 "s-comma"          #'strip-consume
 "s-period"         #'strip-expel
 "s-r"              #'strip-width-preset
 "s-f"              #'strip-width-max
 "s-F"              #'strip-fullscreen
 "s-C"              #'strip-center
 "s-d"              #'strip-width-narrower
 "s-w"              #'strip-width-wider
 "s-D"              #'strip-height-less
 "s-W"              #'strip-height-more
 "s-Tab"            #'strip-next
 "s-ISO_Left_Tab"   #'strip-prev
 "Print"            #'screenshot
 "S-Print"          #'screenshot-full
 "s-E"              #'shutdown-gracefully
 "s-P"              #'monitors-off
 "s-p"              #'launcher
 "s-Return"         #'terminal
 "s-S-Return"       #'terminal-dfly
 "s-u"              #'editor
 "s-o"              #'monitors-on
 "s-O"              #'monitors-off
 "s-semicolon"      #'colon
 "s-b"              #'bar-toggle
 "XF86AudioRaiseVolume"  #'volume-up
 "XF86AudioLowerVolume"  #'volume-down
 "XF86AudioMute"         #'volume-mute
 "XF86AudioMicMute"      #'microphone-mute
 "XF86MonBrightnessUp"   #'brightness-up
 "XF86MonBrightnessDown" #'brightness-down)

;;;; -- Bar --

(setf *bar-font* "ProFontExtended 9"
      *bar-foreground* "#ffffff"
      *bar-background* "#000000"
      *bar-bottom* nil)

;;;; -- Idle: panel off after five minutes without input --

(setf *idle-blank-seconds* 300)

;;;; -- lpsched: tell the power governor which client has focus --

(ignore-errors (require :sb-bsd-sockets))

(defvar *lpsched-sock-path* "/var/run/lpsched.sock")

(defun lpsched-send-fgpid (pid)
  "Send \"fgpid PID\" to lpschedd; 0 means nothing is focused. Never blocks,
every failure is swallowed."
  (when (find-package :sb-bsd-sockets)
    (ignore-errors
      (let ((sock (make-instance (intern "LOCAL-SOCKET" :sb-bsd-sockets)
                                 :type :datagram)))
        (unwind-protect
             (progn
               (funcall (intern "SOCKET-CONNECT" :sb-bsd-sockets) sock *lpsched-sock-path*)
               (funcall (intern "SOCKET-SEND" :sb-bsd-sockets)
                        sock (format nil "fgpid ~D~%" pid) nil))
          (funcall (intern "SOCKET-CLOSE" :sb-bsd-sockets) sock))))))

(defun lpsched-focus-hook (view)
  (lpsched-send-fgpid (or (hrt:view-pid view) 0)))

(pushnew 'lpsched-focus-hook tree:*view-focus-hook*)

;;;; -- Startup --

(when *initializing*
  (run-shell "pgrep -f statusbard >/dev/null || /usr/local/bin/statusbard &")
  (run-shell "pgrep -x alacritty >/dev/null || { rm -f /tmp/alacritty.sock; alacritty --daemon --socket /tmp/alacritty.sock & }")
  (run-shell "pgrep -x swaybg >/dev/null || swaybg -c '#0A0A0A' &")
  (run-shell "pgrep -x wlsunset >/dev/null || wlsunset -t 4000 -T 4001 &")
  (run-shell "/usr/local/bin/brightness restore >/dev/null 2>&1")
  (run-shell "/usr/local/bin/volume restore >/dev/null 2>&1"))

(bar-start)
(idle-start)

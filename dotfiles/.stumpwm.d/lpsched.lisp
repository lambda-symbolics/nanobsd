;;;; lpsched.lisp -- tell the LISPBSD laptop power governor (lpschedd) which
;;;; process has focus, so the kernel exempts it from power packing.
;;;;
;;;; One small datagram to /var/run/lpsched.sock per focus change.  It never
;;;; blocks the WM thread, and every failure (daemon down, window without
;;;; _NET_WM_PID, socket contrib missing) is swallowed.  sb-bsd-sockets is
;;;; reached through INTERN/FUNCALL so this file still loads cleanly when the
;;;; contrib is absent (a package-qualified symbol would be a read error).

(in-package :stumpwm)

(ignore-errors (require :sb-bsd-sockets))

(defvar *lpsched-sock-path* "/var/run/lpsched.sock")

(defun lpsched-window-pid (window)
  "The _NET_WM_PID of WINDOW, or NIL."
  (ignore-errors
    (let ((pid (car (xlib:get-property (window-xwin window) :_NET_WM_PID))))
      (and (integerp pid) (plusp pid) pid))))

(defun lpsched-send-fgpid (pid)
  "Send \"fgpid PID\" to lpschedd; 0 means nothing is focused.
Connect the datagram socket then send: SOCKET-SEND's :ADDRESS wants an
address *list*, and a bare path string makes it signal VALUES-LIST."
  (when (find-package :sb-bsd-sockets)
    (ignore-errors
      (let ((sock (make-instance (intern "LOCAL-SOCKET" :sb-bsd-sockets)
                                 :type :datagram)))
        (unwind-protect
             (progn
               (funcall (intern "SOCKET-CONNECT" :sb-bsd-sockets)
                        sock *lpsched-sock-path*)
               (funcall (intern "SOCKET-SEND" :sb-bsd-sockets)
                        sock (format nil "fgpid ~D~%" pid) nil))
          (funcall (intern "SOCKET-CLOSE" :sb-bsd-sockets) sock))))))

(defun lpsched-focus-hook (new-window old-window)
  (declare (ignore old-window))
  (lpsched-send-fgpid (or (and new-window (lpsched-window-pid new-window)) 0)))

(add-hook *focus-window-hook* #'lpsched-focus-hook)

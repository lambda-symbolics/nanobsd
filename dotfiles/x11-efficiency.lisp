;;;; Asynchronous X event dispatch for StumpWM 24.11.
;;;; The upstream dispatcher synchronizes with the server before every event,
;;;; including its empty-queue check. Flush here; wait for input in the I/O loop.
;;;; Reference: stumpwm/stumpwm, tag 24.11, stumpwm.lisp display-channel methods.
(in-package :stumpwm)

(defun lispbsd-dispatch-x-events (display)
  "Drain queued X events without a synchronous server round trip per event."
  (loop
    (xlib:display-force-output display)
    (unless (xlib:event-listen display 0)
      (return))
    (xlib:with-event-queue (display)
      (run-hook *event-processing-hook*)
      (xlib:process-event display :handler #'handle-event :timeout 0))))

(defmethod io-channel-handle ((channel display-channel) (event (eql :read)) &key)
  (lispbsd-dispatch-x-events (slot-value channel 'display)))

(defmethod io-channel-handle ((channel display-channel) (event (eql :loop)) &key)
  (lispbsd-dispatch-x-events (slot-value channel 'display)))

(defpackage #:lithfetch
  (:use #:cl)
  (:local-nicknames (#:c #:colorist))
  (:documentation "lithfetch: a herald for the LispBSD X1 Nano.

Everything is meant to be changed from ~/.config/lithfetch/init.lisp, which is
LOADed into this package after the defaults: redefine a fact or an entry,
reorder *HERALD*, repaint *PALETTE*, swap the logo.")
  (:export #:main))

;;; cheatsheet --- the LispBSD commands we made, by subject.  -*- Mode: Lisp -*-
(asdf:defsystem #:cheatsheet
  :description "The commands made for the LispBSD X1 Nano, one line each."
  :version "0.1.0"
  :license "ISC"
  :depends-on (#:cl-colorist)
  :components ((:module "src" :components ((:file "cheatsheet")))))

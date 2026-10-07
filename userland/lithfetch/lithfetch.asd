;;; lithfetch --- a Lisp machine herald for the LispBSD X1 Nano.  -*- Mode: Lisp -*-
(asdf:defsystem #:lithfetch
  :description "System information for the LispBSD X1 Nano, as a Lisp machine herald."
  :version "0.1.0"
  :license "ISC"
  :depends-on (#:cl-colorist)
  :serial t
  :components ((:module "src"
                :serial t
                :components ((:file "package")
                             (:file "system")
                             (:file "facts")
                             (:file "look")
                             (:file "logo")
                             (:file "herald")
                             (:file "main")))))

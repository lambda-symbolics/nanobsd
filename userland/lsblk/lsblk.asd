;;; lsblk --- the disks of the LispBSD X1 Nano and what is on them.  -*- Mode: Lisp -*-
(asdf:defsystem #:lsblk
  :description "Disks, wedges, file systems, mounts and NVMe health, in real units and colour."
  :version "0.1.0"
  :license "ISC"
  :depends-on (#:cl-colorist)
  :components ((:module "src" :components ((:file "lsblk")))))

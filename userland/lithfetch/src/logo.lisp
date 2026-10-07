;;;; The LispBSD λ (the one from our pfetch), a row of styles per row.
;;;; Swap it from init.lisp: (setf *logo* '((:logo-1 . "...") ...)).
(in-package #:lithfetch)

(defparameter *lambda-rows*
  '("  ⠠⠟⢧⡀"
    "    ⠈⢷⡀"
    "     ⠘⣷⡀"
    "      ⠘⣷⡀"
    "       ⠘⣷⡄"
    "        ⠘⣿⡄"
    "     ⢭⣦⣀ ⠘⣿⡄"
    "     ⠈⢿⡝⢷⣄⠘⣿⡄"
    "      ⠈⢿⡄⠙⣷⣜⣿⡄"
    "       ⠘⣷⡀⠈⠻⣿⢿⡄"
    "⠓⢦⣄⡀    ⢸⡇  ⠙⢯⠳⡀"
    "  ⠈⠙⠳⢶⣤⣤⡾⠃   ⠈ ⣠"))

(defun shaded (rows)
  "ROWS as logo lines, shaded top to bottom through :LOGO-1 .. :LOGO-6."
  (loop with n = (length rows)
        for row in rows
        for i from 0
        collect (list (cons (aref #(:logo-1 :logo-2 :logo-3 :logo-4 :logo-5 :logo-6)
                                  (min 5 (floor (* i 6) n)))
                            row))))

(defvar *logo* (shaded *lambda-rows*)
  "Lines of spans drawn left of the herald; NIL for none.")

(defvar *logo-gap* 4 "Columns between the logo and the herald.")

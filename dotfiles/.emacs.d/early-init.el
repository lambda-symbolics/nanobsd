;;; early-init.el --- Before the first frame -*- lexical-binding: t; -*-

;; Collect garbage rarely during startup; init.el lowers it again.
(setq gc-cons-threshold most-positive-fixnum
      package-enable-at-startup nil
      frame-resize-pixelwise t
      frame-inhibit-implied-resize t
      inhibit-startup-screen t
      inhibit-startup-message t
      initial-scratch-message nil
      native-comp-async-report-warnings-errors 'silent)

(push '(menu-bar-lines . 0) default-frame-alist)
(push '(tool-bar-lines . 0) default-frame-alist)
(push '(vertical-scroll-bars) default-frame-alist)
;; ProFontExtended 9 pt, the size the bar, wmenu and alacritty use.  Set here
;; so the first frame opens at the right size instead of resizing later.
(push '(font . "ProFontExtended-9") default-frame-alist)

;;; early-init.el ends here

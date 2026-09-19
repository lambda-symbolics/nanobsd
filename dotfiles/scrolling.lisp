;;;; Instant, event-driven scrolling columns for StumpWM 24.11.
(in-package :stumpwm)

(defstruct strip-column
  windows
  (width 0.3)
  saved-width)

(define-swm-class strip-group (float-group)
  ((columns :initform nil :accessor strip-columns)
   (offset :initform 0 :accessor strip-offset)
   (weights :initform (make-hash-table :test #'eq) :reader strip-weights)))

(defparameter *strip-gap* 16)
(defvar *strip-layout-active* nil)

(defun strip-column-for (group window)
  (find window (strip-columns group) :key #'strip-column-windows :test #'member))

(defun strip-current-column (group)
  (strip-column-for group (group-current-window group)))

(defun strip-insert-after (item after items)
  (if after
      (loop for old in items append (if (eq old after) (list old item) (list old)))
      (append items (list item))))

(define-swm-class strip-window (float-window) ())

(defun strip-head (group)
  ;; The Nano has one panel; keep the viewport independent of offscreen windows.
  (first (screen-heads (group-screen group))))

(defmethod group-current-head ((group strip-group)) (strip-head group))

(defun strip-area (group)
  (let* ((head (strip-head group))
         (ml (head-mode-line head))
         (bar (if ml (mode-line-height ml) 0))
         (gap *strip-gap*))
    (values (+ (head-x head) gap)
            (+ (head-y head) gap (if (eq *mode-line-position* :top) bar 0))
            (max 1 (- (head-width head) (* 2 gap)))
            (max 1 (- (head-height head) bar (* 2 gap))))))

(defun strip-pixel-width (column width)
  (max 80 (round (* width (strip-column-width column)))))

(defun strip-place (window x y width height)
  "Move parents during scrolling; configure client sizes only when they change."
  (let* ((width (max 1 (- width (* 2 *float-window-border*))))
         (height (max 1 (- height *float-window-title-height* *float-window-border*)))
         (new-x (unless (= x (window-x window)) x))
         (new-y (unless (= y (window-y window)) y))
         (new-width (unless (= width (window-width window)) width))
         (new-height (unless (= height (window-height window)) height)))
    (when (or new-x new-y new-width new-height)
      (float-window-move-resize window :x new-x :y new-y
                                      :width new-width :height new-height)
      ;; The child offsets are fixed by float-window-align. Reuse known geometry
      ;; rather than querying the X server to build the ICCCM ConfigureNotify.
      (xwin-send-configuration-notify (window-xwin window)
                                      (+ x *float-window-border*)
                                      (+ y *float-window-title-height*)
                                      width height 0))))

(defun strip-layout (group &key center)
  "Reveal the focused column immediately and unmap fully offscreen clients."
  (unless *strip-layout-active*
    (let ((*strip-layout-active* t))
      (multiple-value-bind (left top width height) (strip-area group)
        (let* ((focus (group-current-window group))
               (selected (strip-current-column group))
               (start 0)
               (selected-start 0)
               (selected-width width))
          (dolist (column (strip-columns group))
            (when (eq column selected)
              (setf selected-start start
                    selected-width (strip-pixel-width column width)))
            (incf start (+ (strip-pixel-width column width) *strip-gap*)))
          (when selected
            (setf (strip-offset group)
                  (cond (center (- selected-start (floor (- width selected-width) 2)))
                        ((< selected-start (strip-offset group)) selected-start)
                        ((> (+ selected-start selected-width) (+ (strip-offset group) width))
                         (- (+ selected-start selected-width) width))
                        (t (strip-offset group)))))
          (let ((x (- left (strip-offset group)))
                (fullscreen (and focus (window-fullscreen focus))))
            (dolist (column (strip-columns group))
              (let* ((cw (strip-pixel-width column width))
                     (windows (strip-column-windows column))
                     (available (- height (* *strip-gap* (1- (length windows)))))
                     (total (loop for w in windows sum (gethash w (strip-weights group) 1.0)))
                     (y top))
                (loop for window in windows
                      for last = (eq window (car (last windows)))
                      for wh = (if last (- (+ top height) y)
                                   (round (* available (/ (gethash window (strip-weights group) 1.0) total))))
                      for visible = (if fullscreen (eq window focus)
                                        (and (< x (+ left width)) (> (+ x cw) left)))
                      do (cond
                           (visible
                            (unless (window-fullscreen window)
                              (strip-place window x y cw wh))
                            (when (window-hidden-p window) (unhide-window window)))
                           (t (hide-window window)))
                         (incf y (+ wh *strip-gap*)))
                (incf x (+ cw *strip-gap*))))))))))

(defmethod group-add-window ((group strip-group) window &key raise &allow-other-keys)
  (let ((previous (strip-current-column group)))
    (dynamic-mixins-swm:replace-class window 'strip-window)
    (float-window-align window)
    (sync-minor-modes window)
    (unless (strip-column-for group window)
      (setf (strip-columns group)
            (strip-insert-after (make-strip-column :windows (list window))
                                previous (strip-columns group))))
    (if (and raise (eq group (current-group)))
        (group-focus-window group window)
        (strip-layout group))))

;; Resolve the inherited (GROUP FLOAT-WINDOW) specialization explicitly.
(defmethod group-add-window ((group strip-group) (window float-window)
                             &key raise &allow-other-keys)
  (declare (ignore raise))
  (call-next-method))

(defmethod focus-window :before ((window strip-window) &optional raise)
  (declare (ignore raise))
  (let* ((group (window-group window)) (old (group-current-window group)))
    (when (typep group 'strip-group)
      (when (and old (not (eq old window)) (window-fullscreen old))
        (update-fullscreen old 0))
      (setf (group-current-window group) window)
      (strip-layout group))))

(defmethod group-focus-window ((group strip-group) window)
  (when window (focus-window window)))

(defmethod (setf window-fullscreen) :after (value (window strip-window))
  (let ((group (window-group window)))
    (when (typep group 'strip-group)
      (when value (setf (group-current-window group) window))
      (strip-layout group))))

(defmethod group-delete-window ((group strip-group) (window float-window))
  (let* ((column (strip-column-for group window))
         (index (position column (strip-columns group))))
    (dolist (c (strip-columns group))
      (setf (strip-column-windows c) (remove window (strip-column-windows c))))
    (remhash window (strip-weights group))
    (setf (strip-columns group) (remove-if-not #'strip-column-windows (strip-columns group)))
    (when (eq window (group-current-window group))
      (setf (group-current-window group)
            (first (strip-column-windows
                    (or (nth (min (or index 0) (max 0 (1- (length (strip-columns group)))))
                             (strip-columns group))
                        (make-strip-column))))))
    (when (eq group (current-group)) (group-wake-up group))))

(defmethod group-wake-up ((group strip-group))
  (let ((window (or (group-current-window group)
                    (first (strip-column-windows (or (first (strip-columns group))
                                                    (make-strip-column)))))))
    (if window (group-focus-window group window) (no-focus group nil))))

(defmethod group-lost-focus ((group strip-group))
  ;; Hiding precedes source-column removal during workspace transfers. Relayout
  ;; here would remap the departing window; group-delete-window refocuses after
  ;; removing it. Layout-driven hiding also needs no separate focus action.
  nil)

(defmethod group-resize-request ((group strip-group) window width height)
  (declare (ignore window width height))
  (strip-layout group))

(defmethod group-move-request ((group strip-group) window x y relative-to)
  (declare (ignore window x y relative-to))
  (strip-layout group))

(defmethod group-sync-all-heads ((group strip-group)) (strip-layout group))
(defmethod group-sync-head ((group strip-group) head)
  (declare (ignore head)) (strip-layout group))
(defmethod group-after-resize-head ((group strip-group) head)
  (declare (ignore head)) (strip-layout group))

(defun strip-select-column (group index)
  (let ((column (nth (max 0 (min index (1- (length (strip-columns group)))))
                     (strip-columns group))))
    (when column (group-focus-window group (first (strip-column-windows column))))))

(defcommand strip-focus (direction) ((:string "Direction: "))
  "Focus a column or a window within the current column."
  (let* ((group (current-group)) (column (strip-current-column group))
         (columns (strip-columns group)) (index (or (position column columns) 0)))
    (cond ((string= direction "left") (strip-select-column group (1- index)))
          ((string= direction "right") (strip-select-column group (1+ index)))
          ((string= direction "first") (strip-select-column group 0))
          ((string= direction "last") (strip-select-column group (1- (length columns))))
          (column
           (let* ((windows (strip-column-windows column))
                  (i (or (position (current-window) windows) 0))
                  (next (max 0 (min (1- (length windows))
                                   (+ i (if (string= direction "up") -1 1))))))
             (group-focus-window group (nth next windows)))))))

(defcommand strip-move (direction) ((:string "Direction: "))
  "Reorder a column horizontally or a window vertically."
  (let* ((group (current-group)) (column (strip-current-column group)))
    (when column
      (let* ((vertical (member direction '("up" "down") :test #'string=))
             (items (copy-list (if vertical (strip-column-windows column) (strip-columns group))))
             (item (if vertical (current-window) column))
             (index (position item items))
             (target (cond ((string= direction "first") 0)
                           ((string= direction "last") (1- (length items)))
                           ((member direction '("left" "up") :test #'string=) (1- index))
                           (t (1+ index)))))
        (setf target (max 0 (min target (1- (length items)))))
        (setf items (remove item items))
        (setf items (append (subseq items 0 target) (list item) (nthcdr target items)))
        (if vertical (setf (strip-column-windows column) items)
            (setf (strip-columns group) items))
        (strip-layout group)))))

(defcommand strip-width (action) ((:string "Width: "))
  "Cycle Niri widths, maximize, or adjust by five percent."
  (let* ((group (current-group)) (column (strip-current-column group)))
    (when column
      (let ((width (strip-column-width column)))
        (setf (strip-column-width column)
              (cond ((string= action "preset")
                     (or (find-if (lambda (p) (> p (+ width 0.01))) '(0.33333 0.5 0.66667)) 0.33333))
                    ((string= action "max")
                     (if (strip-column-saved-width column)
                         (prog1 (strip-column-saved-width column)
                           (setf (strip-column-saved-width column) nil))
                         (progn (setf (strip-column-saved-width column) width) 1.0)))
                    (t (max 0.1 (min 1.0 (+ width (if (string= action "+") 0.05 -0.05)))))))
        (unless (string= action "max") (setf (strip-column-saved-width column) nil)))
      (strip-layout group))))

(defcommand strip-height (direction) ((:string "Height: "))
  "Adjust the focused window's share of its column by five percentage points."
  (let* ((group (current-group)) (column (strip-current-column group)) (window (current-window)))
    (when (and column (cdr (strip-column-windows column)))
      (let* ((windows (strip-column-windows column))
             (total (loop for w in windows sum (gethash w (strip-weights group) 1.0)))
             (old (/ (gethash window (strip-weights group) 1.0) total))
             (new (max 0.1 (min 0.9 (+ old (if (string= direction "+") 0.05 -0.05))))))
        (dolist (w windows)
          (setf (gethash w (strip-weights group))
                (if (eq w window) new
                    (* (/ (gethash w (strip-weights group) 1.0) total)
                       (/ (- 1 new) (- 1 old)))))))
      (strip-layout group))))

(defcommand strip-consume () ()
  "Pull the first window from the column on the right into this column."
  (let* ((group (current-group)) (column (strip-current-column group))
         (next (second (member column (strip-columns group)))))
    (when next
      (let ((window (pop (strip-column-windows next))))
        (setf (strip-column-windows column) (append (strip-column-windows column) (list window)))
        (unless (strip-column-windows next)
          (setf (strip-columns group) (remove next (strip-columns group))))
        (strip-layout group)))))

(defcommand strip-expel () ()
  "Move this window out of its stack into a new column on the right."
  (let* ((group (current-group)) (column (strip-current-column group)) (window (current-window)))
    (when (and column (cdr (strip-column-windows column)))
      (setf (strip-column-windows column) (remove window (strip-column-windows column))
            (strip-columns group)
            (strip-insert-after (make-strip-column :windows (list window)
                                                  :width (strip-column-width column))
                                column (strip-columns group)))
      (strip-layout group))))

(defcommand strip-center () ()
  "Center the focused column in the viewport."
  (strip-layout (current-group) :center t))

(defcommand strip-fullscreen () ()
  "Toggle fullscreen on this window, restoring its column afterwards."
  (when (current-window)
    (update-fullscreen (current-window) 2)))

(defcommand strip-workspace (name) ((:string "Workspace: "))
  "Move the complete focused column to another workspace."
  (let* ((group (current-group)) (column (strip-current-column group))
         (target (find-group (current-screen) name)))
    (when (and column (typep target 'strip-group) (not (eq group target)))
      (let* ((windows (copy-list (strip-column-windows column)))
             (weights (mapcar (lambda (window) (gethash window (strip-weights group) 1.0))
                              windows))
             (anchor (strip-current-column target)))
        (loop for window in windows
              for weight in weights
              do (move-window-to-group window target)
                 (setf (gethash window (strip-weights target)) weight))
        ;; The ordinary lifecycle creates one column per moved window. Reassemble it.
        (setf (strip-columns target)
              (remove-if (lambda (c) (intersection windows (strip-column-windows c)))
                         (strip-columns target))
              (strip-column-windows column) windows
              (strip-columns target) (strip-insert-after column anchor (strip-columns target)))
        (strip-layout group)
        (strip-layout target)))))

(defun strip-enable ()
  "Adopt existing windows in number order without restarting applications."
  (setf *float-window-border* 1 *float-window-title-height* 1
        *mouse-focus-policy* :click *default-group-type* 'strip-group)
  (dolist (group (screen-groups (current-screen)))
    (unless (typep group 'strip-group)
      (let ((windows (sort (copy-list (group-windows group)) #'< :key #'window-number))
            (focus (group-current-window group)))
        (dynamic-mixins-swm:replace-class group 'strip-group)
        (dolist (window windows) (group-add-window group window))
        (setf (group-current-window group) (or focus (first windows)))))
    (strip-layout group))
  (group-wake-up (current-group))
  (sync-keys))

(strip-enable)

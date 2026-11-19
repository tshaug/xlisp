; ide.lsp - IDE protocol loop for xlide-host
;
; The host (xlide-host.c) provides %IDE-NEXT-MESSAGE, %IDE-SEND,
; %IDE-SET-EVAL-ID, %IDE-FLUSH and %IDE-TAKE-ERROR-TEXT.  This file replaces
; the console REPL and the error handler with versions that speak the
; protocol.  It mirrors *breaklevel* / *toplevel* in xlinit.lsp.

(define *ide-level* 0)          ; current break level, 0 = top level
(define *ide-current-id* #f)    ; id of the evaluation in progress, if any
(define *ide-current-source* '())  ; its "source" object, echoed in result/error

; evaluate every form in TEXT in ENV; return the values of the last form
(define (ide-eval-text text env)
  (let ((port (make-string-input-stream text)))
    (let loop ((form (read port)) (vals '()))
      (if (eof-object? form)
          vals
          (loop (read port) (multiple-value-list (eval form env)))))))

; the source of an eval message ("eval" id text file line column) as a JSON object
(define (ide-source-json msg)
  (let ((file (cadddr msg)) (line (car (cddddr msg))) (column (cadr (cddddr msg))))
    (if (or file line column)
        (vector "file" file "line" line "column" column)
        '())))

(define (ide-handle-eval msg env)
  (let ((id (cadr msg)) (text (caddr msg)))
    (set! *ide-current-id* id)
    (set! *ide-current-source* (ide-source-json msg))
    (%ide-set-eval-id id)
    (let ((vals (ide-eval-text text env)))
      (%ide-flush)
      (%ide-send "result" "id" id "level" *ide-level* "source" *ide-current-source*
                 "values" (map (lambda (v) (format #f "~S" v)) vals)))
    (set! *ide-current-id* #f)
    (%ide-set-eval-id #f)))

; process messages at the current level; returns 'cleanup when the IDE pops it
(define (ide-run env)
  (let loop ()
    (let* ((msg (%ide-next-message)) (type (car msg)))
      (cond ((string=? type "eval")     (ide-handle-eval msg env) (loop))
            ((string=? type "cleanup")  'cleanup)
            ((string=? type "reset")    (throw-error 'reset))
            ((string=? type "shutdown") (exit))
            (else (loop))))))

; one break level (compare *breaklevel* in xlinit.lsp)
(define (ide-level-loop env level)
  (set! *ide-level* level)
  (let ((code (unwind-protect
                (catch 'error (ide-run env))
                (set! *ide-level* (-1+ level)))))
    (case code
      (continue (ide-level-loop env level))
      (cleanup  (throw-error 'continue))
      (reset    (throw-error 'reset))
      (else     'eof))))

; ---- the call stack as data -------------------------------------------

(define (ide-truncate s)
  (if (> (string-length s) 200)
      (string-append (substring s 0 200) "...")
      s))

(define (ide-print v) (ide-truncate (format #f "~S" v)))

;; symbols print in lower case; SYMBOL->STRING gives the stored upper case
(define (ide-frame-name fr)
  (if (car fr) (format #f "~A" (car fr)) '()))

;; true if FUN is a code object (printed as #<Code ...>)
(define (ide-code? fun)
  (and fun
       (let ((str (format #f "~S" fun)))
         (and (>= (string-length str) 7) (string=? (substring str 0 7) "#<Code ")))))

; The control stack holds the callers of the running function.  Inside the
; error handler, the *error-handler* entry carries the environment of the
; function that failed (FUN), so that entry becomes the innermost user frame.
; The stack is cut where the IDE's own evaluation loop starts.
(define (ide-user-frames fun)
  (let find ((frames (%stack-frames 100)))
    (cond ((null? frames) '())
          ((eq? (car (car frames)) '*error-handler*)
           (let ((top (car frames)))
             (ide-cut-at-eval
               (append (if (ide-code? fun)
                           (list (list (%vector-ref fun 1) (cadr top) (caddr top) fun))
                           '())
                       (cdr frames))
               '())))
          (else (find (cdr frames))))))

; keep frames up to ide-handle-eval; its named-let loop frame is not the user's
(define (ide-cut-at-eval frames kept)
  (cond ((null? frames) (reverse kept))
        ((eq? (car (car frames)) 'ide-handle-eval)
         (reverse (if (and (pair? kept) (eq? (car (car kept)) 'loop)) (cdr kept) kept)))
        (else (ide-cut-at-eval (cdr frames) (cons (car frames) kept)))))

; drop anonymous frames without arguments (the top-level expression itself)
(define (ide-visible-frames frames)
  (cond ((null? frames) '())
        ((and (not (car (car frames))) (null? (cadr (car frames))))
         (ide-visible-frames (cdr frames)))
        (else (cons (car frames) (ide-visible-frames (cdr frames))))))

; one frame as a JSON object: {"name":..., "args":[{"name":...,"value":...}]}
(define (ide-frame-json fr)
  (vector "name" (ide-frame-name fr)
          "args" (map (lambda (a) (vector "name" (format #f "~A" (car a))
                                          "value" (ide-print (cdr a))))
                      (cadr fr))))

; called by the interpreter when an evaluation fails
(define (*error-handler* fun env sp)
  (let ((failed-id *ide-current-id*)
        (source *ide-current-source*)
        (message (%ide-take-error-text))
        (stack (map ide-frame-json (ide-visible-frames (ide-user-frames fun))))
        (level (1+ *ide-level*)))
    (%ide-flush)
    (%ide-send "error" "id" failed-id "level" level "message" message "source" source "stack" stack)
    (unwind-protect
      (ide-level-loop env level)
      ; the failed evaluation is abandoned when its level is popped or reset
      (%ide-send "aborted" "id" failed-id "level" (-1+ level)))))

; the top level: evaluations at level 0
(define (*ide-toplevel*)
  (let loop ()
    (let ((code (catch 'error (ide-run (the-environment)))))
      (if *ide-current-id*          ; interrupted: no error handler ran
          (begin
            (%ide-flush)
            (%ide-send "aborted" "id" *ide-current-id* "level" 0)
            (set! *ide-current-id* #f)
            (%ide-set-eval-id #f)))
      (set! *ide-level* 0)
      (loop))))

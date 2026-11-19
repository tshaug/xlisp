; ide.lsp - IDE protocol loop for xlide-host
;
; The host (xlide-host.c) provides %IDE-NEXT-MESSAGE, %IDE-SEND,
; %IDE-SET-EVAL-ID, %IDE-FLUSH and %IDE-TAKE-ERROR-TEXT.  This file replaces
; the console REPL and the error handler with versions that speak the
; protocol.  It mirrors *breaklevel* / *toplevel* in xlinit.lsp.

(define *ide-level* 0)          ; current break level, 0 = top level
(define *ide-current-id* #f)    ; id of the evaluation in progress, if any

; evaluate every form in TEXT in ENV; return the values of the last form
(define (ide-eval-text text env)
  (let ((port (make-string-input-stream text)))
    (let loop ((form (read port)) (vals '()))
      (if (eof-object? form)
          vals
          (loop (read port) (multiple-value-list (eval form env)))))))

(define (ide-handle-eval msg env)
  (let ((id (cadr msg)) (text (caddr msg)))
    (set! *ide-current-id* id)
    (%ide-set-eval-id id)
    (let ((vals (ide-eval-text text env)))
      (%ide-flush)
      (%ide-send "result" "id" id "level" *ide-level*
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

; called by the interpreter when an evaluation fails
(define (*error-handler* fun env sp)
  (let ((failed-id *ide-current-id*)
        (message (%ide-take-error-text))
        (level (1+ *ide-level*)))
    (%ide-flush)
    (%ide-send "error" "id" failed-id "level" level "message" message)
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

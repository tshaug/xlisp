# xlide-host protocol, version 1

`xlide-host` is the xlisp process the IDE launches, one per workspace. It speaks
**line-delimited JSON** on stdin/stdout: one JSON object per line, UTF-8,
no embedded newlines (the host escapes them). stderr carries host diagnostics
only. The host runs one evaluation at a time; messages that arrive meanwhile
are queued, except `interrupt`, which takes effect immediately.

Every message has a `"type"`. Unknown types and unknown fields are ignored in
both directions, so the protocol can grow. Launch the host with `XLPATH` set to
the directory holding `xlisp.lsp`, `xlinit.lsp` and `ide.lsp` (the default is
the executable's directory).

## Conventions

- **`id`** is an integer chosen by the IDE for each `eval`. The host tags
  `output`, `result`, `error`, `aborted` and `input-request` with it.
- **`level`** is the break level: 0 is the top level; each unhandled error
  enters the next level. Messages carry the level they belong to.
- **Null and false.** In xlisp `#f` and the empty list are the same value, so
  the host writes both as `null`. A `null` where a list is expected means an
  empty list; `id` is `null` or absent for output produced outside an `eval`.
- **`source`** is an optional object `{"file": string, "line": int, "column": int}`
  (any member may be missing). The host never interprets it; it echoes it.

## IDE → host

| type | fields | meaning |
|---|---|---|
| `eval` | `id`, `text`, optional `source` | Read and evaluate every form in `text`, in the environment of the current break level (the top level at level 0, the failed function's context otherwise). The IDE loads a file by sending one `eval` per top-level form. |
| `cleanup` | | Pop one break level. The evaluation that failed into it is abandoned (`aborted`). Ignored at level 0. |
| `reset` | | Abort to the top level, abandoning every pending evaluation (`aborted` for each). |
| `input` | `text` | Answer an `input-request`. A trailing newline is added if missing. |
| `interrupt` | | Abort the running evaluation to the top level. Cooperative: it is noticed within ~1000 bytecodes or on the next output character. If the host is waiting for input, the read gets end-of-file. It has no effect on an idle host. |

Closing stdin shuts the host down.

## Host → IDE

| type | fields | meaning |
|---|---|---|
| `hello` | `protocol` (1), `xlisp` (version string), `capabilities` (array) | Sent once, when the host is ready for `eval`. Capabilities in this version: `interrupt`, `input`, `stack`, `source`. |
| `output` | `id`, `text` | Text the Lisp program wrote. Coalesced: sent when 4 KB are pending, after 50 ms, before any other message, and at the end of an evaluation. xlisp has one console channel, so there is no stdout/stderr distinction. |
| `result` | `id`, `level`, `values` (array of strings), `source` | The evaluation finished. `values` are the printed (`write`) values of the last form; empty if none. |
| `error` | `id`, `level`, `message`, `stack`, `source` | The evaluation failed. `level` is the new break level, which is now active: send `eval` to inspect it, then `cleanup` or `reset`. `message` is xlisp's error text without the `Error:` prefix. |
| `aborted` | `id`, `level` | The evaluation `id` will not produce a `result`: its break level was popped (`cleanup`), reset, or it was interrupted. `level` is the level after the abort. |
| `input-request` | `id` | The program called `read`, `read-line` or similar. The IDE should ask the user and reply with `input` (or `interrupt`). |

### `stack`

An array of frames, innermost first: `{"name": string|null, "args": [{"name": string, "value": string}]}`.
`name` is null for an anonymous function. `value` is the printed value,
truncated to 200 characters. Frames belonging to the host's own loop are left
out. Tail calls do not leave frames (xlisp does not keep them); the interpreter's
`set-debug-mode!` turns tail-call optimisation off for complete stacks.

## Lifecycle of an evaluation

```
IDE  eval id=7                   host  output id=7 ...   (zero or more)
                                 host  result id=7       -- done
or                               host  error id=7 level=1
IDE  eval id=8  (in the level)   host  result id=8 level=1
IDE  cleanup                     host  aborted id=7 level=0
or
IDE  interrupt                   host  aborted id=7 level=0
```

A new `eval` is only started once the previous one has finished, aborted, or
failed into a break level.

## Not in version 1

Evaluate-in-chosen-frame, `trace` events, symbol and class listing, and
completion queries are planned (see the IDE roadmap); they will arrive as new
message types announced through `capabilities`.

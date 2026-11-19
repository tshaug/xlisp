#!/usr/bin/env python3
"""Protocol test for xlide-host.  Usage: host_test.py <bin-dir>
<bin-dir> holds xlide-host(.exe) and the .lsp files (cmake --install)."""
import json, os, queue, subprocess, sys, threading, time

bindir = os.path.abspath(sys.argv[1])
exe = os.path.join(bindir, "xlide-host.exe" if os.name == "nt" else "xlide-host")
env = dict(os.environ, XLPATH=bindir)
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
q = queue.Queue()
threading.Thread(target=lambda: [q.put(l) for l in p.stdout] or q.put(None), daemon=True).start()
failures = []

def send(**m):
    p.stdin.write((json.dumps(m) + "\n").encode()); p.stdin.flush()

def recv(timeout=10):
    l = q.get(timeout=timeout)
    if l is None: raise EOFError("host exited")
    return json.loads(l)

def expect(what, ok, got=None):
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else "  got: %r" % (got,)))
    if not ok: failures.append(what)

def collect(until, timeout=10):
    """collect messages until one has type in `until`"""
    out = []
    while True:
        m = recv(timeout); out.append(m)
        if m["type"] in until: return out

def text(msgs): return "".join(m["text"] for m in msgs if m["type"] == "output")

h = recv()
expect("hello", h["type"] == "hello" and h["protocol"] == 1 and "interrupt" in h["capabilities"], h)

send(type="eval", id=1, text="(+ 1 2)")
r = collect(["result"])[-1]
expect("result", r["id"] == 1 and r["values"] == ["3"] and r["level"] == 0, r)

send(type="eval", id=2, text='(begin (display "hi") (newline) 42)')
m = collect(["result"])
expect("output tagged with id", text(m) == "hi\n" and all(x["id"] == 2 for x in m), m)
expect("result after output", m[-1]["values"] == ["42"], m[-1])

send(type="eval", id=3, text='(* 100000 100000)')
expect("64-bit fixnum", collect(["result"])[-1]["values"] == ["10000000000"])

send(type="eval", id=4, text='"héllo 世界"')
expect("utf-8 round trip", collect(["result"])[-1]["values"] == ['"héllo 世界"'])

# error -> break level, evaluate in it, pop it
send(type="eval", id=5, text="(car 5)")
e = collect(["error"])[-1]
expect("error message", e["id"] == 5 and e["level"] == 1 and "incorrect type" in e["message"], e)
send(type="eval", id=6, text="(+ 1 1)")
r = collect(["result"])[-1]
expect("eval inside break level", r["id"] == 6 and r["level"] == 1 and r["values"] == ["2"], r)
send(type="cleanup")
a = collect(["aborted"])[-1]
expect("cleanup aborts the failed eval", a["id"] == 5 and a["level"] == 0, a)
send(type="eval", id=7, text="(+ 2 2)")
expect("top level works after cleanup", collect(["result"])[-1]["values"] == ["4"])

# nested errors and reset
send(type="eval", id=8, text="(car 5)"); collect(["error"])
send(type="eval", id=9, text="(cdr 6)"); e = collect(["error"])[-1]
expect("nested error level 2", e["level"] == 2, e)
send(type="reset")
ids = sorted(m["id"] for m in collect(["aborted"]) if m["type"] == "aborted")
more = collect(["aborted"], 2) if len(ids) < 2 else []
ids += [m["id"] for m in more if m["type"] == "aborted"]
expect("reset aborts both evals", sorted(ids) == [8, 9], ids)
send(type="eval", id=10, text="(+ 3 3)")
expect("top level works after reset", collect(["result"])[-1]["values"] == ["6"])

# interrupt a runaway evaluation
send(type="eval", id=11, text="(let loop () (loop))")
time.sleep(0.4)
send(type="interrupt")
a = collect(["aborted"])[-1]
expect("interrupt aborts a runaway evaluation", a["id"] == 11 and a["level"] == 0, a)
send(type="eval", id=12, text="(+ 4 4)")
expect("top level works after interrupt", collect(["result"])[-1]["values"] == ["8"])

# Lisp reads input
send(type="eval", id=13, text="(read)")
ir = collect(["input-request"])[-1]
expect("input-request", ir["id"] == 13, ir)
send(type="input", text="(1 2 3)")
expect("input answered", collect(["result"])[-1]["values"] == ["(1 2 3)"])

# shutdown on EOF
p.stdin.close()
try: p.wait(10); expect("exits when stdin closes", p.returncode == 0, p.returncode)
except subprocess.TimeoutExpired: p.kill(); expect("exits when stdin closes", False)

print("host test OK" if not failures else "host test FAILED: %s" % failures)
sys.exit(1 if failures else 0)

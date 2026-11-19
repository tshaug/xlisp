/* xlide-host.c - xlisp host process for the xlisp IDE

   Speaks line-delimited JSON on stdin/stdout (protocol version 1).
   All Lisp console I/O goes through the callbacks below; the REPL loop
   itself lives in ide.lsp and uses the %IDE-* primitives defined here.

   Messages received:  eval, cleanup, reset, input, interrupt
   Messages sent:      hello, output, result, error, aborted, input-request
*/

#include "xlisp.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define PROTOCOL_VERSION 1
#define MAX_LINE (16 * 1024 * 1024)
#define OUT_FLUSH_BYTES 4096
#define OUT_FLUSH_MS 50
#define POLL_INTERVAL_MS 20

/* ---------------------------------------------------------------- time */

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* ------------------------------------------------------- growable buffer */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static void buf_reserve(Buf *b,size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < b->len + extra + 1)
            ncap *= 2;
        b->p = (char *)realloc(b->p,ncap);
        if (!b->p) {
            fprintf(stderr,"xlide-host: out of memory\n");
            exit(3);
        }
        b->cap = ncap;
    }
}

static void buf_add(Buf *b,const char *s,size_t n)
{
    buf_reserve(b,n);
    memcpy(b->p + b->len,s,n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_addc(Buf *b,char c)
{
    buf_add(b,&c,1);
}

static void buf_adds(Buf *b,const char *s)
{
    buf_add(b,s,strlen(s));
}

static void buf_clear(Buf *b)
{
    b->len = 0;
    if (b->p) b->p[0] = '\0';
}

/* ------------------------------------------------------------ raw stdio */

#ifdef _WIN32
static HANDLE hin, hout;

static void io_init(void)
{
    hin = GetStdHandle(STD_INPUT_HANDLE);
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
}

/* 1 = data (or EOF) is available to read, 0 = would block */
static int in_ready(void)
{
    DWORD type = GetFileType(hin), avail;
    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(hin,NULL,0,NULL,&avail,NULL))
            return 1;               /* broken pipe: reading reports EOF */
        return avail > 0;
    }
    return 1;                       /* files: never block */
}

static long in_read(char *buf,size_t n)
{
    DWORD got;
    if (!ReadFile(hin,buf,(DWORD)n,&got,NULL))
        return 0;
    return (long)got;
}

static void out_write(const char *s,size_t n)
{
    while (n > 0) {
        DWORD put;
        if (!WriteFile(hout,s,(DWORD)n,&put,NULL) || put == 0)
            exit(0);                /* the IDE went away */
        s += put;
        n -= put;
    }
}
#else
static void io_init(void)
{
    signal(SIGPIPE,SIG_IGN);
}

static int in_ready(void)
{
    struct pollfd pfd;
    pfd.fd = 0;
    pfd.events = POLLIN;
    return poll(&pfd,1,0) > 0;
}

static long in_read(char *buf,size_t n)
{
    for (;;) {
        ssize_t got = read(0,buf,n);
        if (got < 0 && errno == EINTR)
            continue;
        return got < 0 ? 0 : (long)got;
    }
}

static void out_write(const char *s,size_t n)
{
    while (n > 0) {
        ssize_t put = write(1,s,n);
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            exit(0);                /* the IDE went away */
        s += put;
        n -= (size_t)put;
    }
}
#endif

/* ---------------------------------------------------------- JSON writer */

static void json_string(Buf *b,const char *s,size_t n)
{
    size_t i = 0;
    buf_addc(b,'"');
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       { buf_adds(b,"\\\""); i++; }
        else if (c == '\\') { buf_adds(b,"\\\\"); i++; }
        else if (c == '\n') { buf_adds(b,"\\n"); i++; }
        else if (c == '\r') { buf_adds(b,"\\r"); i++; }
        else if (c == '\t') { buf_adds(b,"\\t"); i++; }
        else if (c < 0x20)  { char t[8]; sprintf(t,"\\u%04x",c); buf_adds(b,t); i++; }
        else if (c < 0x80)  { buf_addc(b,(char)c); i++; }
        else {
            /* copy a valid UTF-8 sequence, replace anything else */
            size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
            size_t k;
            int ok = len && i + len <= n && c <= 0xF4;
            for (k = 1; ok && k < len; k++)
                if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                    ok = 0;
            if (ok) { buf_add(b,s + i,len); i += len; }
            else    { buf_adds(b,"\\ufffd"); i++; }
        }
    }
    buf_addc(b,'"');
}

static void json_long(Buf *b,xlFIXTYPE n)
{
    char t[40];
    sprintf(t,"%" PRIdPTR,n);
    buf_adds(b,t);
}

/* ---------------------------------------------------------- JSON parser */

typedef struct Msg {
    struct Msg *next;
    char *type;
    char *text;
    char *file;
    xlFIXTYPE id, line, column;
    int has_id, has_line, has_column;
} Msg;

typedef struct {
    const char *p, *end;
    Msg *m;
    int ok;
} Parser;

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end && isspace((unsigned char)*ps->p))
        ps->p++;
}

static void put_utf8(Buf *b,unsigned long cp)
{
    if (cp < 0x80)         buf_addc(b,(char)cp);
    else if (cp < 0x800)   { buf_addc(b,(char)(0xC0 | (cp >> 6)));
                             buf_addc(b,(char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { buf_addc(b,(char)(0xE0 | (cp >> 12)));
                             buf_addc(b,(char)(0x80 | ((cp >> 6) & 0x3F)));
                             buf_addc(b,(char)(0x80 | (cp & 0x3F))); }
    else                   { buf_addc(b,(char)(0xF0 | (cp >> 18)));
                             buf_addc(b,(char)(0x80 | ((cp >> 12) & 0x3F)));
                             buf_addc(b,(char)(0x80 | ((cp >> 6) & 0x3F)));
                             buf_addc(b,(char)(0x80 | (cp & 0x3F))); }
}

static int hex4(const char *s,unsigned long *out)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* parse a JSON string into a malloc'ed, NUL-terminated buffer */
static char *parse_string(Parser *ps,size_t *plen)
{
    Buf b = {0,0,0};
    if (ps->p >= ps->end || *ps->p != '"') { ps->ok = 0; return NULL; }
    ps->p++;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') { buf_addc(&b,c); continue; }
        if (ps->p >= ps->end) { ps->ok = 0; break; }
        c = *ps->p++;
        switch (c) {
        case 'b': buf_addc(&b,'\b'); break;
        case 'f': buf_addc(&b,'\f'); break;
        case 'n': buf_addc(&b,'\n'); break;
        case 'r': buf_addc(&b,'\r'); break;
        case 't': buf_addc(&b,'\t'); break;
        case 'u': {
            unsigned long cp, lo;
            if (ps->end - ps->p < 4 || !hex4(ps->p,&cp)) { ps->ok = 0; break; }
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6
            &&  ps->p[0] == '\\' && ps->p[1] == 'u' && hex4(ps->p + 2,&lo)
            &&  lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ps->p += 6;
            }
            put_utf8(&b,cp);
            break;
        }
        default:  buf_addc(&b,c); break;      /* \" \\ \/ */
        }
    }
    if (ps->p >= ps->end) ps->ok = 0;
    else ps->p++;                              /* closing quote */
    if (!b.p) buf_addc(&b,'\0'), b.len = 0;
    if (plen) *plen = b.len;
    return b.p;
}

static void parse_value(Parser *ps,const char *key,int depth);

static void parse_object(Parser *ps,int depth)
{
    if (depth > 8 || ps->p >= ps->end || *ps->p != '{') { ps->ok = 0; return; }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return; }
    while (ps->ok) {
        char *key;
        skip_ws(ps);
        key = parse_string(ps,NULL);
        if (!ps->ok) { free(key); return; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { ps->ok = 0; free(key); return; }
        ps->p++;
        skip_ws(ps);
        parse_value(ps,key,depth);
        free(key);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return; }
        ps->ok = 0;
    }
}

static void parse_value(Parser *ps,const char *key,int depth)
{
    Msg *m = ps->m;
    if (ps->p >= ps->end) { ps->ok = 0; return; }
    if (*ps->p == '"') {
        size_t len;
        char *s = parse_string(ps,&len);
        if (!ps->ok) { free(s); return; }
        if (depth == 1 && !strcmp(key,"type") && !m->type)      m->type = s;
        else if (depth == 1 && !strcmp(key,"text") && !m->text) m->text = s;
        else if (depth == 2 && !strcmp(key,"file") && !m->file) m->file = s;
        else free(s);
    }
    else if (*ps->p == '{') {
        parse_object(ps,depth + 1);
    }
    else if (*ps->p == '[') {
        int nest = 0;                          /* skip arrays */
        do {
            if (*ps->p == '[') nest++;
            else if (*ps->p == ']') nest--;
            else if (*ps->p == '"') { free(parse_string(ps,NULL)); continue; }
            ps->p++;
        } while (nest > 0 && ps->p < ps->end);
    }
    else {
        const char *start = ps->p;             /* number, true, false, null */
        while (ps->p < ps->end && *ps->p != ',' && *ps->p != '}' && *ps->p != ']'
           &&  !isspace((unsigned char)*ps->p))
            ps->p++;
        if (ps->p == start) { ps->ok = 0; return; }
        if (isdigit((unsigned char)*start) || *start == '-') {
            xlFIXTYPE n = (xlFIXTYPE)strtoll(start,NULL,10);
            if (depth == 1 && !strcmp(key,"id"))               { m->id = n; m->has_id = 1; }
            else if (depth == 2 && !strcmp(key,"line"))        { m->line = n; m->has_line = 1; }
            else if (depth == 2 && !strcmp(key,"column"))      { m->column = n; m->has_column = 1; }
        }
    }
}

static void msg_free(Msg *m)
{
    if (!m) return;
    free(m->type); free(m->text); free(m->file);
    free(m);
}

static Msg *parse_line(const char *line,size_t n)
{
    Parser ps;
    Msg *m = (Msg *)calloc(1,sizeof(Msg));
    ps.p = line; ps.end = line + n; ps.m = m; ps.ok = 1;
    skip_ws(&ps);
    parse_object(&ps,1);
    if (!ps.ok || !m->type) { msg_free(m); return NULL; }
    return m;
}

/* ----------------------------------------------------- input and queueing */

static Buf inbuf;                  /* raw bytes read, not yet split into lines */
static int in_eof = 0;
static Msg *qhead = NULL, *qtail = NULL;
static int interrupt_flag = 0;

static void enqueue(Msg *m)
{
    if (!strcmp(m->type,"interrupt")) {
        interrupt_flag = 1;
        msg_free(m);
        return;
    }
    m->next = NULL;
    if (qtail) qtail->next = m; else qhead = m;
    qtail = m;
}

/* split complete lines out of inbuf and queue the parsed messages */
static void split_lines(void)
{
    size_t start = 0, i;
    for (i = 0; i < inbuf.len; i++) {
        if (inbuf.p[i] == '\n') {
            size_t n = i - start;
            if (n > 0 && inbuf.p[start + n - 1] == '\r') n--;
            if (n > 0) {
                Msg *m = parse_line(inbuf.p + start,n);
                if (m) enqueue(m);
                else fprintf(stderr,"xlide-host: ignoring malformed message\n");
            }
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(inbuf.p,inbuf.p + start,inbuf.len - start);
        inbuf.len -= start;
        inbuf.p[inbuf.len] = '\0';
    }
    if (inbuf.len > MAX_LINE) {
        fprintf(stderr,"xlide-host: message too long, dropping\n");
        buf_clear(&inbuf);
    }
}

/* read once from stdin; block only if requested */
static void read_more(int block)
{
    char tmp[8192];
    long got;
    if (in_eof) return;
    if (!block && !in_ready()) return;
    got = in_read(tmp,sizeof(tmp));
    if (got <= 0) { in_eof = 1; return; }
    buf_add(&inbuf,tmp,(size_t)got);
    split_lines();
}

/* ---------------------------------------------------------- output side */

static Buf outbuf;                 /* completed lines waiting to be sent */
static Buf linebuf;                /* the line being built */
static Buf heldbuf;                /* "Error:" text kept for the error message */
static int holding = 0;
static long long out_since = 0;
static xlFIXTYPE cur_id = 0;
static int cur_id_set = 0;
static int col = 0;

static void send_buf(Buf *b)
{
    buf_addc(b,'\n');
    out_write(b->p,b->len);
}

static void send_id(Buf *b)
{
    if (cur_id_set) { buf_adds(b,",\"id\":"); json_long(b,cur_id); }
}

static void flush_output(int include_partial)
{
    if (include_partial && linebuf.len) {
        buf_add(&outbuf,linebuf.p,linebuf.len);
        buf_clear(&linebuf);
    }
    if (outbuf.len) {
        Buf m = {0,0,0};
        buf_adds(&m,"{\"type\":\"output\"");
        send_id(&m);
        buf_adds(&m,",\"text\":");
        json_string(&m,outbuf.p,outbuf.len);
        buf_addc(&m,'}');
        send_buf(&m);
        free(m.p);
        buf_clear(&outbuf);
    }
    out_since = 0;
}

/* a failed evaluation prints "Error: ..." lines; keep them for the error message */
static int is_error_line(const char *s,size_t n)
{
    return (n >= 6 && !strncmp(s,"Error:",6))
        || (holding && ((n >= 12 && !strncmp(s,"happened in:",12))
                     || (n >= 11 && !strncmp(s,"call stack:",11))
                     || (n >= 2 && s[0] == ' ' && s[1] == ' ')));
}

static void host_putc(int ch)
{
    buf_addc(&linebuf,(char)ch);
    if (ch == '\n') {
        col = 0;
        if (is_error_line(linebuf.p,linebuf.len)) {
            holding = 1;
            buf_add(&heldbuf,linebuf.p,linebuf.len);
        }
        else {
            holding = 0;
            buf_add(&outbuf,linebuf.p,linebuf.len);
            if (!out_since) out_since = now_ms();
        }
        buf_clear(&linebuf);
        if (outbuf.len >= OUT_FLUSH_BYTES)
            flush_output(0);
    }
    else
        col++;
}

/* send a message built from primitive arguments (see %IDE-SEND) */
static void json_value(Buf *b,xlValue v)
{
    /* note: in xlisp #f and the empty list are both nil; both become null */
    if (v == xlTrue) buf_adds(b,"true");
    else if (v == xlNil) buf_adds(b,"null");
    else if (xlFixnumP(v)) json_long(b,xlGetFixnum(v));
    else if (xlStringP(v)) json_string(b,xlGetString(v),(size_t)xlGetSLength(v));
    else if (xlConsP(v)) {
        int first = 1;
        buf_addc(b,'[');
        while (xlConsP(v)) {
            if (!first) buf_addc(b,',');
            first = 0;
            json_value(b,xlCar(v));
            v = xlCdr(v);
        }
        buf_addc(b,']');
    }
    else if (xlVectorP(v)) {
        /* a vector of alternating key/value pairs is a JSON object */
        xlFIXTYPE i, n = xlGetSize(v);
        buf_addc(b,'{');
        for (i = 0; i + 1 < n; i += 2) {
            xlValue k = xlGetElement(v,i);
            if (i) buf_addc(b,',');
            if (xlStringP(k)) json_string(b,xlGetString(k),(size_t)xlGetSLength(k));
            else buf_adds(b,"\"?\"");
            buf_addc(b,':');
            json_value(b,xlGetElement(v,i + 1));
        }
        buf_addc(b,'}');
    }
    else buf_adds(b,"null");
}

/* ---------------------------------------------------- console callbacks */

static Buf pending_input;          /* text from an "input" message */
static size_t pending_pos = 0;

static int host_atbol(void)             { return col == 0; }
static void host_flushinput(void)       { buf_clear(&pending_input); pending_pos = 0; }
static void host_flushoutput(void)      { flush_output(1); }

static int host_check(void)
{
    static long long last_poll = 0;
    long long now = now_ms();

    if (out_since && now - out_since >= OUT_FLUSH_MS)
        flush_output(0);
    if (now - last_poll >= POLL_INTERVAL_MS) {
        last_poll = now;
        read_more(0);
    }
    if (interrupt_flag) {
        interrupt_flag = 0;
        return 3;                       /* ^C: abort to top level */
    }
    return 0;
}

static void host_exit(int sts)
{
    flush_output(1);
    exit(sts);
}

/* Lisp code reads from the console: ask the IDE for a line of input */
static int host_getc(void)
{
    if (pending_pos >= pending_input.len) {
        Buf m = {0,0,0};
        buf_clear(&pending_input);
        pending_pos = 0;
        flush_output(1);
        buf_adds(&m,"{\"type\":\"input-request\"");
        send_id(&m);
        buf_addc(&m,'}');
        send_buf(&m);
        free(m.p);
        for (;;) {
            Msg *p, *prev = NULL;
            for (p = qhead; p; prev = p, p = p->next)
                if (!strcmp(p->type,"input")) {
                    if (prev) prev->next = p->next; else qhead = p->next;
                    if (qtail == p) qtail = prev;
                    buf_adds(&pending_input,p->text ? p->text : "");
                    if (!pending_input.len || pending_input.p[pending_input.len - 1] != '\n')
                        buf_addc(&pending_input,'\n');
                    msg_free(p);
                    goto got_input;
                }
            if (interrupt_flag) { interrupt_flag = 0; return EOF; }
            if (in_eof) return EOF;
            read_more(1);
        }
    }
got_input:
    return (unsigned char)pending_input.p[pending_pos++];
}

/* ------------------------------------------------------------ primitives */

/* (%ide-next-message) => ("eval" id text file line column) | ("cleanup") |
   ("reset") | ("input" text) | ("shutdown") ; blocks until a message arrives */
static xlValue x_next_message(void)
{
    Msg *m;
    xlLastArg();
    flush_output(1);
    if (!qhead) interrupt_flag = 0;     /* a stale interrupt must not hit the next eval */
    while (!qhead) {
        if (in_eof) {
            xlVal = xlCons(xlMakeCString("shutdown"),xlNil);
            return xlVal;
        }
        read_more(1);
        if (!qhead) interrupt_flag = 0; /* interrupts for an idle host are ignored */
    }
    m = qhead;
    qhead = m->next;
    if (!qhead) qtail = NULL;

    xlVal = xlNil;
    if (!strcmp(m->type,"eval")) {
        xlValue v;
        xlVal = xlCons(m->has_column ? xlMakeFixnum(m->column) : xlNil,xlVal);
        xlVal = xlCons(m->has_line ? xlMakeFixnum(m->line) : xlNil,xlVal);
        v = m->file ? xlMakeCString(m->file) : xlNil;
        xlVal = xlCons(v,xlVal);
        v = xlMakeCString(m->text ? m->text : "");
        xlVal = xlCons(v,xlVal);
        xlVal = xlCons(m->has_id ? xlMakeFixnum(m->id) : xlNil,xlVal);
    }
    else if (!strcmp(m->type,"input")) {
        xlValue v = xlMakeCString(m->text ? m->text : "");
        xlVal = xlCons(v,xlVal);
    }
    {
        xlValue v = xlMakeCString(m->type);
        xlVal = xlCons(v,xlVal);
    }
    msg_free(m);
    return xlVal;
}

/* (%ide-send type [key value]...) ; values: string, fixnum, #t, #f, nil, list */
static xlValue x_send(void)
{
    Buf m = {0,0,0};
    xlValue type = xlGetArgString();
    flush_output(1);
    buf_adds(&m,"{\"type\":");
    json_string(&m,xlGetString(type),(size_t)xlGetSLength(type));
    while (xlMoreArgsP()) {
        xlValue key = xlGetArgString();
        xlValue val = xlGetArg();
        buf_addc(&m,',');
        json_string(&m,xlGetString(key),(size_t)xlGetSLength(key));
        buf_addc(&m,':');
        json_value(&m,val);
    }
    buf_addc(&m,'}');
    send_buf(&m);
    free(m.p);
    return xlTrue;
}

/* (%ide-set-eval-id id-or-nil): tag subsequent output with this request id */
static xlValue x_set_eval_id(void)
{
    xlValue v = xlGetArg();
    xlLastArg();
    if (xlFixnumP(v)) { cur_id = xlGetFixnum(v); cur_id_set = 1; }
    else cur_id_set = 0;
    return xlTrue;
}

/* (%ide-flush): send any pending output */
static xlValue x_flush(void)
{
    xlLastArg();
    flush_output(1);
    return xlTrue;
}

/* (%ide-take-error-text) => the held "Error:" text (without the prefix), or "" */
static xlValue x_take_error_text(void)
{
    char *s = heldbuf.p ? heldbuf.p : (char *)"";
    size_t n = heldbuf.len, line;
    xlLastArg();
    if (n >= 6 && !strncmp(s,"Error:",6)) { s += 6; n -= 6; }
    while (n && *s == ' ') { s++; n--; }
    for (line = 0; line < n && s[line] != '\n'; line++)
        ;
    xlVal = xlMakeString(s,(xlFIXTYPE)line);
    buf_clear(&heldbuf);
    holding = 0;
    return xlVal;
}

/* -------------------------------------------------------------------- main */

int main(int argc,const char *argv[])
{
    xlCallbacks *cb;

    io_init();
    cb = xlDefaultCallbacks(argv[0]);
    cb->consoleGetC = host_getc;
    cb->consolePutC = host_putc;
    cb->consoleAtBOLP = host_atbol;
    cb->consoleFlushInput = host_flushinput;
    cb->consoleFlushOutput = host_flushoutput;
    cb->consoleCheck = host_check;
    cb->exit = host_exit;

    xlInit(cb,argc,argv,NULL);
    xlSubr("%IDE-NEXT-MESSAGE",x_next_message);
    xlSubr("%IDE-SEND",x_send);
    xlSubr("%IDE-SET-EVAL-ID",x_set_eval_id);
    xlSubr("%IDE-FLUSH",x_flush);
    xlSubr("%IDE-TAKE-ERROR-TEXT",x_take_error_text);

    xlLoadFile("xlisp.lsp");
    xlLoadFile("ide.lsp");
    {
        Buf m = {0,0,0};
        buf_adds(&m,"{\"type\":\"hello\",\"protocol\":");
        json_long(&m,PROTOCOL_VERSION);
        buf_adds(&m,",\"xlisp\":");
        json_string(&m,"3.3",3);
        buf_adds(&m,",\"capabilities\":[\"interrupt\",\"input\",\"stack\",\"source\"]}");
        send_buf(&m);
        free(m.p);
    }
    xlCallFunctionByName(NULL,0,"*IDE-TOPLEVEL*",0);
    host_exit(0);
    return 0;
}

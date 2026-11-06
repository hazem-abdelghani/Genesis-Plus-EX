#!/usr/bin/env python3
"""Makes Languages/Template.pot, the list of the program's text, from the source files.

  python3 tools/make_pot.py            (run in the win32 folder)

A translator copies Template.pot to <Name>.po (Name is what the Options > Language menu shows), fills in the msgstr
of the lines to translate and leaves the others empty (they stay English). The file is UTF-8.
"""
import glob, os, re, sys

SKIP_DIRS = ("picodrive", "xbrz", "lzmasdk", "librashader", "xbr", "tools")
SRC = [f for f in sorted(glob.glob("*.c") + glob.glob("*.h")) if f not in ("stb_image.h", "i18n.c", "i18n.h", "paprium.c", "eggs.c")]      # (paprium.c: the soundtrack's file names and the log; eggs.c: the hidden surprises are not for translators)


def decode(raw):
    out, i = [], 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw):
            i += 1
            c = raw[i]
            if c == "n": out.append("\n")
            elif c == "r": out.append("\r")
            elif c == "t": out.append("\t")
            elif c == "x" or c.isdigit():
                m = re.match(r"x([0-9a-fA-F]{1,2})|([0-7]{1,3})", raw[i:])
                if m:
                    v = int(m.group(1), 16) if m.group(1) else int(m.group(2), 8)
                    out.append(chr(v))
                    i += len(m.group(0)) - 1
            else: out.append(c)
        else: out.append(c)
        i += 1
    return "".join(out)


def literals(text):
    """(line number, text of the line, the concatenated literal) for every run of adjacent string literals in a C file."""
    i, n, line = 0, len(text), 1
    run, run_line = [], 0
    last_end_ws = True
    res = []
    lines = text.split("\n")

    def flush():
        nonlocal run
        if run:
            res.append((run_line, lines[run_line - 1], "".join(run)))
            run = []

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1; i += 1; continue
        if c in " \t\r":
            i += 1; continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j; continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            seg = text[i:(n if j < 0 else j + 2)]
            line += seg.count("\n"); i = n if j < 0 else j + 2; continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            if not run: run_line = line
            run.append(decode(text[i + 1:j]))
            i = j + 1; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            flush(); i = j + 1; continue
        flush(); i += 1
    flush()
    return res


def wanted(s, srcline):
    if s == "English": return False                                                                       # a language is named in its own language
    if not re.search(r"[A-Za-z]{2}", s): return False
    if re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", s): return False                                          # binary data
    if " " not in s and re.search(r"[a-z0-9][A-Z]", s): return False                                      # CamelCase names of API calls
    if re.search(r"\b(ini_|GetPrivateProfile|WritePrivateProfile|gui_log|OutputDebug|GetProcAddress|LoadLibrary|RegisterClass|CreateWindow[A-Za-z]*\(\s*[0-9A-Za-z_|]+\s*,\s*\")", srcline) and "CreateWindow" not in srcline: return False
    if re.fullmatch(r"[A-Za-z0-9_.\\/*%:+-]+", s) and " " not in s and not s[0].isupper(): return False   # names, extensions, paths
    if re.fullmatch(r"[A-Z0-9_]+", s): return False                                                       # CLASS NAMES
    if s.startswith(("#", "http")) or "\\*" in s or s.lower().endswith((".dll", ".exe", ".ini", ".txt", ".png", ".po", ".mp3", ".wav", ".ogg", ".bin", ".gif", ".mp4", ".cht", ".wch", ".gpm", ".slangp")): return False
    if re.fullmatch(r"[%0-9a-zA-Z.\-_ ]*\.(c|h)", s): return False
    if s.startswith("--") or re.match(r'^"?%s"? --', s) or re.match(r"^[A-Za-z]+\\", s) or s == "HTTP/": return False   # command lines and folder paths: not text
    return True


def rc_strings():
    res = []
    for ln, l in enumerate(open("gpex.rc", encoding="utf-8", errors="replace"), 1):
        m = re.match(r'\s*(MENUITEM|POPUP|CAPTION|LTEXT|CTEXT|RTEXT|PUSHBUTTON|DEFPUSHBUTTON|GROUPBOX|CHECKBOX|AUTOCHECKBOX|RADIOBUTTON|AUTORADIOBUTTON|CONTROL)\s+"((?:[^"]|"")*)"', l)
        if not m: continue
        t = decode(m.group(2).replace('""', '"'))
        if m.group(1) == "MENUITEM" and "\t" in t: t = t.split("\t")[0]
        if m.group(1) == "CONTROL" and not re.search(r"BS_|SS_|\"Button\"|\"Static\"", l): continue
        if re.search(r"[A-Za-z]{2}", t): res.append(("gpex.rc:%d" % ln, t))
    return res


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def main():
    seen, order = {}, []
    for f in SRC:
        text = open(f, encoding="utf-8", errors="replace").read()
        for ln, srcline, s in literals(text):
            if len(s) > 300 and "\n" in s:                      # a long text (the User Guide): the program translates it a line at a time
                for part in s.split("\n"):
                    part = part.rstrip("\r")
                    if 0 < len(part) < 1024 and re.search(r"[A-Za-z]{2}", part) and part not in seen:
                        seen[part] = "%s:%d" % (f, ln); order.append(part)
                continue
            if f == "upnp.c" and not re.search(r"router|UPnP", s): continue          # (the rest of upnp.c is the network protocol)
            if s not in seen and wanted(s, srcline):
                seen[s] = "%s:%d" % (f, ln); order.append(s)
    for where, s in rc_strings():
        if s not in seen: seen[s] = where; order.append(s)
    os.makedirs("Languages", exist_ok=True)
    with open("Languages/Template.pot", "w", encoding="utf-8", newline="\n") as o:
        o.write('# Genesis Plus EX: the text of the program.\n# Copy this file to <Language>.po (the name is what the Options > Language menu shows),\n'
                '# fill in msgstr for the text to translate and leave the others empty. Keep the & (the underlined key of a menu item),\n'
                '# the %s %d %X ... (they are replaced by values, in the same order) and the \\n and \\r\\n line ends.\n# Language-Name (in the header below) is how the language is listed in the Options menu: write it in that language (Français).\n\n'
                'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Language: \\n"\n"Language-Name: \\n"\n\n')
        for s in order:
            o.write("#: %s\nmsgid \"%s\"\nmsgstr \"\"\n\n" % (seen[s], esc(s)))
    print(len(order), "texts")


if __name__ == "__main__":
    main()

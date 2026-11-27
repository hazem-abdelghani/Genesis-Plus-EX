#!/usr/bin/env python3
"""Makes Languages/Pseudo.po, a fake language to test how the program looks with long translations.

  python3 tools/make_pseudo.py            (run in the win32 folder)

Every text of Template.pot comes out about 40% longer, with accented letters, between [ and ]: if the ] is missing on the screen,
that control is too narrow for a long translation. The & keys, the %s %d ... and the line ends are kept. Pick "Pseudo" in
Options > Language to try it. Test only: the file is not part of the release.
"""
import re

TOKEN = re.compile(r'\\[nrt"\\]|\\x[0-9a-fA-F]{1,2}|%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|I64|z)?[a-zA-Z%]|&&|&[A-Za-z0-9]|[0-9]+|\s+')
MAP = str.maketrans("aeiouyAEIOUYcnsCNS", "àéîõüÿÀÉÎÕÜÝçñšÇÑŠ")


def pseudo_line(s):
    out, plain, n, last = [], 0, 0, 0
    for m in TOKEN.finditer(s):
        part = s[last:m.start()]
        out.append(part.translate(MAP)); n += len(part)
        t = m.group(0)
        if t.startswith("&") and len(t) == 2 and t != "&&": out.append("&" + t[1].translate(MAP))
        else: out.append(t)
        if not t.startswith(("%", "\\")): n += len(t)
        last = m.end()
    part = s[last:]
    out.append(part.translate(MAP)); n += len(part)
    return "".join(out), n


def pseudo(s):
    if not re.search(r"[A-Za-z]", re.sub(r"%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?[a-zA-Z]|\\.", "", s)): return s
    lines = re.split(r"(\\r\\n|\\n)", s)
    out = []
    for i, ln in enumerate(lines):
        if i % 2 or not ln.strip(): out.append(ln); continue
        txt, n = pseudo_line(ln)
        pad = max(2, n * 2 // 5)
        out.append(txt + "~" * pad)
    r = "".join(out)
    return "[" + r + "]"


def main():
    src = open("Languages/Template.pot", encoding="utf-8").read().split("\n")
    out, i = [], 0
    while i < len(src):
        ln = src[i]
        if ln.startswith("msgid "):
            parts = [re.match(r'msgid "(.*)"$', ln).group(1)]
            i += 1
            while i < len(src) and src[i].startswith('"'):
                parts.append(src[i][1:-1]); i += 1
            mid = "".join(parts)
            out.append(ln if len(parts) == 1 else 'msgid ""')
            if len(parts) > 1:
                out.extend('"%s"' % p for p in parts)
            while i < len(src) and src[i].startswith("msgstr"): i += 1
            if mid == "": out.append('msgstr ""'); continue
            out.append('msgstr "%s"' % pseudo(mid))
            continue
        out.append(ln.replace('"Language-Name: \\n"', '"Language-Name: Pseudo (long text test)\\n"'))
        i += 1
    open("Languages/Pseudo.po", "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print("Languages/Pseudo.po written")


main()

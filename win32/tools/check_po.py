#!/usr/bin/env python3
"""Checks a translation file (Languages/<Name>.po) against the program's English text.

  python3 tools/check_po.py Languages/French.po [more.po ...]      (run in the win32 folder; no file: every .po of Languages)

The program fills values into some texts (%s a text, %d a number, %lu a big number, %X ...). A translation must have the same % markers
as the English text, in the same order: otherwise the program would read a value as another kind and could crash or show garbage, so it does
not use that translation (it stays English). This lists the ones that differ, and warns about line ends (\\n, \\r\\n), &-keys and tabs
that differ (the program still uses those). Exit code 1 when a marker differs.
"""
import glob, os, re, sys

SPEC = re.compile(r"%(?:%|[-+0]*(\*|\d+)?(?:\.(\*|\d+))?(I64|I32|hh|h|ll|l|z|j|t|L|w)?([diouxXeEfgGaAcCsSpn]))")


def signature(text):
    sig = []
    for m in SPEC.finditer(text):
        if m.group(0) == "%%":
            continue
        sig.append(("*" if m.group(1) == "*" else "") + ("*" if m.group(2) == "*" else "") + (m.group(3) or "") + m.group(4))
    return sig


def unquote(s):
    s = s.strip()
    if not (s.startswith('"') and s.endswith('"')):
        return ""
    out, i, s = [], 1, s[:-1]
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            i += 1
            c = {"n": "\n", "r": "\r", "t": "\t"}.get(s[i], s[i])
        out.append(c)
        i += 1
    return "".join(out)


def entries(path):
    """(line number of the msgid, msgid, msgstr, fuzzy) of every entry: each entry is collected whole before it is returned."""
    res = []
    cur = None                                   # {"line", "id", "str", "field", "fuzzy"}
    flag_fuzzy = False

    def close():
        nonlocal cur
        if cur and cur["field"] == 2:
            res.append((cur["line"], cur["id"], cur["str"], cur["fuzzy"]))
        cur = None

    for n, raw in enumerate(open(path, encoding="utf-8-sig", errors="replace"), 1):
        l = raw.strip()
        if l.startswith("#"):
            if cur and cur["field"] == 2:
                close()                           # a comment starts the next entry
            if l.startswith("#,") and "fuzzy" in l:
                flag_fuzzy = True
            continue
        if l.startswith("msgid"):
            close()
            cur = {"line": n, "id": unquote(l[5:]), "str": "", "field": 1, "fuzzy": flag_fuzzy}
            flag_fuzzy = False
        elif l.startswith("msgstr") and cur:
            cur["field"], cur["str"] = 2, unquote(l[6:])
        elif l.startswith('"') and cur:
            cur["id" if cur["field"] == 1 else "str"] += unquote(l)
    close()
    return res


def check(path):
    bad = warn = 0
    for line, mid, mstr, fuzzy in entries(path):
        if not mid or not mstr or fuzzy:
            continue
        a, b = signature(mid), signature(mstr)
        short = (mid[:60] + "...") if len(mid) > 60 else mid
        if a != b:
            bad += 1
            print("%s:%d: the %% markers differ (not used)\n    English:     %s   [%s]\n    Translation: %s   [%s]" % (path, line, short, " ".join(a) or "none", mstr[:60], " ".join(b) or "none"))
            continue
        if mid.count("\n") != mstr.count("\n") or mid.count("\r\n") != mstr.count("\r\n"):
            warn += 1
            print("%s:%d: warning: the line ends differ: %s" % (path, line, short))
        if mid.count("\t") != mstr.count("\t"):
            warn += 1
            print("%s:%d: warning: the tab (the shortcut of a menu item) differs: %s" % (path, line, short))
        if ("&" in mid) != ("&" in mstr) and "\t" not in mid and len(mid) < 60:
            warn += 1
            print("%s:%d: warning: the & (underlined key) differs: %s" % (path, line, short))
    print("%s: %d with different markers, %d warnings" % (path, bad, warn))
    return bad


# The same rules as marker_signature() in i18n.c (the program): a text and the signature both must give. Run: python3 tools/check_po.py --selftest
CASES = [
    ("Loaded %s", ["s"]), ("%d%% done", ["d"]), ("%lu|", ["lu"]), ("%5.2f and %-8s", ["f", "s"]), ("%*d", ["*d"]), ("%I64u bytes", ["I64u"]),
    ("50% of it", []), ("100 %", []), ("%%", []), ("%hhd %lld %zu", ["hhd", "lld", "zu"]), ("%.*s", ["*s"]),
]


def selftest():
    bad = 0
    for text, want in CASES:
        got = signature(text)
        if got != want:
            bad += 1
            print("selftest: %r gives %r, expected %r" % (text, got, want))
    print("selftest: %d cases, %d wrong (i18n.c marker_signature must agree with these)" % (len(CASES), bad))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    files = sys.argv[1:] or sorted(glob.glob(os.path.join("Languages", "*.po")))
    if not files:
        print("no .po file given and none in Languages")
        return 2
    return 1 if sum(check(f) for f in files) else 0


if __name__ == "__main__":
    sys.exit(main())

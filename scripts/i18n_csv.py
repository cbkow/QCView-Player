#!/usr/bin/env python3
"""CSV round trip for QCView's translation catalogs (translations/*.ts).

The .ts files are the source of truth; a CSV is a view onto one of them
that a translator can edit in a spreadsheet. Export writes one row per
string (one per plural form), import copies the translation column back
and refuses rows whose placeholders no longer match the English.

    scripts/i18n_csv.py export translations/qcview_de.ts de.csv
    scripts/i18n_csv.py import translations/qcview_de.ts de.csv [--dry-run] [--force]
    scripts/i18n_csv.py stats  translations/qcview_de.ts

Columns: context, source, disambiguation, developer_comment, form,
translation, status, location. Translators edit `translation` only.
`form` is empty for ordinary strings and 0, 1, ... for the plural forms
of a `%n` string. `status` is unfinished / done / vanished / obsolete;
vanished and obsolete rows are no longer in the app and are skipped.

Standard library only; Python 3.9+.
"""

import argparse
import csv
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

PLACEHOLDER = re.compile(r"%L?\d{1,2}|%n")
HEADER = ["context", "source", "disambiguation", "developer_comment",
          "form", "translation", "status", "location"]


def load(ts_path):
    tree = ET.parse(ts_path)
    return tree, tree.getroot()


def save(ts_path, root):
    body = ET.tostring(root, encoding="unicode")
    Path(ts_path).write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n' + body + "\n",
        encoding="utf-8")


def text(el):
    return "" if el is None or el.text is None else el.text


def status_of(msg):
    tr = msg.find("translation")
    t = tr.get("type") if tr is not None else None
    if t in ("vanished", "obsolete"):
        return t
    if t == "unfinished":
        return "unfinished"
    return "done"


def locations(root):
    """Absolute file:line per message. lupdate -locations relative writes
    line="+N" offsets against the previous location in the same file."""
    out = {}
    last_file, running = None, {}
    for ctx in root.iter("context"):
        for msg in ctx.iter("message"):
            loc = msg.find("location")
            if loc is None:
                out[msg] = ""
                continue
            fn = loc.get("filename") or last_file
            line = loc.get("line") or ""
            if line.startswith(("+", "-")):
                running[fn] = running.get(fn, 0) + int(line)
                line = str(running[fn])
            elif line:
                running[fn] = int(line)
            last_file = fn
            out[msg] = "%s:%s" % (Path(fn or "").as_posix().lstrip("./"), line)
    return out


def placeholders(s):
    return sorted(set(PLACEHOLDER.findall(s)))


def rows_for(ctx_name, msg, loc):
    src = text(msg.find("source"))
    dis = text(msg.find("comment"))
    dev = text(msg.find("extracomment"))
    st = status_of(msg)
    tr = msg.find("translation")
    if msg.get("numerus") == "yes":
        forms = [] if tr is None else tr.findall("numerusform")
        for i, f in enumerate(forms):
            yield [ctx_name, src, dis, dev, str(i), text(f), st, loc]
    else:
        yield [ctx_name, src, dis, dev, "", text(tr), st, loc]


def cmd_export(args):
    _, root = load(args.ts)
    locs = locations(root)
    n = 0
    with open(args.csv, "w", newline="", encoding="utf-8-sig") as fh:
        w = csv.writer(fh)
        w.writerow(HEADER)
        for ctx in root.iter("context"):
            name = text(ctx.find("name"))
            for msg in ctx.iter("message"):
                st = status_of(msg)
                if st in ("vanished", "obsolete") and not args.all:
                    continue
                for row in rows_for(name, msg, locs.get(msg, "")):
                    w.writerow(row)
                    n += 1
    print("wrote %d rows to %s" % (n, args.csv))


def cmd_import(args):
    tree, root = load(args.ts)
    index = {}
    for ctx in root.iter("context"):
        name = text(ctx.find("name"))
        for msg in ctx.iter("message"):
            key = (name, text(msg.find("source")), text(msg.find("comment")))
            index[key] = msg

    applied, skipped, errors, stale = 0, 0, 0, 0
    with open(args.csv, newline="", encoding="utf-8-sig") as fh:
        r = csv.DictReader(fh)
        missing = [c for c in ("context", "source", "form", "translation") if c not in r.fieldnames]
        if missing:
            sys.exit("CSV lacks columns: %s" % ", ".join(missing))
        for ln, row in enumerate(r, start=2):
            key = (row["context"], row["source"], row.get("disambiguation", ""))
            msg = index.get(key)
            if msg is None:
                stale += 1
                print("line %d: no such string in %s: %r" % (ln, args.ts, row["source"][:60]))
                continue
            if status_of(msg) in ("vanished", "obsolete"):
                skipped += 1
                continue
            new = row["translation"]
            if new == "":
                skipped += 1
                continue
            src = row["source"]
            want = placeholders(src)
            have = placeholders(new)
            # Plural forms may legitimately drop %n ("one frame"); every
            # other placeholder has to be there exactly.
            if row["form"] != "":
                want = [p for p in want if p != "%n"]
                have = [p for p in have if p != "%n"]
            if want != have and not args.force:
                errors += 1
                print("line %d: placeholders differ (%s vs %s), skipped: %r -> %r"
                      % (ln, want, have, src[:50], new[:50]))
                continue
            tr = msg.find("translation")
            if tr is None:
                tr = ET.SubElement(msg, "translation")
            if msg.get("numerus") == "yes":
                forms = tr.findall("numerusform")
                try:
                    i = int(row["form"])
                except ValueError:
                    errors += 1
                    print("line %d: plural string needs a numeric form column" % ln)
                    continue
                while len(forms) <= i:
                    forms.append(ET.SubElement(tr, "numerusform"))
                if forms[i].text != new:
                    forms[i].text = new
                    applied += 1
                if all(text(f) for f in forms) and tr.get("type") == "unfinished":
                    del tr.attrib["type"]
            else:
                if tr.text != new:
                    tr.text = new
                    applied += 1
                if tr.get("type") == "unfinished":
                    del tr.attrib["type"]

    print("%d translations applied, %d rows skipped, %d placeholder errors, %d stale rows"
          % (applied, skipped, errors, stale))
    if args.dry_run:
        print("dry run: %s not written" % args.ts)
        return
    save(args.ts, root)
    print("updated %s" % args.ts)


def cmd_stats(args):
    _, root = load(args.ts)
    counts = {}
    for msg in root.iter("message"):
        st = status_of(msg)
        counts[st] = counts.get(st, 0) + 1
    live = counts.get("done", 0) + counts.get("unfinished", 0)
    print("%s: %d strings, %d translated, %d unfinished, %d vanished/obsolete"
          % (args.ts, live, counts.get("done", 0), counts.get("unfinished", 0),
             counts.get("vanished", 0) + counts.get("obsolete", 0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="write a CSV for one .ts")
    e.add_argument("ts"); e.add_argument("csv")
    e.add_argument("--all", action="store_true", help="include vanished/obsolete rows")
    e.set_defaults(fn=cmd_export)
    i = sub.add_parser("import", help="copy a CSV's translation column back into the .ts")
    i.add_argument("ts"); i.add_argument("csv")
    i.add_argument("--dry-run", action="store_true")
    i.add_argument("--force", action="store_true", help="accept placeholder mismatches")
    i.set_defaults(fn=cmd_import)
    s = sub.add_parser("stats", help="count translated / unfinished strings")
    s.add_argument("ts")
    s.set_defaults(fn=cmd_stats)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()

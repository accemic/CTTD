#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
"""This repository is English. Comments and messages included.

Ported from CEDARtools.TraceEncoder's scripts/check_language.py (the
sibling repository that this decoder is pinned by); the rules are the same,
only the exemptions differ -- CTTD has none besides this file.

What this guard checks, in every tracked text file of the repository:

  1. COMMENT BLOCKS -- runs of `#` or `//` lines, `/* ... */` and `<!-- ... -->`
     blocks. A block is reported when it contains at least two DIFFERENT German
     function words. That covers the half-translated block and the fully
     German one in one rule.
  2. SINGLE LINES anywhere in the file -- same threshold. This is what catches
     German inside a string: a printf text, a check label, a JSON value.

Both rules count DISTINCT markers, so one loan word does not trip them. The
word list holds only function words with no English homograph -- `die`, `man`,
`war`, `hat` are deliberately absent, because they are ordinary English words
as well.

Deliberately NOT covered: this file -- it names German words in order to find
them. The exemption is a single path, never a prefix: a guard that cannot see
itself has not been tested.
"""
import re
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUFFIXES = (
    # sources and markup
    ".py", ".js", ".mjs", ".html", ".css", ".sv", ".svh", ".v",
    ".sh", ".ps1", ".json", ".md", ".tcl", ".rdl", ".yml", ".yaml",
    # C and assembly of the example software
    ".c", ".h", ".s", ".ld",
    # device trees and their templates
    ".dts", ".dtsi", ".dtso", ".in",
    # documentation and licence texts
    ".adoc", ".rst", ".txt",
    # build, synthesis and formal flow descriptions
    ".abc", ".ys", ".sby", ".smtc", ".xdc", ".mk", ".f", ".pl",
    # configuration and metadata
    ".config", ".toml", ".cff", ".desc", ".license", ".verible_lint",
    # tabular data carrying prose columns
    ".csv", ".tsv",
    # vendoring deltas
    ".patch", ".diff",
)
SKIP_PARTS = (".git", "upstream", "node_modules")
SKIP_FILES = {
    "scripts/check_language.py",
}

# Function words without an English homograph. Distinct hits are counted.
#
# Every word here was checked against English before it was added, because a
# marker that is also an English word makes the guard fire on English prose,
# and a guard that cries wolf gets switched off. Deliberately ABSENT for that
# reason: die, man, war, hat, is, in, so, bin, hex, den (a den), alt (alt
# text), links (hyperlinks), also, am, an, des (matches DES case-insensitively),
# rechts/oben-style pairs whose partner is English.
GER = re.compile(
    r"\b(nicht|nichts|kein|keine|keinen|keinem|keiner|und|oder|wird|werden"
    r"|wurde|wurden|worden|sind|ist|fuer|für|mit|von|vom|aus"
    r"|nach|ueber|über|unter|durch|ohne|beim|zum|zur|muss|müssen|muessen"
    r"|soll|sollen|sollte|darf|duerfen|dürfen|dann|noch|schon|nur|auch"
    r"|jede|jeder|jedes|jeden|jedem|eine|einen|einem|einer|eines|ein"
    r"|dass|weil|damit|sonst|wenn|sich|ihre|seine|seinen|diese|dieser"
    r"|dieses|diesem|diesen|hier|dort|jetzt|waere|wäre|haette|hätte"
    r"|koennen|können|koennte|könnte|kann|deshalb|daher|dafuer|dafür"
    r"|dabei|davon|dazu|darin|gibt|steht|stehen|liegt|liegen|bleibt"
    r"|bleiben|laeuft|läuft|laufen|gehoert|gehört|gehoeren|gehören"
    r"|haengt|hängt|heisst|heißt|gilt|gelten|zeigt|zeigen|braucht"
    r"|brauchen|nimmt|nehmen|macht|machen|liest|lesen|zwischen|gegen"
    r"|immer|wieder|bereits|jedoch|allerdings|sondern|statt|obwohl"
    r"|nachdem|bevor|solange|sobald|sowie|jeweils|selbst|zusammen"
    r"|weiter|weiterhin|zurueck|zurück|mehr|weniger|etwa|genau|bewusst"
    r"|gemessen|erzeugt|geschrieben|gelesen|gesetzt|verwendet|benutzt"
    r"|alle|allen|aller|alles|beide|beiden|viele|einzeln|einzelne"
    r"|erst|erste|ersten|neue|neuen|alte|alten|ganze|ganzen|unten"
    r"|Zeile|Zeilen|Datei|Dateien|Fehler|Speicher|Adresse|Adressen"
    # ASCII transcriptions. The UMLAUT rule below cannot see these, and
    # in the vendoring deltas that is not an edge case but the norm --
    # every one of them is written Huelle/laesst/gehoeren, so the rule
    # was structurally dead exactly where the German was (2026-08-21).
    r"|Huelle|Groesse|Groessen|Laenge|Laengen|Aenderung|Aenderungen"
    r"|Ueberlauf|Uebersetzung|Ausfuehrung|ausfuehrbar|zufuegen"
    r"|gehoerig|moeglich|noetig|hoeher|groesser|spaeter|naechste"
    r"|waehrend|zunaechst|urspruenglich|vollstaendig|abhaengig"
    r"|unveraendert|veraendert|erklaert|waehlt|zaehlt|haelt|faellt"
    r"|Beispiel|Achtung|Hinweis|siehe|bzw|ggf|Grund|Ursache|Zweck"
    r"|Nachweis|Pruefung|Prüfung|Quelle|Senke|Kern|Kerne|Fenster"
    r"|Ausgabe|Eingabe|Anzahl|Groesse|Größe|Reihenfolge|Bedingung"
    r"|Verzeichnis|Befund|Zaehler|Zähler|Schluessel|Schlüssel"
    r"|Meldung|Meldungen|Knoten|Abzeichen|Szenario|Szenarien"
    r"|wie|das|Wert|Werte|echte|echten|eigene|eigenen|andere|anderen"
    r"|etwas|ausserdem|bisher|dadurch|somit|zugleich|zunaechst)\b", re.I)

# A word carrying an umlaut or an eszett is German, full stop -- no word list
# can be as complete as the alphabet is. Counted as one marker like any other,
# so a single author name (`Preußer`, rtl/external/**) does not trip anything;
# German prose reaches the threshold of two on its own.
UMLAUT = re.compile(r"\b\w*[äöüßÄÖÜ]\w*\b", re.U)

BLOCK_RE = (
    re.compile(r"/\*.*?\*/", re.S),
    re.compile(r"<!--.*?-->", re.S),
)


def tracked_files():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files"],
                         capture_output=True, text=True, encoding="utf-8").stdout
    for rel in out.split("\n"):
        if not rel:
            continue
        p = ROOT / rel
        # A file without any extension is text as often as not (`makefile`,
        # `LICENSE`). Scan them; a binary one drops out at the
        # UnicodeDecodeError below.
        if p.suffix and p.suffix.lower() not in SUFFIXES:
            continue
        if any(part in SKIP_PARTS for part in p.parts):
            continue
        if rel in SKIP_FILES:
            continue
        if not p.is_file():
            continue
        yield rel, p


DIFF_MARK = ("+", "-", " ")


def strip_diff(lines, rel):
    """Remove the leading +/-/space column of a unified diff.

    Without this every line of a `.patch` starts with a diff marker, no line
    starts with `#` or `//` any more, and the comment-block detection finds
    nothing at all -- the single-line pass would still fire, but only where
    two markers meet in ONE line, which German prose rarely does.
    Hunk headers and file headers are dropped: `--- a/foo` and `+++ b/foo`
    are not text anybody wrote.
    """
    if not rel.endswith((".patch", ".diff")):
        return lines
    out = []
    for ln in lines:
        if ln.startswith(("---", "+++", "@@", "diff ", "index ")):
            out.append("")
            continue
        out.append(ln[1:] if ln[:1] in DIFF_MARK else ln)
    return out


def line_blocks(lines, marker):
    """Runs of consecutive lines starting with `marker`."""
    i = 0
    while i < len(lines):
        if lines[i].strip().startswith(marker):
            j = i
            while j < len(lines) and lines[j].strip().startswith(marker):
                j += 1
            yield i + 1, "\n".join(lines[i:j])
            i = j
        else:
            i += 1


def german(text):
    hits = {m.lower() for m in GER.findall(text)}
    hits |= {w.lower() for w in UMLAUT.findall(text)}
    return hits


def main() -> int:
    findings = []
    scanned = 0
    for rel, path in tracked_files():
        try:
            src = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        scanned += 1
        lines = strip_diff(src.splitlines(), rel)
        seen = set()

        for marker in ("#", "//"):
            for start, text in line_blocks(lines, marker):
                hits = german(text)
                if len(hits) >= 2:
                    findings.append((rel, start, "comment block",
                                     sorted(hits)[:4]))
                    seen.update(range(start, start + text.count("\n") + 1))

        block_src = "\n".join(lines) if rel.endswith((".patch", ".diff")) else src
        for rx in BLOCK_RE:
            for m in rx.finditer(block_src):
                hits = german(m.group(0))
                if len(hits) >= 2:
                    start = block_src.count("\n", 0, m.start()) + 1
                    findings.append((rel, start, "comment block",
                                     sorted(hits)[:4]))
                    seen.update(range(start, start + m.group(0).count("\n") + 1))

        for i, ln in enumerate(lines, 1):
            if i in seen:
                continue
            hits = german(ln)
            if len(hits) >= 2:
                findings.append((rel, i, "line", sorted(hits)[:4]))

    if findings:
        print("[check_language] %d German passage(s) in an English repository:"
              % len(findings))
        for rel, start, kind, hits in findings[:60]:
            print("  %s:%d  (%s: %s)" % (rel, start, kind, ", ".join(hits)))
        if len(findings) > 60:
            print("  ... and %d more" % (len(findings) - 60))
        print("  Translate the whole passage. A phrase-wise sweep produces")
        print("  half-translated blocks, which read worse than the original.")
        return 1
    print("[check_language] OK: %d file(s), no German passages" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())

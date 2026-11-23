#!/usr/bin/env python3
"""tools/kernels-json.py: write kernels.json, the list of published synodeb kernels, for NAS
managers and other tools that let the user choose when to install a kernel.

  tools/kernels-json.py PAGES [VULNS]

PAGES is the GitHub Pages tree (build/pages, with the apt repository in debian/); the file is
written to PAGES/kernels.json. VULNS, optional, is a checkout of the Linux kernel CVE
repository (https://git.kernel.org/pub/scm/linux/security/vulns.git): with it every kernel
lists the CVEs fixed upstream since the previous kernel of its series. Without it the CVE
lists of the previous kernels.json are kept.

The history comes from git: every published kernel has a commit "kernel <release> (<series>)"
on main, made by the kernel workflow right after publishing. The kernel being published now
(kernel/<series>/VERSION, SERIES in the environment) has no such commit yet and is added from
HEAD. The output depends only on the git history, the repository pool and VULNS, so running
it again gives the same file.

Format (format 1):
  {"format": 1, "generated": "<UTC>",
   "series": {"longterm": {"branch": "6.18", "metapackage": "linux-image-synodeb-longterm"}, ...},
   "kernels": [  # newest first
     {"release": "6.18.64-12-synodeb", "series": "longterm", "upstream": "6.18.64",
      "package": "linux-image-6.18.64-12-synodeb", "version": "6.18.64-12",
      "published": "<UTC>", "available": true,
      "changelog": "https://cdn.kernel.org/pub/linux/kernel/v6.x/ChangeLog-6.18.64",
      "previous": "6.18.63-11-synodeb",
      "changes": ["<subject of a synodeb commit to the kernel sources>", ...],
      "cves": ["CVE-2026-...", ...]}  # null: not known (no VULNS, or a new branch)
   ],
   "cve_titles": {"CVE-2026-...": "<title>"}}
"available": the package is in the apt repository (it keeps the two newest kernels of each
branch). "changes" are the synodeb commits to the kernel sources (patches, drivers, config)
since the previous kernel of the series; an empty list means a plain upstream update (and is
also given for the first kernel of a series).
"""
import datetime
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL_PATHS = ["kernel", "platforms/*/kernel", ":(exclude)kernel/*/VERSION"]
SERIES_META = {"longterm": "linux-image-synodeb-longterm", "stable": "linux-image-synodeb-stable"}


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True,
                          text=True).stdout


def read_version(series, rev="HEAD"):
    v = dict(re.findall(r"^(KVER|ABI)=(\S+)", git("show", f"{rev}:kernel/{series}/VERSION"),
                        re.M))
    ups = v["KVER"] if v["KVER"].count(".") == 2 else v["KVER"] + ".0"
    return ups, v["ABI"]


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def branch(v):
    return ".".join(v.split(".")[:2])


def utc(epoch):
    return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def history():
    """[(release, series, commit, epoch)] oldest first, from the release commits on HEAD."""
    out = []
    pat = re.compile(r"^kernel (\d+\.\d+(?:\.\d+)?-\d+-(?:synodeb|ds923)) \((longterm|stable)\)$")
    for line in git("log", "--reverse", "--format=%H %ct %s", "HEAD").splitlines():
        h, t, s = line.split(" ", 2)
        m = pat.match(s)
        if m:
            out.append((m.group(1), m.group(2), h, t))
    return out


def changes(base, tip):
    rng = f"{base}..{tip}" if base else tip
    log = git("log", "--format=%s", "--no-merges", rng, "--", *KERNEL_PATHS)
    return [s for s in log.splitlines()
            if not re.match(r"^kernel \S+ \((longterm|stable)\)$", s)]


def cve_fixes(vulns):
    """{stable version: {CVE id}} and {CVE id: title} from the .dyad files of VULNS."""
    fixes, titles = {}, {}
    pub = os.path.join(vulns, "cve", "published")
    for year in sorted(os.listdir(pub)):
        d = os.path.join(pub, year)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith(".dyad"):
                continue
            cve = f[:-5]
            with open(os.path.join(d, f), encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("#"):
                        continue
                    p = line.strip().split(":")
                    if len(p) == 4 and re.fullmatch(r"\d+\.\d+\.\d+", p[2]):
                        fixes.setdefault(p[2], set()).add(cve)
                        titles[cve] = None
    for cve in titles:
        try:
            with open(os.path.join(pub, cve.split("-")[1], cve + ".json"), encoding="utf-8") as fh:
                titles[cve] = json.load(fh)["containers"]["cna"].get("title")
        except (OSError, ValueError, KeyError):
            pass
    return fixes, titles


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    pages = sys.argv[1]
    vulns = sys.argv[2] if len(sys.argv) == 3 else None
    out_path = os.path.join(pages, "kernels.json")
    try:
        with open(out_path, encoding="utf-8") as fh:
            prev_doc = json.load(fh)
        old = {k["release"]: k for k in prev_doc.get("kernels", [])}
        old_titles = prev_doc.get("cve_titles", {})
    except (OSError, ValueError):
        old, old_titles = {}, {}

    rels = history()
    now_series = os.environ.get("SERIES")
    if now_series:
        ups, abi = read_version(now_series)
        krel = f"{ups}-{abi}-synodeb"
        if krel not in [r[0] for r in rels]:
            rels.append((krel, now_series, None, git("log", "-1", "--format=%ct", "HEAD").strip()))

    pool = os.path.join(pages, "debian", "pool", "main")
    have = set(os.listdir(pool)) if os.path.isdir(pool) else set()
    fixes, titles = cve_fixes(vulns) if vulns else (None, {})

    kernels, last = [], {}
    for krel, series, commit, epoch in rels:
        ups = krel.split("-")[0]
        prev = last.get(series)
        base = prev[2] if prev else None
        tip = f"{commit}^" if commit else "HEAD"
        pkgver = krel.rsplit("-", 1)[0]
        k = {
            "release": krel, "series": series, "upstream": ups,
            "package": f"linux-image-{krel}", "version": pkgver,
            "published": utc(epoch),
            "available": f"linux-image-{krel}_{pkgver}_amd64.deb" in have,
            "changelog": f"https://cdn.kernel.org/pub/linux/kernel/v{ups.split('.')[0]}.x/ChangeLog-{ups}"
                         if ups.count(".") == 2 and not ups.endswith(".0") else None,
            "previous": prev[0] if prev else None,
            "changes": changes(base, tip) if prev else [],
        }
        pups = prev[0].split("-")[0] if prev else None
        if fixes is None:
            k["cves"] = old.get(krel, {}).get("cves")
        elif pups is None or branch(pups) != branch(ups):
            k["cves"] = None
        else:
            k["cves"] = sorted((c for v, s in fixes.items()
                                if branch(v) == branch(ups) and vkey(pups) < vkey(v) <= vkey(ups)
                                for c in s), key=lambda c: tuple(int(x) for x in c.split("-")[1:]))
        kernels.append(k)
        last[series] = (krel, series, commit or "HEAD")

    kernels.reverse()
    used = {c for k in kernels for c in (k["cves"] or [])}
    all_titles = {**old_titles, **{c: t for c, t in titles.items() if t}}
    series = {}
    for s, meta in SERIES_META.items():
        try:
            series[s] = {"branch": branch(read_version(s)[0]), "metapackage": meta}
        except subprocess.CalledProcessError:
            pass
    doc = {
        "format": 1,
        "generated": utc(git("log", "-1", "--format=%ct", "HEAD").strip()),
        "series": series,
        "kernels": kernels,
        "cve_titles": {c: all_titles.get(c) for c in sorted(used)},
    }
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, out_path)
    print(f"kernels.json: {len(kernels)} kernels, {len(used)} CVEs")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Regenerates the document fixtures next to this script, byte for byte.

two_pages.pdf  Two 200x300 pt pages and no fonts: page 1 fills a red
               rectangle (50,75)-(150,225), page 2 a blue one (20,20)-(180,280).
               Flat fills only, so tests may probe exact pixels without
               depending on any text rasterizer.
reflow.epub    A reflowable two-chapter book of plain paragraphs; chapter one
               is long enough to paginate several times at a 450x600 pt
               layout, chapter two is short. Every zip entry is stored (not
               deflated) with a fixed timestamp, so the bytes do not depend
               on the zlib in use.
probe.cbz      A zip archive exercising archive_names' sanitize/dedupe rule:
               ch1/010.png, ch1/002.png, a path that collapses to ch1/002.png
               through its own ".." segments (the dedupe target), ../evil.png
               (the zip-slip probe), notes.txt (not a page) and a zero-length
               empty.png. Page bytes are ../cover_2x3.png; entries stored,
               fixed timestamp, same determinism as reflow.epub.
probe.cbt      The same six entries as a tar archive (uncompressed, fixed
               mtime/uid/gid), so the read loop's format-agnostic path is
               exercised without depending on libarchive's zip vs. tar code.

Standard library only. Run from anywhere:
    python3 tests/fixtures/docs/gen_docs.py
"""

import io
import os
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
STAMP = (1980, 1, 1, 0, 0, 0)  # the earliest zip timestamp; deterministic bytes.


def write_pdf(path):
    page1 = b"1 0 0 rg 50 75 100 150 re f"
    page2 = b"0 0 1 rg 20 20 160 260 re f"
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(page1) + page1 + b"\nendstream",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 6 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(page2) + page2 + b"\nendstream",
    ]
    out = b"%PDF-1.4\n"
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, xref))
    with open(path, "wb") as f:
        f.write(out)


def _entry(name):
    info = zipfile.ZipInfo(name, date_time=STAMP)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    return info


def write_epub(path):
    container = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles>\n'
        '    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>\n'
        '  </rootfiles>\n'
        '</container>\n')
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        '    <dc:title>Reflow Fixture</dc:title>\n'
        '    <dc:identifier id="uid">shigoku-reflow-fixture</dc:identifier>\n'
        '    <dc:language>en</dc:language>\n'
        '  </metadata>\n'
        '  <manifest>\n'
        '    <item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>\n'
        '    <item id="c2" href="ch2.xhtml" media-type="application/xhtml+xml"/>\n'
        '  </manifest>\n'
        '  <spine>\n'
        '    <itemref idref="c1"/>\n'
        '    <itemref idref="c2"/>\n'
        '  </spine>\n'
        '</package>\n')
    sentence = "The quick brown fox jumps over the lazy dog. "
    para = "<p>" + sentence * 40 + "</p>\n"

    def chapter(title, paras):
        return (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml">\n'
            '<head><title>' + title + '</title></head>\n'
            '<body>\n<h1>' + title + '</h1>\n' + para * paras + '</body>\n</html>\n')

    with zipfile.ZipFile(path, "w") as z:
        z.writestr(_entry("mimetype"), "application/epub+zip")  # first, stored.
        z.writestr(_entry("META-INF/container.xml"), container)
        z.writestr(_entry("OEBPS/content.opf"), opf)
        z.writestr(_entry("OEBPS/ch1.xhtml"), chapter("Chapter One", 12))
        z.writestr(_entry("OEBPS/ch2.xhtml"), chapter("Chapter Two", 3))


# Raw archive entry names for probe.cbz/.cbt. archive_names.sanitize_entry_name
# flattens ch1/010.png -> ch1_010.png; the third entry's ".." segments cancel
# their siblings down to ch1/002.png, the same result as the second entry
# (the dedupe target); the fourth's leading ".." has nothing to pop, so it
# drops and evil.png lands at the archive root. notes.txt is a real file but
# not an image extension, so it never makes it into the page list. empty.png
# is a zero-length regular file with an image extension: it extracts (an
# empty file on disk), decode failure is the image pager's business, not
# extraction's.
ARCHIVE_ENTRIES = [
    ("ch1/010.png", "png"),
    ("ch1/002.png", "png"),
    ("sub/../weird/../ch1/002.png", "png"),
    ("../evil.png", "png"),
    ("notes.txt", "text"),
    ("empty.png", "empty"),
]


def _archive_bytes(kind, png_bytes):
    if kind == "png":
        return png_bytes
    if kind == "text":
        return b"not a page\n"
    return b""


def write_cbz(path, png_bytes):
    with zipfile.ZipFile(path, "w") as z:
        for name, kind in ARCHIVE_ENTRIES:
            z.writestr(_entry(name), _archive_bytes(kind, png_bytes))


def write_cbt(path, png_bytes):
    with tarfile.open(path, "w") as t:
        for name, kind in ARCHIVE_ENTRIES:
            data = _archive_bytes(kind, png_bytes)
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            info.mtime = 0
            info.mode = 0o644
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            t.addfile(info, io.BytesIO(data))


if __name__ == "__main__":
    write_pdf(os.path.join(HERE, "two_pages.pdf"))
    write_epub(os.path.join(HERE, "reflow.epub"))
    with open(os.path.join(HERE, "..", "cover_2x3.png"), "rb") as f:
        _png = f.read()
    write_cbz(os.path.join(HERE, "probe.cbz"), _png)
    write_cbt(os.path.join(HERE, "probe.cbt"), _png)

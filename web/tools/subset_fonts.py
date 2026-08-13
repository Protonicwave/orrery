"""Cut the three faces down to the characters this client actually sets.

The client makes no third party request, so the fonts are served from the same
origin as everything else. A complete face is far larger than the site needs:
IBM Plex Sans Regular is 130 kB of TrueType covering Latin, Greek, Cyrillic and
Vietnamese, and this interface sets English, digits and a dozen typographic
marks. Subsetting to the characters below and converting to WOFF2 leaves a few
kilobytes a face, which is what makes a preload affordable.

The output is committed, so a build needs neither this script nor the network.
Run it only when the character set or a font version changes:

    pip install "fonttools[woff]"
    python tools/subset_fonts.py

Sources are pinned by version and checked by hash, for the reason ADR-0002
gives for the C++ dependencies: a tag can be moved onto different bytes without
anything in this repository changing.
"""

from __future__ import annotations

import hashlib
import io
import sys
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

try:
    from fontTools import subset
    from fontTools.ttLib import TTFont
except ImportError:  # pragma: no cover - a message beats a traceback here
    sys.exit('fonttools is needed: pip install "fonttools[woff]"')

WEB = Path(__file__).resolve().parent.parent
OUT = WEB / 'public' / 'fonts'
CACHE = WEB / 'node_modules' / '.cache' / 'fonts'

# The characters the interface and the reading half set, stated rather than
# guessed. Anything outside this list falls back to the stack in tokens.css,
# which is the correct outcome for a character the design never uses.
#
# The em-dash is absent on purpose. The house style does not use one, and a
# face that cannot draw it cannot grow one by accident.
CHARACTERS = (
    ''.join(chr(c) for c in range(0x20, 0x7F))  # printable ASCII
    + ' '  # no-break space
    + '°'  # degree, for angles
    + '±'  # plus-minus, for a spread
    + '·'  # middle dot, the separator in the build line
    + '×'  # multiplication sign, for powers of ten
    + 'Δ'  # capital delta, for a change in a conserved quantity
    + ' '  # thin space, the digit group separator
    + '‑'  # non-breaking hyphen, for names like barnes-hut
    + '–'  # en dash, for ranges
    + '’'  # right single quote, the apostrophe
    + '“”'  # double quotes
    + '…'  # ellipsis
    + '→'  # rightwards arrow, for a value moving from one figure to another
    + '−'  # minus sign, which is not a hyphen
)

# Tabular and lining figures are the whole reason a value updating sixty times
# a second does not move the layout, so those two features survive the cut.
LAYOUT_FEATURES = ['tnum', 'lnum']


@dataclass(frozen=True)
class Archive:
    """A release archive, pinned by URL and content."""

    url: str
    sha256: str


@dataclass(frozen=True)
class Face:
    """One weight of one family, and where it comes from."""

    archive: Archive
    member: str
    output: str


PLEX_SANS = Archive(
    url='https://github.com/IBM/plex/releases/download'
    '/%40ibm%2Fplex-sans%401.1.0/ibm-plex-sans.zip',
    sha256='fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200',
)
PLEX_MONO = Archive(
    url='https://github.com/IBM/plex/releases/download'
    '/%40ibm%2Fplex-mono%402.5.0/ibm-plex-mono.zip',
    sha256='6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c',
)
SOURCE_SERIF = Archive(
    url='https://github.com/adobe-fonts/source-serif/releases/download'
    '/4.005R/source-serif-4.005_Desktop.zip',
    sha256='549fdb8f9a682bd06944298621404969f6de77c2e422ff3b8244a1dcd6a0c425',
)

FACES = (
    Face(PLEX_SANS, 'ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Regular.ttf',
         'plex-sans-400.woff2'),
    Face(PLEX_SANS, 'ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-Medium.ttf',
         'plex-sans-500.woff2'),
    Face(PLEX_SANS, 'ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-SemiBold.ttf',
         'plex-sans-600.woff2'),
    Face(PLEX_MONO, 'ibm-plex-mono/fonts/complete/ttf/IBMPlexMono-Regular.ttf',
         'plex-mono-400.woff2'),
    Face(PLEX_MONO, 'ibm-plex-mono/fonts/complete/ttf/IBMPlexMono-Medium.ttf',
         'plex-mono-500.woff2'),
    Face(SOURCE_SERIF, 'source-serif-4.005_Desktop/TTF/SourceSerif4-Regular.ttf',
         'source-serif-400.woff2'),
    Face(SOURCE_SERIF, 'source-serif-4.005_Desktop/TTF/SourceSerif4-Semibold.ttf',
         'source-serif-600.woff2'),
)


def fetch(archive: Archive) -> zipfile.ZipFile:
    """Download an archive if it is not cached, and refuse it if it has moved."""
    CACHE.mkdir(parents=True, exist_ok=True)
    cached = CACHE / archive.url.rsplit('/', 1)[-1]
    if not cached.exists():
        print(f'fetching {archive.url}')
        with urllib.request.urlopen(archive.url) as response:  # noqa: S310
            cached.write_bytes(response.read())

    payload = cached.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != archive.sha256:
        cached.unlink()
        raise SystemExit(
            f'{cached.name} hashes to {digest}, not {archive.sha256}. '
            'The release has been rewritten; check it before updating the pin.'
        )
    return zipfile.ZipFile(io.BytesIO(payload))


def cut(face: Face, source: zipfile.ZipFile) -> int:
    """Subset one face and write it as WOFF2. Returns the size in bytes."""
    font = TTFont(io.BytesIO(source.read(face.member)))
    options = subset.Options()
    options.layout_features += LAYOUT_FEATURES
    options.desubroutinize = True
    options.drop_tables += ['DSIG']
    options.notdef_outline = True
    options.recalc_bounds = True

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=CHARACTERS)
    subsetter.subset(font)

    font.flavor = 'woff2'
    destination = OUT / face.output
    font.save(destination)
    font.close()
    return destination.stat().st_size


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    archives: dict[str, zipfile.ZipFile] = {}
    total = 0
    for face in FACES:
        if face.archive.url not in archives:
            archives[face.archive.url] = fetch(face.archive)
        size = cut(face, archives[face.archive.url])
        total += size
        print(f'{face.output:24} {size / 1024:6.1f} kB')
    print(f'{"total":24} {total / 1024:6.1f} kB, {len(CHARACTERS)} characters')


if __name__ == '__main__':
    main()

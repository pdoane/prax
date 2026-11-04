"""The checkouts under third_party, each pinned, and fetching them.

Every corpus that a suite reads is pinned, because an ERRATA entry names a case by its index in the corpus. A corpus
that moved on its own would leave those entries pointing at other cases while the run still looked green. Moving to a
newer commit means editing the commit here and reading the suite numbers again, which is a deliberate act.

    git ls-remote <url> HEAD    the commit to move a pin to

The clones are sparse and hold no blobs that were not requested, because these are large repositories and the engine
reads one directory of each. A checkout fetches only what it lacks, and a file whose source moves, as a version or a
commit names it, is fetched again.
"""

import hashlib
import os
import shutil
import tempfile
import urllib.request

from tools import dev

THIRD_PARTY = os.path.join(dev.ROOT, "third_party")


class Git:
    """A clone of `url` at `commit` in `directory`, of the `paths` named, or whole if there are none."""

    def __init__(self, directory, url, commit, *paths):
        self.directory = directory
        self.url = url
        self.commit = commit
        self.paths = paths

    def fetch(self):
        directory = os.path.join(THIRD_PARTY, self.directory)
        git = ["git", "-C", directory]
        if not os.path.isdir(os.path.join(directory, ".git")):
            dev.run(["git", "init", "-q", directory])
            dev.run(git + ["remote", "add", "origin", self.url])
            if self.paths:
                dev.run(git + ["config", "extensions.partialClone", "origin"])
                dev.run(git + ["sparse-checkout", "init", "--cone"])
        if self.paths:
            # Setting the patterns writes the checkout's files under a lock, which every worktree shares through
            # third_party, so a checkout that already has them is left alone.
            listed = dev.output(git + ["sparse-checkout", "list"], check=False) or ""
            if sorted(listed.split("\n")) != sorted(self.paths):
                dev.run(git + ["sparse-checkout", "set", *self.paths])
        elif dev.output(git + ["config", "core.sparseCheckout"], check=False) == "true":
            # A checkout that a sparse clone made becomes whole.
            dev.run(git + ["sparse-checkout", "disable"])
        if dev.output(git + ["rev-parse", "HEAD"], check=False) != self.commit:
            print("fetching %s at %s" % (self.directory, self.commit), flush=True)
            dev.run(git + ["fetch", "-q", "--depth", "1"] + (["--filter=blob:none"] if self.paths else [])
                    + ["origin", self.commit])
            dev.run(git + ["checkout", "-q", "-f", "--detach", "FETCH_HEAD"])


class Files:
    """Files fetched into `directory`, by name and URL. With a `stamp`, the files are what that version or commit
    names, and are fetched again when it changes. With `digests`, by name, a file is pinned to the SHA-256 that names
    it, which pins a source that has no versions. A fetch that brings anything else fails."""

    def __init__(self, directory, files, stamp=None, digests=None):
        self.directory = directory
        self.files = files
        self.stamp = stamp
        self.digests = digests or {}

    def fetch(self):
        directory = os.path.join(THIRD_PARTY, self.directory)
        os.makedirs(directory, exist_ok=True)
        stamp_file = os.path.join(directory, self.stamp[0]) if self.stamp else None
        if stamp_file:
            try:
                with open(stamp_file) as f:
                    current = f.read().strip()
            except FileNotFoundError:
                current = None
            if current != self.stamp[1]:
                for name, _ in self.files:
                    path = os.path.join(directory, name)
                    if os.path.exists(path):
                        os.remove(path)
        for name, url in self.files:
            path = os.path.join(directory, name)
            if not os.path.exists(path):
                print("fetching %s/%s" % (self.directory, name), flush=True)
                download(url, path, self.digests.get(name))
        if stamp_file:
            with open(stamp_file, "w") as f:
                f.write(self.stamp[1] + "\n")


class Captures:
    """The pages of a checkout, each a path under `checkout`, captured into `directory` by tools/pages/pages.py with
    what they load from other origins, so that they load the same on every run and without the network. A capture is
    made once, and again only when the `stamp` changes, since what a server answers can change while the pin does
    not."""

    def __init__(self, directory, checkout, pages, stamp):
        self.directory = directory
        self.checkout = checkout
        self.pages = pages
        self.stamp = stamp

    def fetch(self):
        directory = os.path.join(THIRD_PARTY, self.directory)
        stamp_file = os.path.join(directory, self.stamp[0])
        try:
            with open(stamp_file) as f:
                current = f.read().strip()
        except FileNotFoundError:
            current = None
        if current != self.stamp[1]:
            shutil.rmtree(directory, ignore_errors=True)
            os.makedirs(directory)
            with open(stamp_file, "w") as f:
                f.write(self.stamp[1] + "\n")
        missing = [page for page in self.pages
                   if not os.path.exists(os.path.join(directory, os.path.splitext(page)[0] + ".json"))]
        if missing:
            print("capturing %d pages of %s" % (len(missing), self.checkout), flush=True)
            dev.python("tools.pages.pages", "local", os.path.join(THIRD_PARTY, self.checkout), directory, *missing)


class Pages:
    """The real pages that the benchmarks read, captured by tools/pages/pages.py."""

    def fetch(self, *args):
        os.makedirs(os.path.join(THIRD_PARTY, "pages"), exist_ok=True)
        dev.python("tools.pages.pages", "roll", *args)


# The User-Agent that a download sends: the engine's own (kPrFetchDefaultUserAgent in lib/fetch), as Prax's browser
# sends it, rather than the one urllib sends.
USER_AGENT = "Mozilla/5.0 (compatible; Prax)"


def download(url, path, digest=None):
    """Writes what `url` holds to `path`, through a temporary file, so that a failed download leaves nothing. With a
    `digest`, what it holds must have that SHA-256."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path))
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with os.fdopen(handle, "wb") as out, urllib.request.urlopen(request) as response:
            shutil.copyfileobj(response, out)
        if digest is not None:
            with open(temporary, "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
            if got != digest:
                raise ValueError("its SHA-256 is %s, not the pinned %s" % (got, digest))
        os.replace(temporary, path)
    except Exception as error:
        os.remove(temporary)
        dev.fail("cannot fetch %s: %s" % (url, error))


# ----------------------------------------------------------------------------------------------------------------------
# The pins

# The HarfBuzz release whose test fonts the font corpus takes.
HARFBUZZ_TAG = "14.5.0"
NOTO_EMOJI_TAG = "v2.047"
WEBREF_COMMIT = "5216c7c2066f47b7f80c9620e56827f20d4c8283"
WPT_COMMIT = "269bca0dd35c303639f3c9cf1d8bcb3d911bdb60"
CSSPARSER_COMMIT = "d128a90e1203fc4afb959420d6dc5e24f77a3191"
HTML5LIB_COMMIT = "224991ec10db04f056a89eed8b0bd8695fd2950e"
RESVG_TESTS_COMMIT = "d8e064337faf01bc5a9579187a56dbdbe3eacc72"
GOOGLE_FONTS_COMMIT = "23e54b51ddffbc7713c583748e3bd86f62b1fa4a"
COLOR_FONTS_COMMIT = "0046ea4c3b69e9fbbe464c2594816894e3aa5e4b"
# The CSS Working Group's drafts: the color profile that their CMYK examples are computed with, and the sources that
# generators read.
CSSWG_DRAFTS_COMMIT = "3110cd02e77fa9e3869e433e1d40f615b6e611e4"
# Each Lottie player's version, and the commit of its own tests, which are a suite's corpus: one revision for both.
# lottie-web's commit is its version's, and Skia's is the one that released CanvasKit's version.
LOTTIE_WEB_VERSION = "5.13.0"
LOTTIE_WEB_COMMIT = "bede03d25d232826e0c9dca1733d542d8a7754fb"
CANVASKIT_VERSION = "0.42.0"
SKIA_COMMIT = "66108990f8f6edbfe9650d7023b5cf1154d71910"
LOTTIE_IOS_COMMIT = "b20173c0b6cf47d6159ea00e9584de5e0133da77"
RLOTTIE_COMMIT = "683bbaa39dd0d366cf6b4bc300b4dfbee677ea6b"
DOTLOTTIE_WEB_VERSION = "0.80.0"
LOTTIE_SPEC_COMMIT = "4b55957472f8718e34e9c1298f0e8f021b6c597f"
# The Encoding Standard's repository, whose encodings and indexes the charset generator reads.
ENCODING_COMMIT = "be17ac5c49881336538acfc7b40dba3c8009144b"
# release-48
# MathML Core's repository, whose tables the MathML generator reads.
MATHML_CORE_COMMIT = "66256baa0e7125171ff16b3831acfba3c43179c7"
CLDR_COMMIT = "acd6d88ae493633240e19a87a721076a8a75c310"
# CommonMark 0.31.2, and cmark-gfm 0.29.0.gfm.13: the corpora of the Markdown suites, and the implementation that
# lib/markdown is compared with.
COMMONMARK_SPEC_COMMIT = "9103e341a973013013bb1a80e13567007c5cef6f"
CMARK_GFM_COMMIT = "587a12bb54d95ac37241377e6ddc93ea0e45439b"
PRINCE_SAMPLES_COMMIT = "ebf33b86eaacff2df6acd2e3babcba11dd875b82"
# The licensed directories of prince-samples, each with the documents of it that Prince prints and the viewer lists.
PRINCE_SAMPLES = {
    "campaign": ("letter.html",),
    "catalogue": ("PrinceCatalogue.html", "Cover.html"),
    "digte": ("digte.html",),
    "flyer": ("flyer.html",),
    "invoice-colorful": ("invoicesample.html",),
    "invoice-plain": ("index.html",),
    "letter": ("letter.html",),
    "magic6": ("magic.xhtml",),
    "magic8": ("index.html",),
    "malthus": ("essay.html",),
    "textbook": ("somatosensory.html",),
    "twist": ("oliver-twist.html",),
    "usenix": ("example.html",),
    "webarch": ("webarch.html",),
    "winelist": ("winelist.html",),
}
# Fred Wang's MathFonts, which holds math fonts as web fonts and Mozilla's MathML torture test, whose formulas stand
# beside images of TeX's setting of them. The commit is of the site's branch (gh-pages), which holds the built fonts.
MATHFONTS_COMMIT = "9182b146cb64ac1a5e9cb92a09cd79612ca95d1c"

UCD = "https://www.unicode.org/Public/17.0.0/ucd/"
OFL = "https://raw.githubusercontent.com/google/fonts/%s/ofl/" % GOOGLE_FONTS_COMMIT
COLOR = "https://raw.githubusercontent.com/googlefonts/color-fonts/%s/fonts/" % COLOR_FONTS_COMMIT
WPT_RAW = "https://raw.githubusercontent.com/web-platform-tests/wpt/%s/" % WPT_COMMIT
NPM = "https://cdn.jsdelivr.net/npm/"


def web_features_version():
    """The web-features version that tools/suite/wpt/web_features.txt rules on and states."""
    with open(os.path.join(dev.ROOT, "tools", "suite", "wpt", "web_features.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("# web-features "):
                return line[len("# web-features "):].strip()
    dev.fail("tools/suite/wpt/web_features.txt states no web-features version")


MATH_TEST_FONTS = (
    "axisheight5000-verticalarrow14000",
    "fraction-axisheight7000-rulethickness1000",
    "fraction-denominatordisplaystylegapmin5000-rulethickness1000",
    "fraction-denominatordisplaystyleshiftdown6000-axisheight1000-rulethickness1000",
    "fraction-denominatorgapmin4000-rulethickness1000",
    "fraction-denominatorshiftdown3000-axisheight1000-rulethickness1000",
    "fraction-numeratordisplaystylegapmin8000-rulethickness1000",
    "fraction-numeratordisplaystyleshiftup2000-axisheight1000-rulethickness1000",
    "fraction-numeratorgapmin9000-rulethickness1000",
    "fraction-numeratorshiftup11000-axisheight1000-rulethickness1000",
    "fraction-rulethickness10000",
    "largeop-displayoperatorminheight2000-2AFF-italiccorrection3000",
    "largeop-displayoperatorminheight5000",
    "limits-lowerlimitbaselinedropmin3000",
    "limits-lowerlimitgapmin11000",
    "limits-upperlimitbaselinerisemin5000",
    "limits-upperlimitgapmin7000",
    "operators",
    "radical-degreebottomraisepercent25-rulethickness1000",
    "radical-displaystyleverticalgap7000-rulethickness1000",
    "radical-extraascender3000-rulethickness1000",
    "radical-kernafterdegreeminus5000-rulethickness1000",
    "radical-kernbeforedegree4000-rulethickness1000",
    "radical-rtlm",
    "radical-rulethickness8000",
    "radical-verticalgap6000-rulethickness1000",
    "scriptpercentscaledown0-scriptscriptpercentscaledown40",
    "scriptpercentscaledown80-scriptscriptpercentscaledown0",
    "scriptpercentscaledown80-scriptscriptpercentscaledown40",
    "scripts-spaceafterscript3000",
    "scripts-subscriptbaselinedropmin9000",
    "scripts-subscriptshiftdown6000",
    "scripts-subscripttopmax4000",
    "scripts-subsuperscriptgapmin11000",
    "scripts-subsuperscriptgapmin11000-superscriptbottommaxwithsubscript3000",
    "scripts-superscriptbaselinedropmax10000",
    "scripts-superscriptbottommin8000",
    "scripts-superscriptshiftup7000",
    "scripts-superscriptshiftupcramped5000",
    "stack-bottomdisplaystyleshiftdown5000-axisheight1000",
    "stack-bottomshiftdown6000-axisheight1000",
    "stack-displaystylegapmin4000",
    "stack-gapmin8000",
    "stack-topdisplaystyleshiftup3000-axisheight1000",
    "stack-topshiftup9000-axisheight1000",
    "stretchstack-bottomshiftdown3000",
    "stretchstack-gapabovemin7000",
    "stretchstack-gapbelowmin11000",
    "stretchstack-topshiftup5000",
    "stretchy",
    "stretchy-text-direction-asymetrical",
    "underover-accentbaseheight4000-overbarextraascender3000",
    "underover-accentbaseheight4000-overbarverticalgap11000",
    "underover-accentbaseheight4000-underbarextradescender5000",
    "underover-accentbaseheight4000-underbarverticalgap7000",
)


def fonts():
    """The fonts that the text tests and the viewer draw with, each chosen for what it carries: scripts, color glyphs,
    variation axes, vertical metrics and baseline tables. The color-fonts project's test glyphs exercise every COLRv1
    paint."""
    files = [
        ("NotoSans.ttf", OFL + "notosans/NotoSans%5Bwdth,wght%5D.ttf"),
        ("NotoSans-Italic.ttf", OFL + "notosans/NotoSans-Italic%5Bwdth,wght%5D.ttf"),
        ("NotoSansMono.ttf", OFL + "notosansmono/NotoSansMono%5Bwdth,wght%5D.ttf"),
        ("NotoSerif.ttf", OFL + "notoserif/NotoSerif%5Bwdth,wght%5D.ttf"),
        ("NotoSansArabic.ttf", OFL + "notosansarabic/NotoSansArabic%5Bwdth,wght%5D.ttf"),
        ("NotoSansDevanagari.ttf", OFL + "notosansdevanagari/NotoSansDevanagari%5Bwdth,wght%5D.ttf"),
        ("Nabla.ttf", OFL + "nabla/Nabla%5BEDPT,EHLT%5D.ttf"),
        ("KalniaGlaze.ttf", OFL + "kalniaglaze/KalniaGlaze%5Bwdth,wght%5D.ttf"),
        ("BungeeSpice.ttf", OFL + "bungeespice/BungeeSpice-Regular.ttf"),
        ("NotoSansJP.ttf", OFL + "notosansjp/NotoSansJP%5Bwght%5D.ttf"),
        # Scripts for suites/prax's writing modes and bidi: Hebrew, Korean, which sets Hangul upright in vertical text,
        # Mongolian, written only vertically, and Ogham, which turns -90° in vertical text.
        ("NotoSansHebrew.ttf", OFL + "notosanshebrew/NotoSansHebrew%5Bwdth,wght%5D.ttf"),
        ("NotoSansKR.ttf", OFL + "notosanskr/NotoSansKR%5Bwght%5D.ttf"),
        ("NotoSansMongolian.ttf", OFL + "notosansmongolian/NotoSansMongolian-Regular.ttf"),
        ("NotoSansOgham.ttf", OFL + "notosansogham/NotoSansOgham-Regular.ttf"),
        # Scripts for suites/prax's line breaking and spacing: Thai and Tai Tham, broken by dictionary or not at all;
        # Balinese, by orthographic syllable; Ethiopic and Phoenician, whose word separators are not spaces.
        ("NotoSansThai.ttf", OFL + "notosansthai/NotoSansThai%5Bwdth,wght%5D.ttf"),
        ("NotoSansTaiTham.ttf", OFL + "notosanstaitham/NotoSansTaiTham%5Bwght%5D.ttf"),
        ("NotoSansBalinese.ttf", OFL + "notosansbalinese/NotoSansBalinese%5Bwght%5D.ttf"),
        ("NotoSansEthiopic.ttf", OFL + "notosansethiopic/NotoSansEthiopic%5Bwdth,wght%5D.ttf"),
        ("NotoSansPhoenician.ttf", OFL + "notosansphoenician/NotoSansPhoenician-Regular.ttf"),
        # Mathematical alphanumerics, which text-transform: math-auto maps a lone letter to.
        ("NotoSansMath.ttf", OFL + "notosansmath/NotoSansMath-Regular.ttf"),
    ]
    # The scripts of the predefined counter styles, and the symbols of disclosure and list markers, for suites/prax's
    # lists and counter styles: each saved as <Family>.ttf.
    for path in ("notosansarmenian/NotoSansArmenian%5Bwdth,wght%5D.ttf",
                 "notosansgeorgian/NotoSansGeorgian%5Bwdth,wght%5D.ttf",
                 "notosansbengali/NotoSansBengali%5Bwdth,wght%5D.ttf", "notosanskhmer/NotoSansKhmer%5Bwdth,wght%5D.ttf",
                 "notosansgujarati/NotoSansGujarati%5Bwdth,wght%5D.ttf",
                 "notosansgurmukhi/NotoSansGurmukhi%5Bwdth,wght%5D.ttf",
                 "notosanskannada/NotoSansKannada%5Bwdth,wght%5D.ttf", "notosanslao/NotoSansLao%5Bwdth,wght%5D.ttf",
                 "notosansmalayalam/NotoSansMalayalam%5Bwdth,wght%5D.ttf",
                 "notosansmyanmar/NotoSansMyanmar%5Bwdth,wght%5D.ttf",
                 "notosansoriya/NotoSansOriya%5Bwdth,wght%5D.ttf", "notosanstamil/NotoSansTamil%5Bwdth,wght%5D.ttf",
                 "notosanstelugu/NotoSansTelugu%5Bwdth,wght%5D.ttf", "notoseriftibetan/NotoSerifTibetan%5Bwght%5D.ttf",
                 "notosanssc/NotoSansSC%5Bwght%5D.ttf", "notosanstc/NotoSansTC%5Bwght%5D.ttf",
                 "notosanssymbols2/NotoSansSymbols2-Regular.ttf"):
        family = path.split("/")[1].split("%")[0].split("-")[0]
        files.append((family + ".ttf", OFL + path))
    files += [
        ("Literata.ttf", OFL + "literata/Literata%5Bopsz,wght%5D.ttf"),
        ("RobotoFlex.ttf",
         OFL + "robotoflex/RobotoFlex%5BGRAD,XOPQ,XTRA,YOPQ,YTAS,YTDE,YTFI,YTLC,YTUC,opsz,slnt,wdth,wght%5D.ttf"),
        # Fonts with Arial's, Times New Roman's and Courier New's metrics, in which the SVG corpora's references draw
        # the generic families.
        ("Arimo.ttf", OFL + "arimo/Arimo%5Bwght%5D.ttf"),
        ("Arimo-Italic.ttf", OFL + "arimo/Arimo-Italic%5Bwght%5D.ttf"),
        ("Tinos.ttf", OFL + "tinos/Tinos-Regular.ttf"),
        ("Tinos-Bold.ttf", OFL + "tinos/Tinos-Bold.ttf"),
        ("Tinos-Italic.ttf", OFL + "tinos/Tinos-Italic.ttf"),
        ("Cousine.ttf", OFL + "cousine/Cousine-Regular.ttf"),
        ("Twemoji.ttf", COLOR + "twemoji-glyf_colr_1.ttf"),
        ("ColrTestGlyphs.ttf", COLOR + "test_glyphs-glyf_colr_1.ttf"),
        ("HandwritingCff2.otf", COLOR + "noto_handwriting-cff2_colr_1.otf"),
        # Color bitmap strikes: sbix from the color-fonts project, and CBDT from Noto Color Emoji and from a subset of
        # it in HarfBuzz's tests that has a second strike.
        ("TwemojiSmileySbix.ttf", COLOR + "twemoji_smiley-sbix.ttf"),
        ("NotoFlagsSbix.ttf", COLOR + "noto_flags-sbix.ttf"),
        ("NotoColorEmoji.ttf",
         "https://raw.githubusercontent.com/googlefonts/noto-emoji/%s/fonts/NotoColorEmoji.ttf" % NOTO_EMOJI_TAG),
        ("NotoColorEmojiCbdt.ttf", "https://raw.githubusercontent.com/harfbuzz/harfbuzz/%s/test/api/fonts/"
                                   "NotoColorEmoji.subset.multiple_size_tables.ttf" % HARFBUZZ_TAG),
        # A WOFF file whose tables are some compressed and some stored, and Ahem, whose glyphs are boxes of the em,
        # from WPT's fonts.
        ("AD.woff", WPT_RAW + "fonts/AD.woff"),
        ("Ahem.ttf", WPT_RAW + "fonts/Ahem.ttf"),
        # WPT's underline test fonts, Chromium's, Apache 2: "A", "G", "a" and "g" only, with an underline far from the
        # baseline and one thick, for text decorations from the font.
        ("UnderlineTest-Far.ttf", WPT_RAW + "css/css-text-decor/resources/UnderlineTest-Far.ttf"),
        ("UnderlineTest-Thick.ttf", WPT_RAW + "css/css-text-decor/resources/UnderlineTest-Thick.ttf"),
        # WPT's baseline diagnostic font: every baseline a BASE table can give, each at its own height and drawn by its
        # glyphs, and its variant whose alphabetic baseline is at zero.
        ("BaselineDiagnostic.ttf", WPT_RAW + "fonts/baseline-diagnostic/BaselineDiagnostic.ttf"),
        ("BaselineDiagnosticAlphabeticZero.ttf",
         WPT_RAW + "fonts/baseline-diagnostic/BaselineDiagnosticAlphabeticZero.ttf"),
        # Literata's Latin subset as Fontsource publishes it: as WOFF and as WOFF2, and its variable font as WOFF2.
        ("Literata-Latin.woff", NPM + "@fontsource/literata@5.2.8/files/literata-latin-400-normal.woff"),
        ("Literata-Latin.woff2", NPM + "@fontsource/literata@5.2.8/files/literata-latin-400-normal.woff2"),
        ("Literata-Latin-Variable.woff2",
         NPM + "@fontsource-variable/literata@5.2.8/files/literata-latin-opsz-normal.woff2"),
    ]
    # WOFF2 files of WPT's WOFF2 tests, valid and invalid, and the fonts the valid ones hold.
    files += [("woff2/" + name, WPT_RAW + "css/WOFF2/support/" + name)
              for name in ("SFNT-TTF-Reference.ttf", "SFNT-CFF-Reference.otf", "valid-001.woff2", "valid-005.woff2",
                           "tabledata-transform-hmtx-001.woff2", "available-002.woff2",
                           "tabledata-glyf-bbox-002.woff2", "tabledata-brotli-001.woff2", "header-length-001.woff2",
                           "directory-mismatched-tables-001.woff2")]
    # WPT's math fonts, for the tests of lib/font's MATH reader and of math layout. Each sets one or a few OpenType MATH
    # parameters or glyph constructions to whole numbers of ems and the rest to zero, so that a test gives the exact
    # position that the parameter decides. Their names and WPT's mathml/tools, which generated them, give their
    # parameters.
    files += [("math/%s.woff" % name, WPT_RAW + "fonts/math/%s.woff" % name) for name in MATH_TEST_FONTS]
    return [
        # The CMYK profile css-color's calibrated CMYK and device-cmyk() examples are computed with, for suites/prax.
        Files("icc", [("Coated_Fogra39L_VIGC_300.icc",
                       "https://raw.githubusercontent.com/w3c/csswg-drafts/%s/css-color-4/ICCprofiles/"
                       "Coated_Fogra39L_VIGC_300.icc" % CSSWG_DRAFTS_COMMIT)]),
        Files("fonts", files),
    ]


def rlottie():
    """rlottie, Samsung's Lottie player: the corpus of the rlottie suite is its examples. The build compiles the player,
    the suite's reference (rlottie_reference), at the same commit (cmake/dependencies.cmake)."""
    return [Git("lottie/rlottie", "https://github.com/Samsung/rlottie.git", RLOTTIE_COMMIT, "example")]


# eyeasme.com's MathML browser test: formulas in MathML, each beside images of TeX's setting of it and Firefox's. The
# site has no versions, so each file is pinned by its SHA-256.
EYEASME = "http://eyeasme.com/Joe/MathML/"
EYEASME_FILES = {
    "MathML_browser_test.html": "3df1e25152f45429e639fb115ecbb7cbc21ecc868d3018204acee07cbb7f85bf",
    "Images/MathML/axiom_of_power_set.png": "b97a5946114f8ed2480377092fc0efe43daa4558d162cf125a6412a2682b9821",
    "Images/MathML/binomial_coefficient.png": "d2173ed18995ed5370f8a2168ed2eea1e873c897abc23174bfa8cce9b63cf207",
    "Images/MathML/cichons_diagram.png": "5e07d54922cf425a2a7ff78245d0eb8b43f1aec5f718d6a70d39491d3bac0a1a",
    "Images/MathML/complex_number.png": "c7de52e2df790159248f9bbb11fd08f49aa3132615707bb9d5fd3a61e20d0aaf",
    "Images/MathML/deMorgans_law.png": "2254ac6e11c5493ed65d514d29bd5898c45dee142fc5a4b913a2d56c6b0998be",
    "Images/MathML/differentiable_manifold.png": "e078ea8fae656830ce39bd85b7d2ff8424b4b99adce6b2ceebe006612f84ca1b",
    "Images/MathML/divergence.png": "b3221ff0a180e87545cfc9bfac97ee19f6562274f043e3002c3f7a48717848a0",
    "Images/MathML/font_sizes.png": "dc19e45dbd8a74b9785d7fc7a281bba4c6aaf42efd41327dd413e560a499d07d",
    "Images/MathML/moore_determinant.png": "b2eeb45280a3c7b4c1287ac640d15f6aaaffa22ad0dddc075542530040c776d8",
    "Images/MathML/multiscripts_and_greek_alphabet.png": "93e90b907a92d03b44f8e7ae660085cf133c5a125b2ea4bd19f1095dc9ae786c",
    "Images/MathML/nested_matrices.png": "f29adb006714431fa5e15de9d96f65a734990a60d70cac02c4ba94d2044b48b0",
    "Images/MathML/nested_roots.png": "edaa3fbca8253c6886b741d9968b9e89acbe628d86daa89b50ce2e55e3d563b5",
    "Images/MathML/quadratic_formula.png": "2bd43dc661c6c3c10119c9c499b84f65b8e1d9a7c82c67d3966cf527c20bac58",
    "Images/MathML/schwinger_dyson.png": "46307d4c86dc70f5095d708ffc59346c05063af74c39e15a447b0b50c674d03e",
    "Images/MathML/sophomores_dream.png": "610566553215305d542968283ac01be70c36526e572091ff1f9f6bc24b0c306b",
    "Images/MathML/sphere_volume.png": "f31d5e7d9f1196468836ad08e5453ba7675817607d05635a6162427b46c110c5",
    "Images/TeX/axiom_of_power_set.png": "e22b8cc5190c2d1a3c6fdf6347304326fa7a490100a9bdeb376e01ad10f069da",
    "Images/TeX/binomial_coefficient.png": "05b31e48c49bf7d39de0db2725e4f5c3942ef22114e6e4ed3e01ef605a447374",
    "Images/TeX/cichons_diagram.png": "3c394fef78d35482ae5085c3467b8d03db6aa39874500fd23131508dab8cbbf7",
    "Images/TeX/complex_number.png": "107f4b9ceb39303d7352e6b3a0affe71b40f46f7205a1be02cb18bb864fe002a",
    "Images/TeX/deMorgans_law.png": "48815c963617130fb196b023610a293036a8a38733b836de1976e1b3e5690dae",
    "Images/TeX/differentiable_manifold.png": "72831fbd1e1f2f81301f1b640235fcd42ec96d90f21e74f1d5e09813862b0941",
    "Images/TeX/divergence.png": "f21582291790febb6e5ba459b11f7ce7569155fb869fe30d012fbbd47b14d995",
    "Images/TeX/font_sizes.png": "cdae710772d2ed9976b1c57a73718b27f8e15a7fb36b51c9cb30c37fcbafe8e6",
    "Images/TeX/moore_determinant.png": "86ca2ca15a5e68432a1f267cdb3880df92d83151ea4eab37eb8229aa19b548de",
    "Images/TeX/multiscripts_and_greek_alphabet.png": "afe149330fbf96c9c98ec11ac605e9fe855fb8cd225a14c5e52dcadd32f2b26d",
    "Images/TeX/nested_matrices.png": "1fa2a092aa732a0433b7b2115a733812d44c83878fb6d529bb010802f62bafef",
    "Images/TeX/nested_roots.png": "f3dcaa8d6803f81fc625e13d69b245e0c012b2dde2e1c3cf0251ef507477ba98",
    "Images/TeX/quadratic_formula.png": "dd7cf9e46df2fa5903495ca4afeb98dfaf4224e1b4e7581793f69dbb404f1f7b",
    "Images/TeX/schwinger_dyson.png": "6b002233682c66655b4b38ba2b03e76b9f036d47822c77761ac178c5732aff0f",
    "Images/TeX/sophomores_dream.png": "1b20721b33517b101f96f3c16ebf71bc7deeaf8637868446efec9ba4135cd525",
    "Images/TeX/sphere_volume.png": "032dc88627f74d1907fcf584f0844cfde33fad03d8f43a3d7804001050082f2d",
}


def eyeasme():
    """The MathML browser test's page and its images, as the site lays them out."""
    return [Files("eyeasme", [(name, EYEASME + name) for name in EYEASME_FILES], digests=EYEASME_FILES)]


# The checkouts, by name, each as what it fetches.
CHECKOUTS = {
    # metadata for code generation.
    "webref": lambda: [Git("webref", "https://github.com/w3c/webref.git", WEBREF_COMMIT, "ed/css", "ed/dfns",
                           "ed/elements", "ed/headings", "ed/ids", "ed/idl")],
    # web-platform-tests, the conformance corpus, whole: a suite's pages load from across it, and
    # tools/suite/wpt/wpt.py's SUITES chooses the directories that each suite runs.
    "wpt": lambda: [Git("wpt", "https://github.com/web-platform-tests/wpt.git", WPT_COMMIT)],
    # servo/rust-cssparser, for css-parsing-tests.
    "cssparser": lambda: [Git("cssparser", "https://github.com/servo/rust-cssparser.git", CSSPARSER_COMMIT,
                              "src/css-parsing-tests")],
    # html5lib/html5lib-tests
    "html5lib": lambda: [Git("html5lib", "https://github.com/html5lib/html5lib-tests.git", HTML5LIB_COMMIT,
                             "tokenizer", "encoding")],
    # The resvg test suite: each test is an SVG beside the PNG it should render as. Also the fonts and resources the
    # tests read.
    "resvg_tests": lambda: [Git("resvg-tests", "https://github.com/linebender/resvg-test-suite.git",
                                RESVG_TESTS_COMMIT, "tests", "fonts", "resources")],
    # lottie-web, the player that the web plays Lottie with. Its SVG renderer is the reference of the lottie-web suite
    # (tools/suite/lottie/player.html). This is its full build, including expressions.
    "lottie_web": lambda: [Files("lottie-web", [("lottie.min.js", NPM + "lottie-web@%s/build/player/lottie.min.js"
                                                 % LOTTIE_WEB_VERSION)], ("version", LOTTIE_WEB_VERSION))],
    # dotLottie's web player, which is ThorVG compiled to WebAssembly. The viewer compares lib/lottie with it
    # (tools/suite/lottie/dotlottie.html).
    "dotlottie_web": lambda: [Files("dotlottie-web", [
        (name, NPM + "@lottiefiles/dotlottie-web@%s/dist/%s" % (DOTLOTTIE_WEB_VERSION, name))
        for name in ("index.js", "dotlottie-player.wasm")], ("version", DOTLOTTIE_WEB_VERSION))],
    # CanvasKit, Skia's WebAssembly build, whose full build holds Skottie, Skia's Lottie player: the reference of the
    # skottie suite (tools/suite/lottie/skottie.html).
    "canvaskit": lambda: [Files("canvaskit", [(name, NPM + "canvaskit-wasm@%s/bin/full/%s" % (CANVASKIT_VERSION, name))
                                              for name in ("canvaskit.js", "canvaskit.wasm")],
                                ("version", CANVASKIT_VERSION))],
    "rlottie": rlottie,
    # The Lottie animations that the viewer and the suites play, from the players' own tests: lottie-ios's samples
    # (JSON and dotLottie) with the snapshots that its tests took of them, Skottie's feature tests, rlottie's examples,
    # lottie-web's, and the specification's, which test only reading.
    "lottie_corpora": lambda: [
        Git("lottie/lottie-ios", "https://github.com/airbnb/lottie-ios.git", LOTTIE_IOS_COMMIT, "Tests/Samples",
            "Tests/__Snapshots__/SnapshotTests"),
        Git("lottie/skia", "https://github.com/google/skia.git", SKIA_COMMIT, "resources/skottie"),
        *rlottie(),
        Git("lottie/lottie-web", "https://github.com/airbnb/lottie-web.git", LOTTIE_WEB_COMMIT, "test/animations",
            "demo"),
        Git("lottie/lottie-spec", "https://github.com/lottie/lottie-spec.git", LOTTIE_SPEC_COMMIT, "tests"),
    ],
    "fonts": fonts,
    # Visual suites of MathML for a person to compare with TeX and the browsers (./dev.sh probe, with --root
    # third_party for MathFonts' pages, which name their scripts and fonts from the root).
    "mathfonts": lambda: [Git("MathFonts", "https://github.com/fred-wang/MathFonts.git", MATHFONTS_COMMIT,
                              "mozilla_mathml_test", "NewComputerModern", "LatinModern", "STIX")],
    "eyeasme": eyeasme,
    # The stylesheets the benchmark parses, pinned so that each number is comparable with the last. Three are in the
    # form a stylesheet is written in, and six are in the form one is shipped in: minified, tree-shaken and without
    # comments.
    "css": lambda: [Files("css", [
        ("bootstrap.css", NPM + "bootstrap@5.3.3/dist/css/bootstrap.css"),
        ("bulma.css", NPM + "bulma@1.0.2/css/bulma.css"),
        ("normalize.css", NPM + "normalize.css@8.0.1/normalize.css"),
        ("bootstrap.min.css", NPM + "bootstrap@5.3.3/dist/css/bootstrap.min.css"),
        ("bulma.min.css", NPM + "bulma@1.0.2/css/bulma.min.css"),
        ("tailwind.min.css", NPM + "tailwindcss@2.2.19/dist/tailwind.min.css"),
        ("primer.css", NPM + "@primer/css@21.5.1/dist/primer.css"),
        ("fontawesome.min.css", NPM + "@fortawesome/fontawesome-free@6.5.2/css/all.min.css"),
        ("animate.min.css", NPM + "animate.css@4.1.1/animate.min.css"),
    ])],
    # The real pages the benchmarks read, which are whatever is in third_party/pages. Unlike a corpus that a suite
    # reads, this one is meant to move, but only when asked: `./dev.sh pages roll` (or `./dev.sh fetch pages`) takes
    # the day's pages from the sources that tools/pages/pages.py names and drops the oldest, so the set rolls forward
    # and nothing can be tuned to it. Every page taken is also kept in third_party/pages-corpus, which only grows. A
    # benchmark run takes pages only if there are none, so that it stays quick and needs no network. The pages are a
    # benchmark's inputs, so a comparison across a roll reports that its workload changed (tools/bench/perf.py).
    "pages": lambda: [Pages()],
    # Long documents the document benchmark loads, scrolls and navigates. The Vulkan specification moves with each
    # release; a benchmark records the hash of the copy it read, so runs compare only on the same one.
    "docs": lambda: [Files("docs", [("vulkan.html",
                                     "https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html")])],
    # The video example's video (examples/video): a ginger cat on a side table, from the Internet Archive under the
    # Public Domain Mark 1.0 (archive.org/details/the-table-and-the-kitten). H.264 High in MP4, 720 by 1280, BT.709,
    # 30 seconds, which every platform's media backend and every browser plays.
    "videos": lambda: [Files("videos", [
        ("cat.mp4", "https://archive.org/download/the-table-and-the-kitten/The%20Table%20And%20The%20Kitten.mp4"),
    ])],
    # The named character references, which html-parsing provides as one JSON file.
    "entities": lambda: [Files("html", [("entities.json", "https://html.spec.whatwg.org/entities.json")])],
    # The Encoding Standard's encodings, with their labels, and its indexes, for the charset generator.
    "encodings": lambda: [Files("encoding", [
        (name + ".json", "https://raw.githubusercontent.com/whatwg/encoding/%s/%s.json" % (ENCODING_COMMIT, name))
        for name in ("encodings", "indexes")], ("commit", ENCODING_COMMIT))],
    # The Unicode Character Database files that the generators read, at one version, UTS #46's mapping table and
    # conformance tests for that version, and UAX #31's revision for that version, whose Table 5 lists the scripts in
    # modern use.
    "unicode": lambda: [Files("unicode", [
        (path.split("/")[-1], UCD + path) for path in (
            "extracted/DerivedBidiClass.txt", "extracted/DerivedJoiningType.txt", "BidiBrackets.txt", "BidiMirroring.txt",
            "BidiTest.txt",
            "BidiCharacterTest.txt", "LineBreak.txt", "auxiliary/LineBreakTest.txt", "EastAsianWidth.txt",
            "extracted/DerivedGeneralCategory.txt", "auxiliary/GraphemeBreakProperty.txt",
            "auxiliary/GraphemeBreakTest.txt",
            "auxiliary/WordBreakProperty.txt", "auxiliary/WordBreakTest.txt",
            "emoji/emoji-data.txt", "DerivedCoreProperties.txt", "PropList.txt",
            "UnicodeData.txt", "SpecialCasing.txt", "CaseFolding.txt", "Scripts.txt", "VerticalOrientation.txt",
            "PropertyValueAliases.txt", "CompositionExclusions.txt", "NormalizationTest.txt")] + [
        ("IdnaMappingTable.txt", "https://www.unicode.org/Public/17.0.0/idna/IdnaMappingTable.txt"),
        ("IdnaTestV2.txt", "https://www.unicode.org/Public/17.0.0/idna/IdnaTestV2.txt"),
        ("tr31-45.html", "https://www.unicode.org/reports/tr31/tr31-45.html"),
    ])],
    # Unicode CLDR, for the language generator: its supplemental data (likely subtags, populations), each locale's
    # delimiters, and the copies of the IANA Language Subtag Registry and ISO 639-3's code table it keeps, which neither
    # authority publishes at a versioned URL.
    "cldr": lambda: [Git("cldr", "https://github.com/unicode-org/cldr.git", CLDR_COMMIT, "common/supplemental",
                         "common/main", "tools/cldr-code/src/main/resources/org/unicode/cldr/util/data")],
    # The web-features data, at the version that tools/suite/wpt/web_features.txt rules on and states.
    "webfeatures": lambda: [Files("web-features", [
        (web_features_version() + ".json", NPM + "web-features@%s/data.json" % web_features_version())])],
    # The RFCs that generators read. A published RFC never changes, so its number pins it.
    "rfcs": lambda: [Files("rfc", [("rfc7932.txt", "https://www.rfc-editor.org/rfc/rfc7932.txt")])],
    # MathML Core's operator dictionary, the operators that stretch along the inline axis, the combining characters
    # that stand for spacing accents, and the glyph assemblies built from Unicode characters, for the MathML generator,
    # with MathML 3's dictionary, the last that lists the accent property, at its dated Recommendation.
    "mathml": lambda: [Files("mathml", [
        (name, "https://raw.githubusercontent.com/w3c/mathml-core/%s/tables/%s" % (MATHML_CORE_COMMIT, name))
        for name in ("operator-dictionary-compact.html", "inline-axis-operators.txt", "combining-operators.html",
                     "unicode-assembly.html")] + [
        ("mathml3-operator-dictionary.html", "https://www.w3.org/TR/2014/REC-MathML3-20140410/appendixc.html")],
        ("commit", MATHML_CORE_COMMIT))],
    # The sources of the CSS Working Group's drafts that generators read: css-counter-styles-3's predefined counter
    # styles, for the CSS generator.
    "drafts": lambda: [Files("drafts", [
        ("css-counter-styles-3.bs", "https://raw.githubusercontent.com/w3c/csswg-drafts/%s/css-counter-styles-3/"
                                    "Overview.bs" % CSSWG_DRAFTS_COMMIT)])],
    # commonmark/commonmark-spec: the specification, whose examples are the suite.
    "commonmark": lambda: [Git("commonmark-spec", "https://github.com/commonmark/commonmark-spec.git",
                               COMMONMARK_SPEC_COMMIT, "test")],
    # github/cmark-gfm: GFM's specification and its tests of the extensions. The build compiles cmark-gfm itself at
    # the same commit (cmake/dependencies.cmake).
    "cmark_gfm": lambda: [Git("cmark-gfm", "https://github.com/github/cmark-gfm.git", CMARK_GFM_COMMIT, "test")],
    # yeslogic/prince-samples: documents written for print, each with the PDF that Prince made of it, which the viewer
    # shows beside the engine's own PDF (tools/viewer/suite_prince.cpp). The repository provides a file without license
    # information only for evaluating Prince, so only the directories whose README licenses both the HTML and its PDF
    # are fetched: under CC BY 4.0, CC BY-SA 4.0 (textbook), or the Project Gutenberg License (malthus, twist). Each
    # README, which carries its sample's attribution, is fetched with its directory, and so is the root README. Most
    # documents load fonts, sheets and images from the web, so each is captured with what it loads from other origins
    # (third_party/prince-samples-capture), and the viewer and `./dev.sh document` print it the same on every run. What
    # a capture holds belongs to its owners under their own terms: it is a local copy for testing, never committed and
    # never redistributed.
    "prince_samples": lambda: [
        Git("prince-samples", "https://github.com/yeslogic/prince-samples.git", PRINCE_SAMPLES_COMMIT,
            *PRINCE_SAMPLES),
        Captures("prince-samples-capture", "prince-samples",
                 [directory + "/" + page for directory, pages in PRINCE_SAMPLES.items() for page in pages],
                 ("commit", PRINCE_SAMPLES_COMMIT)),
    ],
}


def fetch(*names, pages_args=()):
    """Fetches the checkouts named, or every one. The real pages take `pages_args`, as --if-empty."""
    for name in names or CHECKOUTS:
        if name not in CHECKOUTS:
            dev.fail("no checkout named %s; one of: %s" % (name, " ".join(CHECKOUTS)))
        for part in CHECKOUTS[name]():
            if isinstance(part, Pages):
                part.fetch(*pages_args)
            else:
                part.fetch()

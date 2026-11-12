# The files that the tests read: fonts, and html5lib's encoding tests. Each is fetched by pr_dependency, pinned and
# checked against its hash, into the download cache that the rest of the build's dependencies share, and linked into a
# directory of the build (pr_dependency_link). A test reads them there by name, so a build has everything its tests
# read, and nothing a suite or a benchmark reads.
#
# A font that a font bundle carries (font_bundle.cmake) is the bundle's file, so a build that fetches the bundle fetches
# it once.

# The sources, each at a commit or version that pins it.
set(_pr_test_ofl https://raw.githubusercontent.com/google/fonts/23e54b51ddffbc7713c583748e3bd86f62b1fa4a/ofl)
set(_pr_test_color https://raw.githubusercontent.com/googlefonts/color-fonts/0046ea4c3b69e9fbbe464c2594816894e3aa5e4b/fonts)
set(_pr_test_wpt https://raw.githubusercontent.com/web-platform-tests/wpt/269bca0dd35c303639f3c9cf1d8bcb3d911bdb60)
set(_pr_test_html5lib https://raw.githubusercontent.com/html5lib/html5lib-tests/224991ec10db04f056a89eed8b0bd8695fd2950e)

# Fonts each chosen for what they carry: color glyphs (COLRv1, sbix and CBDT), CFF2, variation axes, WOFF and WOFF2, and
# Ahem, whose glyphs are boxes of the em. WPT gives its WOFF2 tests, valid and invalid, and the fonts the valid ones
# hold, and its CID-keyed CFF fonts.
pr_dependency(test_font_AD
    URL ${_pr_test_wpt}/fonts/AD.woff
    URL_HASH SHA256=2cdf52a7bd609fa613597d0004da7d858c42de0c56a0816e7530827a04607235
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME AD.woff)
pr_dependency(test_font_Ahem
    URL ${_pr_test_wpt}/fonts/Ahem.ttf
    URL_HASH SHA256=b719ecb31c5b21fc573c03f6421c74ac63c271a5a3ff841e34f9705fb94b8448
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME Ahem.ttf)
pr_dependency(test_font_BungeeSpice
    URL ${_pr_test_ofl}/bungeespice/BungeeSpice-Regular.ttf
    URL_HASH SHA256=ccf8e3f7eb1ac87ed217097332856f9007484362a00ae3b42a03c6ef851158d0
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME BungeeSpice.ttf)
pr_dependency(test_font_ColrTestGlyphs
    URL ${_pr_test_color}/test_glyphs-glyf_colr_1.ttf
    URL_HASH SHA256=8aa611b1ca97044ac6f13dc982fde29256612f0a5acc6ef47ca541a7a5b99b28
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME ColrTestGlyphs.ttf)
pr_dependency(test_font_KalniaGlaze
    URL ${_pr_test_ofl}/kalniaglaze/KalniaGlaze%5Bwdth,wght%5D.ttf
    URL_HASH SHA256=526a3f364fff570bf01dfeb71b2de2f52bac0376b51004804e703df698b964ac
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME KalniaGlaze.ttf)
pr_dependency(test_font_Literata
    URL ${_pr_test_ofl}/literata/Literata%5Bopsz,wght%5D.ttf
    URL_HASH SHA256=b41138c9373112f32abb589cc22e8674b06ed4048b0c513be922bdd26f274440
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME Literata.ttf)
pr_dependency(test_font_Nabla
    URL ${_pr_test_ofl}/nabla/Nabla%5BEDPT,EHLT%5D.ttf
    URL_HASH SHA256=e45cec60eb2099b4b4ffee8ebe005d1d3060771071ede1caeeeb12e37a2c00ae
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME Nabla.ttf)
pr_dependency(test_font_NotoFlagsSbix
    URL ${_pr_test_color}/noto_flags-sbix.ttf
    URL_HASH SHA256=6eca255b00e49ac6f0a06f96ec1e3c44f0412b8024820f9f4c99430bfbf43752
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoFlagsSbix.ttf)
pr_dependency(test_font_NotoSansTaiTham
    URL ${_pr_test_ofl}/notosanstaitham/NotoSansTaiTham%5Bwght%5D.ttf
    URL_HASH SHA256=a053b99fc5ed0a3e5fb14eecbbf137245cb0b8c34c2cb4ffd0872db3097bcd0a
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansTaiTham.ttf)
pr_dependency(test_font_RobotoFlex
    URL ${_pr_test_ofl}/robotoflex/RobotoFlex%5BGRAD,XOPQ,XTRA,YOPQ,YTAS,YTDE,YTFI,YTLC,YTUC,opsz,slnt,wdth,wght%5D.ttf
    URL_HASH SHA256=9b523f7d82593df0107173849ebb8c817471a1df4b4fb2c3cbf40cfd810c8281
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME RobotoFlex.ttf)
pr_dependency(test_font_Twemoji
    URL ${_pr_test_color}/twemoji-glyf_colr_1.ttf
    URL_HASH SHA256=50eabcbdf1edae2de36a211cea0e257d9b7195abe1d4ee8a260d6b39d82e35ba
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME Twemoji.ttf)
pr_dependency(test_font_TwemojiSmileySbix
    URL ${_pr_test_color}/twemoji_smiley-sbix.ttf
    URL_HASH SHA256=7767316e76003dff2415c0bec6d8972024d8f308349bb576abce7b26b5319c9f
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME TwemojiSmileySbix.ttf)
pr_dependency(test_font_available002
    URL ${_pr_test_wpt}/css/WOFF2/support/available-002.woff2
    URL_HASH SHA256=9bd2c1130ba26da89b9693ab663fb59a329d475597f19c48f1f5d83af63d2725
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME available-002.woff2)
pr_dependency(test_font_directorymismatchedtables001
    URL ${_pr_test_wpt}/css/WOFF2/support/directory-mismatched-tables-001.woff2
    URL_HASH SHA256=ed144837bbe272203e1a57e9ad36d84df565714d5933bd313f4ce8b56541083f
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME directory-mismatched-tables-001.woff2)
pr_dependency(test_font_headerlength001
    URL ${_pr_test_wpt}/css/WOFF2/support/header-length-001.woff2
    URL_HASH SHA256=9831401ab231fccb0a5d19f501632a7cea1f865b1cfdfbbe9955608dbb9b1166
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME header-length-001.woff2)
pr_dependency(test_font_SFNTCFFReference
    URL ${_pr_test_wpt}/css/WOFF2/support/SFNT-CFF-Reference.otf
    URL_HASH SHA256=5673677e8028c3be7b5459ef5d911d5da5903f9f5d8de447c16c8cfabaa372dc
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME SFNT-CFF-Reference.otf)
pr_dependency(test_font_SFNTTTFReference
    URL ${_pr_test_wpt}/css/WOFF2/support/SFNT-TTF-Reference.ttf
    URL_HASH SHA256=81147c9a2419338631e311fbc9ba524ba4828b33d2c6e5fd4e7703876ebbfefb
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME SFNT-TTF-Reference.ttf)
pr_dependency(test_font_tabledatabrotli001
    URL ${_pr_test_wpt}/css/WOFF2/support/tabledata-brotli-001.woff2
    URL_HASH SHA256=ff937a5586fb546627d618607216a3632fd61e48e422c62f9e2deeba235d5346
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME tabledata-brotli-001.woff2)
pr_dependency(test_font_tabledataglyfbbox002
    URL ${_pr_test_wpt}/css/WOFF2/support/tabledata-glyf-bbox-002.woff2
    URL_HASH SHA256=440b02e0d33a1160aeb9172f64f0c0dcf377af771aa6e5cf425dade4237d5219
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME tabledata-glyf-bbox-002.woff2)
pr_dependency(test_font_tabledatatransformhmtx001
    URL ${_pr_test_wpt}/css/WOFF2/support/tabledata-transform-hmtx-001.woff2
    URL_HASH SHA256=62a9934d08c576ce59cdfda6e1b31746e5785f58562b3fd545df791075e5796c
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME tabledata-transform-hmtx-001.woff2)
pr_dependency(test_font_valid001
    URL ${_pr_test_wpt}/css/WOFF2/support/valid-001.woff2
    URL_HASH SHA256=92cd7b916e02cb77a9b21fce3b89f588ba060eda31784c693093438a1a9a8a69
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME valid-001.woff2)
pr_dependency(test_font_valid005
    URL ${_pr_test_wpt}/css/WOFF2/support/valid-005.woff2
    URL_HASH SHA256=62a9934d08c576ce59cdfda6e1b31746e5785f58562b3fd545df791075e5796c
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME valid-005.woff2)
pr_dependency(test_font_CSSHWOrientationTest
    URL ${_pr_test_wpt}/fonts/adobe-fonts/CSSHWOrientationTest.otf
    URL_HASH SHA256=52e06e07aee1434de90aa94986b6adcbe2c841e3768e4ff0a4d97012f137f202
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME CSSHWOrientationTest.otf)
pr_dependency(test_font_CSSFWOrientationTest
    URL ${_pr_test_wpt}/fonts/adobe-fonts/CSSFWOrientationTest.otf
    URL_HASH SHA256=6415b31659d4137850f284484574b9232a00ce23ee4400a6655d98031a034cce
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME CSSFWOrientationTest.otf)
pr_dependency(test_font_NotoSansCJKkrRegularsubset
    URL ${_pr_test_wpt}/fonts/noto/cjk-kr/NotoSansCJKkr-Regular-subset.otf
    URL_HASH SHA256=762e0ecb5d295b7f6108ecd06d4de7f327853b422d83baca05c31ec31b95b0fe
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansCJKkr-Regular-subset.otf)

set(PR_TEST_FONTS
    "test_font_AD/AD.woff"
    "test_font_Ahem/Ahem.ttf"
    "font_ArimoItalic/Arimo-Italic.ttf"
    "font_ArimoRegular/Arimo-Regular.ttf"
    "test_font_available002/available-002.woff2"
    "test_font_BungeeSpice/BungeeSpice.ttf"
    "test_font_ColrTestGlyphs/ColrTestGlyphs.ttf"
    "font_CousineRegular/Cousine-Regular.ttf"
    "test_font_CSSFWOrientationTest/CSSFWOrientationTest.otf"
    "test_font_CSSHWOrientationTest/CSSHWOrientationTest.otf"
    "test_font_directorymismatchedtables001/directory-mismatched-tables-001.woff2"
    "test_font_headerlength001/header-length-001.woff2"
    "test_font_KalniaGlaze/KalniaGlaze.ttf"
    "font_LastResortRegular/LastResort-Regular.ttf"
    "font_LevibatsRegular/Levibats-Regular.ttf"
    "font_LeviSymbolRegular/LeviSymbol-Regular.ttf"
    "test_font_Literata/Literata.ttf"
    "test_font_Nabla/Nabla.ttf"
    "font_NewCM/fonts/opentype/public/newcomputermodern/NewCMMath-Regular.otf"
    "test_font_NotoFlagsSbix/NotoFlagsSbix.ttf"
    "font_NotoNastaliqUrdu/NotoNastaliqUrdu[wght].ttf"
    "font_NotoSansItalic/NotoSans-Italic[wdth,wght].ttf"
    "font_NotoSans/NotoSans[wdth,wght].ttf"
    "font_NotoSansArabic/NotoSansArabic[wdth,wght].ttf"
    "font_NotoSansBengali/NotoSansBengali[wdth,wght].ttf"
    "test_font_NotoSansCJKkrRegularsubset/NotoSansCJKkr-Regular-subset.otf"
    "font_NotoSansDevanagari/NotoSansDevanagari[wdth,wght].ttf"
    "font_NotoSansHebrew/NotoSansHebrew[wdth,wght].ttf"
    "font_NotoSansJPVF/NotoSansJP-VF.otf"
    "font_NotoSansMathRegular/NotoSansMath-Regular.ttf"
    "font_NotoSansMongolianRegular/NotoSansMongolian-Regular.ttf"
    "font_NotoSansMono/NotoSansMono[wdth,wght].ttf"
    "font_NotoSansSymbols2Regular/NotoSansSymbols2-Regular.ttf"
    "test_font_NotoSansTaiTham/NotoSansTaiTham.ttf"
    "font_NotoSerif/NotoSerif[wdth,wght].ttf"
    "test_font_RobotoFlex/RobotoFlex.ttf"
    "test_font_SFNTCFFReference/SFNT-CFF-Reference.otf"
    "test_font_SFNTTTFReference/SFNT-TTF-Reference.ttf"
    "font_NerdFontsSymbolsOnly/SymbolsNerdFontMono-Regular.ttf"
    "test_font_tabledatabrotli001/tabledata-brotli-001.woff2"
    "test_font_tabledataglyfbbox002/tabledata-glyf-bbox-002.woff2"
    "test_font_tabledatatransformhmtx001/tabledata-transform-hmtx-001.woff2"
    "font_TinosBold/Tinos-Bold.ttf"
    "font_TinosItalic/Tinos-Italic.ttf"
    "font_TinosRegular/Tinos-Regular.ttf"
    "test_font_Twemoji/Twemoji.ttf"
    "test_font_TwemojiSmileySbix/TwemojiSmileySbix.ttf"
    "test_font_valid001/valid-001.woff2"
    "test_font_valid005/valid-005.woff2"
    # From HarfBuzz's tests, in the archive that the build compiles HarfBuzz from (cmake/dependencies.cmake): a CFF2
    # variable font and a CFF2 font of COLRv1 glyphs, a subset of Noto Color Emoji with two CBDT strikes, a font of
    # variation sequences, and STIX Two Math, which has the flattened accents (flac) that the bundled math fonts lack.
    "harfbuzz/test/api/fonts/TestCFF2VF.otf"
    "harfbuzz/test/api/fonts/noto_handwriting-cff2_colr_1.otf"
    "harfbuzz/test/api/fonts/NotoColorEmoji.subset.multiple_size_tables.ttf"
    "harfbuzz/test/subset/data/fonts/cmap14_font1.otf"
    "harfbuzz/test/subset/data/fonts/STIXTwoMath-Regular.ttf")

# WPT's math test fonts, for the tests of lib/font's MATH reader and of math layout, in math under the fonts. Each sets
# one or a few OpenType MATH parameters or glyph constructions to whole numbers of ems and the rest to zero, so that a
# test gives the exact position that the parameter decides. Their names and WPT's mathml/tools, which generated them,
# give their parameters. Each is its name and its hash.
set(_pr_test_math_fonts
    "fraction-axisheight7000-rulethickness1000 959aa71732b7ede5ff66f74c12e254b0c51250fed955f2b5755fafb2d47a2315"
    "largeop-displayoperatorminheight2000-2AFF-italiccorrection3000 e39a2d2ffcf5f52d330dffa5418495845f6496d2a19dd8e185f227f9f0a98673"
    "limits-upperlimitgapmin7000 89da743eb0e88cf49e1d4670e81556b5c9b561a828c41a441adb2b76641bcec6"
    "radical-degreebottomraisepercent25-rulethickness1000 079c679066fb06c71e5ecac1652c16a86795b8680af44bdbe66963df45c1a63a"
    "radical-kernafterdegreeminus5000-rulethickness1000 6f33e7782a2c11b8ad547256554734fb5534bb454e494744bf781d3f3cec6b0a"
    "radical-rtlm 5f274691620db70eb7a57ac7b64e0a3438fcf8e61003e96d0090a44a12c9b6da"
    "scriptpercentscaledown0-scriptscriptpercentscaledown40 4f7233fabdfc1150e3108cb012ff9e1f3e4cfb289416d9c24de7b946632e0908"
    "scriptpercentscaledown80-scriptscriptpercentscaledown0 bc8790bfc4de0fb013ecaef060b1bf09e41d1fb42d112939f4e9118a77aae673"
    "scripts-subscriptshiftdown6000 dee32822fc76b0cce71b745138d871e415f79400c9725028da64fb6775b2ed1c"
    "scripts-subsuperscriptgapmin11000 8d51b89f65c3feb3b721e85813c3a51db2b56ae9798612887c70c39ed67aeb06"
    "scripts-superscriptbaselinedropmax10000 84d09c60df54071f673fc1650d84497c802c64f3d58782be49e9290969950876"
    "stack-gapmin8000 e96d0b9bc63d992b77ad533618af51ffd7d445b61ca003df633931e5bf16ca04"
    "stretchy 02462f896bceecaf2d4f96e9cda795c81aee4452cdf993d0566d29ab79245fb3"
    "stretchy-text-direction-asymetrical 81434a3cc83626306a1d1ff2e9ea3f8faf9672e6e196b3718e82787f788507c6"
    "underover-accentbaseheight4000-overbarextraascender3000 ee4966ea05a93e3188d7171af760d90bcde0cf3584c1056097130b7285f81bb2")
set(PR_TEST_MATH_FONTS "")
foreach (entry IN LISTS _pr_test_math_fonts)
    string(REPLACE " " ";" entry "${entry}")
    list(GET entry 0 name)
    list(GET entry 1 hash)
    string(MAKE_C_IDENTIFIER "test_math_${name}" dependency)
    pr_dependency(${dependency}
        URL ${_pr_test_wpt}/fonts/math/${name}.woff
        URL_HASH SHA256=${hash}
        DOWNLOAD_NO_EXTRACT ON
        DOWNLOAD_NAME ${name}.woff)
    list(APPEND PR_TEST_MATH_FONTS "${dependency}/${name}.woff")
endforeach()

# html5lib's tests of the encoding sniffing algorithm.
pr_dependency(test_html5lib_tests1
    URL ${_pr_test_html5lib}/encoding/tests1.dat
    URL_HASH SHA256=ef54dd3d9c256a6830c266808c0277545a156a241b350c613c996a4cea52e5f7
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME tests1.dat)
pr_dependency(test_html5lib_tests2
    URL ${_pr_test_html5lib}/encoding/tests2.dat
    URL_HASH SHA256=5e00d81e6207307f1186ea85f85b5f58003af94f5a7c6a3e06ca19c17567f51b
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME tests2.dat)
pr_dependency(test_html5lib_testyahoojp
    URL ${_pr_test_html5lib}/encoding/test-yahoo-jp.dat
    URL_HASH SHA256=dc353a7ceaa9428a668e7846e78efd18f7e0a09e768d6be61166c12fc223a432
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME test-yahoo-jp.dat)

set(PR_TEST_HTML5LIB
    "test_html5lib_tests1/tests1.dat"
    "test_html5lib_tests2/tests2.dat"
    "test_html5lib_testyahoojp/test-yahoo-jp.dat")

# The directories that the tests read the files from.
set(PR_TEST_FONTS_DIR "${PROJECT_BINARY_DIR}/test_data/fonts")
set(PR_TEST_HTML5LIB_DIR "${PROJECT_BINARY_DIR}/test_data/html5lib")
pr_dependency_link("${PR_TEST_FONTS_DIR}" ${PR_TEST_FONTS})
pr_dependency_link("${PR_TEST_FONTS_DIR}/math" ${PR_TEST_MATH_FONTS})
pr_dependency_link("${PR_TEST_HTML5LIB_DIR}/encoding" ${PR_TEST_HTML5LIB})

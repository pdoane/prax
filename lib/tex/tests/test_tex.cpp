#include "pr/string_ops.h"
#include "pr/tex.h"

#include "tex/src/tex_symbols.h"

#include "tools/harness/test.h"

extern "C" int prTestTexCApi(void);

namespace pr_impl {

namespace {

// Returns the markup of `source` with `options`.
String markup(PrStringView source, const PrTexOptions& options) {
    PrRawString out = prRawStringEmpty();
    prTexAppendMathMl(source, &options, &out);
    return String::adopt(out);
}

// Returns the presentation of `source`: its MathML without the <math> element and the annotation.
String mathml(PrStringView source, PrTexDisplay display = PrTexDisplay_Inline) {
    PrTexOptions options = prTexOptionsDefault();
    options.display = display;
    options.annotate = false;
    String text = markup(source, options);
    PrStringView whole = text;
    PrStringView open = display == PrTexDisplay_Block ? "<math display=\"block\">" : "<math display=\"inline\">";
    if (prStringStartsWith(whole, open) && whole.size >= open.size + 7)
        return String(prStringSlice(whole, open.size, whole.size - open.size - 7));
    return text;
}

String display(PrStringView source) {
    return mathml(source, PrTexDisplay_Block);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestTexCApi() == 0);
}

TEST_CASE("a formula is a math element annotated with its source, escaped") {
    PrTexOptions options = prTexOptionsDefault();
    CHECK(markup("x<y", options) == "<math display=\"inline\"><semantics><mrow><mi>x</mi><mo>&lt;</mo><mi>y</mi></mrow>"
        "<annotation encoding=\"application/x-tex\">x&lt;y</annotation></semantics></math>");
    options.display = PrTexDisplay_Block;
    CHECK(markup("a\\&b", options) ==
        "<math display=\"block\"><semantics><mrow><mi>a</mi><mo>&amp;</mo><mi>b</mi></mrow><annotation encoding=\"ap"
        "plication/x-tex\">a\\&amp;b</annotation></semantics></math>");
    options.annotate = false;
    CHECK(markup("a b", options) == "<math display=\"block\"><mi>a</mi><mi>b</mi></math>");
    CHECK(markup("", options) == "<math display=\"block\"></math>");
}

TEST_CASE("letters, numbers and operators are tokens") {
    CHECK(mathml("x") == "<mi>x</mi>");
    CHECK(mathml("ab") == "<mi>a</mi><mi>b</mi>");
    CHECK(mathml("3.14 + .5 - 1,000") ==
        "<mn>3.14</mn><mo>+</mo><mn>.5</mn><mo>−</mo><mn>1</mn><mo>,</mo><mn>000</mn>");
    CHECK(mathml("x = y < z > w") == "<mi>x</mi><mo>=</mo><mi>y</mi><mo>&lt;</mo><mi>z</mi><mo>&gt;</mo><mi>w</mi>");
    CHECK(mathml("f(x) [a] |b| a/b a*b") ==
        "<mi>f</mi><mo stretchy=\"false\">(</mo><mi>x</mi><mo stretchy=\"false\">)</mo><mo stretchy=\"false\">[</mo>"
        "<mi>a</mi><mo stretchy=\"false\">]</mo><mo lspace=\"0em\" rspace=\"0em\">|</mo><mi>b</mi><mo lspace=\"0em\""
        " rspace=\"0em\">|</mo><mi>a</mi><mo>/</mo><mi>b</mi><mi>a</mi><mo lspace=\"0.2222em\" rspace=\"0.2222em\">∗"
        "</mo><mi>b</mi>");
    CHECK(mathml("α≤β é") == "<mi>α</mi><mo>≤</mo><mi>β</mi><mi>é</mi>");
}

TEST_CASE("symbols are their characters, with uppercase Greek upright") {
    CHECK(mathml("\\alpha\\varepsilon\\phi\\varphi\\Gamma\\varGamma") ==
        "<mi>α</mi><mi>ε</mi><mi>ϕ</mi><mi>φ</mi><mi mathvariant=\"normal\">Γ</mi><mi>Γ</mi>");
    CHECK(mathml("\\infty \\partial \\nabla \\forall \\exists \\emptyset \\hbar \\ell \\aleph") ==
        "<mi>∞</mi><mi>∂</mi><mi mathvariant=\"normal\">∇</mi><mo>∀</mo><mo>∃</mo><mi>∅</mi><mi>ℏ</mi><mi>ℓ</mi><mi>"
        "ℵ</mi>");
    CHECK(mathml("\\leq \\in \\subseteq \\to \\Rightarrow \\mapsto \\cdot \\times \\oplus") ==
        "<mo lspace=\"0em\" rspace=\"0em\">≤</mo><mo lspace=\"0em\" rspace=\"0em\">∈</mo><mo lspace=\"0em\" rspace="
        "\"0em\">⊆</mo><mo lspace=\"0em\" rspace=\"0em\">→</mo><mo lspace=\"0em\" rspace=\"0em\">⇒</mo><mo lspace=\""
        "0em\">↦</mo><mo lspace=\"0em\">⋅</mo><mo lspace=\"0.0556em\">×</mo><mo lspace=\"0.0556em\" rspace=\"0em\">⊕"
        "</mo>");
    CHECK(mathml("\\ldots \\cdots \\vdots \\ddots") ==
        "<mo lspace=\"0em\" rspace=\"0em\">…</mo><mo lspace=\"0.1667em\" rspace=\"0em\">⋯</mo><mo lspace=\"0.1667em"
        "\" rspace=\"0em\">⋮</mo><mo lspace=\"0.1667em\" rspace=\"0em\">⋱</mo>");
    CHECK(mathml("\\langle x \\rangle \\lfloor y \\rfloor \\{ z \\}") ==
        "<mo stretchy=\"false\">⟨</mo><mi>x</mi><mo stretchy=\"false\">⟩</mo><mo stretchy=\"false\">⌊</mo><mi>y</mi>"
        "<mo stretchy=\"false\">⌋</mo><mo stretchy=\"false\">{</mo><mi>z</mi><mo stretchy=\"false\">}</mo>");
}

TEST_CASE("binary operators become ordinary where TeX makes them so") {
    CHECK(mathml("-x") == "<mo>−</mo><mi>x</mi>");
    CHECK(mathml("a = -b") == "<mi>a</mi><mo>=</mo><mo lspace=\"0em\" rspace=\"0em\">−</mo><mi>b</mi>");
    CHECK(mathml("a + -b") == "<mi>a</mi><mo>+</mo><mo lspace=\"0em\" rspace=\"0em\">−</mo><mi>b</mi>");
    CHECK(mathml("(+)") ==
        "<mo stretchy=\"false\">(</mo><mo lspace=\"0em\" rspace=\"0em\">+</mo><mo stretchy=\"false\">)</mo>");
    CHECK(mathml("a +") == "<mi>a</mi><mo lspace=\"0em\" rspace=\"0em\">+</mo>");
    CHECK(mathml("a, -b") == "<mi>a</mi><mo>,</mo><mo lspace=\"0em\" rspace=\"0em\">−</mo><mi>b</mi>");
}

TEST_CASE("atoms are spaced by TeX's table, without the conditional spaces in scripts") {
    CHECK(mathml("a \\times b") == "<mi>a</mi><mo lspace=\"0.2222em\" rspace=\"0.2222em\">×</mo><mi>b</mi>");
    CHECK(mathml("a:b") == "<mi>a</mi><mo lspace=\"0.2778em\" rspace=\"0.2778em\">:</mo><mi>b</mi>");
    CHECK(mathml("f\\colon A \\to B") == "<mi>f</mi><mo>:</mo><mi>A</mi><mo>→</mo><mi>B</mi>");
    CHECK(mathml("x^{a=b}") ==
        "<msup><mi>x</mi><mrow><mi>a</mi><mo lspace=\"0em\" rspace=\"0em\">=</mo><mi>b</mi></mrow></msup>");
    CHECK(mathml("\\frac{1}{2}x") ==
        "<mfrac><mn>1</mn><mn>2</mn></mfrac><mspace width=\"0.1667em\"></mspace><mi>x</mi>");
    CHECK(mathml("a, \\ldots, z") ==
        "<mi>a</mi><mo>,</mo><mo lspace=\"0em\" rspace=\"0.1667em\">…</mo><mo>,</mo><mi>z</mi>");
    CHECK(mathml("\\left(a\\right) b") ==
        "<mrow><mo>(</mo><mi>a</mi><mo>)</mo></mrow><mspace width=\"0.1667em\"></mspace><mi>b</mi>");
    CHECK(mathml("\\mathop{T} \\mathrel{x} y") ==
        "<mi>T</mi><mspace width=\"0.2778em\"></mspace><mi>x</mi><mspace width=\"0.2778em\"></mspace><mi>y</mi>");
    CHECK(mathml("a \\mathbin{\\#} b") ==
        "<mi>a</mi><mspace width=\"0.2222em\"></mspace><mi>#</mi><mspace width=\"0.2222em\"></mspace><mi>b</mi>");
}

TEST_CASE("scripts attach to the atom before them") {
    CHECK(mathml("x^2 y_1 z_i^2 w^{2}_{ij}") ==
        "<msup><mi>x</mi><mn>2</mn></msup><msub><mi>y</mi><mn>1</mn></msub><msubsup><mi>z</mi><mi>i</mi><mn>2</mn></"
        "msubsup><msubsup><mi>w</mi><mrow><mi>i</mi><mi>j</mi></mrow><mn>2</mn></msubsup>");
    CHECK(mathml("x^12") == "<msup><mi>x</mi><mn>1</mn></msup><mn>2</mn>");
    CHECK(mathml("{}^{14}C") == "<msup><mrow></mrow><mn>14</mn></msup><mi>C</mi>");
    CHECK(mathml("f' g'' h''' k'^2") ==
        "<msup><mi>f</mi><mo>′</mo></msup><msup><mi>g</mi><mo>″</mo></msup><msup><mi>h</mi><mo>‴</mo></msup><msup><m"
        "i>k</mi><mrow><mo>′</mo><mn>2</mn></mrow></msup>");
    CHECK(mathml("x^1^2") == "<msup><mi>x</mi><mrow><mn>1</mn><merror><mtext>^2</mtext></merror></mrow></msup>");
}

TEST_CASE("primes after a superscript are a double superscript") {
    CHECK(mathml("x^2'") == "<msup><mi>x</mi><mrow><mn>2</mn><merror><mtext>'</mtext></merror></mrow></msup>");
    CHECK(mathml("x'^2''") ==
        "<msup><mi>x</mi><mrow><mo>′</mo><mn>2</mn><merror><mtext>''</mtext></merror></mrow></msup>");
    CHECK(mathml("x'_i'") ==
        "<msubsup><mi>x</mi><mi>i</mi><mrow><mo>′</mo><merror><mtext>'</mtext></merror></mrow></msubsup>");
}

TEST_CASE("large operators take limits in display style and by movablelimits") {
    CHECK(mathml("\\sum_{i=1}^n x_i") ==
        "<munderover><mo lspace=\"0em\">∑</mo><mrow><mi>i</mi><mo lspace=\"0em\" rspace=\"0em\">=</mo><mn>1</mn></mr"
        "ow><mi>n</mi></munderover><msub><mi>x</mi><mi>i</mi></msub>");
    CHECK(display("\\sum_{i=1}^n x_i") ==
        "<munderover><mo lspace=\"0em\">∑</mo><mrow><mi>i</mi><mo lspace=\"0em\" rspace=\"0em\">=</mo><mn>1</mn></mr"
        "ow><mi>n</mi></munderover><msub><mi>x</mi><mi>i</mi></msub>");
    CHECK(mathml("\\int_0^1 f") == "<msubsup><mo lspace=\"0em\">∫</mo><mn>0</mn><mn>1</mn></msubsup><mi>f</mi>");
    CHECK(mathml("\\int\\limits_0^1 \\sum\\nolimits_i") ==
        "<munderover><mo lspace=\"0em\">∫</mo><mn>0</mn><mn>1</mn></munderover><msub><mo lspace=\"0em\" rspace=\"0em"
        "\">∑</mo><mi>i</mi></msub>");
    CHECK(mathml("\\sum\\limits_i") ==
        "<munder><mo lspace=\"0em\" rspace=\"0em\" movablelimits=\"false\">∑</mo><mi>i</mi></munder>");
    CHECK(mathml("\\prod \\coprod \\bigcup \\bigoplus \\oint \\iint") ==
        "<mo lspace=\"0em\">∏</mo><mo lspace=\"0em\">∐</mo><mo lspace=\"0em\">⋃</mo><mo lspace=\"0em\">⨁</mo><mo lsp"
        "ace=\"0em\">∮</mo><mo lspace=\"0em\" rspace=\"0em\">∬</mo>");
}

TEST_CASE("operator names apply as functions") {
    CHECK(mathml("\\sin x + \\cos(y) + \\log_2 n") ==
        "<mi>sin</mi><mo rspace=\"0.1667em\">⁡</mo><mi>x</mi><mo>+</mo><mi>cos</mi><mo>⁡</mo><mo stretchy=\"false\">"
        "(</mo><mi>y</mi><mo stretchy=\"false\">)</mo><mo>+</mo><msub><mi>log</mi><mn>2</mn></msub><mo rspace=\"0.16"
        "67em\">⁡</mo><mi>n</mi>");
    CHECK(mathml("\\sin^2 x") == "<msup><mi>sin</mi><mn>2</mn></msup><mo rspace=\"0.1667em\">⁡</mo><mi>x</mi>");
    CHECK(mathml("2\\sin x") ==
        "<mn>2</mn><mspace width=\"0.1667em\"></mspace><mi>sin</mi><mo rspace=\"0.1667em\">⁡</mo><mi>x</mi>");
    CHECK(mathml("\\lim_{x\\to 0} f") ==
        "<munder><mo lspace=\"0em\" rspace=\"0.1667em\" movablelimits=\"true\">lim</mo><mrow><mi>x</mi><mo lspace=\""
        "0em\" rspace=\"0em\">→</mo><mn>0</mn></mrow></munder><mi>f</mi>");
    CHECK(mathml("\\max A \\det B \\liminf") ==
        "<mo lspace=\"0em\" rspace=\"0.1667em\" movablelimits=\"true\">max</mo><mi>A</mi><mo lspace=\"0.1667em\" rsp"
        "ace=\"0.1667em\" movablelimits=\"true\">det</mo><mi>B</mi><mo lspace=\"0.1667em\" rspace=\"0em\" movablelim"
        "its=\"true\">lim inf</mo>");
    CHECK(mathml("\\operatorname{rank} A") == "<mi>rank</mi><mo rspace=\"0.1667em\">⁡</mo><mi>A</mi>");
    CHECK(mathml("\\operatorname{f}") == "<mi mathvariant=\"normal\">f</mi>");
    CHECK(mathml("\\operatorname*{argmax}_x f") ==
        "<munder><mo lspace=\"0em\" rspace=\"0.1667em\" movablelimits=\"true\">argmax</mo><mi>x</mi></munder><mi>f</"
        "mi>");
    CHECK(mathml("\\DeclareMathOperator{\\Tr}{Tr} \\Tr A") == "<mi>Tr</mi><mo rspace=\"0.1667em\">⁡</mo><mi>A</mi>");
}

TEST_CASE("modular arithmetic is spaced as amsmath spaces it") {
    CHECK(mathml("a \\bmod b") == "<mi>a</mi><mo>mod</mo><mi>b</mi>");
    CHECK(mathml("a \\pmod{n}") ==
        "<mi>a</mi><mrow><mspace width=\"0.4444em\"></mspace><mo stretchy=\"false\">(</mo><mi>mod</mi><mspace width="
        "\"0.3333em\"></mspace><mi>n</mi><mo stretchy=\"false\">)</mo></mrow>");
    CHECK(display("a \\pmod{n}") ==
        "<mi>a</mi><mrow><mspace width=\"1em\"></mspace><mo stretchy=\"false\">(</mo><mi>mod</mi><mspace width=\"0.3"
        "333em\"></mspace><mi>n</mi><mo stretchy=\"false\">)</mo></mrow>");
    CHECK(mathml("a \\mod n") ==
        "<mi>a</mi><mrow><mspace width=\"0.6667em\"></mspace><mi>mod</mi><mspace width=\"0.3333em\"></mspace><mi>n</"
        "mi></mrow>");
    CHECK(mathml("a \\pod n") ==
        "<mi>a</mi><mrow><mspace width=\"0.4444em\"></mspace><mo stretchy=\"false\">(</mo><mi>n</mi><mo stretchy=\"f"
        "alse\">)</mo></mrow>");
}

TEST_CASE("fractions, binomials and generalized fractions") {
    CHECK(mathml("\\frac{a}{b}") == "<mfrac><mi>a</mi><mi>b</mi></mfrac>");
    CHECK(mathml("\\frac12") == "<mfrac><mn>1</mn><mn>2</mn></mfrac>");
    CHECK(mathml("\\dfrac{a}{b} \\tfrac{a}{b}") ==
        "<mstyle displaystyle=\"true\" scriptlevel=\"0\"><mfrac><mi>a</mi><mi>b</mi></mfrac></mstyle><mspace width="
        "\"0.1667em\"></mspace><mfrac><mi>a</mi><mi>b</mi></mfrac>");
    CHECK(display("\\dfrac{a}{b} \\tfrac{a}{b}") ==
        "<mfrac><mi>a</mi><mi>b</mi></mfrac><mspace width=\"0.1667em\"></mspace><mstyle displaystyle=\"false\" scrip"
        "tlevel=\"0\"><mfrac><mi>a</mi><mi>b</mi></mfrac></mstyle>");
    CHECK(mathml("\\cfrac{1}{x}") ==
        "<mstyle displaystyle=\"true\" scriptlevel=\"0\"><mfrac><mstyle displaystyle=\"true\" scriptlevel=\"0\"><mro"
        "w><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mrow></mstyle><mstyle displaystyle=\"true"
        "\" scriptlevel=\"0\"><mi>x</mi></mstyle></mfrac></mstyle>");
    CHECK(mathml("\\binom{n}{k}") ==
        "<mrow><mo minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mfrac linethickness=\"0\"><mi>n</mi><mi>k</mi></mfrac>"
        "<mo minsize=\"1.2em\" maxsize=\"1.2em\">)</mo></mrow>");
    CHECK(display("\\binom{n}{k} \\tbinom{n}{k}") ==
        "<mrow><mo minsize=\"2.4em\" maxsize=\"2.4em\">(</mo><mfrac linethickness=\"0\"><mi>n</mi><mi>k</mi></mfrac>"
        "<mo minsize=\"2.4em\" maxsize=\"2.4em\">)</mo></mrow><mspace width=\"0.1667em\"></mspace><mstyle displaysty"
        "le=\"false\" scriptlevel=\"0\"><mrow><mo minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mfrac linethickness=\"0"
        "\"><mi>n</mi><mi>k</mi></mfrac><mo minsize=\"1.2em\" maxsize=\"1.2em\">)</mo></mrow></mstyle>");
    CHECK(mathml("\\genfrac{[}{]}{0pt}{}{a}{b}") ==
        "<mrow><mo minsize=\"1.2em\" maxsize=\"1.2em\">[</mo><mfrac linethickness=\"0\"><mi>a</mi><mi>b</mi></mfrac>"
        "<mo minsize=\"1.2em\" maxsize=\"1.2em\">]</mo></mrow>");
    CHECK(mathml("a \\over b") == "<mfrac><mi>a</mi><mi>b</mi></mfrac>");
    CHECK(mathml("n \\choose k") ==
        "<mrow><mo minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mfrac linethickness=\"0\"><mi>n</mi><mi>k</mi></mfrac>"
        "<mo minsize=\"1.2em\" maxsize=\"1.2em\">)</mo></mrow>");
    CHECK(mathml("a \\atop b") == "<mfrac linethickness=\"0\"><mi>a</mi><mi>b</mi></mfrac>");
    CHECK(mathml("a \\above 2pt b") == "<mfrac linethickness=\"0.2em\"><mi>a</mi><mi>b</mi></mfrac>");
    CHECK(mathml("\\frac{1}") == "<merror><mtext>\\frac{1}</mtext></merror>");
}

TEST_CASE("an infix fraction takes the whole list before it, declarations and all") {
    // A style is the numerator's alone, as TeX starts the denominator in the fraction's style.
    CHECK(mathml("a \\displaystyle b \\over c") ==
        "<mfrac><mrow><mi>a</mi><mstyle displaystyle=\"true\" scriptlevel=\"0\"><mi>b</mi></mstyle></mrow><mi>c</mi>"
        "</mfrac>");
    CHECK(mathml("a \\over \\displaystyle b") ==
        "<mfrac><mi>a</mi><mstyle displaystyle=\"true\" scriptlevel=\"0\"><mi>b</mi></mstyle></mfrac>");
    // A color or size lasts to the end of the group, through the denominator.
    CHECK(mathml("x \\color{red} y \\over z") ==
        "<mfrac><mrow><mi>x</mi><mstyle mathcolor=\"red\"><mi>y</mi></mstyle></mrow><mstyle mathcolor=\"red\"><mi>z"
        "</mi></mstyle></mfrac>");
    CHECK(mathml("{a \\small b \\atop c} d") ==
        "<mfrac linethickness=\"0\"><mrow><mi>a</mi><mstyle mathsize=\"0.9em\"><mi>b</mi></mstyle></mrow><mstyle mat"
        "hsize=\"0.9em\"><mi>c</mi></mstyle></mfrac><mi>d</mi>");
    CHECK(mathml("a \\color{red} b \\textstyle c \\large d \\over e") ==
        "<mfrac><mrow><mi>a</mi><mstyle mathcolor=\"red\"><mi>b</mi><mstyle displaystyle=\"false\" scriptlevel=\"0\""
        "><mi>c</mi><mstyle mathsize=\"1.2em\"><mi>d</mi></mstyle></mstyle></mstyle></mrow><mstyle mathcolor=\"red\""
        "><mstyle mathsize=\"1.2em\"><mi>e</mi></mstyle></mstyle></mfrac>");
    CHECK(mathml("\\color{red} a \\over b") ==
        "<mfrac><mstyle mathcolor=\"red\"><mi>a</mi></mstyle><mstyle mathcolor=\"red\"><mi>b</mi></mstyle></mfrac>");
}

TEST_CASE("\\cfrac sets its numerator flush left or right") {
    CHECK(mathml("\\cfrac[l]{1}{2} \\cfrac[r]{1}{2}") ==
        "<mstyle displaystyle=\"true\" scriptlevel=\"0\"><mfrac numalign=\"left\"><mstyle displaystyle=\"true\" scrip"
        "tlevel=\"0\"><mrow><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mrow></mstyle><mstyle dis"
        "playstyle=\"true\" scriptlevel=\"0\"><mn>2</mn></mstyle></mfrac></mstyle><mspace width=\"0.1667em\"></mspace"
        "><mstyle displaystyle=\"true\" scriptlevel=\"0\"><mfrac numalign=\"right\"><mstyle displaystyle=\"true\" scr"
        "iptlevel=\"0\"><mrow><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mrow></mstyle><mstyle d"
        "isplaystyle=\"true\" scriptlevel=\"0\"><mn>2</mn></mstyle></mfrac></mstyle>");
}

TEST_CASE("radicals") {
    CHECK(mathml("\\sqrt{2}") == "<msqrt><mn>2</mn></msqrt>");
    CHECK(mathml("\\sqrt{x+1}") == "<msqrt><mi>x</mi><mo>+</mo><mn>1</mn></msqrt>");
    CHECK(mathml("\\sqrt[3]{x}") == "<mroot><mi>x</mi><mn>3</mn></mroot>");
}

TEST_CASE("delimiters stretch with \\left and \\right, and are sized by \\big") {
    CHECK(mathml("\\left( \\frac{a}{b} \\right)") ==
        "<mrow><mo>(</mo><mfrac><mi>a</mi><mi>b</mi></mfrac><mo>)</mo></mrow>");
    CHECK(mathml("\\left\\{ x \\middle| x > 0 \\right\\}") ==
        "<mrow><mo>{</mo><mi>x</mi><mo lspace=\"0em\" rspace=\"0em\" stretchy=\"true\" symmetric=\"true\">|</mo><mi>"
        "x</mi><mo>&gt;</mo><mn>0</mn><mo>}</mo></mrow>");
    CHECK(mathml("\\left. \\frac{a}{b} \\right|_0") ==
        "<msub><mrow><mspace width=\"0.12em\"></mspace><mfrac><mi>a</mi><mi>b</mi></mfrac><mo>|</mo></mrow><mn>0</mn"
        "></msub>");
    CHECK(mathml("\\left\\langle x \\right\\rangle \\left/ x \\right\\backslash") ==
        "<mrow><mo>⟨</mo><mi>x</mi><mo>⟩</mo></mrow><mspace width=\"0.1667em\"></mspace><mrow><mo stretchy=\"true\" "
        "symmetric=\"true\">/</mo><mi>x</mi><mo stretchy=\"true\" symmetric=\"true\">\\</mo></mrow>");
    CHECK(mathml("\\big( \\Big[ \\bigg\\{ \\Bigg|") ==
        "<mo minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mo minsize=\"1.8em\" maxsize=\"1.8em\">[</mo><mo minsize=\"2"
        ".4em\" maxsize=\"2.4em\">{</mo><mo minsize=\"3em\" maxsize=\"3em\">|</mo>");
    CHECK(mathml("\\bigl( x \\bigr) \\bigm| y") ==
        "<mo minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mi>x</mi><mo minsize=\"1.2em\" maxsize=\"1.2em\">)</mo><mo s"
        "tretchy=\"true\" symmetric=\"true\" minsize=\"1.2em\" maxsize=\"1.2em\">|</mo><mi>y</mi>");
    CHECK(mathml("\\left( x") == "<mrow><mo>(</mo><mi>x</mi></mrow>");
    CHECK(mathml("\\right)") == "<merror><mtext>\\right)</mtext></merror>");
    CHECK(mathml("\\left x") == "<merror><mtext>\\left</mtext></merror><mi>x</mi>");
}

TEST_CASE("accents sit over or under their bases") {
    CHECK(mathml("\\hat{x} \\widehat{xy} \\tilde a \\widetilde{ab}") ==
        "<mover accent=\"true\"><mi>x</mi><mo stretchy=\"false\">^</mo></mover><mover accent=\"true\"><mrow><mi>x</m"
        "i><mi>y</mi></mrow><mo>^</mo></mover><mover accent=\"true\"><mi>a</mi><mo stretchy=\"false\">~</mo></mover>"
        "<mover accent=\"true\"><mrow><mi>a</mi><mi>b</mi></mrow><mo>~</mo></mover>");
    CHECK(mathml("\\bar x \\overline{AB} \\underline{x} \\vec v") ==
        "<mover accent=\"true\"><mi>x</mi><mo stretchy=\"false\">¯</mo></mover><mover accent=\"false\"><mrow><mi>A</mi><mi>B</mi></mrow><mo scriptlevel=\"+0\">‾</mo></mover><munder accentunder=\"false\"><mi>x</mi><mo scriptlevel=\"+0\">_</mo></munder><mover accent=\"true\"><mi>v</mi><mo>⃗</mo></mover>");
    CHECK(mathml("\\dot x \\ddot x \\dddot x \\acute x \\grave x \\breve x \\check x \\mathring x") ==
        "<mover accent=\"true\"><mi>x</mi><mo>˙</mo></mover><mover accent=\"true\"><mi>x</mi><mo>¨</mo></mover><move"
        "r accent=\"true\"><mi>x</mi><mo>⃛</mo></mover><mover accent=\"true\"><mi>x</mi><mo>´</mo></mover><mover acc"
        "ent=\"true\"><mi>x</mi><mo>`</mo></mover><mover accent=\"true\"><mi>x</mi><mo>˘</mo></mover><mover accent="
        "\"true\"><mi>x</mi><mo stretchy=\"false\">ˇ</mo></mover><mover accent=\"true\"><mi>x</mi><mo>˚</mo></mover>");
    CHECK(mathml("\\overrightarrow{AB} \\overleftarrow{x} \\overleftrightarrow{x} \\underleftarrow{x}") ==
        "<mover accent=\"false\"><mrow><mi>A</mi><mi>B</mi></mrow><mo scriptlevel=\"+0\">→</mo></mover><mover accent=\"false\"><mi>x</mi><mo scriptlevel=\"+0\">←</mo></mover><mover accent=\"false\"><mi>x</mi><mo scriptlevel=\"+0\">↔</mo></mover><munder accentunder=\"false\"><mi>x</mi><mo scriptlevel=\"+0\">←</mo></munder>");
    CHECK(mathml("\\overbrace{a+b}^{n} \\underbrace{c}_{m}") ==
        "<mover><mover accent=\"false\"><mrow><mi>a</mi><mo>+</mo><mi>b</mi></mrow><mo scriptlevel=\"+0\">⏞</mo></mover><mi>n</mi></mover><mspace width=\"0.1667em\"></mspace><munder><munder accentunder=\"false\"><mi>c</mi><mo scriptlevel=\"+0\">⏟</mo></munder><mi>m</mi></munder>");
}

TEST_CASE("stacked symbols and extensible arrows") {
    CHECK(mathml("\\overset{def}{=} \\underset{x}{\\to} \\stackrel{?}{=}") ==
        "<mover><mo lspace=\"0em\" rspace=\"0em\">=</mo><mrow><mi>d</mi><mi>e</mi><mi>f</mi></mrow></mover><munder><"
        "mo lspace=\"0em\" rspace=\"0em\" stretchy=\"false\">→</mo><mi>x</mi></munder><mover><mo lspace=\"0em\" rspa"
        "ce=\"0em\">=</mo><mo>?</mo></mover>");
    CHECK(mathml("a \\xrightarrow{f} b") ==
        "<mi>a</mi><mover><mo>→</mo><mpadded lspace=\"0.2778em\" width=\"+0.5556em\"><mi>f</mi></mpadded></mover><mi>"
        "b</mi>");
    CHECK(mathml("\\xleftarrow[g]{f}") ==
        "<munderover><mo lspace=\"0em\" rspace=\"0em\">←</mo><mpadded lspace=\"0.2778em\" width=\"+0.5556em\"><mi>g</"
        "mi></mpadded><mpadded lspace=\"0.2778em\" width=\"+0.5556em\"><mi>f</mi></mpadded></munderover>");
}

TEST_CASE("fonts are mathematical alphanumerics") {
    CHECK(mathml("\\mathbb{R} \\mathbb{C} \\mathbb{N} \\mathbb{1}") == "<mi>ℝ</mi><mi>ℂ</mi><mi>ℕ</mi><mn>𝟙</mn>");
    CHECK(mathml("\\mathbf{v} \\mathbf{12} \\mathbf{\\Gamma}") == "<mi>𝐯</mi><mn>𝟏𝟐</mn><mi>𝚪</mi>");
    CHECK(mathml("\\mathcal{L} \\mathscr{B} \\mathfrak{g} \\mathfrak{H}") ==
        "<mi>ℒ</mi><mi>ℬ\ufe01</mi><mi>𝔤</mi><mi>ℌ</mi>");
    CHECK(mathml("\\mathrm{d}x \\mathrm{max} \\mathit{diff} \\mathit{x}") ==
        "<mi mathvariant=\"normal\">d</mi><mi>x</mi><mi>max</mi><mi>𝑑𝑖𝑓𝑓</mi><mi>x</mi>");
    CHECK(mathml("\\mathsf{A} \\mathtt{x1} \\mathnormal{\\Gamma}") ==
        "<mi>𝖠</mi><mrow><mi>𝚡</mi><mn>𝟷</mn></mrow><mi>Γ</mi>");
    CHECK(mathml("\\boldsymbol{\\alpha} \\bm{x} \\boldsymbol{\\Gamma} \\boldsymbol{=}") ==
        "<mi>𝜶</mi><mi>𝒙</mi><mi>𝚪</mi><mo rspace=\"0em\" style=\"font-weight: bold\">=</mo>");
    // Calligraphic and roundhand capitals are the script capitals with Unicode's variation selectors.
    CHECK(mathml("\\mathcal{A} \\mathscr{A} \\mathbfcal{B} \\mathbfscr{B} \\mathcal{a} \\mathscr{1} {\\cal E}") ==
        "<mi>𝒜</mi><mi>𝒜\ufe01</mi><mi>𝓑</mi><mi>𝓑\ufe01</mi><mi>𝒶</mi><mn>1</mn><mi>ℰ</mi>");
    CHECK(mathml("{\\rm ab} {\\bf x}") == "<mi>ab</mi><mi>𝐱</mi>");
    CHECK(mathml("\\mathrm{ab}^2") == "<msup><mi>ab</mi><mn>2</mn></msup>");
}

TEST_CASE("text is mtext, with math inside it") {
    CHECK(mathml("\\text{if } x") == "<mtext>if\u00a0</mtext><mi>x</mi>");
    CHECK(mathml("\\text{for $x>0$ and \\textbf{bold}}") ==
        "<mrow><mtext>for\u00a0</mtext><mrow><mi>x</mi><mo>&gt;</mo><mn>0</mn></mrow><mtext>\u00a0and\u00a0</mtext><"
        "mtext style=\"font-weight: bold\">bold</mtext></mrow>");
    CHECK(mathml("\\textit{a} \\textsf{b} \\texttt{c} \\mbox{d} \\textrm{e}") ==
        "<mtext style=\"font-style: italic\">a</mtext><mtext style=\"font-family: sans-serif\">b</mtext><mtext style"
        "=\"font-family: monospace\">c</mtext><mtext>d</mtext><mtext>e</mtext>");
    CHECK(mathml("\\text{a -- b --- c ``d''}") == "<mtext>a – b — c “d”</mtext>");
    CHECK(mathml("\\text{\\'e\\\"o}") == "<mtext>éö</mtext>");
    CHECK(mathml("\\text{a\\foo}") == "<mrow><mtext>a</mtext><merror><mtext>\\foo</mtext></merror></mrow>");
}

TEST_CASE("spaces are mspaces in em") {
    CHECK(mathml("a\\,b\\:c\\;d\\!e\\ f~g") ==
        "<mi>a</mi><mspace width=\"0.1667em\"></mspace><mi>b</mi><mspace width=\"0.2222em\"></mspace><mi>c</mi><mspac"
        "e width=\"0.2778em\"></mspace><mi>d</mi><mspace width=\"-0.1667em\"></mspace><mi>e</mi><mspace width=\"0.333"
        "3em\"></mspace><mi>f</mi><mspace width=\"0.3333em\"></mspace><mi>g</mi>");
    CHECK(mathml("\\quad\\qquad\\enspace\\thinspace\\medspace\\thickspace") ==
        "<mspace width=\"1em\"></mspace><mspace width=\"2em\"></mspace><mspace width=\"0.5em\"></mspace><mspace widt"
        "h=\"0.1667em\"></mspace><mspace width=\"0.2222em\"></mspace><mspace width=\"0.2778em\"></mspace>");
    CHECK(mathml("\\hspace{1em} \\hspace{10pt} \\kern2mu \\mkern18mu \\mspace{1.5ex}") ==
        "<mspace width=\"1em\"></mspace><mspace width=\"1em\"></mspace><mspace width=\"0.1111em\"></mspace><mspace w"
        "idth=\"1em\"></mspace><mspace width=\"0.6465em\"></mspace>");
    CHECK(mathml("\\hspace{1furlong}") == "<merror><mtext>\\hspace{1furlong}</mtext></merror>");
}

TEST_CASE("a dimension is at most TeX's largest") {
    CHECK(mathml("\\kern{99999999999999999999999999em} \\kern{-99999999999999999999999999em}") ==
        "<mspace width=\"1638.4em\"></mspace><mspace width=\"-1638.4em\"></mspace>");
    String digits;
    digits.append(60, '9');
    String source = concat("\\hspace{", digits, "pt}");
    CHECK(mathml(source) == "<mspace width=\"1638.4em\"></mspace>");
}

TEST_CASE("styles set displaystyle and scriptlevel") {
    CHECK(mathml("a \\displaystyle \\sum_i b") ==
        "<mi>a</mi><mstyle displaystyle=\"true\" scriptlevel=\"0\"><munder><mo>∑</mo><mi>i</mi></munder><mi>b</mi></"
        "mstyle>");
    CHECK(display("\\textstyle a \\scriptstyle b \\scriptscriptstyle c") ==
        "<mstyle displaystyle=\"false\" scriptlevel=\"0\"><mi>a</mi><mstyle displaystyle=\"false\" scriptlevel=\"1\""
        "><mi>b</mi><mstyle displaystyle=\"false\" scriptlevel=\"2\"><mi>c</mi></mstyle></mstyle></mstyle>");
    CHECK(mathml("\\large x \\tiny y") ==
        "<mstyle mathsize=\"1.2em\"><mi>x</mi><mstyle mathsize=\"0.5em\"><mi>y</mi></mstyle></mstyle>");
}

TEST_CASE("colors and boxes") {
    CHECK(mathml("a \\color{red} b + c") ==
        "<mi>a</mi><mstyle mathcolor=\"red\"><mi>b</mi><mo>+</mo><mi>c</mi></mstyle>");
    CHECK(mathml("\\textcolor{blue}{x} \\textcolor[RGB]{255,0,0}{y} \\color[HTML]{00FF00} z") ==
        "<mi mathcolor=\"blue\">x</mi><mi mathcolor=\"#ff0000\">y</mi><mstyle mathcolor=\"#00FF00\"><mi>z</mi></msty"
        "le>");
    CHECK(mathml("\\colorbox{yellow}{hi} \\fcolorbox{red}{white}{x} \\fbox{t}") ==
        "<mpadded lspace=\"0.3em\" width=\"+0.6em\" height=\"+0.3em\" depth=\"+0.3em\" mathbackground=\"yellow\"><mte"
        "xt>hi</mtext></mpadded><mpadded lspace=\"0.3em\" width=\"+0.6em\" height=\"+0.3em\" depth=\"+0.3em\" mathbac"
        "kground=\"white\" style=\"border: 0.04em solid red\"><mtext>x</mtext></mpadded><menclose notation=\"box\"><m"
        "padded lspace=\"0.3em\" width=\"+0.6em\" height=\"+0.3em\" depth=\"+0.3em\"><mtext>t</mtext></mpadded></menc"
        "lose>");
    CHECK(mathml("\\boxed{x}") ==
        "<menclose notation=\"box\"><mpadded lspace=\"0.3em\" width=\"+0.6em\" height=\"+0.3em\" depth=\"+0.3em\"><ms"
        "tyle displaystyle=\"true\" scriptlevel=\"0\"><mi>x</mi></mstyle></mpadded></menclose>");
    CHECK(mathml("\\textcolor{nope!}{x}") == "<merror><mtext>\\textcolor{nope!}{x}</mtext></merror>");
}

TEST_CASE("phantoms, smashes and laps") {
    CHECK(mathml("\\phantom{x} \\hphantom{y} \\vphantom{z}") ==
        "<mphantom><mi>x</mi></mphantom><mpadded height=\"0\" depth=\"0\"><mphantom><mi>y</mi></mphantom></mpadded><"
        "mpadded width=\"0\"><mphantom><mi>z</mi></mphantom></mpadded>");
    CHECK(mathml("\\smash{w} \\smash[b]{v}") ==
        "<mpadded height=\"0\" depth=\"0\"><mi>w</mi></mpadded><mpadded depth=\"0\"><mi>v</mi></mpadded>");
    CHECK(mathml("\\mathrlap{a} \\mathllap{b} \\mathclap{c}") ==
        "<mpadded width=\"0\"><mi>a</mi></mpadded><mpadded width=\"0\" lspace=\"-1width\"><mi>b</mi></mpadded><mpadde"
        "d width=\"0\" lspace=\"-0.5width\"><mi>c</mi></mpadded>");
    CHECK(mathml("\\rule{1em}{2pt}") ==
        "<mspace width=\"1em\" height=\"0.2em\" mathbackground=\"currentcolor\"></mspace>");
    CHECK(mathml("\\strut \\mathstrut") ==
        "<mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mpadded width=\"0\"><mphantom><mo stretchy=\"false\">("
        "</mo></mphantom></mpadded>");
}

TEST_CASE("cancellations are menclose") {
    CHECK(mathml("\\cancel{x} \\bcancel{y} \\xcancel{z} \\sout{w}") ==
        "<menclose notation=\"updiagonalstrike\"><mi>x</mi></menclose><menclose notation=\"downdiagonalstrike\"><mi>"
        "y</mi></menclose><menclose notation=\"updiagonalstrike downdiagonalstrike\"><mi>z</mi></menclose><menclose "
        "notation=\"horizontalstrike\"><mi>w</mi></menclose>");
    CHECK(mathml("\\enclose{circle}{x}") == "<menclose notation=\"circle\"><mi>x</mi></menclose>");
}

TEST_CASE("\\not crosses out a relation") {
    CHECK(mathml("a \\not= b \\not\\in c \\not< d") ==
        "<mi>a</mi><mo>≠</mo><mi>b</mi><mo>∉</mo><mi>c</mi><mo>≮</mo><mi>d</mi>");
    CHECK(mathml("\\not\\equiv \\not\\approx \\not\\models") ==
        "<mo lspace=\"0em\" rspace=\"0em\">≢</mo><mo lspace=\"0em\" rspace=\"0em\">≉</mo><mo lspace=\"0em\" rspace="
        "\"0em\">⊭</mo>");
    CHECK(mathml("\\not\\heartsuit") == "<mi>♡̸</mi>");
}

TEST_CASE("matrices are tables of centered columns") {
    CHECK(mathml("\\begin{matrix} a & b \\\\ c & d \\end{matrix}") ==
        "<mtable columnspacing=\"1em\" rowspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace"
        "><mi>a</mi></mtd><mtd><mi>b</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><m"
        "i>c</mi></mtd><mtd><mi>d</mi></mtd></mtr></mtable>");
    CHECK(mathml("\\begin{pmatrix} 1 \\end{pmatrix} \\begin{bmatrix} 1 \\end{bmatrix}") ==
        "<mrow><mo>(</mo><mtable columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>"
        "1</mn></mtd></mtr></mtable><mo>)</mo></mrow><mspace width=\"0.1667em\"></mspace><mrow><mo>[</mo><mtable column"
        "spacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mtd></mtr></mtable><mo"
        ">]</mo></mrow>");
    CHECK(mathml("\\begin{Bmatrix} 1 \\end{Bmatrix} \\begin{vmatrix} 1 \\end{vmatrix} "
        "\\begin{Vmatrix} 1 \\end{Vmatrix}") ==
        "<mrow><mo>{</mo><mtable columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>"
        "1</mn></mtd></mtr></mtable><mo>}</mo></mrow><mspace width=\"0.1667em\"></mspace><mrow><mo>|</mo><mtable column"
        "spacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mtd></mtr></mtable><mo"
        ">|</mo></mrow><mspace width=\"0.1667em\"></mspace><mrow><mo>‖</mo><mtable columnspacing=\"0\"><mtr><mtd><mspac"
        "e height=\"0.84em\" depth=\"0.36em\"></mspace><mn>1</mn></mtd></mtr></mtable><mo>‖</mo></mrow>");
    CHECK(mathml("\\begin{matrix*}[r] 1 & 20 \\end{matrix*}") ==
        "<mtable columnalign=\"right\" columnspacing=\"1em\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></m"
        "space><mn>1</mn></mtd><mtd><mn>20</mn></mtd></mtr></mtable>");
    CHECK(mathml("\\begin{smallmatrix} a & b \\end{smallmatrix}") ==
        "<mtable displaystyle=\"false\" scriptlevel=\"1\" columnspacing=\"0.2778em\"><mtr><mtd><mspace height=\"0.85e"
        "m\" depth=\"0.35em\"></mspace><mi>a</mi></mtd><mtd><mi>b</mi></mtd></mtr></mtable>");
}

TEST_CASE("cases are tables of left aligned columns in a brace") {
    CHECK(mathml("\\begin{cases} 1 & x > 0 \\\\ 0 & \\text{otherwise} \\end{cases}") ==
        "<mrow><mo>{</mo><mtable columnalign=\"left\" columnspacing=\"1em\" rowspacing=\"0\"><mtr><mtd><mspace height"
        "=\"1.008em\" depth=\"0.432em\"></mspace><mn>1</mn></mtd><mtd><mrow><mi>x</mi><mo>&gt;</mo><mn>0</mn></mrow><"
        "/mtd></mtr><mtr><mtd><mspace height=\"1.008em\" depth=\"0.432em\"></mspace><mn>0</mn></mtd><mtd><mtext>other"
        "wise</mtext></mtd></mtr></mtable><mspace width=\"0.12em\"></mspace></mrow>");
    CHECK(mathml("\\begin{dcases} \\frac{1}{2} & x \\end{dcases}") ==
        "<mrow><mo>{</mo><mtable displaystyle=\"true\" columnalign=\"left\" columnspacing=\"1em\"><mtr><mtd><mspace h"
        "eight=\"1.008em\" depth=\"0.432em\"></mspace><mfrac><mn>1</mn><mn>2</mn></mfrac></mtd><mtd><mi>x</mi></mtd><"
        "/mtr></mtable><mspace width=\"0.12em\"></mspace></mrow>");
    CHECK(mathml("\\begin{rcases} a \\end{rcases}") ==
        "<mrow><mspace width=\"0.12em\"></mspace><mtable columnalign=\"left\" columnspacing=\"0\"><mtr><mtd><mspace hei"
        "ght=\"1.008em\" depth=\"0.432em\"></mspace><mi>a</mi></mtd></mtr></mtable><mo>}</mo></mrow>");
}

TEST_CASE("arrays follow their column specifications") {
    CHECK(mathml("\\begin{array}{lcr} a & b & c \\end{array}") ==
        "<mpadded lspace=\"0.5em\" width=\"+1em\"><mtable columnalign=\"left center right\" columnspacing=\"1em\"><mt"
        "r><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mi>b</mi></mtd><mtd><mi>c</"
        "mi></mtd></mtr></mtable></mpadded>");
    CHECK(mathml("\\begin{array}{|l|c|} \\hline a & b \\\\ \\hline c & d \\\\ \\hline \\end{array}") ==
        "<mtable columnalign=\"left center\" columnspacing=\"1em\" rowspacing=\"0\" rowlines=\"solid\" columnlines=\""
        "solid\" frame=\"solid\" framespacing=\"0.5em 0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspa"
        "ce><mi>a</mi></mtd><mtd><mi>b</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace>"
        "<mi>c</mi></mtd><mtd><mi>d</mi></mtd></mtr></mtable>");
    CHECK(mathml("\\begin{array}{c:c} a & b \\\\ \\hdashline c & d \\end{array}") ==
        "<mpadded lspace=\"0.5em\" width=\"+1em\"><mtable columnspacing=\"1em\" rowspacing=\"0\" rowlines=\"dashed\" "
        "columnlines=\"dashed\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><m"
        "i>b</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>c</mi></mtd><mtd><mi>d"
        "</mi></mtd></mtr></mtable></mpadded>");
    CHECK(mathml("\\begin{array}{@{}l@{}} a \\end{array}") ==
        "<mtable columnalign=\"left\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace"
        "><mi>a</mi></mtd></mtr></mtable>");
    CHECK(mathml("\\begin{array}{*{2}{c}} a & b \\end{array}") ==
        "<mpadded lspace=\"0.5em\" width=\"+1em\"><mtable columnspacing=\"1em\"><mtr><mtd><mspace height=\"0.84em\" d"
        "epth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mi>b</mi></mtd></mtr></mtable></mpadded>");
    // The rules of each repetition carry across into the next, and before the first.
    CHECK(mathml("\\begin{array}{*{2}{c|}c} a & b & c \\end{array}") ==
        mathml("\\begin{array}{c|c|c} a & b & c \\end{array}"));
    CHECK(mathml("\\begin{array}{c|*{2}{c}} a & b & c \\end{array}") ==
        mathml("\\begin{array}{c|cc} a & b & c \\end{array}"));
    CHECK(mathml("\\begin{array}{c*{2}{@{}c}} a & b & c \\end{array}") ==
        mathml("\\begin{array}{c@{}c@{}c} a & b & c \\end{array}"));
    CHECK(mathml("\\begin{array}{c} a \\\\[2pt] b \\end{array}") ==
        "<mpadded lspace=\"0.5em\" width=\"+1em\"><mtable rowspacing=\"0.2em\"><mtr><mtd><mspace height=\"0.84em\" de"
        "pth=\"0.36em\"></mspace><mi>a</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace>"
        "<mi>b</mi></mtd></mtr></mtable></mpadded>");
    CHECK(mathml("\\sum_{\\substack{i < n \\\\ j}} x") ==
        "<munder><mo lspace=\"0em\">∑</mo><mtable displaystyle=\"false\" scriptlevel=\"1\" rowspacing=\"0\"><mtr><mtd"
        "><mrow><mi>i</mi><mo lspace=\"0em\" rspace=\"0em\">&lt;</mo><mi>n</mi></mrow></mtd></mtr><mtr><mtd><mi>j</mi"
        "></mtd></mtr></mtable></munder><mi>x</mi>");
    CHECK(mathml("\\begin{subarray}{l} a \\\\ b \\end{subarray}") ==
        "<mtable displaystyle=\"false\" scriptlevel=\"1\" columnalign=\"left\" rowspacing=\"0\"><mtr><mtd><mi>a</mi><"
        "/mtd></mtr><mtr><mtd><mi>b</mi></mtd></mtr></mtable>");
}

TEST_CASE("a column specification makes at most 1024 columns, however its repetitions nest") {
    String most = mathml("\\begin{array}{*{1024}{c}} a \\end{array}");
    CHECK(mathml("\\begin{array}{*{2000}{c}} a \\end{array}") == most);
    CHECK(mathml("\\begin{array}{*{99999999999999999999}{c}} a \\end{array}") == most);
    CHECK(mathml("\\begin{array}{*{64}{*{64}{*{64}{*{64}{*{64}{c}}}}}} a \\end{array}") == most);
    // Repetitions that make no columns stop at once.
    CHECK(mathml("\\begin{array}{*{1000}{*{1000}{*{1000}{*{1000}{|}}}}c} a \\end{array}") ==
        mathml("\\begin{array}{|c} a \\end{array}"));
    // Repetitions nested past the parser's depth make nothing.
    String deep("\\begin{array}{c");
    for (int i = 0; i < 10000; ++i)
        deep.append("*{1}{");
    deep.append("c");
    deep.append(10000, '}');
    deep.append("} a \\end{array}");
    CHECK(mathml(deep) == mathml("\\begin{array}{c} a \\end{array}"));
}

TEST_CASE("alignments set right and left columns in turn, \\jot apart") {
    CHECK(display("\\begin{aligned} a &= b + c \\\\ &= d \\end{aligned}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left\" columnspacing=\"0\" rowspacing=\"0.3em\"><mtr><mtd>"
        "<mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi><mo>+</mo"
        "><mi>c</mi></mrow></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace></mtd><mtd><mrow"
        "><mo>=</mo><mi>d</mi></mrow></mtd></mtr></mtable>");
    CHECK(display("\\begin{align} a &= b & c &= d \\end{align}") ==
        "<mtable displaystyle=\"true\" columnalign=\"center right left center right left center\" columnspacing=\"0 0 0"
        " 2em 0 0\" columnwidth=\"fit auto auto fit auto auto fit\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext><"
        "/mtd><mtd></mtd><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo>"
        "<mi>b</mi></mrow></mtd><mtd></mtd><mtd><mi>c</mi></mtd><mtd><mrow><mo>=</mo><mi>d</mi></mrow></mtd><mtd></mtd>"
        "</mlabeledtr></mtable>");
    CHECK(display("\\begin{alignat}{2} a &= b & c &= d \\end{alignat}") ==
        "<mtable displaystyle=\"true\" columnalign=\"center right left right left center\" columnspacing=\"0\" columnwi"
        "dth=\"fit auto auto auto auto fit\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><mtd></mtd><mtd><m"
        "space height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd>"
        "<mtd><mi>c</mi></mtd><mtd><mrow><mo>=</mo><mi>d</mi></mrow></mtd><mtd></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{gather} a \\\\ b \\end{gather}") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><"
        "mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd></mlabeledtr><mlabeledtr><mtd><mtext"
        ">(2)</mtext></mtd><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>b</mi></mtd></mlabeledtr></mt"
        "able>");
    CHECK(display("\\begin{split} a &= b \\end{split}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84"
        "em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd></mtr></mtable>");
    CHECK(display("\\begin{multline} a \\\\ b \\\\ c \\end{multline}") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\" width=\"100%\"><mtr columnalign=\"left\"><mtd><mspace hei"
        "ght=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0"
        ".36em\"></mspace><mi>b</mi></mtd></mtr><mlabeledtr columnalign=\"right\"><mtd><mtext>(1)</mtext></mtd><mtd><"
        "mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>c</mi></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{equation} E = mc^2 \\end{equation}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><mtd"
        "><mrow><mi>E</mi><mo>=</mo><mi>m</mi><msup><mi>c</mi><mn>2</mn></msup></mrow></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{aligned} a &+ b \\end{aligned}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84"
        "em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo lspace=\"0.2222em\" rspace=\"0.2222em\">+</mo>"
        "<mi>b</mi></mrow></mtd></mtr></mtable>");
}

TEST_CASE("tags sit at the right margin of display math") {
    CHECK(display("E = mc^2 \\tag{1}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><mtd"
        "><mrow><mi>E</mi><mo>=</mo><mi>m</mi><msup><mi>c</mi><mn>2</mn></msup></mrow></mtd></mlabeledtr></mtable>");
    CHECK(display("x \\tag*{A}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>A</mtext></mtd><mtd><"
        "mi>x</mi></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{align} a &= b \\tag{2} \\\\ c &= d \\notag \\end{align}") ==
        "<mtable displaystyle=\"true\" columnalign=\"center right left center\" columnspacing=\"0\" rowspacing=\"0.3em"
        "\" columnwidth=\"fit auto auto fit\" width=\"100%\"><mlabeledtr><mtd><mtext>(2)</mtext></mtd><mtd></mtd><mtd><"
        "mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd"
        "><mtd></mtd></mlabeledtr><mtr><mtd></mtd><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>c</mi></"
        "mtd><mtd><mrow><mo>=</mo><mi>d</mi></mrow></mtd><mtd></mtd></mtr></mtable>");
    CHECK(mathml("x \\tag{1}") == "<mi>x</mi><merror><mtext>\\tag{1}</mtext></merror>");
}

TEST_CASE("environments number their equations, which \\notag and \\nonumber exempt and \\tag replaces") {
    CHECK(display("\\begin{equation} a \\end{equation} \\begin{equation*} b \\end{equation*}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><mtd"
        "><mi>a</mi></mtd></mlabeledtr></mtable><mi>b</mi>");
    CHECK(display("\\begin{gather} a \\notag \\\\ b \\nonumber \\\\ c \\tag*{$\\ast$} \\end{gather}") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\" width=\"100%\"><mtr><mtd><mspace height=\"0.84em\" depth="
        "\"0.36em\"></mspace><mi>a</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>"
        "b</mi></mtd></mtr><mlabeledtr><mtd><mrow><mo lspace=\"0em\" rspace=\"0em\">∗</mo></mrow></mtd><mtd><mspace h"
        "eight=\"0.84em\" depth=\"0.36em\"></mspace><mi>c</mi></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{multline} a \\\\ b \\notag \\end{multline}") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\"><mtr columnalign=\"left\"><mtd><mspace height=\"0.84em\" "
        "depth=\"0.36em\"></mspace><mi>a</mi></mtd></mtr><mtr columnalign=\"right\"><mtd><mspace height=\"0.84em\" de"
        "pth=\"0.36em\"></mspace><mi>b</mi></mtd></mtr></mtable>");
    CHECK(display("\\begin{multline} a \\tag{M} \\\\ b \\end{multline}") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\" width=\"100%\"><mtr columnalign=\"left\"><mtd><mspace hei"
        "ght=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd></mtr><mlabeledtr columnalign=\"right\"><mtd><mtext"
        ">(M)</mtext></mtd><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>b</mi></mtd></mlabeledtr></mt"
        "able>");
    CHECK(display("\\begin{align*} a &= b \\tag{T} \\end{align*}") ==
        "<mtable displaystyle=\"true\" columnalign=\"center right left center\" columnspacing=\"0\" columnwidth=\"fit a"
        "uto auto fit\" width=\"100%\"><mlabeledtr><mtd><mtext>(T)</mtext></mtd><mtd></mtd><mtd><mspace height=\"0.84em"
        "\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd><mtd></mtd></mlabeled"
        "tr></mtable>");
    // Inline math numbers nothing.
    CHECK(mathml("\\begin{align} a &= b \\end{align}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84"
        "em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd></mtr></mtable>");
}

TEST_CASE("\\tag is an error where no equation is numbered") {
    CHECK(mathml("x \\tag{1} \\tag*{2}") ==
        "<mi>x</mi><merror><mtext>\\tag{1}</mtext></merror><merror><mtext>\\tag*{2}</mtext></merror>");
    CHECK(display("\\begin{aligned} a \\tag{1} \\end{aligned}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" de"
        "pth=\"0.36em\"></mspace><mrow><mi>a</mi><merror><mtext>\\tag{1}</mtext></merror></mrow></mtd></mtr></mtable>");
}

TEST_CASE("labels name the tags of equations for \\ref and \\eqref") {
    CHECK(display("\\begin{equation} a \\tag{E} \\label{x} \\end{equation} \\ref{x} \\eqref{x}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>(E)</mtext></mtd><mtd"
        "><mi>a</mi></mtd></mlabeledtr></mtable><mtext>E</mtext><mtext>(E)</mtext>");
    // A label later in the formula is known to a reference before it, and one in an environment inside an equation
    // names the equation.
    CHECK(display("\\eqref{later} \\begin{equation} \\begin{split} a &= b \\end{split} \\label{later} \\end{equation}") ==
        "<mtext>(1)</mtext><mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)"
        "</mtext></mtd><mtd><mtable displaystyle=\"true\" columnalign=\"right left\" columnspacing=\"0\"><mtr><mtd><msp"
        "ace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd></"
        "mtr></mtable></mtd></mlabeledtr></mtable>");
    // A label that no equation defines is LaTeX's.
    CHECK(display("\\ref{missing} \\eqref{missing}") == "<mtext>?\?</mtext><mtext>(?\?)</mtext>");
}

TEST_CASE("a document numbers its formulas' equations in order, and its labels reach every later formula") {
    PrTexDocument* document = prTexDocumentCreate();
    PrTexOptions options = prTexOptionsDefault();
    options.annotate = false;
    options.display = PrTexDisplay_Block;
    options.document = document;
    CHECK(markup("\\begin{equation} a \\label{first} \\end{equation}", options) ==
        "<math display=\"block\"><mtable displaystyle=\"true\" columnspacing=\"0\" width=\"100%\"><mlabeledtr><mtd><mte"
        "xt>(1)</mtext></mtd><mtd><mi>a</mi></mtd></mlabeledtr></mtable></math>");
    String second = markup("\\begin{gather} b \\\\ c \\label{third} \\end{gather}", options);
    CHECK(prStringContains(second, "<mtext>(2)</mtext>"));
    CHECK(prStringContains(second, "<mtext>(3)</mtext>"));
    CHECK(prTexDocumentEquationCount(document) == 3);
    CHECK(prTexDocumentLabel(document, "first") == "1");
    CHECK(prTexDocumentLabel(document, "third") == "3");
    CHECK(prTexDocumentLabel(document, "none").data == nullptr);
    options.display = PrTexDisplay_Inline;
    CHECK(markup("\\eqref{first} \\ref{third}", options) ==
        "<math display=\"inline\"><mtext>(1)</mtext><mtext>3</mtext></math>");
    prTexDocumentClear(document);
    CHECK(prTexDocumentEquationCount(document) == 0);
    CHECK(prTexDocumentLabel(document, "first").data == nullptr);
    prTexDocumentDestroy(document);
    // A formula without a document is a document of its own.
    options.document = nullptr;
    options.display = PrTexDisplay_Block;
    String alone = markup("\\begin{equation} a \\end{equation}", options);
    CHECK(alone == markup("\\begin{equation} a \\end{equation}", options));
    CHECK(prStringContains(alone, "<mtext>(1)</mtext>"));
}

TEST_CASE("flalign spreads its pairs of columns across the line, flush with its edges") {
    CHECK(display("\\begin{flalign} a &= b & c &= d \\end{flalign}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left center right left\" columnspacing=\"0\" columnwidth="
        "\"auto auto fit auto auto\" width=\"100%\"><mlabeledtr><mtd><mtext>(1)</mtext></mtd><mtd><mspace height=\"0."
        "84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mrow><mo>=</mo><mi>b</mi></mrow></mtd><mtd></mtd><mtd"
        "><mi>c</mi></mtd><mtd><mrow><mo>=</mo><mi>d</mi></mrow></mtd></mlabeledtr></mtable>");
    CHECK(display("\\begin{flalign*} a &= b \\end{flalign*}") ==
        "<mtable displaystyle=\"true\" columnalign=\"right left center\" columnspacing=\"0\" columnwidth=\"auto auto "
        "fit\" width=\"100%\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>a</mi></mtd><mtd><mro"
        "w><mo>=</mo><mi>b</mi></mrow></mtd><mtd></mtd></mtr></mtable>");
}

TEST_CASE("commutative diagrams set objects and arrows as the cells of a table") {
    CHECK(display("\\begin{CD} A @>f>> B \\\\ @VgVV @VVhV \\\\ C @<<k< D \\end{CD}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" rowspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth"
        "=\"0.36em\"></mspace><mi>A</mi></mtd><mtd><mover><mo lspace=\"0em\" rspace=\"0em\">→</mo><mpadded lspace=\"0"
        ".2778em\" width=\"+0.5556em\"><mi>f</mi></mpadded></mover></mtd><mtd><mi>B</mi></mtd></mtr><mtr><mtd><mspace"
        " height=\"0.84em\" depth=\"0.36em\"></mspace><mrow><mpadded width=\"0\" lspace=\"-1width\"><mi>g</mi><mspace"
        " width=\"0.1667em\"></mspace></mpadded><mo lspace=\"0em\" rspace=\"0em\" symmetric=\"true\" minsize=\"2.4em"
        "\" maxsize=\"2.4em\">↓</mo></mrow></mtd><mtd></mtd><mtd><mrow><mo lspace=\"0em\" rspace=\"0em\" symmetric=\""
        "true\" minsize=\"2.4em\" maxsize=\"2.4em\">↓</mo><mpadded width=\"0\"><mspace width=\"0.1667em\"></mspace><m"
        "i>h</mi></mpadded></mrow></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>C</mi"
        "></mtd><mtd><munderover><mo lspace=\"0em\" rspace=\"0em\">←</mo><mpadded lspace=\"0.2778em\" width=\"+0.5556"
        "em\"><mi>k</mi></mpadded><mspace width=\"3em\"></mspace></munderover></mtd><mtd><mi>D</mi></mtd></mtr></mtab"
        "le>");
    CHECK(display("\\begin{CD} A @= B @. C \\\\ @| @. @AAA \\end{CD}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\" rowspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth"
        "=\"0.36em\"></mspace><mi>A</mi></mtd><mtd><mover><mo lspace=\"0em\" rspace=\"0em\" stretchy=\"true\">=</mo><"
        "mspace width=\"3em\"></mspace></mover></mtd><mtd><mi>B</mi></mtd><mtd></mtd><mtd><mi>C</mi></mtd></mtr><mtr>"
        "<mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mo minsize=\"2.4em\" maxsize=\"2.4em\">‖</mo></mtd"
        "><mtd></mtd><mtd></mtd><mtd></mtd><mtd><mo lspace=\"0em\" rspace=\"0em\" symmetric=\"true\" minsize=\"2.4em"
        "\" maxsize=\"2.4em\">↑</mo></mtd></mtr></mtable>");
    CHECK(mathml("\\begin{CD} A @X B \\end{CD}") ==
        "<mtable displaystyle=\"true\" columnspacing=\"0\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></msp"
        "ace><mi>A</mi></mtd><mtd><merror><mtext>@</mtext></merror></mtd><mtd><mrow><mi>X</mi><mi>B</mi></mrow></mtd>"
        "</mtr></mtable>");
}
TEST_CASE("lines of a formula are the rows of a gather") {
    CHECK(display("a \\\\ b") ==
        "<mtable displaystyle=\"true\" rowspacing=\"0.3em\"><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></ms"
        "pace><mi>a</mi></mtd></mtr><mtr><mtd><mspace height=\"0.84em\" depth=\"0.36em\"></mspace><mi>b</mi></mtd></m"
        "tr></mtable>");
}

TEST_CASE("macros expand with their arguments") {
    CHECK(mathml("\\newcommand{\\R}{\\mathbb{R}} \\R^n") == "<msup><mi>ℝ</mi><mi>n</mi></msup>");
    CHECK(mathml("\\newcommand\\pair[2]{(#1, #2)} \\pair{a}{b}") ==
        "<mo stretchy=\"false\">(</mo><mi>a</mi><mo>,</mo><mi>b</mi><mo stretchy=\"false\">)</mo>");
    CHECK(mathml("\\newcommand{\\opt}[2][x]{#1+#2} \\opt{y} \\opt[z]{y}") ==
        "<mi>x</mi><mo>+</mo><mi>y</mi><mi>z</mi><mo>+</mo><mi>y</mi>");
    CHECK(mathml("\\def\\sq#1{#1^2} \\sq{x} \\sq y") ==
        "<msup><mi>x</mi><mn>2</mn></msup><msup><mi>y</mi><mn>2</mn></msup>");
    CHECK(mathml("\\let\\g\\alpha \\g \\let\\h=+ a\\h b") == "<mi>α</mi><mi>a</mi><mo>+</mo><mi>b</mi>");
    CHECK(mathml("\\renewcommand{\\sin}{S} \\sin") == "<mi>S</mi>");
    CHECK(mathml("\\providecommand{\\alpha}{A} \\alpha") == "<mi>α</mi>");
    CHECK(mathml("\\def\\loop{\\loop} \\loop") == "<merror><mtext>\\loop</mtext></merror>");
    // Each expansion doubles its argument, which the limit on the tokens that expansions produce stops.
    CHECK(mathml("\\def\\a#1{\\a{#1#1}} \\a{x}") == "<merror><mtext>\\a{x}</mtext></merror>");
    CHECK(mathml("\\iff \\implies") ==
        "<mspace width=\"0.2778em\"></mspace><mo lspace=\"0em\" rspace=\"0em\">⟺</mo><mspace width=\"0.2778em\"></ms"
        "pace><mspace width=\"0.2778em\"></mspace><mo lspace=\"0em\" rspace=\"0em\">⟹</mo><mspace width=\"0.2778em\""
        "></mspace>");
}

TEST_CASE("\\def matches its delimited parameters") {
    CHECK(mathml("\\def\\a#1.#2\\end{#1+#2} \\a x.y\\end") == "<mi>x</mi><mo>+</mo><mi>y</mi>");
    CHECK(mathml("\\def\\b#1#2.{[#1#2]} \\b xyz.") ==
        "<mo stretchy=\"false\">[</mo><mi>x</mi><mi>y</mi><mi>z</mi><mo stretchy=\"false\">]</mo>");
    // Text before the first parameter must follow the name.
    CHECK(mathml("\\def\\c(#1){f_{#1}} \\c(n) \\c x") ==
        "<msub><mi>f</mi><mi>n</mi></msub><merror><mtext>\\c</mtext></merror><mi>x</mi>");
    // A delimited argument that is one group loses its braces, which hide the delimiter.
    CHECK(mathml("\\def\\d#1;{<#1>} \\d {a;b};") ==
        "<mo lspace=\"0em\">&lt;</mo><mi>a</mi><mo>;</mo><mi>b</mi><mo rspace=\"0em\">&gt;</mo>");
    CHECK(mathml("\\def\\e #1 {(#1)} \\e ab c") ==
        "<mo stretchy=\"false\">(</mo><mi>a</mi><mi>b</mi><mo stretchy=\"false\">)</mo><mi>c</mi>");
}
TEST_CASE("a document shares definitions between formulas, and global definitions add to it") {
    PrTexDocument* document = prTexDocumentCreate();
    prTexDocumentDefineMacro(document, "RR", 0, "\\mathbb{R}");
    prTexDocumentDefineMacro(document, "norm", 1, "\\lVert #1 \\rVert");
    PrTexOptions options = prTexOptionsDefault();
    options.annotate = false;
    options.document = document;
    CHECK(markup("\\RR \\norm{v}", options) ==
        "<math display=\"inline\"><mi>ℝ</mi><mo stretchy=\"false\">‖</mo><mi>v</mi><mo stretchy=\"false\">‖</mo></ma"
        "th>");
    CHECK(markup("\\gdef\\one{1} \\newcommand{\\two}{2}", options) == "<math display=\"inline\"></math>");
    CHECK(prTexDocumentMacroCount(document) == 3);
    CHECK(markup("\\one \\two", options) ==
        "<math display=\"inline\"><mn>1</mn><merror><mtext>\\two</mtext></merror></math>");
    prTexDocumentClear(document);
    CHECK(prTexDocumentMacroCount(document) == 0);
    CHECK(markup("\\RR", options) == "<math display=\"inline\"><merror><mtext>\\RR</mtext></merror></math>");
    prTexDocumentDestroy(document);
}

TEST_CASE("links, characters and attributes") {
    CHECK(mathml("\\href{https://x.org}{x} \\href{javascript:alert(1)}{y}") ==
        "<mrow href=\"https://x.org\"><mi>x</mi></mrow><mrow><mi>y</mi></mrow>");
    CHECK(mathml("\\url{https://x.org}") ==
        "<mtext href=\"https://x.org\" style=\"font-family: monospace\">https://x.org</mtext>");
    CHECK(mathml("\\unicode{x221E} \\unicode{8734} \\char\"41 \\char`B \\char67") ==
        "<mi>∞</mi><mi>∞</mi><mi>A</mi><mi>B</mi><mi>C</mi>");
    CHECK(mathml("\\class{c}{x} \\cssId{i}{y} \\style{color: red}{z}") ==
        "<mrow class=\"c\"><mi>x</mi></mrow><mrow id=\"i\"><mi>y</mi></mrow><mrow style=\"color: red\"><mi>z</mi></m"
        "row>");
}

TEST_CASE("sidesets and mathchoice") {
    CHECK(mathml("\\sideset{_a^b}{'}\\sum") ==
        "<mmultiscripts><mo lspace=\"0em\" rspace=\"0em\">∑</mo><mrow></mrow><mo>′</mo><mprescripts></mprescripts><m"
        "i>a</mi><mi>b</mi></mmultiscripts>");
    CHECK(mathml("\\mathchoice{D}{T}{S}{SS} x^{\\mathchoice{D}{T}{S}{SS}}") ==
        "<mi>T</mi><msup><mi>x</mi><mi>S</mi></msup>");
    CHECK(display("\\mathchoice{D}{T}{S}{SS}") == "<mi>D</mi>");
}

TEST_CASE("errors hold their source, and the rest of the formula is set around them") {
    CHECK(mathml("\\foo + 1") ==
        "<merror><mtext>\\foo</mtext></merror><mo lspace=\"0.2222em\" rspace=\"0.2222em\">+</mo><mn>1</mn>");
    CHECK(mathml("a } b") == "<mi>a</mi><merror><mtext>}</mtext></merror><mi>b</mi>");
    CHECK(mathml("a & b") == "<mi>a</mi><merror><mtext>&amp;</mtext></merror><mi>b</mi>");
    CHECK(mathml("\\begin{foo} x \\end{foo} y") ==
        "<merror><mtext>\\begin{foo} x \\end{foo}</mtext></merror><mi>y</mi>");
    CHECK(mathml("\\end{foo}") == "<merror><mtext>\\end{foo}</mtext></merror>");
    CHECK(mathml("\\sum\\limits") == "<mo lspace=\"0em\" rspace=\"0em\" movablelimits=\"false\">∑</mo>");
    CHECK(mathml("x\\limits") == "<mi>x</mi><merror><mtext>\\limits</mtext></merror>");
    CHECK(mathml("\\sqrt") == "<msqrt><merror><mtext>\\sqrt</mtext></merror></msqrt>");
    CHECK(mathml("{{{{x") == "<mi>x</mi>");
    CHECK(mathml("\\'e") == "<merror><mtext>\\'</mtext></merror><mi>e</mi>");
}

TEST_CASE("deep nesting is an error, not a crash") {
    String deep;
    deep.append(500, '{');
    String out = mathml(deep);
    CHECK(prStringContains(out, "<merror>"));
}

TEST_CASE("control sequences and symbols are looked up by name and by character") {
    REQUIRE(texEntryFromName("alpha") != nullptr);
    CHECK(PrStringView(texEntryFromName("alpha")->text) == "α");
    CHECK(texEntryFromName("nosuchcommand") == nullptr);
    CHECK(texEntryFromName("") == nullptr);
    REQUIRE(texEntryFromCodePoint(0x2264) != nullptr);
    CHECK(PrStringView(texEntryFromCodePoint(0x2264)->name) == "leq");
    CHECK(texEntryFromCodePoint('x') == nullptr);
}

TEST_CASE("fonts map to the alphabets of the Mathematical Alphanumeric Symbols, holes included") {
    CHECK(texFontMap('A', TexFont_Bold) == 0x1D400);
    CHECK(texFontMap('h', TexFont_Italic) == 0x210E);
    CHECK(texFontMap('R', TexFont_DoubleStruck) == 0x211D);
    CHECK(texFontMap('1', TexFont_DoubleStruck) == 0x1D7D9);
    CHECK(texFontMap(0x3B1, TexFont_Bold) == 0x1D6C2);
    CHECK(texFontMap(0x2202, TexFont_BoldItalic) == 0x1D74F);
    CHECK(texFontMap(0x3B1, TexFont_Script) == 0x3B1);
    CHECK(texFontMap('+', TexFont_Bold) == '+');
    // Unicode's variation sequences choose chancery or roundhand script capitals, the holes among them included.
    CHECK(texFontVariation(0x1D49C, TexFont_Calligraphic) == 0);
    CHECK(texFontVariation(0x212C, TexFont_Script) == 0xFE01);
    CHECK(texFontVariation(0x1D4D0, TexFont_BoldCalligraphic) == 0);
    CHECK(texFontVariation(0x1D4B6, TexFont_Script) == 0);
    CHECK(texFontVariation(0x210E, TexFont_Calligraphic) == 0);
    CHECK(texFontVariation(0x1D400, TexFont_Bold) == 0);
}

TEST_CASE("the operator dictionary gives MathML Core's properties, with its fallback between forms") {
    TexCategoryValues plus = texOperatorDictionary("+", TexForm_Infix, false);
    CHECK((plus.lspace == 4 && plus.rspace == 4 && plus.flags == 0));
    TexCategoryValues paren = texOperatorDictionary("(", TexForm_Infix, false);
    CHECK((paren.lspace == 0 && paren.flags == (TexOperatorFlag_Stretchy | TexOperatorFlag_Symmetric)));
    CHECK(texOperatorDictionary("(", TexForm_Infix, true).flags == 0);
    TexCategoryValues bar = texOperatorDictionary("|", TexForm_Infix, false);
    CHECK((bar.lspace == 5 && bar.flags == 0));
    TexCategoryValues sum = texOperatorDictionary("∑", TexForm_Prefix, false);
    CHECK((sum.lspace == 3 &&
        sum.flags == (TexOperatorFlag_Symmetric | TexOperatorFlag_LargeOp | TexOperatorFlag_MovableLimits)));
    TexCategoryValues integral = texOperatorDictionary("∫", TexForm_Infix, false);
    CHECK(integral.flags == (TexOperatorFlag_Symmetric | TexOperatorFlag_LargeOp));
    TexCategoryValues name = texOperatorDictionary("lim", TexForm_Prefix, false);
    CHECK((name.lspace == 5 && name.flags == 0));
    CHECK(texOperatorDictionary("=\xcc\xb8", TexForm_Infix, false).lspace == 5);
    CHECK(texOperatorDictionary(",", TexForm_Infix, false).rspace == 3);
    // A pair of ASCII characters, which the dictionary lists by its own index.
    CHECK(texOperatorDictionary("<=", TexForm_Infix, false).lspace == 5);
    CHECK(texOperatorDictionary("++", TexForm_Postfix, false).lspace == 0);
    CHECK(texOperatorDictionary("𞻰", TexForm_Postfix, false).flags == TexOperatorFlag_Stretchy);
    CHECK(texIsInlineAxis(0x2192));
    CHECK(!texIsInlineAxis('('));
}

TEST_CASE("every formula of the sample of mathematics converts without an error") {
    // tests/mathematics.md is a sample of real mathematics, for a person to review as it renders. Its math is $...$ and
    // $$...$$, as lib/markdown reads it.
    String file = harness::readText(format("{}/mathematics.md", TEX_TESTS_DIR));
    PrStringView text = file;
    uint32_t formulas = 0;
    for (size_t at = prStringFind(text, "$"); at != text.size; at = prStringFindFrom(text, "$", at)) {
        bool block = prStringStartsWith(prStringFrom(text, at), "$$");
        size_t start = at + (block ? 2 : 1);
        size_t end = prStringFindFrom(text, block ? "$$" : "$", start);
        REQUIRE(end != text.size);
        PrStringView source = prStringSlice(text, start, end - start);
        INFO(source);
        CHECK(!prStringContains(mathml(source, block ? PrTexDisplay_Block : PrTexDisplay_Inline), "<merror"));
        ++formulas;
        at = end + (block ? 2 : 1);
    }
    CHECK(formulas > 50);
}

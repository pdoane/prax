// TeX math's control sequences, its fonts as Unicode's mathematical alphanumerics, and the properties that MathML
// Core's operator dictionary gives an operator.

#include "tex/src/tex_symbols.h"

#include "pr/count_of.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/span.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Entries

constexpr TexEntry letter(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_Letter, TexClass_Ord, 0};
}

constexpr TexEntry upright(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_Upright, TexClass_Ord, 0};
}

constexpr TexEntry op(const char* name, const char* text, TexClass texClass) {
    return TexEntry{name, text, TexEntryKind_Operator, texClass, 0};
}

constexpr TexEntry ord(const char* name, const char* text) {
    return op(name, text, TexClass_Ord);
}

constexpr TexEntry bin(const char* name, const char* text) {
    return op(name, text, TexClass_Bin);
}

constexpr TexEntry rel(const char* name, const char* text) {
    return op(name, text, TexClass_Rel);
}

constexpr TexEntry open(const char* name, const char* text) {
    return op(name, text, TexClass_Open);
}

constexpr TexEntry close(const char* name, const char* text) {
    return op(name, text, TexClass_Close);
}

constexpr TexEntry punct(const char* name, const char* text) {
    return op(name, text, TexClass_Punct);
}

constexpr TexEntry large(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_LargeOperator, TexClass_Op, 0};
}

constexpr TexEntry integral(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_Integral, TexClass_Op, 0};
}

constexpr TexEntry function(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_Function, TexClass_Op, 0};
}

constexpr TexEntry limitFunction(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_LimitFunction, TexClass_Op, 0};
}

constexpr TexEntry accent(const char* name, const char* text, int16_t flags) {
    return TexEntry{name, text, TexEntryKind_Accent, TexClass_Ord, flags};
}

constexpr TexEntry textAccent(const char* name, const char* mark) {
    return TexEntry{name, mark, TexEntryKind_TextAccent, TexClass_Ord, 0};
}

constexpr TexEntry textSymbol(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_TextSymbol, TexClass_Ord, 0};
}

constexpr TexEntry space(const char* name, int16_t mu) {
    return TexEntry{name, nullptr, TexEntryKind_Space, TexClass_None, mu};
}

constexpr TexEntry font(const char* name, TexFont value) {
    return TexEntry{name, nullptr, TexEntryKind_Font, TexClass_Ord, value};
}

constexpr TexEntry fontSwitch(const char* name, TexFont value) {
    return TexEntry{name, nullptr, TexEntryKind_FontSwitch, TexClass_None, value};
}

constexpr TexEntry textFont(const char* name, TexTextFont value) {
    return TexEntry{name, nullptr, TexEntryKind_TextFont, TexClass_Ord, value};
}

constexpr TexEntry style(const char* name, TexStyle value) {
    return TexEntry{name, nullptr, TexEntryKind_Style, TexClass_None, value};
}

constexpr TexEntry size(const char* name, int16_t hundredths) {
    return TexEntry{name, nullptr, TexEntryKind_Size, TexClass_None, hundredths};
}

constexpr TexEntry big(const char* name, int16_t size, TexClass texClass) {
    return TexEntry{name, nullptr, TexEntryKind_BigDelimiter, texClass, int16_t(size | (texClass << 4))};
}

constexpr TexEntry arrow(const char* name, const char* text) {
    return TexEntry{name, text, TexEntryKind_ExtensibleArrow, TexClass_Rel, 0};
}

constexpr TexEntry macro(const char* name, const char* replacement, int16_t parameterCount = 0) {
    return TexEntry{name, replacement, TexEntryKind_Macro, TexClass_None, parameterCount};
}

constexpr TexEntry command(const char* name, TexCommand value) {
    return TexEntry{name, nullptr, TexEntryKind_Command, TexClass_None, int16_t(value)};
}

// Every control sequence. An entry whose text is one character is the symbol that the character stands for when it is
// written as itself, unless an earlier entry has the same text.
constexpr TexEntry kEntries[] = {
    // Greek
    letter("alpha", "α"), letter("beta", "β"), letter("gamma", "γ"), letter("delta", "δ"), letter("epsilon", "ϵ"),
    letter("varepsilon", "ε"), letter("zeta", "ζ"), letter("eta", "η"), letter("theta", "θ"),
    letter("vartheta", "ϑ"), letter("iota", "ι"), letter("kappa", "κ"), letter("varkappa", "ϰ"),
    letter("lambda", "λ"), letter("mu", "μ"), letter("nu", "ν"), letter("xi", "ξ"), letter("omicron", "ο"),
    letter("pi", "π"), letter("varpi", "ϖ"), letter("rho", "ρ"), letter("varrho", "ϱ"), letter("sigma", "σ"),
    letter("varsigma", "ς"), letter("tau", "τ"), letter("upsilon", "υ"), letter("phi", "ϕ"), letter("varphi", "φ"),
    letter("chi", "χ"), letter("psi", "ψ"), letter("omega", "ω"), letter("digamma", "ϝ"),
    upright("Gamma", "Γ"), upright("Delta", "Δ"), upright("Theta", "Θ"), upright("Lambda", "Λ"), upright("Xi", "Ξ"),
    upright("Pi", "Π"), upright("Sigma", "Σ"), upright("Upsilon", "Υ"), upright("Phi", "Φ"), upright("Psi", "Ψ"),
    upright("Omega", "Ω"), upright("Alpha", "Α"), upright("Beta", "Β"), upright("Epsilon", "Ε"), upright("Zeta", "Ζ"),
    upright("Eta", "Η"), upright("Iota", "Ι"), upright("Kappa", "Κ"), upright("Mu", "Μ"), upright("Nu", "Ν"),
    upright("Omicron", "Ο"), upright("Rho", "Ρ"), upright("Tau", "Τ"), upright("Chi", "Χ"),
    letter("varGamma", "Γ"), letter("varDelta", "Δ"), letter("varTheta", "Θ"), letter("varLambda", "Λ"),
    letter("varXi", "Ξ"), letter("varPi", "Π"), letter("varSigma", "Σ"), letter("varUpsilon", "Υ"),
    letter("varPhi", "Φ"), letter("varPsi", "Ψ"), letter("varOmega", "Ω"),

    // Letter-like symbols
    letter("partial", "∂"), letter("infty", "∞"), upright("nabla", "∇"), letter("hbar", "ℏ"), letter("hslash", "ℏ"),
    letter("ell", "ℓ"), letter("wp", "℘"), letter("Re", "ℜ"), letter("Im", "ℑ"), letter("aleph", "ℵ"),
    letter("beth", "ℶ"), letter("gimel", "ℷ"), letter("daleth", "ℸ"), letter("imath", "ı"), letter("jmath", "ȷ"),
    letter("eth", "ð"), upright("mho", "℧"), upright("Finv", "Ⅎ"), upright("Game", "⅁"), letter("Bbbk", "𝕜"),
    upright("emptyset", "∅"), upright("varnothing", "⌀"), upright("complement", "∁"), letter("angle", "∠"),
    letter("measuredangle", "∡"), letter("sphericalangle", "∢"), letter("top", "⊤"), letter("bot", "⊥"),
    letter("flat", "♭"), letter("natural", "♮"), letter("sharp", "♯"), letter("clubsuit", "♣"),
    letter("diamondsuit", "♢"), letter("heartsuit", "♡"), letter("spadesuit", "♠"), letter("varclubsuit", "♧"),
    letter("vardiamondsuit", "♦"), letter("varheartsuit", "♥"), letter("varspadesuit", "♤"),
    letter("triangle", "△"), letter("vartriangle", "△"), letter("triangledown", "▽"),
    letter("blacktriangle", "▲"), letter("blacktriangledown", "▼"), letter("square", "□"), letter("Box", "□"),
    letter("blacksquare", "■"), letter("lozenge", "◊"), letter("blacklozenge", "⧫"), letter("bigstar", "★"),
    letter("diagup", "╱"), letter("diagdown", "╲"), letter("surd", "√"), letter("checkmark", "✓"),
    letter("maltese", "✠"), letter("circledR", "®"), letter("circledS", "Ⓢ"), letter("yen", "¥"),
    letter("pounds", "£"), letter("euro", "€"), letter("degree", "°"), letter("S", "§"), letter("P", "¶"),
    letter("copyright", "©"), letter("dag", "†"), letter("ddag", "‡"), letter("$", "$"), letter("%", "%"),
    letter("#", "#"), letter("_", "_"), ord("&", "&"), letter("backprime", "‵"),

    // Ordinary operators
    ord("prime", "′"), ord("forall", "∀"), ord("exists", "∃"), ord("nexists", "∄"), ord("neg", "¬"),
    ord("lnot", "¬"), ord("vert", "|"), ord("|", "‖"), ord("Vert", "‖"), ord("backslash", "\\"),
    ord("vdots", "⋮"), ord("arrowvert", "⏐"), ord("Arrowvert", "‖"), ord("bracevert", "⎪"),
    op("ldots", "…", TexClass_Inner), op("cdots", "⋯", TexClass_Inner), op("ddots", "⋱", TexClass_Inner),
    op("iddots", "⋰", TexClass_Inner), op("adots", "⋰", TexClass_Inner), op("dotsc", "…", TexClass_Inner),
    op("dotso", "…", TexClass_Inner), op("dotsb", "⋯", TexClass_Inner), op("dotsm", "⋯", TexClass_Inner),
    op("dotsi", "⋯", TexClass_Inner), op("mathellipsis", "…", TexClass_Inner),
    punct("ldotp", "."), punct("cdotp", "⋅"), punct("colon", ":"),

    // Binary operators
    bin("pm", "±"), bin("mp", "∓"), bin("times", "×"), bin("div", "÷"), bin("cdot", "⋅"), bin("ast", "∗"),
    bin("star", "⋆"), bin("circ", "∘"), bin("bullet", "∙"), bin("oplus", "⊕"), bin("ominus", "⊖"),
    bin("otimes", "⊗"), bin("oslash", "⊘"), bin("odot", "⊙"), bin("cap", "∩"), bin("cup", "∪"), bin("uplus", "⊎"),
    bin("sqcap", "⊓"), bin("sqcup", "⊔"), bin("vee", "∨"), bin("lor", "∨"), bin("wedge", "∧"), bin("land", "∧"),
    bin("setminus", "∖"), bin("smallsetminus", "∖"), bin("wr", "≀"), bin("diamond", "⋄"),
    bin("bigtriangleup", "△"), bin("bigtriangledown", "▽"), bin("triangleleft", "◃"), bin("triangleright", "▹"),
    bin("lhd", "⊲"), bin("rhd", "⊳"), bin("unlhd", "⊴"), bin("unrhd", "⊵"), bin("amalg", "⨿"), bin("dagger", "†"),
    bin("ddagger", "‡"), bin("bigcirc", "◯"), bin("centerdot", "⋅"), bin("dotplus", "∔"), bin("ltimes", "⋉"),
    bin("rtimes", "⋊"), bin("leftthreetimes", "⋋"), bin("rightthreetimes", "⋌"), bin("curlywedge", "⋏"),
    bin("curlyvee", "⋎"), bin("circleddash", "⊝"), bin("circledast", "⊛"), bin("circledcirc", "⊚"),
    bin("boxminus", "⊟"), bin("boxtimes", "⊠"), bin("boxdot", "⊡"), bin("boxplus", "⊞"),
    bin("divideontimes", "⋇"), bin("Cap", "⋒"), bin("Cup", "⋓"), bin("doublecap", "⋒"), bin("doublecup", "⋓"),
    bin("barwedge", "⊼"), bin("veebar", "⊻"), bin("doublebarwedge", "⩞"), bin("intercal", "⊺"),

    // Relations
    rel("leq", "≤"), rel("le", "≤"), rel("geq", "≥"), rel("ge", "≥"), rel("neq", "≠"), rel("ne", "≠"),
    rel("equiv", "≡"), rel("approx", "≈"), rel("approxeq", "≊"), rel("sim", "∼"), rel("simeq", "≃"),
    rel("cong", "≅"), rel("asymp", "≍"), rel("propto", "∝"), rel("varpropto", "∝"), rel("ll", "≪"),
    rel("gg", "≫"), rel("lll", "⋘"), rel("ggg", "⋙"), rel("llless", "⋘"), rel("gggtr", "⋙"), rel("subset", "⊂"),
    rel("supset", "⊃"), rel("subseteq", "⊆"), rel("supseteq", "⊇"), rel("subsetneq", "⊊"),
    rel("supsetneq", "⊋"), rel("varsubsetneq", "⊊"), rel("varsupsetneq", "⊋"), rel("subseteqq", "⫅"),
    rel("supseteqq", "⫆"), rel("subsetneqq", "⫋"), rel("supsetneqq", "⫌"), rel("varsubsetneqq", "⫋"),
    rel("varsupsetneqq", "⫌"), rel("Subset", "⋐"), rel("Supset", "⋑"), rel("sqsubset", "⊏"),
    rel("sqsupset", "⊐"), rel("sqsubseteq", "⊑"), rel("sqsupseteq", "⊒"), rel("in", "∈"), rel("isin", "∈"),
    rel("notin", "∉"), rel("ni", "∋"), rel("owns", "∋"), rel("notni", "∌"), rel("vdash", "⊢"), rel("dashv", "⊣"),
    rel("models", "⊨"), rel("vDash", "⊨"), rel("Vdash", "⊩"), rel("Vvdash", "⊪"), rel("nvdash", "⊬"),
    rel("nvDash", "⊭"), rel("nVdash", "⊮"), rel("nVDash", "⊯"), rel("mid", "∣"), rel("nmid", "∤"),
    rel("shortmid", "∣"), rel("nshortmid", "∤"), rel("parallel", "∥"), rel("nparallel", "∦"),
    rel("shortparallel", "∥"), rel("nshortparallel", "∦"), rel("perp", "⊥"), rel("smile", "⌣"),
    rel("frown", "⌢"), rel("smallsmile", "⌣"), rel("smallfrown", "⌢"), rel("bowtie", "⋈"), rel("Join", "⋈"),
    rel("prec", "≺"), rel("succ", "≻"), rel("preceq", "⪯"), rel("succeq", "⪰"), rel("precsim", "≾"),
    rel("succsim", "≿"), rel("precapprox", "⪷"), rel("succapprox", "⪸"), rel("preccurlyeq", "≼"),
    rel("succcurlyeq", "≽"), rel("curlyeqprec", "⋞"), rel("curlyeqsucc", "⋟"), rel("precneqq", "⪵"),
    rel("succneqq", "⪶"), rel("precnsim", "⋨"), rel("succnsim", "⋩"), rel("precnapprox", "⪹"),
    rel("succnapprox", "⪺"), rel("nprec", "⊀"), rel("nsucc", "⊁"), rel("npreceq", "⋠"), rel("nsucceq", "⋡"),
    rel("doteq", "≐"), rel("doteqdot", "≑"), rel("Doteq", "≑"), rel("risingdotseq", "≓"),
    rel("fallingdotseq", "≒"), rel("coloneqq", "≔"), rel("coloneq", "≔"), rel("eqqcolon", "≕"),
    rel("eqcolon", "≕"), rel("Coloneqq", "⩴"), rel("Colon", "∷"), rel("vcentcolon", "∶"), rel("ratio", "∶"),
    rel("eqcirc", "≖"), rel("circeq", "≗"), rel("triangleq", "≜"), rel("eqdef", "≝"), rel("bumpeq", "≏"),
    rel("Bumpeq", "≎"), rel("thicksim", "∼"), rel("thickapprox", "≈"), rel("backsim", "∽"),
    rel("backsimeq", "⋍"), rel("eqsim", "≂"), rel("lesssim", "≲"), rel("gtrsim", "≳"), rel("lessapprox", "⪅"),
    rel("gtrapprox", "⪆"), rel("lesseqgtr", "⋚"), rel("gtreqless", "⋛"), rel("lesseqqgtr", "⪋"),
    rel("gtreqqless", "⪌"), rel("lessgtr", "≶"), rel("gtrless", "≷"), rel("leqq", "≦"), rel("geqq", "≧"),
    rel("leqslant", "⩽"), rel("geqslant", "⩾"), rel("eqslantless", "⪕"), rel("eqslantgtr", "⪖"),
    rel("lneq", "⪇"), rel("gneq", "⪈"), rel("lneqq", "≨"), rel("gneqq", "≩"), rel("lvertneqq", "≨"),
    rel("gvertneqq", "≩"), rel("lnsim", "⋦"), rel("gnsim", "⋧"), rel("lnapprox", "⪉"), rel("gnapprox", "⪊"),
    rel("nless", "≮"), rel("ngtr", "≯"), rel("nleq", "≰"), rel("ngeq", "≱"), rel("nleqslant", "≰"),
    rel("ngeqslant", "≱"), rel("nleqq", "≰"), rel("ngeqq", "≱"), rel("nsim", "≁"), rel("ncong", "≇"),
    rel("nsubseteq", "⊈"), rel("nsupseteq", "⊉"), rel("nsubseteqq", "⊈"), rel("nsupseteqq", "⊉"),
    rel("nsubset", "⊄"), rel("nsupset", "⊅"), rel("ntriangleleft", "⋪"), rel("ntriangleright", "⋫"),
    rel("ntrianglelefteq", "⋬"), rel("ntrianglerighteq", "⋭"), rel("vartriangleleft", "⊲"),
    rel("vartriangleright", "⊳"), rel("trianglelefteq", "⊴"), rel("trianglerighteq", "⊵"),
    rel("blacktriangleleft", "◀"), rel("blacktriangleright", "▶"), rel("therefore", "∴"), rel("because", "∵"),
    rel("between", "≬"), rel("pitchfork", "⋔"), rel("lessdot", "⋖"), rel("gtrdot", "⋗"), rel("backepsilon", "϶"),
    rel("multimap", "⊸"), rel("origof", "⊶"), rel("imageof", "⊷"), rel("relbar", "−"), rel("Relbar", "="),

    // Arrows
    rel("leftarrow", "←"), rel("gets", "←"), rel("rightarrow", "→"), rel("to", "→"), rel("uparrow", "↑"),
    rel("downarrow", "↓"), rel("leftrightarrow", "↔"), rel("updownarrow", "↕"), rel("Leftarrow", "⇐"),
    rel("Rightarrow", "⇒"), rel("Uparrow", "⇑"), rel("Downarrow", "⇓"), rel("Leftrightarrow", "⇔"),
    rel("Updownarrow", "⇕"), rel("longleftarrow", "⟵"), rel("longrightarrow", "⟶"),
    rel("longleftrightarrow", "⟷"), rel("Longleftarrow", "⟸"), rel("Longrightarrow", "⟹"),
    rel("Longleftrightarrow", "⟺"), rel("mapsto", "↦"), rel("longmapsto", "⟼"), rel("mapsfrom", "↤"),
    rel("longmapsfrom", "⟻"), rel("hookleftarrow", "↩"), rel("hookrightarrow", "↪"), rel("nearrow", "↗"),
    rel("searrow", "↘"), rel("swarrow", "↙"), rel("nwarrow", "↖"), rel("leftharpoonup", "↼"),
    rel("leftharpoondown", "↽"), rel("rightharpoonup", "⇀"), rel("rightharpoondown", "⇁"),
    rel("upharpoonleft", "↿"), rel("upharpoonright", "↾"), rel("restriction", "↾"), rel("downharpoonleft", "⇃"),
    rel("downharpoonright", "⇂"), rel("rightleftharpoons", "⇌"), rel("leftrightharpoons", "⇋"),
    rel("leadsto", "⇝"), rel("rightsquigarrow", "⇝"), rel("leftrightsquigarrow", "↭"),
    rel("leftleftarrows", "⇇"), rel("rightrightarrows", "⇉"), rel("leftrightarrows", "⇆"),
    rel("rightleftarrows", "⇄"), rel("upuparrows", "⇈"), rel("downdownarrows", "⇊"), rel("Lleftarrow", "⇚"),
    rel("Rrightarrow", "⇛"), rel("twoheadleftarrow", "↞"), rel("twoheadrightarrow", "↠"),
    rel("leftarrowtail", "↢"), rel("rightarrowtail", "↣"), rel("looparrowleft", "↫"),
    rel("looparrowright", "↬"), rel("curvearrowleft", "↶"), rel("curvearrowright", "↷"),
    rel("circlearrowleft", "↺"), rel("circlearrowright", "↻"), rel("Lsh", "↰"), rel("Rsh", "↱"),
    rel("nleftarrow", "↚"), rel("nrightarrow", "↛"), rel("nLeftarrow", "⇍"), rel("nRightarrow", "⇏"),
    rel("nleftrightarrow", "↮"), rel("nLeftrightarrow", "⇎"), rel("dashrightarrow", "⇢"),
    rel("dashleftarrow", "⇠"),

    // Delimiters
    open("{", "{"), close("}", "}"), open("lbrace", "{"), close("rbrace", "}"), open("langle", "⟨"),
    close("rangle", "⟩"), open("lang", "⟨"), close("rang", "⟩"), open("lAngle", "⟪"), close("rAngle", "⟫"),
    open("lvert", "|"), close("rvert", "|"), open("lVert", "‖"), close("rVert", "‖"), open("lfloor", "⌊"),
    close("rfloor", "⌋"), open("lceil", "⌈"), close("rceil", "⌉"), open("lgroup", "⟮"), close("rgroup", "⟯"),
    open("lmoustache", "⎰"), close("rmoustache", "⎱"), open("llbracket", "⟦"), close("rrbracket", "⟧"),
    open("lparen", "("), close("rparen", ")"), open("lbrack", "["), close("rbrack", "]"), open("ulcorner", "⌜"),
    close("urcorner", "⌝"), open("llcorner", "⌞"), close("lrcorner", "⌟"),

    // Large operators
    large("sum", "∑"), large("prod", "∏"), large("coprod", "∐"), large("bigcup", "⋃"), large("bigcap", "⋂"),
    large("bigvee", "⋁"), large("bigwedge", "⋀"), large("bigoplus", "⨁"), large("bigotimes", "⨂"),
    large("bigodot", "⨀"), large("biguplus", "⨄"), large("bigsqcup", "⨆"), large("bigsqcap", "⨅"),
    large("intop", "∫"), integral("int", "∫"), integral("iint", "∬"), integral("iiint", "∭"),
    integral("iiiint", "⨌"), integral("oint", "∮"), integral("oiint", "∯"), integral("oiiint", "∰"),
    integral("intclockwise", "∱"), integral("varointclockwise", "∲"), integral("ointctrclockwise", "∳"),
    integral("smallint", "∫"),

    // Operator names
    function("arccos", "arccos"), function("arcsin", "arcsin"), function("arctan", "arctan"),
    function("arccot", "arccot"), function("arcsec", "arcsec"), function("arccsc", "arccsc"), function("arg", "arg"),
    function("cos", "cos"), function("cosh", "cosh"), function("cot", "cot"), function("coth", "coth"),
    function("csc", "csc"), function("csch", "csch"), function("deg", "deg"), function("dim", "dim"),
    function("exp", "exp"), function("hom", "hom"), function("ker", "ker"), function("lg", "lg"),
    function("ln", "ln"), function("log", "log"), function("sec", "sec"), function("sech", "sech"),
    function("sin", "sin"), function("sinh", "sinh"), function("tan", "tan"), function("tanh", "tanh"),
    function("sh", "sh"), function("ch", "ch"), function("th", "th"), function("cth", "cth"), function("tg", "tg"),
    function("ctg", "ctg"), function("cotg", "cotg"), function("arctg", "arctg"), function("arcctg", "arcctg"),
    limitFunction("det", "det"), limitFunction("gcd", "gcd"), limitFunction("inf", "inf"),
    // A name of two words has a thin space between them, as amsmath's \liminf does.
    limitFunction("lim", "lim"), limitFunction("liminf", "lim inf"), limitFunction("limsup", "lim sup"),
    limitFunction("max", "max"), limitFunction("min", "min"), limitFunction("Pr", "Pr"),
    limitFunction("sup", "sup"), limitFunction("injlim", "inj lim"), limitFunction("projlim", "proj lim"),
    limitFunction("argmax", "arg max"), limitFunction("argmin", "arg min"),

    // Accents
    accent("hat", "^", 0), accent("widehat", "^", TexAccentFlag_Stretchy), accent("check", "ˇ", 0),
    accent("widecheck", "ˇ", TexAccentFlag_Stretchy), accent("tilde", "~", 0),
    accent("widetilde", "~", TexAccentFlag_Stretchy), accent("acute", "´", 0), accent("grave", "`", 0),
    accent("dot", "˙", 0), accent("ddot", "¨", 0), accent("dddot", "⃛", 0), accent("ddddot", "⃜", 0),
    accent("breve", "˘", 0), accent("bar", "¯", 0), accent("mathring", "˚", 0), accent("vec", "⃗", 0),
    accent("overline", "‾", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("underline", "_", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),
    accent("overrightarrow", "→", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("overleftarrow", "←", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("overleftrightarrow", "↔", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("Overrightarrow", "⇒", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("overrightharpoon", "⇀", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("overleftharpoon", "↼", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("underrightarrow", "→", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),
    accent("underleftarrow", "←", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),
    accent("underleftrightarrow", "↔", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),
    accent("utilde", "~", TexAccentFlag_Stretchy | TexAccentFlag_Under),
    accent("overbrace", "⏞", TexAccentFlag_Stretchy | TexAccentFlag_Limits | TexAccentFlag_Clear),
    accent("underbrace", "⏟",
        TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Limits | TexAccentFlag_Clear),
    accent("overparen", "⏜", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("underparen", "⏝", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),
    accent("overgroup", "⏠", TexAccentFlag_Stretchy | TexAccentFlag_Clear),
    accent("undergroup", "⏡", TexAccentFlag_Stretchy | TexAccentFlag_Under | TexAccentFlag_Clear),

    // Text mode
    textAccent("'", "\xcc\x81"), textAccent("`", "\xcc\x80"), textAccent("^", "\xcc\x82"),
    textAccent("\"", "\xcc\x88"), textAccent("~", "\xcc\x83"), textAccent("=", "\xcc\x84"),
    textAccent(".", "\xcc\x87"), textAccent("u", "\xcc\x86"), textAccent("v", "\xcc\x8c"),
    textAccent("H", "\xcc\x8b"), textAccent("c", "\xcc\xa7"), textAccent("r", "\xcc\x8a"),
    textSymbol("textbackslash", "\\"), textSymbol("textasciitilde", "~"), textSymbol("textasciicircum", "^"),
    textSymbol("textbar", "|"), textSymbol("textless", "<"), textSymbol("textgreater", ">"),
    textSymbol("textunderscore", "_"), textSymbol("textendash", "–"), textSymbol("textemdash", "—"),
    textSymbol("textquoteleft", "‘"), textSymbol("textquoteright", "’"), textSymbol("textquotedblleft", "“"),
    textSymbol("textquotedblright", "”"), textSymbol("textellipsis", "…"), textSymbol("textregistered", "®"),
    textSymbol("texttrademark", "™"), textSymbol("textdagger", "†"), textSymbol("textdaggerdbl", "‡"),
    textSymbol("textbullet", "•"), textSymbol("textdegree", "°"), textSymbol("i", "ı"), textSymbol("j", "ȷ"),
    textSymbol("ss", "ß"), textSymbol("ae", "æ"), textSymbol("AE", "Æ"), textSymbol("oe", "œ"), textSymbol("OE", "Œ"),
    textSymbol("o", "ø"), textSymbol("O", "Ø"), textSymbol("aa", "å"), textSymbol("AA", "Å"), textSymbol("l", "ł"),
    textSymbol("L", "Ł"),

    // Spaces, in mu
    space(",", 3), space("thinspace", 3), space(":", 4), space(">", 4), space("medspace", 4), space(";", 5),
    space("thickspace", 5), space("!", -3), space("negthinspace", -3), space("negmedspace", -4),
    space("negthickspace", -5), space(" ", 6), space("space", 6), space("nobreakspace", 6), space("quad", 18),
    space("qquad", 36), space("enspace", 9), space("enskip", 9), space("joinrel", -3),

    // Fonts
    font("mathbf", TexFont_Bold), font("mathit", TexFont_Italic), font("mathrm", TexFont_Roman),
    font("mathup", TexFont_Roman), font("mathsf", TexFont_SansSerif), font("mathtt", TexFont_Monospace),
    font("mathcal", TexFont_Calligraphic), font("mathscr", TexFont_Script), font("mathfrak", TexFont_Fraktur),
    font("mathbb", TexFont_DoubleStruck), font("boldsymbol", TexFont_BoldItalic), font("bm", TexFont_BoldItalic),
    font("mathnormal", TexFont_Normal), font("mathbfit", TexFont_BoldItalic), font("mathsfit", TexFont_SansSerifItalic),
    font("mathbfsf", TexFont_SansSerifBold), font("mathbfscr", TexFont_BoldScript),
    font("mathbffrak", TexFont_BoldFraktur), font("mathbfcal", TexFont_BoldCalligraphic), font("bold", TexFont_Bold),
    font("Bbb", TexFont_DoubleStruck), font("frak", TexFont_Fraktur), fontSwitch("bf", TexFont_Bold),
    fontSwitch("rm", TexFont_Roman), fontSwitch("it", TexFont_Italic), fontSwitch("sf", TexFont_SansSerif),
    fontSwitch("tt", TexFont_Monospace), fontSwitch("cal", TexFont_Calligraphic), fontSwitch("mit", TexFont_Normal),
    fontSwitch("bfseries", TexFont_Bold), fontSwitch("boldmath", TexFont_BoldItalic),
    textFont("textrm", TexTextFont_Upright), textFont("textnormal", TexTextFont_Upright),
    textFont("textup", TexTextFont_Upright), textFont("textmd", TexTextFont_Upright),
    textFont("textbf", TexTextFont_Bold), textFont("textit", TexTextFont_Italic), textFont("emph", TexTextFont_Italic),
    textFont("textsl", TexTextFont_Italic), textFont("textsf", TexTextFont_SansSerif),
    textFont("texttt", TexTextFont_Monospace),

    // Styles and sizes
    style("displaystyle", TexStyle_Display), style("textstyle", TexStyle_Text), style("scriptstyle", TexStyle_Script),
    style("scriptscriptstyle", TexStyle_ScriptScript), size("tiny", 50), size("scriptsize", 70),
    size("footnotesize", 80), size("small", 90), size("normalsize", 100), size("large", 120), size("Large", 144),
    size("LARGE", 173), size("huge", 207), size("Huge", 249),

    // Delimiter sizes
    big("big", 1, TexClass_Ord), big("Big", 2, TexClass_Ord), big("bigg", 3, TexClass_Ord),
    big("Bigg", 4, TexClass_Ord), big("bigl", 1, TexClass_Open), big("Bigl", 2, TexClass_Open),
    big("biggl", 3, TexClass_Open), big("Biggl", 4, TexClass_Open), big("bigr", 1, TexClass_Close),
    big("Bigr", 2, TexClass_Close), big("biggr", 3, TexClass_Close), big("Biggr", 4, TexClass_Close),
    big("bigm", 1, TexClass_Rel), big("Bigm", 2, TexClass_Rel), big("biggm", 3, TexClass_Rel),
    big("Biggm", 4, TexClass_Rel),

    // Extensible arrows
    arrow("xrightarrow", "→"), arrow("xleftarrow", "←"), arrow("xRightarrow", "⇒"), arrow("xLeftarrow", "⇐"),
    arrow("xleftrightarrow", "↔"), arrow("xLeftrightarrow", "⇔"), arrow("xhookleftarrow", "↩"),
    arrow("xhookrightarrow", "↪"), arrow("xmapsto", "↦"), arrow("xrightharpoondown", "⇁"),
    arrow("xrightharpoonup", "⇀"), arrow("xleftharpoondown", "↽"), arrow("xleftharpoonup", "↼"),
    arrow("xrightleftharpoons", "⇌"), arrow("xleftrightharpoons", "⇋"), arrow("xlongequal", "="),
    arrow("xtwoheadrightarrow", "↠"), arrow("xtwoheadleftarrow", "↞"), arrow("xtofrom", "⇄"),

    // Macros, as amsmath and LaTeX define them
    macro("iff", "\\;\\Longleftrightarrow\\;"), macro("implies", "\\;\\Longrightarrow\\;"),
    macro("impliedby", "\\;\\Longleftarrow\\;"), macro("varliminf", "\\mathop{\\underline{\\mathrm{lim}}}"),
    macro("varlimsup", "\\mathop{\\overline{\\mathrm{lim}}}"),
    macro("varinjlim", "\\mathop{\\underrightarrow{\\mathrm{lim}}}"),
    macro("varprojlim", "\\mathop{\\underleftarrow{\\mathrm{lim}}}"),
    macro("operatornamewithlimits", "\\operatorname*{#1}", 1), macro("TeX", "\\text{T\\kern-.1667em\\raisebox{-.5ex}{E}\\kern-.125emX}"),
    macro("LaTeX", "\\text{L\\kern-.36em\\raisebox{.21em}{\\scriptsize A}\\kern-.15em\\TeX}"),
    macro("nonumber", "\\notag"), macro("lbag", "⟅"), macro("rbag", "⟆"), macro("textdollar", "\\$"),

    // Commands
    command("frac", TexCommand_Frac), command("dfrac", TexCommand_DisplayFrac), command("tfrac", TexCommand_TextFrac),
    command("cfrac", TexCommand_ContinuedFrac), command("binom", TexCommand_Binom),
    command("dbinom", TexCommand_DisplayBinom), command("tbinom", TexCommand_TextBinom),
    command("genfrac", TexCommand_GenFrac), command("over", TexCommand_Over), command("atop", TexCommand_Atop),
    command("choose", TexCommand_Choose), command("brack", TexCommand_Brack), command("brace", TexCommand_Brace),
    command("above", TexCommand_Above), command("sqrt", TexCommand_Sqrt), command("left", TexCommand_Left),
    command("right", TexCommand_Right), command("middle", TexCommand_Middle), command("limits", TexCommand_Limits),
    command("nolimits", TexCommand_NoLimits), command("displaylimits", TexCommand_DisplayLimits),
    command("overset", TexCommand_OverSet), command("underset", TexCommand_UnderSet),
    command("stackrel", TexCommand_StackRel), command("operatorname", TexCommand_OperatorName),
    command("mathord", TexCommand_MathOrd), command("mathop", TexCommand_MathOp),
    command("mathbin", TexCommand_MathBin), command("mathrel", TexCommand_MathRel),
    command("mathopen", TexCommand_MathOpen), command("mathclose", TexCommand_MathClose),
    command("mathpunct", TexCommand_MathPunct), command("mathinner", TexCommand_MathInner),
    command("hspace", TexCommand_HSpace), command("hskip", TexCommand_Kern), command("kern", TexCommand_Kern),
    command("mkern", TexCommand_MKern), command("mskip", TexCommand_MKern), command("mspace", TexCommand_MKern),
    command("color", TexCommand_Color), command("textcolor", TexCommand_TextColor),
    command("colorbox", TexCommand_ColorBox), command("fcolorbox", TexCommand_FColorBox),
    command("boxed", TexCommand_Boxed), command("fbox", TexCommand_FBox), command("phantom", TexCommand_Phantom),
    command("hphantom", TexCommand_HPhantom), command("vphantom", TexCommand_VPhantom),
    command("smash", TexCommand_Smash), command("mathrlap", TexCommand_RLap), command("rlap", TexCommand_RLap),
    command("mathllap", TexCommand_LLap), command("llap", TexCommand_LLap), command("mathclap", TexCommand_CLap),
    command("clap", TexCommand_CLap), command("cancel", TexCommand_Cancel), command("bcancel", TexCommand_BCancel),
    command("xcancel", TexCommand_XCancel), command("sout", TexCommand_SOut), command("enclose", TexCommand_Enclose),
    command("not", TexCommand_Not), command("begin", TexCommand_Begin), command("end", TexCommand_End),
    command("\\", TexCommand_Newline), command("newline", TexCommand_Newline), command("cr", TexCommand_Newline),
    command("hline", TexCommand_HLine), command("hdashline", TexCommand_HDashLine), command("tag", TexCommand_Tag),
    command("notag", TexCommand_NoTag), command("label", TexCommand_Label),
    command("newcommand", TexCommand_NewCommand), command("renewcommand", TexCommand_RenewCommand),
    command("providecommand", TexCommand_ProvideCommand), command("def", TexCommand_Def),
    command("gdef", TexCommand_GDef), command("global", TexCommand_Global), command("let", TexCommand_Let),
    command("DeclareMathOperator", TexCommand_DeclareMathOperator), command("href", TexCommand_Href),
    command("url", TexCommand_Url), command("unicode", TexCommand_Unicode), command("char", TexCommand_Char),
    command("class", TexCommand_Class), command("cssId", TexCommand_CssId), command("style", TexCommand_Style),
    command("mod", TexCommand_Mod), command("bmod", TexCommand_BMod), command("pmod", TexCommand_PMod),
    command("pod", TexCommand_Pod), command("dots", TexCommand_Dots), command("substack", TexCommand_SubStack),
    command("mathchoice", TexCommand_MathChoice), command("relax", TexCommand_Relax),
    command("allowbreak", TexCommand_Relax), command("nobreak", TexCommand_Relax),
    command("displaybreak", TexCommand_Relax), command("strut", TexCommand_Strut),
    command("mathstrut", TexCommand_MathStrut), command("raisebox", TexCommand_RaiseBox),
    command("rule", TexCommand_Rule), command("sideset", TexCommand_SideSet),
    command("ensuremath", TexCommand_Ensuremath), command("pmb", TexCommand_Pmb), command("ref", TexCommand_Ref),
    command("eqref", TexCommand_Ref), command("text", TexCommand_TextMode), command("mbox", TexCommand_TextMode),
    command("hbox", TexCommand_TextMode),
};

constexpr uint32_t kEntryCount = countOf(kEntries);
static_assert(kEntryCount < 0xffff, "an entry's index fits in 16 bits");

// The entries by name, and the symbols by code point.
struct EntryIndex {
    uint16_t byName[kEntryCount];
    uint32_t symbolCount;
    uint32_t symbolCodePoints[kEntryCount];
    uint16_t symbols[kEntryCount];  // by position in symbolCodePoints
};

bool isSymbolKind(TexEntryKind kind) {
    switch (kind) {
        case TexEntryKind_Letter:
        case TexEntryKind_Upright:
        case TexEntryKind_Operator:
        case TexEntryKind_LargeOperator:
        case TexEntryKind_Integral:
            return true;
        default:
            return false;
    }
}

// Returns the one code point of `text`, or 0 if it has none or more than one.
uint32_t singleCodePoint(const char* PR_NULLABLE text) {
    if (text == nullptr)
        return 0;
    PrStringView view = text;
    if (view.size == 0)
        return 0;
    uint32_t codePoint = 0;
    uint32_t size = prUtf8Decode(view, 0, &codePoint);
    return size == view.size ? codePoint : 0;
}

EntryIndex buildIndex() {
    EntryIndex index{};
    for (uint32_t i = 0; i < kEntryCount; ++i)
        index.byName[i] = uint16_t(i);
    // A stable order: the earlier entry of two with the same name comes first, so it is the one a lookup finds.
    sortItems(Span<uint16_t>(index.byName, kEntryCount), [](uint16_t a, uint16_t b) {
        int order = prStringCompare(kEntries[a].name, kEntries[b].name);
        return order != 0 ? order < 0 : a < b;
    });
    uint16_t symbols[kEntryCount];
    uint32_t count = 0;
    for (uint32_t i = 0; i < kEntryCount; ++i) {
        if (isSymbolKind(kEntries[i].kind) && singleCodePoint(kEntries[i].text) != 0)
            symbols[count++] = uint16_t(i);
    }
    sortItems(Span<uint16_t>(symbols, count), [](uint16_t a, uint16_t b) {
        uint32_t left = singleCodePoint(kEntries[a].text);
        uint32_t right = singleCodePoint(kEntries[b].text);
        return left != right ? left < right : a < b;
    });
    for (uint32_t i = 0; i < count; ++i) {
        index.symbols[i] = symbols[i];
        index.symbolCodePoints[i] = singleCodePoint(kEntries[symbols[i]].text);
    }
    index.symbolCount = count;
    return index;
}

// The index, built once on first use. A function-local static initializes thread-safely.
const EntryIndex& entryIndex() {
    static const EntryIndex index = buildIndex();
    return index;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fonts

// The first code points of a font's alphabets, or 0 for an alphabet that the font does not have.
struct Alphabet {
    uint32_t latin;                 // A, followed by Z and then a to z
    uint32_t greek;                 // Α, in the layout of U+1D6A8: 25 capitals, ∇, 25 small letters and 7 symbols
    uint32_t digits;
};

Alphabet alphabetOf(TexFont font) {
    switch (font) {
        case TexFont_Bold:
            return Alphabet{0x1D400, 0x1D6A8, 0x1D7CE};
        case TexFont_Italic:
        case TexFont_Normal:
            return Alphabet{0x1D434, 0x1D6E2, 0};
        case TexFont_BoldItalic:
            return Alphabet{0x1D468, 0x1D71C, 0x1D7CE};
        case TexFont_Script:
        case TexFont_Calligraphic:
            return Alphabet{0x1D49C, 0, 0};
        case TexFont_BoldScript:
        case TexFont_BoldCalligraphic:
            return Alphabet{0x1D4D0, 0, 0x1D7CE};
        case TexFont_Fraktur:
            return Alphabet{0x1D504, 0, 0};
        case TexFont_BoldFraktur:
            return Alphabet{0x1D56C, 0, 0x1D7CE};
        case TexFont_DoubleStruck:
            return Alphabet{0x1D538, 0, 0x1D7D8};
        case TexFont_SansSerif:
            return Alphabet{0x1D5A0, 0, 0x1D7E2};
        case TexFont_SansSerifBold:
            return Alphabet{0x1D5D4, 0x1D756, 0x1D7EC};
        case TexFont_SansSerifItalic:
            return Alphabet{0x1D608, 0, 0x1D7E2};
        case TexFont_SansSerifBoldItalic:
            return Alphabet{0x1D63C, 0x1D790, 0x1D7EC};
        case TexFont_Monospace:
            return Alphabet{0x1D670, 0, 0x1D7F6};
        case TexFont_None:
        case TexFont_Roman:
            break;
    }
    return Alphabet{0, 0, 0};
}

// A letter that Unicode encodes outside the block, in the Letterlike Symbols, where the block has a hole.
struct Hole {
    uint32_t inBlock;
    uint32_t encoded;
};

constexpr Hole kHoles[] = {
    {0x1D455, 0x210E}, {0x1D49D, 0x212C}, {0x1D4A0, 0x2130}, {0x1D4A1, 0x2131}, {0x1D4A3, 0x210B},
    {0x1D4A4, 0x2110}, {0x1D4A7, 0x2112}, {0x1D4A8, 0x2133}, {0x1D4AD, 0x211B}, {0x1D4BA, 0x212F},
    {0x1D4BC, 0x210A}, {0x1D4C4, 0x2134}, {0x1D506, 0x212D}, {0x1D50B, 0x210C}, {0x1D50C, 0x2111},
    {0x1D515, 0x211C}, {0x1D51D, 0x2128}, {0x1D53A, 0x2102}, {0x1D53F, 0x210D}, {0x1D545, 0x2115},
    {0x1D547, 0x2119}, {0x1D548, 0x211A}, {0x1D549, 0x211D}, {0x1D551, 0x2124},
};

uint32_t fillHole(uint32_t codePoint) {
    for (const Hole& hole : kHoles) {
        if (hole.inBlock == codePoint)
            return hole.encoded;
    }
    return codePoint;
}

// Returns the position of `codePoint` in the layout of a Greek alphabet, or -1.
int greekPosition(uint32_t codePoint) {
    if (codePoint >= 0x391 && codePoint <= 0x3A9 && codePoint != 0x3A2)
        return int(codePoint - 0x391);
    if (codePoint == 0x3F4)
        return 17;
    if (codePoint == 0x2207)
        return 25;
    if (codePoint >= 0x3B1 && codePoint <= 0x3C9)
        return int(26 + codePoint - 0x3B1);
    switch (codePoint) {
        case 0x2202:
            return 51;
        case 0x3F5:
            return 52;
        case 0x3D1:
            return 53;
        case 0x3F0:
            return 54;
        case 0x3D5:
            return 55;
        case 0x3F1:
            return 56;
        case 0x3D6:
            return 57;
        default:
            return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The operator dictionary

// The maximum number of code points in the dictionary's content, which is at most two UTF-16 code units.
constexpr uint32_t kMaxContent = 2;

// Writes the code points of `content` to `out`, up to kMaxContent, and returns their number in UTF-16 code units, which
// is more than two for content that the dictionary cannot hold.
uint32_t contentOf(PrStringView content, uint32_t out[PR_NONNULL kMaxContent], uint32_t* count) {
    uint32_t units = 0;
    *count = 0;
    for (size_t at = 0; at < content.size && units <= 2;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(content, at, &codePoint);
        units += codePoint > 0xffff ? 2 : 1;
        if (*count < kMaxContent)
            out[(*count)++] = codePoint;
    }
    return units;
}

// Returns the category of an operator whose content is `content` in `form`, by the algorithm to determine the category
// of an operator (mathml-core/#operator-dictionary).
TexOperatorCategory categoryOf(PrStringView content, TexForm form) {
    uint32_t codePoints[kMaxContent] = {};
    uint32_t count = 0;
    uint32_t units = contentOf(content, codePoints, &count);
    if (units != 1 && units != 2)
        return TexOperatorCategory_Default;
    uint32_t key = codePoints[0];
    if (units == 1 && key >= 0x320 && key <= 0x3ff)
        return TexOperatorCategory_Default;
    if (units == 2) {
        if (count == 1) {
            // The dictionary lists only two operators outside the BMP, both Arabic and postfix.
            if ((key == 0x1eef0 || key == 0x1eef1) && form == TexForm_Postfix)
                return TexOperatorCategory_I;
            return TexOperatorCategory_Default;
        }
        if (codePoints[1] == 0x338 || codePoints[1] == 0x20d2) {
            // The negated forms of an operator take its category.
        } else if (key < 0x80 && codePoints[1] < 0x80) {
            key = texDictionaryAsciiPair(char(key), char(codePoints[1]));
            if (key == 0)
                return TexOperatorCategory_Default;
        } else {
            return TexOperatorCategory_Default;
        }
    }
    if (form == TexForm_Infix && (key == 0x7c || key == 0x223c))
        return TexOperatorCategory_ForceDefault;
    if (key > 0x3ff && (key < 0x2000 || key > 0x2bff))
        return TexOperatorCategory_Default;
    return texDictionaryCategory(key, form);
}

} // namespace

const TexEntry* PR_NULLABLE texEntryFromName(PrStringView name) {
    const EntryIndex& index = entryIndex();
    uint32_t at = partitionPoint(0, kEntryCount, [&](uint32_t i) {
        return prStringCompare(kEntries[index.byName[i]].name, name) < 0;
    });
    if (at == kEntryCount || !(PrStringView(kEntries[index.byName[at]].name) == name))
        return nullptr;
    return &kEntries[index.byName[at]];
}

const TexEntry* PR_NULLABLE texEntryFromCodePoint(uint32_t codePoint) {
    const EntryIndex& index = entryIndex();
    uint32_t at = partitionPoint(0, index.symbolCount, [&](uint32_t i) {
        return index.symbolCodePoints[i] < codePoint;
    });
    if (at == index.symbolCount || index.symbolCodePoints[at] != codePoint)
        return nullptr;
    return &kEntries[index.symbols[at]];
}

uint32_t texFontMap(uint32_t codePoint, TexFont font) {
    Alphabet alphabet = alphabetOf(font);
    if (codePoint >= 'A' && codePoint <= 'Z' && alphabet.latin != 0)
        return fillHole(alphabet.latin + codePoint - 'A');
    if (codePoint >= 'a' && codePoint <= 'z' && alphabet.latin != 0)
        return fillHole(alphabet.latin + 26 + codePoint - 'a');
    if (codePoint >= '0' && codePoint <= '9' && alphabet.digits != 0)
        return alphabet.digits + codePoint - '0';
    if (alphabet.greek != 0) {
        int position = greekPosition(codePoint);
        if (position >= 0)
            return alphabet.greek + uint32_t(position);
    }
    if (font == TexFont_Italic || font == TexFont_Normal) {
        if (codePoint == 0x131)
            return 0x1D6A4;
        if (codePoint == 0x237)
            return 0x1D6A5;
    }
    return codePoint;
}

uint32_t texFontVariation(uint32_t codePoint, TexFont font) {
    // Unicode defines the sequences for the script and bold script capitals, including those that the Letterlike
    // Symbols encode, such as U+212C ℬ (StandardizedVariants.txt).
    bool capital = (codePoint >= 0x1D49C && codePoint <= 0x1D4B5) || (codePoint >= 0x1D4D0 && codePoint <= 0x1D4E9);
    for (const Hole& hole : kHoles) {
        if (hole.encoded == codePoint && hole.inBlock >= 0x1D49C && hole.inBlock <= 0x1D4B5)
            capital = true;
    }
    if (!capital)
        return 0;
    switch (font) {
        case TexFont_Script:
        case TexFont_BoldScript:
            return 0xFE01;
        default:
            return 0;
    }
}

bool texIsAutoItalic(uint32_t codePoint) {
    if ((codePoint >= 'A' && codePoint <= 'Z') || (codePoint >= 'a' && codePoint <= 'z'))
        return true;
    return codePoint == 0x131 || codePoint == 0x237 || greekPosition(codePoint) >= 0;
}

uint32_t texNegate(uint32_t codePoint) {
    struct Negation {
        uint32_t codePoint;
        uint32_t negated;
    };
    static constexpr Negation kNegations[] = {
        {'=', 0x2260}, {'<', 0x226E}, {'>', 0x226F}, {0x2208, 0x2209}, {0x220B, 0x220C}, {0x2264, 0x2270},
        {0x2265, 0x2271}, {0x2282, 0x2284}, {0x2283, 0x2285}, {0x2286, 0x2288}, {0x2287, 0x2289}, {0x2261, 0x2262},
        {0x223C, 0x2241}, {0x2243, 0x2244}, {0x2248, 0x2249}, {0x2245, 0x2247}, {0x2223, 0x2224}, {0x2225, 0x2226},
        {0x2203, 0x2204}, {0x227A, 0x2280}, {0x227B, 0x2281}, {0x2AAF, 0x22E0}, {0x2AB0, 0x22E1}, {0x22A2, 0x22AC},
        {0x22A8, 0x22AD}, {0x22A9, 0x22AE}, {0x22AB, 0x22AF}, {0x2291, 0x22E2}, {0x2292, 0x22E3}, {0x22B4, 0x22EC},
        {0x22B5, 0x22ED}, {0x22B2, 0x22EA}, {0x22B3, 0x22EB}, {0x224D, 0x226D}, {0x2190, 0x219A}, {0x2192, 0x219B},
        {0x2194, 0x21AE}, {0x21D0, 0x21CD}, {0x21D2, 0x21CF}, {0x21D4, 0x21CE}, {0x2272, 0x2274}, {0x2273, 0x2275},
        {0x2276, 0x2278}, {0x2277, 0x2279}, {0x227C, 0x22E0}, {0x227D, 0x22E1},
    };
    for (const Negation& negation : kNegations) {
        if (negation.codePoint == codePoint)
            return negation.negated;
    }
    return 0;
}

TexCategoryValues texOperatorDictionary(PrStringView content, TexForm form, bool explicitForm) {
    TexOperatorCategory category = categoryOf(content, form);
    constexpr TexForm kFallbacks[] = {TexForm_Infix, TexForm_Postfix, TexForm_Prefix};
    for (uint32_t i = 0; i < 3 && !explicitForm && category == TexOperatorCategory_Default; ++i)
        category = categoryOf(content, kFallbacks[i]);
    return texCategoryValues(category);
}

} // namespace pr_impl

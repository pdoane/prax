# Mathematics

A sample of real mathematics in TeX, for a person to read beside TeX's own setting of it. Each formula is one that a
typographer knows, and what to look at is said before it.

## Classics

The roots of $ax^2 + bx + c = 0$, for $a \neq 0$, are given by the quadratic formula, whose fraction keeps its numerator
and denominator in text style when it is inline, as in $x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}$, and in display style
when it is displayed:

$$
x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}
$$

The Gaussian integral, whose limits sit beside the integral sign and whose exponent has a superscript of its own:

$$
\int_{-\infty}^{\infty} e^{-x^2} \, dx = \sqrt{\pi}
$$

The Basel problem, whose limits sit above and below the summation sign in display style and beside it in text style,
as in $\sum_{n=1}^\infty 1/n^2 = \pi^2/6$:

$$
\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6}
$$

Euler's identity, $e^{i\pi} + 1 = 0$, and his product, whose primes are a limit in small type:

$$
\sum_{n=1}^{\infty} \frac{1}{n^s} = \prod_{p \text{ prime}} \frac{1}{1 - p^{-s}}
$$

Cauchy's integral formula, with a contour integral and a fraction in its integrand:

$$
f(a) = \frac{1}{2\pi i} \oint_\gamma \frac{f(z)}{z - a} \, dz
$$

Stirling's approximation, with a relation of its own:

$$
n! \sim \sqrt{2\pi n} \left( \frac{n}{e} \right)^n
$$

Maxwell's equations, aligned on their relations and numbered:

$$
\begin{align}
\nabla \cdot \mathbf{E} &= \frac{\rho}{\varepsilon_0} \\
\nabla \cdot \mathbf{B} &= 0 \\
\nabla \times \mathbf{E} &= -\frac{\partial \mathbf{B}}{\partial t} \\
\nabla \times \mathbf{B} &= \mu_0 \mathbf{J} + \mu_0 \varepsilon_0 \frac{\partial \mathbf{E}}{\partial t} \tag{M}
\end{align}
$$

## Structures

Cases, whose brace covers its rows and whose conditions align on their left:

$$
|x| = \begin{cases} x & \text{if } x \geq 0, \\ -x & \text{otherwise.} \end{cases}
$$

A rotation, whose parentheses are as tall as TeX's for two rows, and a determinant beside it:

$$
R(\theta) = \begin{pmatrix} \cos\theta & -\sin\theta \\ \sin\theta & \cos\theta \end{pmatrix},
\qquad
\det \begin{bmatrix} a & b \\ c & d \end{bmatrix} = \begin{vmatrix} a & b \\ c & d \end{vmatrix} = ad - bc
$$

A continued fraction, whose numerators are centered over full-size denominators:

$$
\sqrt{2} = 1 + \cfrac{1}{2 + \cfrac{1}{2 + \cfrac{1}{2 + \cdots}}}
$$

Binomials, in text style as $\binom{n}{k}$ and in display style:

$$
(x + y)^n = \sum_{k=0}^{n} \binom{n}{k} x^{n-k} y^k
$$

Nested radicals, whose signs grow with what they hold, and a root with an index:

$$
\sqrt{1 + \sqrt{1 + \sqrt{1 + \sqrt{1 + x}}}} \qquad \sqrt[3]{\frac{a}{b}} \qquad \sqrt[n]{x^n + y^n}
$$

## Accents and braces

Accents sit on their bases' slants: $\hat{x}$, $\bar{x}$, $\vec{v}$, $\dot{x}$, $\ddot{y}$, $\tilde{n}$, $\widehat{xyz}$,
$\widetilde{abc}$, and bars over and under whole formulas, $\overline{z + w} = \bar{z} + \bar{w}$ and
$\underline{a + b}$.

$$
\vec{F} = m \vec{a} \qquad \overrightarrow{AB} + \overrightarrow{BC} = \overrightarrow{AC}
$$

Braces carry their labels:

$$
\underbrace{1 + 2 + \cdots + n}_{n \text{ terms}} = \frac{n(n+1)}{2}
\qquad
\overbrace{a + a + \cdots + a}^{k \text{ times}} = ka
$$

## Delimiters

Delimiters that \left and \right size to what they enclose, with \middle between:

$$
\left\{ x \in \mathbb{R} \;\middle|\; \frac{x^2}{2} < 1 \right\}
\qquad
\left( \sum_{i=1}^n a_i b_i \right)^2 \leq \left( \sum_{i=1}^n a_i^2 \right) \left( \sum_{i=1}^n b_i^2 \right)
$$

The sizes of \big and its kin, in order:

$$
\Bigg( \bigg( \Big( \big( ( x ) \big) \Big) \bigg) \Bigg)
\qquad
\Bigg[ \bigg[ \Big[ \big[ [ x ] \big] \Big] \bigg] \Bigg]
\qquad
\Bigg\| \bigg| \Big| \big| | x | \big| \Big| \bigg| \Bigg\|
$$

## Alphabets

The double-struck $\mathbb{N} \subset \mathbb{Z} \subset \mathbb{Q} \subset \mathbb{R} \subset \mathbb{C}$, the calligraphic
$\mathcal{A}, \mathcal{L}, \mathcal{O}$, the script $\mathscr{F}, \mathscr{H}, \mathscr{L}$, the Fraktur
$\mathfrak{g}, \mathfrak{sl}_2, \mathfrak{A}$, and the bold $\mathbf{v}, \boldsymbol{\alpha}$.

## Running text

Inline math keeps the lines of its paragraph evenly spaced where TeX keeps them: a variable $x$, a subscript $a_i$, a
superscript $x^2$, both $x_i^2$, a function $f(x) = \sin x + \cos x$, and the sets $A \cup B$ and $A \cap B$ sit within
the line, as do $\alpha$, $\beta$ and $\gamma$. A line with a taller formula, such as $\frac{1}{2}$ or $\sum_{i=1}^n i$,
opens by as much as the formula needs, and no more. The derivative $\frac{d}{dx} e^x = e^x$, the limit
$\lim_{x \to 0} \frac{\sin x}{x} = 1$ and the norm $\|v\|_2 = \sqrt{v \cdot v}$ are typical of what a paragraph holds.

#include "pr/mathml.h"

int prTestMathMlCApi(void);

// Tests that the public headers compile as C. Null is no MathML element.
int prTestMathMlCApi(void) {
    return prMathMlElementQuery(NULL) != NULL;
}

#include "pr/color_profile.h"
#include "pr/color_space.h"

int prTestColorCApi(void);

// The public headers compile as C. White converts to itself, and sRGB is named.
int prTestColorCApi(void) {
    double white[3] = {1, 1, 1};
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_SrgbLinear, white, white);
    PrStringView name = prColorSpaceName(PrColorSpace_Srgb);
    return white[0] < 0.999 || white[0] > 1.001 || name.size != 4;
}

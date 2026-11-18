#include <assert.h>
#include "util/flex_scale.h"

int main(void) {
    assert(sc_flex_scale_dimension(3200, 50, false) == 1600);
    assert(sc_flex_scale_dimension(2000, 50, false) == 1000);
    assert(sc_flex_scale_dimension(3840, 75, false) == 2880);
    assert(sc_flex_scale_dimension(65535, 100, false) == 65535);
    assert(sc_flex_scale_dimension(1, 25, false) == 16);
    assert(sc_flex_scale_dimension(1919, 50, false) == 959);
    assert(sc_flex_unscale_dimension(1600, 50, false) == 3200);
    assert(sc_flex_unscale_dimension(2880, 75, false) == 3840);
    assert(sc_flex_unscale_dimension(65535, 25, false) == 65535);
    assert(sc_flex_integer_factor(3200, 2000, 1600, 1000, 50) == 2);
    assert(sc_flex_integer_factor(3200, 2000, 800, 500, 25) == 4);
    // Encoder alignment and odd window sizes leave spare pixels as borders.
    assert(sc_flex_integer_factor(1919, 1079, 952, 536, 50) == 2);
    assert(sc_flex_integer_factor(1919, 1079, 472, 264, 25) == 4);
    assert(sc_flex_integer_factor(1079, 1919, 536, 952, 50) == 2);
    // Noninteger percentages, initial/stale frames and tiny windows fall back.
    assert(sc_flex_integer_factor(3200, 2000, 2400, 1500, 75) == 0);
    assert(sc_flex_integer_factor(3200, 2000, 3200, 2000, 100) == 0);
    assert(sc_flex_integer_factor(3200, 2000, 1056, 656, 33) == 3);
    assert(sc_flex_integer_factor(1280, 800, 1280, 800, 50) == 0);
    assert(sc_flex_integer_factor(1919, 1079, 960, 536, 50) == 0);
    assert(sc_flex_integer_factor(1919, 1079, 952, 540, 50) == 0);
    assert(sc_flex_integer_factor(20, 20, 16, 16, 25) == 0);
    assert(sc_flex_integer_factor(3200, 2000, 0, 1000, 50) == 0);
    assert(sc_flex_integer_factor(65535, 65535, 65535, 65535, 25) == 0);
    // 33% is exactly one third only with integer scaling enabled.
    assert(sc_flex_scale_dimension(3000, 33, true) == 1000);
    assert(sc_flex_scale_dimension(3000, 33, false) == 990);
    assert(sc_flex_unscale_dimension(1000, 33, true) == 3000);
    assert(sc_flex_unscale_dimension(1000, 33, false) == 3030);
    assert(sc_flex_scale_dimension(1919, 33, true) == 639);
    assert(sc_flex_scale_dimension(1, 33, true) == 16);
    assert(sc_flex_unscale_dimension(65535, 33, true) == 65535);
    assert(sc_flex_integer_factor(3000, 1800, 1000, 600, 33) == 3);
    assert(sc_flex_integer_factor(1919, 1079, 632, 352, 33) == 3);
    assert(sc_flex_integer_factor(1079, 1919, 352, 632, 33) == 3);
    assert(sc_flex_integer_factor(1919, 1079, 640, 352, 33) == 0);
    assert(sc_flex_integer_factor(1919, 1079, 632, 360, 33) == 0);
    assert(sc_flex_integer_factor(3000, 1800, 3000, 1800, 33) == 0);
    // Existing 2x, 4x and noninteger conversions remain unchanged.
    assert(sc_flex_scale_dimension(3200, 50, true) == 1600);
    assert(sc_flex_scale_dimension(3200, 25, true) == 800);
    assert(sc_flex_scale_dimension(3200, 75, true) == 2400);
    assert(sc_flex_unscale_dimension(1600, 50, true) == 3200);
    assert(sc_flex_unscale_dimension(800, 25, true) == 3200);
    assert(sc_flex_unscale_dimension(2400, 75, true) == 3200);
    return 0;
}

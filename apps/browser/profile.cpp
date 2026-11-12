// The user's profile: the files that keep the user's favorites and history between runs.

#include "apps/browser/profile.h"

#include "pr/file.h"
#include "pr/string.h"
#include "pr/string_ops.h"

namespace browser {

bool profileRead(PrStringView path, String* out) {
    PrByteView mapping{};
    if (!prFileMap(path, &mapping))
        return false;
    *out = prStringFromBytes(mapping);
    prFileUnmap(&mapping);
    return true;
}

bool profileWrite(PrStringView path, PrStringView text) {
    PrStringView parent = prFilePathParent(path);
    if (!parent.isEmpty())
        prFileCreateDirectory(parent);
    return prFileWrite(path, prStringBytes(text));
}

} // namespace browser

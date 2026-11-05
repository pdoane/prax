// The user's profile: the files that keep the user's favorites and history between runs.

#include "examples/browser/profile.h"

#include "pr/file.h"
#include "pr/string.h"
#include "pr/string_ops.h"

namespace browser {

bool profileRead(PrStringView path, String* out) {
    PrByteView mapping{};
    if (!prFileMap(path, &mapping))
        return false;
    *out = PrStringView{reinterpret_cast<const char*>(mapping.data), mapping.size};
    prFileUnmap(&mapping);
    return true;
}

bool profileWrite(PrStringView path, PrStringView text) {
    PrStringView parent = prFilePathParent(path);
    if (!parent.isEmpty())
        prFileCreateDirectory(parent);
    return prFileWrite(path, PrByteView{prStringBytes(text), text.size});
}

} // namespace browser

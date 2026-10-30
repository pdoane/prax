// The user's profile: the files that keep the user's favorites and history between runs.

#include "examples/browser/profile.h"

#include "pr/file.h"

namespace browser {

bool profileRead(PrStringView path, std::string* out) {
    PrFileMapping mapping{};
    if (!prFileMap(path, &mapping))
        return false;
    out->clear();
    if (mapping.size != 0)
        out->assign(reinterpret_cast<const char*>(mapping.data), mapping.size);
    prFileUnmap(&mapping);
    return true;
}

bool profileWrite(PrStringView path, PrStringView text) {
    std::string file(path.data, path.size);
    size_t slash = file.find_last_of('/');
    if (slash != std::string::npos)
        prFileCreateDirectory(PrStringView{file.data(), slash});
    return prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
}

} // namespace browser

#include "sdmc.hpp"

namespace sdmc {

    namespace {

        FsFileSystem sdmc;
    }

    Result Open() {
        return fsOpenSdCardFileSystem(&sdmc);
    }

    void Close() {
        fsFsClose(&sdmc);
    }

    Result OpenFile(FsFile *file, const char *path, int open_mode) {
        if (file == nullptr || path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenFile(&sdmc, path, open_mode, file);
    }

    Result OpenDir(FsDir *dir, const char *path, int open_mode) {
        if (dir == nullptr || path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenDirectory(&sdmc, path, open_mode, dir);
    }

    Result GetType(const char* path, FsDirEntryType* type) {
        if (path == nullptr || type == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsGetEntryType(&sdmc, path, type);
    }

    bool FileExists(const char* path) {
        FsDirEntryType type;
        return R_SUCCEEDED(GetType(path, &type)) && type == FsDirEntryType_File;
    }

    Result CreateFolder(const char* path) {
        if (path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsCreateDirectory(&sdmc, path);
    }

}

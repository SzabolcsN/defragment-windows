#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <limits>

// Print a Windows error message
static void PrintLastError(const wchar_t *msgPrefix) {
    DWORD errCode = GetLastError();
    std::wcerr << msgPrefix << L" (Error " << errCode << L")" << std::endl;
    LPWSTR errText = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errCode,
        0,
        (LPWSTR)&errText,
        0,
        NULL);
    if (errText) {
        std::wcerr << L"Reason: " << errText << std::endl;
        LocalFree(errText);
    }
}

// Enable a named privilege (e.g. "SeManageVolumePrivilege") in this process
bool EnablePrivilege(const wchar_t *privName) {
    HANDLE hToken = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        PrintLastError(L"OpenProcessToken failed");
        return false;
    }
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, privName, &luid)) {
        PrintLastError(L"LookupPrivilegeValueW failed");
        CloseHandle(hToken);
        return false;
    }
    TOKEN_PRIVILEGES tp;
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        PrintLastError(L"AdjustTokenPrivileges failed");
        CloseHandle(hToken);
        return false;
    }

    if (GetLastError() != ERROR_SUCCESS) {
        PrintLastError(L"AdjustTokenPrivileges error (post-check)");
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);
    return true;
}

// Get volume geometry: total clusters and bytes per cluster
static bool GetVolumeClusterInfo(const std::wstring &rootPath, ULONGLONG &totalClusters, DWORD &bytesPerCluster) {
    DWORD sectorsPerCluster = 0;
    DWORD bytesPerSector = 0;
    DWORD numberOfFreeClusters = 0;
    DWORD totalNumberOfClusters = 0;
    if (!GetDiskFreeSpaceW(rootPath.c_str(),
                           &sectorsPerCluster,
                           &bytesPerSector,
                           &numberOfFreeClusters,
                           &totalNumberOfClusters)) {
        PrintLastError(L"GetDiskFreeSpaceW failed");
        return false;
    }
    totalClusters = static_cast<ULONGLONG>(totalNumberOfClusters);
    bytesPerCluster = sectorsPerCluster * bytesPerSector;
    return true;
}

// Retrieve the entire NTFS volume bitmap (1=allocated, 0=free) in chunks
bool GetVolumeBitmapChunked(HANDLE volumeHandle, ULONGLONG totalClusters, std::vector<BYTE> &outBitmap) {
    outBitmap.clear();
    outBitmap.resize(static_cast<size_t>((totalClusters + 7) / 8), 0);
    STARTING_LCN_INPUT_BUFFER inBuf = {};
    inBuf.StartingLcn.QuadPart = 0; // start at LCN 0
    const size_t BUF_SIZE = 64 * 1024;
    std::vector<BYTE> tempBuf(BUF_SIZE, 0);
    LONGLONG maxLCN = (LONGLONG)totalClusters - 1;
    while (true) {
        ZeroMemory(tempBuf.data(), tempBuf.size());
        DWORD bytesReturned = 0;
        BOOL success = DeviceIoControl(
            volumeHandle,
            FSCTL_GET_VOLUME_BITMAP,
            &inBuf,
            sizeof(inBuf),
            tempBuf.data(),
            (DWORD)tempBuf.size(),
            &bytesReturned,
            NULL);
        DWORD dwErr = GetLastError();
        if (bytesReturned < sizeof(VOLUME_BITMAP_BUFFER)) {
            if (!success) {
                PrintLastError(L"FSCTL_GET_VOLUME_BITMAP failed (no valid header)");
            } else {
                std::wcerr << L"Unexpected: not enough data for VOLUME_BITMAP_BUFFER\n";
            }
            break;
        }

        auto pVolBmp = reinterpret_cast<PVOLUME_BITMAP_BUFFER>(tempBuf.data());
        LONGLONG startLCN = pVolBmp->StartingLcn.QuadPart;
        LONGLONG chunkBits = pVolBmp->BitmapSize.QuadPart;
        size_t chunkBytes = (size_t)((chunkBits + 7) / 8);
        DWORD headerSize = sizeof(VOLUME_BITMAP_BUFFER);
        DWORD bytesReturnedBody = (bytesReturned > headerSize) ? (bytesReturned - headerSize) : 0;

        if (chunkBytes > bytesReturnedBody) {
            chunkBytes = bytesReturnedBody;
        }
        LONGLONG chunkBitsAvailable = (LONGLONG)chunkBytes * 8;

        if (chunkBits > chunkBitsAvailable) {
            chunkBits = chunkBitsAvailable;
            }
        BYTE *srcBits = pVolBmp->Buffer;
        for (LONGLONG i = 0; i < chunkBits; i++) {
            LONGLONG clusterIndex = startLCN + i;
            if (clusterIndex >= (LONGLONG)totalClusters){
                break;
            }
            int srcByteIndex = (int)(i / 8);
            int srcBitOffset = (int)(i % 8);
            int bitVal = (srcBits[srcByteIndex] >> srcBitOffset) & 1;

            if (bitVal == 1) {
                size_t destByteIndex = (size_t)(clusterIndex / 8);
                int destBitOffset = (int)(clusterIndex % 8);
                outBitmap[destByteIndex] |= (BYTE)(1 << destBitOffset);
            }
        }
        LONGLONG nextLCN = startLCN + chunkBits;
        if (!success) {
            // ERROR_MORE_DATA indicates partial result.
            if (dwErr == ERROR_MORE_DATA) {
                if (nextLCN > maxLCN) {
                    break;
                }
                inBuf.StartingLcn.QuadPart = nextLCN;
            } else {
                PrintLastError(L"FSCTL_GET_VOLUME_BITMAP truly failed");
                break;
            }
        } else {
            if (chunkBits == 0 || nextLCN > maxLCN) {
                break;
            }
            inBuf.StartingLcn.QuadPart = nextLCN;
        }
    }
    return !outBitmap.empty();
}

// Structure to hold a file’s cluster mapping
struct FileClusters {
    std::vector<LONGLONG> vcns;
    std::vector<LONGLONG> lcns;
};

// Retrieve all extents for a file (even if very fragmented) by looping over FSCTL_GET_RETRIEVAL_POINTERS
bool GetAllFileRetrievalPointers(HANDLE fileHandle, FileClusters &outClusters) {
    outClusters.vcns.clear();
    outClusters.lcns.clear();
    STARTING_VCN_INPUT_BUFFER inBuf = {};
    inBuf.StartingVcn.QuadPart = 0;
    while (true) {
        const DWORD BUF_SIZE = 16 * 1024;
        std::vector<BYTE> buffer(BUF_SIZE, 0);
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            fileHandle,
            FSCTL_GET_RETRIEVAL_POINTERS,
            &inBuf,
            sizeof(inBuf),
            buffer.data(),
            (DWORD)buffer.size(),
            &bytesReturned,
            NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                // No more extents
                break;
            }
            PrintLastError(L"FSCTL_GET_RETRIEVAL_POINTERS failed");
            return false;
        }

        if (bytesReturned < sizeof(RETRIEVAL_POINTERS_BUFFER)) {
            std::wcerr << L"Not enough data returned for RETRIEVAL_POINTERS_BUFFER.\n";
            return false;
        }

        auto pRet = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(buffer.data());

        if (pRet->ExtentCount == 0){
            break;
        }
        LONGLONG currentVcn = pRet->StartingVcn.QuadPart;
        for (DWORD i = 0; i < pRet->ExtentCount; i++)
        {
            LONGLONG nextVcn = pRet->Extents[i].NextVcn.QuadPart;
            LONGLONG lcn = pRet->Extents[i].Lcn.QuadPart;
            if (lcn == -1) { // sparse or unallocated
                currentVcn = nextVcn;
                continue;
            }

            LONGLONG count = nextVcn - currentVcn;

            for (LONGLONG c = 0; c < count; c++) {
                outClusters.vcns.push_back(currentVcn + c);
                outClusters.lcns.push_back(lcn + c);
            }
            currentVcn = nextVcn;
        }

        LONGLONG lastNextVcn = pRet->Extents[pRet->ExtentCount - 1].NextVcn.QuadPart;
        if (lastNextVcn <= inBuf.StartingVcn.QuadPart) {
            break;
        }
        inBuf.StartingVcn.QuadPart = lastNextVcn;
    }
    return true;
}

// Move a single cluster from srcVcn to dstLcn in the target file (via FSCTL_MOVE_FILE)
bool MoveSingleCluster(HANDLE volumeHandle,
                       HANDLE fileHandle,
                       LONGLONG srcVcn,
                       LONGLONG dstLcn) {
    MOVE_FILE_DATA moveData = {};
    moveData.FileHandle = fileHandle;
    moveData.StartingVcn.QuadPart = srcVcn; // which VCN in file to move
    moveData.StartingLcn.QuadPart = dstLcn; // destination LCN on disk
    moveData.ClusterCount = 1;              // move one cluster
    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        volumeHandle,
        FSCTL_MOVE_FILE,
        &moveData,
        sizeof(moveData),
        NULL,
        0,
        &bytesReturned,
        NULL);

    if (!ok) {
        PrintLastError(L"FSCTL_MOVE_FILE failed");
        return false;
    }
    return true;
}

// Fragment a single file by performing a number of random single-cluster moves
bool FragmentFileRandomly(const std::wstring &filePath,
                          HANDLE volumeHandle,
                          std::vector<BYTE> &volumeBitmap,
                          ULONGLONG totalClusters,
                          int movesToPerform) {
    // Open the file
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        PrintLastError((L"Failed to open file: " + filePath).c_str());
        return false;
    }

    FileClusters fc;
    if (!GetAllFileRetrievalPointers(hFile, fc)) {
        std::wcerr << L"Could not get retrieval pointers for file: " << filePath << L"\n";
        CloseHandle(hFile);
        return false;
    }

    if (fc.vcns.empty()) {
        std::wcerr << L"File has no allocated clusters: " << filePath << L"\n";
        CloseHandle(hFile);
        return false;
    }

    std::srand((unsigned)std::time(nullptr));
    for (int i = 0; i < movesToPerform; i++) {
        int randomIndex = std::rand() % (int)fc.vcns.size();
        LONGLONG srcVcn = fc.vcns[randomIndex];
        LONGLONG srcLcn = fc.lcns[randomIndex];
        // Find a free cluster
        ULONGLONG newLcn = 0;
        bool foundFree = false;
        const int RANDOM_ATTEMPTS = 2000;
        for (int attempt = 0; attempt < RANDOM_ATTEMPTS; attempt++) {
            ULONGLONG candidate = std::rand() % totalClusters;
            size_t byteIndex = (size_t)(candidate / 8);
            int bitOffset = (int)(candidate % 8);
            int bitVal = (volumeBitmap[byteIndex] >> bitOffset) & 1;
            if (bitVal == 0) {
                newLcn = candidate;
                foundFree = true;
                break;
            }
        }

        // Fallback linear search
        if (!foundFree) {
            for (ULONGLONG c = 0; c < totalClusters; c++) {
                size_t byteIndex = (size_t)(c / 8);
                int bitOffset = (int)(c % 8);
                int bitVal = (volumeBitmap[byteIndex] >> bitOffset) & 1;
                if (bitVal == 0) {
                    newLcn = c;
                    foundFree = true;
                    break;
                }
            }
        }

        if (!foundFree) {
            std::wcerr << L"Could not find a free cluster for file: " << filePath
                       << L" (volume may be nearly full)\n";
            CloseHandle(hFile);
            return false;
        }

        std::wcout << L"[File: " << filePath << L"] Move " << (i + 1)
                   << L"/" << movesToPerform << L": VCN=" << srcVcn
                   << L" (LCN=" << srcLcn << L") -> LCN=" << newLcn << L"\n";

        if (!MoveSingleCluster(volumeHandle, hFile, srcVcn, newLcn)) {
            std::wcerr << L"Cluster move failed for file: " << filePath << L"\n";
            continue;
        }
        // Update volume bitmap.
        {
            size_t oldByteIndex = (size_t)(srcLcn / 8);
            int oldBitOffset = (int)(srcLcn % 8);
            volumeBitmap[oldByteIndex] &= ~(1 << oldBitOffset);
            size_t newByteIndex = (size_t)(newLcn / 8);
            int newBitOffset = (int)(newLcn % 8);
            volumeBitmap[newByteIndex] |= (1 << newBitOffset);
        }
        fc.lcns[randomIndex] = newLcn;
    }
    CloseHandle(hFile);
    return true;
}

// Recursively fragment all files in a given directory
// For each file in the directory tree, perform 'movesPerFile' moves
bool FragmentAllFilesInDirectory(const std::wstring &dirPath,
                                 HANDLE volumeHandle,
                                 std::vector<BYTE> &volumeBitmap,
                                 ULONGLONG totalClusters,
                                 int movesPerFile) {

    std::wstring searchPath = dirPath;

    if (!searchPath.empty() && searchPath.back() != L'\\') {
        searchPath += L"\\";
    }

    searchPath += L"*"; // wildcard for all entries
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &ffd);

    if (hFind == INVALID_HANDLE_VALUE) {
        PrintLastError((L"FindFirstFileW failed on " + searchPath).c_str());
        return false;
    }

    bool success = true;

    do {
        std::wstring fileName = ffd.cFileName;
        if (fileName == L"." || fileName == L"..") {
            continue;
        }

        std::wstring fullPath = dirPath;

        if (!fullPath.empty() && fullPath.back() != L'\\') {
            fullPath += L"\\";
        }

        fullPath += fileName;

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wcout << L"Entering subdirectory: " << fullPath << std::endl;
            if (!FragmentAllFilesInDirectory(fullPath, volumeHandle, volumeBitmap, totalClusters, movesPerFile)) {
                std::wcerr << L"Failed to fragment subdirectory: " << fullPath << std::endl;
                success = false;
            }
        } else {
            std::wcout << L"Fragmenting file: " << fullPath << std::endl;
            if (!FragmentFileRandomly(fullPath, volumeHandle, volumeBitmap, totalClusters, movesPerFile)) {
                std::wcerr << L"FragmentFileRandomly failed on: " << fullPath << std::endl;
                success = false;
            }
        }
    } while (FindNextFileW(hFind, &ffd) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        PrintLastError(L"FindNextFileW ended unexpectedly");
        success = false;
    }

    FindClose(hFind);
    return success;
}

int main() {
    std::wcout << L"Attempting to enable SeManageVolumePrivilege...\n";
    if (!EnablePrivilege(L"SeManageVolumePrivilege")) {
        std::wcerr << L"Failed to enable SeManageVolumePrivilege. Try running as Administrator.\n";
    }

    // Ask for drive letter
    std::wstring driveLetter;
    std::wcout << L"Enter drive letter (e.g. C): ";
    std::wcin >> driveLetter;
    if (driveLetter.empty()) {
        std::wcerr << L"No drive letter provided.\n";
        return 1;
    }

    // Build paths
    std::wstring rootPath = driveLetter + L":\\";
    std::wstring volumePath = L"\\\\.\\" + driveLetter + L":";

    // Get volume geometry
    ULONGLONG totalClusters = 0;
    DWORD bytesPerCluster = 0;
    if (!GetVolumeClusterInfo(rootPath, totalClusters, bytesPerCluster)) {
        std::wcerr << L"GetVolumeClusterInfo failed.\n";
        return 1;
    }

    if (totalClusters == 0) {
        std::wcerr << L"Volume reports 0 clusters?\n";
        return 1;
    }

    std::wcout << L"Volume has " << totalClusters
               << L" clusters. Bytes/cluster = " << bytesPerCluster << L"\n";
            
    // Open the volume (with read/write access)
    HANDLE hVolume = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (hVolume == INVALID_HANDLE_VALUE) {
        PrintLastError((L"Failed to open volume " + volumePath).c_str());
        return 1;
    }

    // Retrieve the volume bitmap
    std::vector<BYTE> volumeBitmap;
    if (!GetVolumeBitmapChunked(hVolume, totalClusters, volumeBitmap)) {
        std::wcerr << L"GetVolumeBitmapChunked failed.\n";
        CloseHandle(hVolume);
        return 1;
    }

    std::wcout << L"Bitmap retrieved: " << volumeBitmap.size() << L" bytes.\n";
    ULONGLONG freeCount = 0;

    for (ULONGLONG c = 0; c < totalClusters; c++) {
        size_t byteIndex = (size_t)(c / 8);
        int bitOffset = (int)(c % 8);
        int bitVal = (volumeBitmap[byteIndex] >> bitOffset) & 1;
        if (bitVal == 0)
            freeCount++;
    }

    std::wcout << L"Free clusters: " << freeCount << L" / " << totalClusters << std::endl;

    // Ask how many moves per file
    int movesPerFile = 5;
    std::wcout << L"How many single-cluster moves to perform per file? (default = 5): ";
    std::wcin >> movesPerFile;
    std::wcout << L"Fragmenting entire volume (starting at " << rootPath << L")...\n";
    if (!FragmentAllFilesInDirectory(rootPath, hVolume, volumeBitmap, totalClusters, movesPerFile)) {
        std::wcerr << L"Fragmentation of the volume encountered errors.\n";
    } else {
        std::wcout << L"Fragmentation complete.\n";
    }

    CloseHandle(hVolume);
    std::wcout << L"\nDone. Press Enter to exit...";
    std::wcin.ignore(std::numeric_limits<std::streamsize>::max(), L'\n');
    std::wcin.get();
    return 0;
}

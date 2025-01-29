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

// Get volume geometry (#clusters, bytes/cluster)
static bool GetVolumeClusterInfo(const std::wstring &rootPath, ULONGLONG &totalClusters, DWORD &bytesPerCluster) {
    DWORD sectorsPerCluster = 0;
    DWORD bytesPerSector = 0;
    DWORD numberOfFreeClusters = 0;
    DWORD totalNumberOfClusters = 0;

    if (!GetDiskFreeSpaceW(rootPath.c_str(), &sectorsPerCluster, &bytesPerSector, &numberOfFreeClusters, &totalNumberOfClusters)) {
        PrintLastError(L"GetDiskFreeSpaceW failed");
        return false;
    }

    totalClusters = static_cast<ULONGLONG>(totalNumberOfClusters);
    bytesPerCluster = sectorsPerCluster * bytesPerSector;
    return true;
}


// Retrieve the entire NTFS volume bitmap in chunks
// Bits: 1=allocated, 0=free
bool GetVolumeBitmapChunked(HANDLE volumeHandle, ULONGLONG totalClusters, std::vector<BYTE> &outBitmap) {
    outBitmap.clear();
    // allocate for all clusters
    outBitmap.resize(static_cast<size_t>((totalClusters + 7) / 8), 0);

    STARTING_LCN_INPUT_BUFFER inBuf = {};
    inBuf.StartingLcn.QuadPart = 0; // begin at LCN=0

    // 64 KB chunk
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
                std::wcerr << L"Unexpected: success but not enough data for VOLUME_BITMAP_BUFFER\n";
            }
            break;
        }

        auto pVolBmp = reinterpret_cast<PVOLUME_BITMAP_BUFFER>(tempBuf.data());
        LONGLONG startLCN = pVolBmp->StartingLcn.QuadPart;
        LONGLONG chunkBits = pVolBmp->BitmapSize.QuadPart;

        // # bytes we'd expect
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

        // copy bits from chunk
        for (LONGLONG i = 0; i < chunkBits; i++) {
            LONGLONG clusterIndex = startLCN + i;
            if (clusterIndex >= (LONGLONG)totalClusters) {
                break;
            }
            int srcByteIndex = (int)(i / 8);
            int srcBitOffset = (int)(i % 8);
            int bitVal = (srcBits[srcByteIndex] >> srcBitOffset) & 1;
            // 1=allocated, 0=free

            if (bitVal == 1) {
                // set the bit in outBitmap
                size_t destByteIndex = (size_t)(clusterIndex / 8);
                int destBitOffset = (int)(clusterIndex % 8);
                outBitmap[destByteIndex] |= (BYTE)(1 << destBitOffset);
            }
        }

        // next iteration
        LONGLONG nextLCN = startLCN + chunkBits;

        if (!success) {
            // partial => ERROR_MORE_DATA
            if (dwErr == ERROR_MORE_DATA) {
                if (nextLCN > maxLCN)
                {
                    break;
                }
                inBuf.StartingLcn.QuadPart = nextLCN;
            } else {
                PrintLastError(L"FSCTL_GET_VOLUME_BITMAP truly failed");
                break;
            }
        } else {
            // success=TRUE => maybe final chunk
            if (chunkBits == 0) {
                break;
            }
            if (nextLCN > maxLCN) {
                break;
            }
            inBuf.StartingLcn.QuadPart = nextLCN;
        }
    }

    // return true if we got anything
    return !outBitmap.empty();
}

// Count how many free clusters (bit=0) in the bitmap
ULONGLONG CountFreeClusters(const std::vector<BYTE> &volumeBitmap, ULONGLONG totalClusters) {
    ULONGLONG freeCount = 0;
    for (ULONGLONG c = 0; c < totalClusters; c++) {
        size_t byteIndex = (size_t)(c / 8);
        int bitOffset = (int)(c % 8);
        int bitVal = (volumeBitmap[byteIndex] >> bitOffset) & 1;
        if (bitVal == 0) {
            freeCount++;
        }
    }
    return freeCount;
}

// Linear search for free clusters
std::vector<ULONGLONG> LinearFindFreeClusters(const std::vector<BYTE> &bitmap, ULONGLONG totalClusters, int howMany) {
    std::vector<ULONGLONG> out;
    out.reserve(howMany);

    for (ULONGLONG c = 0; c < totalClusters && (int)out.size() < howMany; c++) {
        size_t byteIndex = (size_t)(c / 8);
        int bitOffset = (int)(c % 8);
        int bitVal = (bitmap[byteIndex] >> bitOffset) & 1; // 1=allocated,0=free
        if (bitVal == 0) {
            out.push_back(c);
        }
    }
    return out;
}


// Random search for free clusters
std::vector<ULONGLONG> FindRandomFreeClusters(const std::vector<BYTE> &bitmap, ULONGLONG totalClusters, int howMany) {
    std::srand((unsigned)std::time(nullptr));
    std::vector<ULONGLONG> found;
    found.reserve(howMany);

    ULONGLONG attempts = 0;
    ULONGLONG maxAttempts = totalClusters * 10ULL; // to avoid infinite loops

    while ((int)found.size() < howMany && attempts < maxAttempts) {
        attempts++;
        // pick random
        ULONGLONG candidate = std::rand() % totalClusters;

        size_t byteIndex = (size_t)(candidate / 8);
        int bitOffset = (int)(candidate % 8);
        int bitVal = (bitmap[byteIndex] >> bitOffset) & 1;
        if (bitVal == 0) {
            found.push_back(candidate);
        }
    }

    return found;
}


// main
int main() {
    // 1) Ask for drive letter
    std::wstring driveLetter;
    std::wcout << L"Enter drive letter (e.g. C): ";
    std::wcin >> driveLetter;
    if (driveLetter.empty()) {
        std::wcerr << L"No drive letter.\n";
        return 1;
    }

    // Build paths
    std::wstring rootPath = driveLetter + L":\\";
    std::wstring volumePath = L"\\\\.\\" + driveLetter + L":";

    // 2) Get volume geometry
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
               << L" clusters. Bytes/cluster=" << bytesPerCluster << L"\n";

    // 3) Open volume
    HANDLE hVolume = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (hVolume == INVALID_HANDLE_VALUE) {
        PrintLastError((L"Failed to open volume " + volumePath).c_str());
        return 1;
    }

    // 4) Retrieve bitmap
    std::vector<BYTE> volumeBitmap;
    if (!GetVolumeBitmapChunked(hVolume, totalClusters, volumeBitmap)) {
        std::wcerr << L"GetVolumeBitmapChunked failed.\n";
        CloseHandle(hVolume);
        return 1;
    }
    CloseHandle(hVolume);

    std::wcout << L"Bitmap retrieved: " << volumeBitmap.size()
               << L" bytes.\n";

    // 5) Count free clusters
    ULONGLONG freeCount = CountFreeClusters(volumeBitmap, totalClusters);
    std::wcout << L"According to the bitmap, free clusters = "
               << freeCount << L" / " << totalClusters << std::endl;

    // 6) Linear search test
    const int NEEDED = 10;
    auto linearFound = LinearFindFreeClusters(volumeBitmap, totalClusters, NEEDED);
    if ((int)linearFound.size() < NEEDED) {
        std::wcout << L"Linear search found only " << linearFound.size()
                   << L" free clusters. Fewer than " << NEEDED << L".\n";
    }
    else {
        std::wcout << L"Linear search found " << linearFound.size()
                   << L" free clusters. First few:\n";
        for (int i = 0; i < NEEDED; i++) {
            std::wcout << L"  LCN=" << linearFound[i] << L"\n";
        }
    }

    // 7) Random search test
    auto randomFound = FindRandomFreeClusters(volumeBitmap, totalClusters, NEEDED);
    if ((int)randomFound.size() < NEEDED) {
        std::wcout << L"Random search found only " << randomFound.size()
                   << L" free clusters. Fewer than " << NEEDED << L".\n";
    } else {
        std::wcout << L"Random search found " << randomFound.size()
                   << L" free clusters. First few:\n";
        for (int i = 0; i < NEEDED; i++) {
            std::wcout << L"  LCN=" << randomFound[i] << L"\n";
        }
    }

    std::wcout << L"\nDone. Press Enter to exit...";
    std::wcin.ignore(std::numeric_limits<std::streamsize>::max(), L'\n');
    std::wcin.get();
    return 0;
}

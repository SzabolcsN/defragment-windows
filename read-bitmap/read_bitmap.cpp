#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include <limits>

// Helper to print a Windows error message
static void PrintLastError(const wchar_t *msgPrefix) {
    DWORD errCode = GetLastError();
    std::wcerr << msgPrefix << L" Error: " << errCode << std::endl;

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

// Helper to get total clusters and bytes-per-cluster for a given drive root path
// E.g. rootPath = L"C:\\"
static bool GetVolumeClusterInfo(
    const std::wstring &rootPath,
    ULONGLONG &totalClusters,
    DWORD &bytesPerCluster) {
    DWORD sectorsPerCluster = 0;
    DWORD bytesPerSector = 0;
    DWORD numberOfFreeClusters = 0;
    DWORD totalNumberOfClusters = 0;

    if (!GetDiskFreeSpaceW(
            rootPath.c_str(),
            &sectorsPerCluster,
            &bytesPerSector,
            &numberOfFreeClusters,
            &totalNumberOfClusters)) {
        PrintLastError(L"GetDiskFreeSpaceW failed");
        return false;
    }

    // Fill out parameters
    totalClusters = static_cast<ULONGLONG>(totalNumberOfClusters);
    bytesPerCluster = sectorsPerCluster * bytesPerSector;
    return true;
}

int main() {
    // 1) Ask for a drive letter (e.g. "C")
    std::wstring driveLetter;
    std::wcout << L"Enter drive letter (e.g. C): ";
    std::wcin >> driveLetter;

    // Construct root path => "C:\\"
    std::wstring rootPath = driveLetter + L":\\";

    // 2) Get volume geometry (#clusters, bytes/cluster)
    ULONGLONG totalClusters = 0;
    DWORD bytesPerCluster = 0;
    if (!GetVolumeClusterInfo(rootPath, totalClusters, bytesPerCluster)) {
        std::wcerr << L"Failed to get volume cluster info for " << rootPath << std::endl;
        return 1;
    }

    // The highest valid LCN is totalClusters - 1
    LONGLONG maxLCN = static_cast<LONGLONG>(totalClusters) - 1;
    std::wcout << L"Volume has " << totalClusters
               << L" clusters. Max LCN = " << maxLCN << std::endl;

    // 3) Construct the volume device path => "\\\\.\\C:"
    std::wstring volumePath = L"\\\\.\\" + driveLetter + L":";

    // 4) Open the volume
    HANDLE hVolume = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (hVolume == INVALID_HANDLE_VALUE) {
        PrintLastError((L"Failed to open volume " + volumePath).c_str());
        return 1;
    }

    // Prepare the input for FSCTL_GET_VOLUME_BITMAP
    STARTING_LCN_INPUT_BUFFER inBuf;
    inBuf.StartingLcn.QuadPart = 0; // begin at LCN=0

    // We will pick a 64KB buffer for our data
    std::vector<BYTE> outBuf(64 * 1024);

    // 5) Loop calling FSCTL_GET_VOLUME_BITMAP until we have covered all clusters
    while (true) {
        // Clear output buffer each iteration
        ZeroMemory(outBuf.data(), outBuf.size());

        DWORD bytesReturned = 0;

        BOOL success = DeviceIoControl(
            hVolume,
            FSCTL_GET_VOLUME_BITMAP,
            &inBuf,
            sizeof(inBuf),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &bytesReturned,
            NULL);

        DWORD dwErr = GetLastError();

        // Basic sanity check on returned data
        if (bytesReturned < sizeof(VOLUME_BITMAP_BUFFER)) {
            if (!success) {
                PrintLastError(L"FSCTL_GET_VOLUME_BITMAP failed (no valid header returned)");
            } else {
                std::wcerr << L"Unexpected: success but not enough data for VOLUME_BITMAP_BUFFER" << std::endl;
            }
            break;
        }

        auto pVolBmp = reinterpret_cast<PVOLUME_BITMAP_BUFFER>(outBuf.data());
        LONGLONG startLCN = pVolBmp->StartingLcn.QuadPart;     // typically matches inBuf.StartingLcn
        LONGLONG chunkClusters = pVolBmp->BitmapSize.QuadPart; // how many clusters are described in pVolBmp->Buffer

        // Each bit in pVolBmp->Buffer corresponds to one cluster (0=free, 1=allocated)
        // The number of bytes of actual bitmap bits is (chunkClusters + 7) / 8

        if (!success) {
            if (dwErr == ERROR_MORE_DATA) {
                // We got partial data, but it's still valid
                std::wcout << L"Partial data returned. StartingLCN="
                           << startLCN << L", chunkClusters="
                           << chunkClusters << std::endl;

                // Next LCN = start + #clusters we just got
                LONGLONG nextLCN = startLCN + chunkClusters;
                if (nextLCN > maxLCN) {
                    // We already covered all clusters, so stop
                    std::wcout << L"We have covered the volume. Done." << std::endl;
                    break;
                }

                // Move on to the next chunk
                inBuf.StartingLcn.QuadPart = nextLCN;
            } else {
                // Some real error
                PrintLastError(L"FSCTL_GET_VOLUME_BITMAP failed");
                break;
            }
        } else {
            // success == TRUE => typically means the final chunk or the entire bitmap

            std::wcout << L"Success (possibly final chunk). StartingLCN="
                       << startLCN << L", chunkClusters="
                       << chunkClusters << std::endl;

            if (chunkClusters == 0) {
                // 0 means no more data
                std::wcout << L"No more clusters to read. Done." << std::endl;
                break;
            }

            // Otherwise, we might have more clusters
            // Let's attempt to read further. However, often no ERROR_MORE_DATA
            // means we are at the end. If not, we can do:
            LONGLONG nextLCN = startLCN + chunkClusters;
            if (nextLCN > maxLCN) {
                // We covered the volume
                std::wcout << L"Reached or exceeded max LCN. Done." << std::endl;
                break;
            }
            inBuf.StartingLcn.QuadPart = nextLCN;

            // We will continue the loop in case the driver still has more to give
        }
    }

    CloseHandle(hVolume);
    std::cout << "Program finished successfully.\n" 
              << "Press Enter to exit...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
    return 0;
}

#include <windows.h>
#include <iostream>
#include <string>
#include <limits>

int main() {
    // Prompt user to type just "C"
    std::wstring driveLetter;
    std::wcout << L"Enter the drive letter (e.g. C): ";
    std::wcin >> driveLetter;

    // Construct the volume path: L"\\\\.\\C:"
    std::wstring volumePath = L"\\\\.\\" + driveLetter + L":";

    // Try opening the volume
    HANDLE hVolume = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hVolume == INVALID_HANDLE_VALUE) {
        DWORD dwError = GetLastError();
        std::cerr << "Failed to open volume " 
                  << std::string(driveLetter.begin(), driveLetter.end()) 
                  << ": Error = " << dwError << "\n";

        LPWSTR errText = nullptr;
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            dwError,
            0,
            (LPWSTR)&errText,
            0,
            NULL
        );
        if (errText) {
            std::wcerr << L"Reason: " << errText << std::endl;
            LocalFree(errText);
        }
        return 1;
    }

    std::cout << "Successfully opened volume "
              << std::string(driveLetter.begin(), driveLetter.end()) 
              << std::endl;

    CloseHandle(hVolume);
    std::cout << "Program finished successfully.\n"
              << "Press Enter to exit...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
    return 0;
}

# Volume Opener

This code demonstrates how to open a volume on a Windows system by using the **WinAPI**. The program allows the user to specify a drive letter (e.g., `C`) and attempts to open the corresponding volume (`\\.\C:`) with both **read** and **write** access. If successful, it reports success; otherwise, it provides detailed error information

---

**Error Examples**
   - `ERROR_ACCESS_DENIED (5)`: Insufficient permissions to access the volume
   - `ERROR_INVALID_NAME (123)`: Invalid or missing drive letter
   - `ERROR_SHARING_VIOLATION (32)`: The volume is in use and cannot be accessed

---

## How to Run
1. Compile the code using a C++ compiler with Windows API support (e.g., MSVC or MinGW)
2. Run the program as **Administrator** to avoid access issues
3. Enter a valid drive letter when prompted (e.g., `C`) and observe the output

# NTFS Free Clusters Finder

Retrieves the **NTFS volume bitmap** in chunks (via the `FSCTL_GET_VOLUME_BITMAP` Win32 API), analyzes how many clusters are free vs allocated and then demonstrates two ways to find free clusters:

1. **Linear search** (scanning from the beginning)
2. **Random search** of free clusters

> **Important**
> - You must run the program **as Administrator** on Windows
> - This sample code works **only on NTFS volumes**

---

## How It Works

1. **Get Volume Geometry**
   - Uses [`GetDiskFreeSpaceW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew) to retrieve the total number of clusters (`totalClusters`) and the size (`bytesPerCluster`)

2. **Open the Volume**
   - Constructs the volume path like `\\.\C:` for drive `C:`
   - Opens it with `CreateFileW` using `GENERIC_READ`
   - Requires **Administrator privileges** or it typically fails with `ERROR_ACCESS_DENIED`

3. **Retrieve the NTFS Bitmap**
   - Calls [`FSCTL_GET_VOLUME_BITMAP`](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap) in a **loop**, handling the case when `ERROR_MORE_DATA` indicates partial data
   - **Clamps** how many bits to parse based on how many bytes are actually returned (avoiding out-of-bounds reads if only partial chunk data is received)
   - Assembles all bits into a `std::vector<BYTE> volumeBitmap`, where each bit = 1 if allocated, 0 if free

4. **Count Free Clusters**
   - The function `CountFreeClusters` scans each bit in the `volumeBitmap`
   - This confirms how many clusters are free vs. allocated

5. **Linear Search**
   - `LinearFindFreeClusters` scans from LCN=0 upward until it finds the requested number of free clusters (up to `howMany`)

6. **Random Search**
   - `FindRandomFreeClusters` picks random LCN indices in `[0..totalClusters-1]`, checks if the bit is free, and gathers up to `howMany`
   - If the volume is **mostly free**, this should quickly find enough free clusters

7. **Output & Debug**  
   - Prints the total clusters, free cluster count, and shows results of both linear and random searches
   - Will say "Not enough free clusters found" if it fails to locate `howMany` free clusters (for example, if the volume is nearly full)

---

## References

- [Microsoft Docs: **FSCTL_GET_VOLUME_BITMAP**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)  
  Explanation of the FSCTL_GET_VOLUME_BITMAP control code used to retrieve the NTFS volume bitmap
- [Microsoft Docs: **DeviceIoControl**](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol)  
  Detailed documentation for the DeviceIoControl function used to send I/O control codes to a specified device
- [Microsoft Docs: **GetDiskFreeSpaceW**](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew)  
  Reference for GetDiskFreeSpaceW, which retrieves information about the file system, such as the total number of clusters and bytes per cluster
  
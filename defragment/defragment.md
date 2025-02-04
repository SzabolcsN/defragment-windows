# NTFS Free Clusters Finder and Volume **Defragmentation**
**Note: This is a simplified approach compared to advanced defragmentation tools. Real-world defragmenters may combine partial moves, shuffle other files around, or handle extremely large or compressed files differently**

This project demonstrates two low-level NTFS operations on Windows:

1. **NTFS Free Clusters Finder**  
   Retrieves the **NTFS volume bitmap** in chunks (via the `FSCTL_GET_VOLUME_BITMAP` Win32 API) and determines how many clusters are free (versus allocated)

2. **NTFS Volume Defragmentation**  
   Consolidates each file's data by relocating its clusters into a single contiguous block whenever possible, thereby improving overall contiguous space usage on the volume

> **Important**  
> - You **must run the program as Administrator** on Windows
> - The sample code works **only on NTFS volumes**
> - **WARNING:** Although this code aims to consolidate files, it still performs low-level cluster moves. **Use only on non-critical, test volumes** or virtual machines to avoid potential data loss if something unexpected occurs

---

## NTFS Free Clusters Finder

### How It Works

1. **Get Volume Geometry**
   - Uses [`GetDiskFreeSpaceW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew) to retrieve the total number of clusters (`totalClusters`) and the size (`bytesPerCluster`)

2. **Open the Volume**
   - Constructs the volume path like `\\.\C:` for drive `C:`
   - Opens it with `CreateFileW` using `GENERIC_READ` (or `GENERIC_READ | GENERIC_WRITE` for operations that need write access)
   - Requires **Administrator privileges**, otherwise, it fails with `ERROR_ACCESS_DENIED`

3. **Retrieve the NTFS Bitmap**
   - Calls [`FSCTL_GET_VOLUME_BITMAP`](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap) in a **loop**, handling the case when `ERROR_MORE_DATA` indicates that only partial data was returned.
   - **Clamps** how many bits to parse based on the actual returned bytes (to avoid out-of-bounds reads)
   - Assembles all bits into a `std::vector<BYTE> volumeBitmap`, where each bit equals **1** if allocated and **0** if free

4. **Count Free Clusters**
   - The function `CountFreeClusters` scans each bit in the `volumeBitmap`
   - This confirms how many clusters are free versus allocated

---

## NTFS Volume Defragmentation

### Overview of the Approach

1. **Enumerate Files**  
   - Recursively traverses the root directory using `FindFirstFileW` / `FindNextFileW` to gather every file path on the volume

2. **Retrieve File Extents**  
   - For each file, calls [`FSCTL_GET_RETRIEVAL_POINTERS`](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_retrieval_pointers) to obtain the mapping between its Virtual Cluster Numbers (VCNs) and Logical Cluster Numbers (LCNs)

3. **Check for Contiguity**  
   - Examines the file's physical clusters to see if each consecutive pair is strictly `(previous LCN + 1)`
   - Skips files that already occupy a contiguous region

4. **Locate a Single Large Free Block**  
   - Searches the volume bitmap for a contiguous run of free clusters large enough to hold the entire file
   - Uses a helper function to scan from the beginning until it finds a run that matches the file's cluster count

5. **Relocate All Clusters**  
   - If a sufficiently large run is found, each cluster is moved to that run with [`FSCTL_MOVE_FILE`](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_move_file)
   - As each move completes, the bitmap is updated so the old location becomes free and the new location becomes allocated

6. **If No Suitable Run Exists**  
   - The file is skipped if there is no contiguous free block matching its size
   - In more advanced scenarios, partial moves or disk rearrangement could be used, but this sample keeps the approach straightforward

---

## References

- [Microsoft Docs: **FSCTL_GET_VOLUME_BITMAP**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)  
- [Microsoft Docs: **FSCTL_GET_RETRIEVAL_POINTERS**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_retrieval_pointers)  
- [Microsoft Docs: **FSCTL_MOVE_FILE**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_move_file)  
- [Microsoft Docs: **DeviceIoControl**](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol)  
- [Microsoft Docs: **GetDiskFreeSpaceW**](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew)  

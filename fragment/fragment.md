# NTFS Free Clusters Finder and Volume Fragmentation

This project demonstrates two key low-level NTFS operations on Windows:

1. **NTFS Free Clusters Finder**  
   Retrieves the **NTFS volume bitmap** in chunks (via the `FSCTL_GET_VOLUME_BITMAP` Win32 API), analyzes how many clusters are free versus allocated, and then demonstrates two methods for finding free clusters:
   - **Linear search** (scanning from the beginning)
   - **Random search** of free clusters

2. **NTFS Volume Fragmentation**  
   Intentionally fragments the NTFS volume by randomly moving clusters of every file on the drive. This is essentially the reverse of defragmentation. It uses low-level NTFS I/O control codes (such as `FSCTL_GET_RETRIEVAL_POINTERS` and `FSCTL_MOVE_FILE`) to retrieve file extents and then relocate individual clusters to random free spaces on the disk

> **Important**  
> - You **must run the program as Administrator** on Windows
> - The sample code works **only on NTFS volumes**
> - **WARNING:** The fragmentation part intentionally disrupts file layouts. **Use only on non-critical, test volumes or virtual machines.** Incorrect use may lead to data loss or filesystem corruption

---

## NTFS Free Clusters Finder

### How It Works

1. **Get Volume Geometry**
   - Uses [`GetDiskFreeSpaceW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew) to retrieve the total number of clusters (`totalClusters`) and the size (`bytesPerCluster`)

2. **Open the Volume**
   - Constructs the volume path like `\\.\C:` for drive `C:`.
   - Opens it with `CreateFileW` using `GENERIC_READ` (or `GENERIC_READ | GENERIC_WRITE` for operations that need write access)
   - Requires **Administrator privileges**, otherwise, it fails with `ERROR_ACCESS_DENIED`

3. **Retrieve the NTFS Bitmap**
   - Calls [`FSCTL_GET_VOLUME_BITMAP`](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap) in a **loop**, handling the case when `ERROR_MORE_DATA` indicates that only partial data was returned.
   - **Clamps** how many bits to parse based on the actual returned bytes (to avoid out-of-bounds reads)
   - Assembles all bits into a `std::vector<BYTE> volumeBitmap`, where each bit equals **1** if allocated and **0** if free

4. **Count Free Clusters**
   - The function `CountFreeClusters` scans each bit in the `volumeBitmap`
   - This confirms how many clusters are free versus allocated

5. **Linear Search**
   - `LinearFindFreeClusters` scans from LCN=0 upward until it finds the requested number of free clusters (up to a specified count)

6. **Random Search**
   - `FindRandomFreeClusters` picks random LCN indices in `[0, totalClusters-1]`, checks if the bit is free, and gathers up to the specified number
   - If the volume is **mostly free**, this method should quickly locate enough free clusters

7. **Output & Debug**
   - The program prints the total clusters, free cluster count, and displays the results of both linear and random searches
   - It will note "Not enough free clusters found" if it fails to locate the requested number (e.g., if the volume is nearly full)

---

## NTFS Volume Fragmentation

The fragmentation part of this project purposefully disrupts the physical layout of files on an NTFS volume by randomly moving clusters to new locations. In essence, it “breaks up” a file into many non-contiguous pieces (i.e., it fragments the file). This is the opposite of what defragmentation tools attempt to do

### How It Works

1. **File Enumeration**
   - The program starts at the root directory of the given drive and recursively enumerates every file and subdirectory
   - It uses Win32 API functions such as `FindFirstFileW` and `FindNextFileW` to traverse the entire directory tree

2. **Retrieving File Extents**
   - For each file found, the program retrieves its cluster mapping using `FSCTL_GET_RETRIEVAL_POINTERS`
   - This operation returns the mapping between the file’s Virtual Cluster Numbers (VCNs) and its Logical Cluster Numbers (LCNs)
   - The code handles files with multiple extents and even those requiring multiple calls to gather all extents

3. **Random Cluster Moves**
   - For each file, the program randomly selects one or more clusters from its allocated extents
   - For every selected cluster, a free cluster is identified by scanning the NTFS volume bitmap using both random and fallback linear searches
   - A free cluster is then chosen for relocation

4. **Fragmenting the File**
   - The program uses `FSCTL_MOVE_FILE` to move the selected cluster from its original location (source LCN) to the free cluster (destination LCN)
   - This move is done on a per-cluster basis (i.e., moving one cluster at a time), and the process is repeated for a specified number of moves per file
   - Repeating this process causes the file’s data to be spread out across the volume

5. **Volume-Wide Fragmentation**
   - The fragmentation routine is applied to **every file** on the volume (by recursively processing the entire directory tree), leading to a highly fragmented drive
   - **Note:** This process is dangerous and can lead to severe fragmentation or even data loss. **Use it only in a controlled test environment**

### Important Considerations

- **Administrator Privileges:**  
  Running the fragmentation routine requires Administrator rights and the `SeManageVolumePrivilege`. Without these, operations like `FSCTL_MOVE_FILE` will fail

- **Risk of Data Loss:**  
  Since the fragmentation process involves low-level modifications to file clusters, an error or interruption during the process may corrupt files or the entire filesystem

- **Testing Environment:**  
  It is strongly recommended to run this on a non-critical test volume (such as a virtual machine or a disposable partition) rather than on a production system

---

## References

- [Microsoft Docs: **FSCTL_GET_VOLUME_BITMAP**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)  
  Explanation of the control code used to retrieve the NTFS volume bitmap
- [Microsoft Docs: **DeviceIoControl**](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol)  
  Detailed documentation for the DeviceIoControl function
- [Microsoft Docs: **GetDiskFreeSpaceW**](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew)  
  Reference for retrieving file system information such as cluster counts and sizes
- [Microsoft Docs: **FSCTL_GET_RETRIEVAL_POINTERS**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_retrieval_pointers)  
  Documentation for retrieving file extents
- [Microsoft Docs: **FSCTL_MOVE_FILE**](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_move_file)  
  Details the control code used for moving file clusters on disk

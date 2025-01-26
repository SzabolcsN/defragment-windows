# Reading the Entire NTFS Volume Bitmap

This code demonstrates how to **retrieve the entire NTFS volume bitmap** (the metadata indicating which clusters are free or allocated) using **Windows I/O control** (`FSCTL_GET_VOLUME_BITMAP`). The bitmap is retrieved in **chunks** to avoid overloading memory, and the process continues until all clusters are covered

## Key Features

1. **Prompt for a Drive Letter**  
   - The user is asked for a drive letter (e.g., `"C"`) to analyze the corresponding volume

2. **Get Volume Cluster Information**  
   - The program retrieves the total number of clusters and cluster size for the specified volume using [`GetDiskFreeSpaceW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew)

3. **Open the Volume**  
   - The volume is opened using [`CreateFileW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) with:
     - `GENERIC_READ` access for reading the bitmap
     - `FILE_SHARE_READ | FILE_SHARE_WRITE` to avoid locking the volume for other processes
   - If the handle is invalid, an error is displayed, and the program exits

4. **Prepare Input/Output Buffers**  
   - A `STARTING_LCN_INPUT_BUFFER` is used to specify the starting cluster (`StartingLcn`) for the request, initialized to `0`
   - An output buffer (`std::vector<BYTE>`) is allocated to store the bitmap data. In this case, a **64 KB buffer** is used for each chunk

5. **Loop to Retrieve Bitmap Chunks**  
   - The program calls [`DeviceIoControl`](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol) in a loop, requesting a portion of the bitmap in each iteration:
     - On success, the bitmap data is parsed
     - If the system returns `ERROR_MORE_DATA`, the program continues to the next chunk by updating `StartingLcn`
     - The loop stops when all clusters are covered or a critical error occurs

6. **Parse the Volume Bitmap**  
   - The bitmap data in the output buffer is interpreted as a `VOLUME_BITMAP_BUFFER` structure:
     - `StartingLcn`: The starting cluster number of the chunk
     - `BitmapSize.QuadPart`: The number of clusters described in the chunk
     - `Buffer[]`: The actual bitmap bits where each bit corresponds to a cluster (`1` = allocated, `0` = free)
   - This program prints summary information but does not interpret the individual bits

7. **Stop When the Bitmap Is Fully Covered**  
   - The loop stops if:
     - The `StartingLcn` exceeds the highest valid cluster number (`totalClusters - 1`)
     - `BitmapSize.QuadPart == 0`, meaning there are no more clusters to describe

8. **Close the Handle**  
   - The program closes the volume handle with `CloseHandle(hVolume)`

---

## Key Steps in the Process

### Step 1: Query Volume Information
The program uses `GetDiskFreeSpaceW` to get the following:
- Total number of clusters
- Bytes per cluster
- Highest valid logical cluster number (`MaxLCN = totalClusters - 1`)

This ensures the program doesn't request clusters beyond the end of the volume, avoiding errors like `ERROR_INVALID_PARAMETER (87)`.

---

### Step 2: Call `DeviceIoControl` in a Loop
The loop repeatedly calls `DeviceIoControl` with:
- `FSCTL_GET_VOLUME_BITMAP` as the control code
- The starting cluster (`StartingLcn`) to retrieve the next chunk of the bitmap

Each call handles:
- **Success**: The data chunk is processed, and the starting cluster is updated for the next request
- **Partial Data**: `ERROR_MORE_DATA` indicates that more data exists, and the program continues to retrieve the next chunk
- **Stop Conditions**:
  - When `BitmapSize.QuadPart == 0` (no more data)
  - When the next starting cluster (`StartingLcn`) exceeds the total clusters

---

### Step 3: Interpret the Bitmap Data
The program extracts:
- `StartingLcn`: Where the chunk starts
- `BitmapSize.QuadPart`: Number of clusters covered in the chunk
- `Buffer[]`: The bitmap data itself (not fully analyzed in this example)

Each bit in the bitmap corresponds to a cluster:  
- `1` = Cluster is allocated
- `0` = Cluster is free

---

## Key Points to Note

1. **Administrator Privileges**  
   - Accessing the volume bitmap usually requires **elevated privileges**. Run the program as Administrator

2. **NTFS-Only**  
   - `FSCTL_GET_VOLUME_BITMAP` works only on volumes that support a bitmap (e.g., NTFS, ReFS). It fails with `ERROR_INVALID_PARAMETER (87)` on non-supported file systems (e.g., FAT32, exFAT)

3. **Large Volumes**  
   - For very large volumes, the program retrieves the bitmap in multiple chunks. Ensure the buffer size is appropriate to reduce the number of iterations

4. **Stopping Conditions**  
   - The program ensures it stops once all clusters are covered, avoiding unnecessary calls to `FSCTL_GET_VOLUME_BITMAP`

---

## Output
For each chunk, the program prints:
- Starting Logical Cluster Number (`StartingLCN`)
- The number of clusters described in the chunk (`BitmapSize.QuadPart`)
- Whether it's a partial chunk (via `ERROR_MORE_DATA`) or the final chunk

At the end, the program prints a message confirming the bitmap is fully read

---

## References
- [DeviceIoControl function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol)  
- [FSCTL_GET_VOLUME_BITMAP](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)  
- [VOLUME_BITMAP_BUFFER structure](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-volume_bitmap_buffer)  
- [CreateFileW function](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)  
- [GetDiskFreeSpaceW function](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacew)  

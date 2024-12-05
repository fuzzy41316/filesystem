#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "wfs.h"

///////////////////////
// Global Variables //
/////////////////////
typedef enum {
    RAID_UNKNOWN = -1,
    RAID_0 = 0,
    RAID_1 = 1,
    RAID_1V = 2
} raid_mode_t;

/* RAID Explanations:
 * RAID 0 (Striping):
 *   - RAID 0 applied to only data block
 *   - 1 data stripe is 512B (first 512B written to disk 1, the next 512B written to disk 2, and so on)
 * RAID 1 (Mirroring):
 *   - RAID 1 applied to superblock, inode bitmap, data bitmap, inode blocks, and data blocks
 * RAID 1V (Mirroring with Versioning):
 *   - RAID 1V applied to superblock, inode bitmap, data bitmap, inode blocks, and data blocks
 *   - Read operations compare all copies of data blocks on different drives and return data block present on majority of drives
 *   - If tie, data block on with lower mount position is returned
 */


struct wfs_sb *superblock = NULL;  // Contains metadata about filesystem
int raid_mode = -1; // RAID mode
int disk_count = 0; // Number of disks in filesystem
char *disk_files[MAX_DISKS]; // Disk file names
size_t disk_size[MAX_DISKS]; // Disk file sizes
void *disk_mmap[MAX_DISKS]; // Mapped disk files

///////////////////////////
// Function Definitions //
/////////////////////////

static int wfs_getattr(const char* path, struct stat* stbuf);
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev);
static int wfs_mkdir(const char* path, mode_t mode);
static int wfs_unlink(const char* path);
static int wfs_rmdir(const char* path);
static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi);
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi);
static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi);

///////////////////////
// HELPER FUNCTIONS //
/////////////////////
int get_inode_num(const char *path);
static int create_new_entry(const char *path, mode_t mode);
struct wfs_inode *get_inode_from_num(int inode_num);
struct wfs_dentry *get_directory_entry(int block_number);
void set_bitmap_bit(char *bitmap, off_t index);
void clear_bitmap_bit(char *bitmap, off_t offset);
bool is_bitmap_bit_set(char *bitmap, off_t index);
static int allocate_data_block();
static off_t *get_block_ptr(struct wfs_inode *file_inode_ptr, off_t block_index, int allocate);
void allocate_raid1(void *disk_mmap[]);


// Allocate all data and metadata blocks for RAID 1V
void allocate_raid1(void *disk_mmap[])
{
    // In RAID 1 or 1v, all data and metadata blocks are mirrored
    // Disks have independent data bitmaps. This is consistent with what you have already implemented in mkfs.
    if (raid_mode == RAID_1 || raid_mode == RAID_1V)
    {
        // Copy superblock to all disks
        for (int i = 1; i < disk_count; i++)
            memcpy(disk_mmap[i], disk_mmap[0], BLOCK_SIZE);

        // Copy inode bitmap to all disks
        for (int i = 1; i < disk_count; i++)
            memcpy((char *)disk_mmap[i] + superblock->i_bitmap_ptr, (char *)disk_mmap[0] + superblock->i_bitmap_ptr, BLOCK_SIZE);

        // Copy data bitmap to all disks
        for (int i = 1; i < disk_count; i++)
            memcpy((char *)disk_mmap[i] + superblock->d_bitmap_ptr, (char *)disk_mmap[0] + superblock->d_bitmap_ptr, BLOCK_SIZE);

        // Copy inode blocks to all disks
        for (int i = 1; i < disk_count; i++)
            memcpy((char *)disk_mmap[i] + superblock->i_blocks_ptr, (char *)disk_mmap[0] + superblock->i_blocks_ptr, superblock->num_inodes * BLOCK_SIZE);

        // Copy data blocks to all disks
        for (int i = 1; i < disk_count; i++)
            memcpy((char *)disk_mmap[i] + superblock->d_blocks_ptr, (char *)disk_mmap[0] + superblock->d_blocks_ptr, superblock->num_data_blocks * BLOCK_SIZE);
    }
}

static off_t *get_block_ptr(struct wfs_inode *file_inode_ptr, off_t block_index, int allocate)
{
    printf("get_block_ptr: block_index = %ld, allocate = %d\n", block_index, allocate);

    if (block_index < D_BLOCK)
    {
        // Direct block access
        printf("Accessing direct block at index %ld\n", block_index);
        if (file_inode_ptr->blocks[block_index] == 0 && allocate)
        {
            printf("Direct block not allocated, attempting to allocate...\n");
            // Allocate a new data block if it's not already allocated
            int dataIndex = allocate_data_block();
            if (dataIndex == -ENOSPC)
            {
                printf("No space available for data block, returning NULL\n");
                // No space available for data block
                return NULL;
            }
            file_inode_ptr->blocks[block_index] = dataIndex;
            printf("Allocated new data block at index %d\n", dataIndex);
        }
        else if (file_inode_ptr->blocks[block_index] == 0)
            return NULL;
        return (off_t *)(file_inode_ptr->blocks[block_index] + (off_t)disk_mmap[0]);
    }
    else
    {
        // Indirect block access
        off_t indirect_block_index = block_index - D_BLOCK;
        printf("Accessing indirect block at index %ld\n", indirect_block_index);
        if (indirect_block_index >= BLOCK_SIZE / sizeof(off_t))
        {
            printf("Invalid block index, returning NULL\n");
            // Invalid block index
            return NULL;
        }

        // Check if the indirect block is allocated
        if (file_inode_ptr->blocks[IND_BLOCK] == 0)
        {
            if (!allocate)
            {
                printf("Indirect block not allocated and allocation not allowed, returning NULL\n");
                // Indirect block not allocated and we're not allowed to allocate
                return NULL;
            }

            printf("Indirect block not allocated, attempting to allocate...\n");
            // Allocate a new indirect block
            int ind_block = allocate_data_block();
            if (ind_block == -ENOSPC)
            {
                printf("No space available for indirect block, returning NULL\n");
                // No space available for indirect block
                return NULL;
            }
            file_inode_ptr->blocks[IND_BLOCK] = ind_block;
            // Clear the new indirect block
            memset((char *)((ind_block) + (off_t)disk_mmap[0]), 0, BLOCK_SIZE);
            printf("Allocated new indirect block at index %d\n", ind_block);
        }

        // Get the pointer to the indirect block
        off_t *indirect_block_ptr = (off_t *)((file_inode_ptr->blocks[IND_BLOCK]) + (off_t)disk_mmap[0]);

        // Check if the entry within the indirect block is allocated, if not, allocate a new data block
        if (indirect_block_ptr[indirect_block_index] == 0 && allocate)
        {
            printf("Indirect block entry not allocated, attempting to allocate...\n");
            int dataIndex = allocate_data_block();
            if (dataIndex == -ENOSPC)
            {
                printf("No space available for data block, returning NULL\n");
                // No space available for data block
                return NULL;
            }
            indirect_block_ptr[indirect_block_index] = dataIndex;
            printf("Allocated new data block at index %d for indirect block\n", dataIndex);
        }

        // Point to the data block within the indirect block
        return (off_t *)(indirect_block_ptr[indirect_block_index] + (off_t)disk_mmap[0]);
    }
}

// Helper function to allocate a new data block
static int allocate_data_block()
{
    // Get the data bitmap from the d_bitmap ptr + offset
    char *data_bitmap = (char *)superblock->d_bitmap_ptr + (off_t)disk_mmap[0];

    // Iterate through a superblocks data blocks to find one that is not set
    for (size_t i = 0; i < superblock->num_data_blocks; i++)
    {
        if (!is_bitmap_bit_set(data_bitmap, i))
        {
            set_bitmap_bit(data_bitmap, i);
            return (int)(i * BLOCK_SIZE) + (int)(superblock->d_blocks_ptr); // Return the block number
        }
    }
    return -ENOSPC; // No space left on device
}

// Helper function to set a bit in a bitmap
void set_bitmap_bit(char *bitmap, off_t index)
{
    bitmap[index / 8] |= (1 << (index % 8));    // Set the flag at the index
}

// Helper function to clear a bit in a bitmap
void clear_bitmap_bit(char *bitmap, off_t offset)
{
    bitmap[offset / 8] &= ~(1 << (offset % 8));  // Clear the flag at the offset
}

// Helper function to check if a bit is set in a bitmap
bool is_bitmap_bit_set(char *bitmap, off_t index)
{
    if (bitmap[index/8] & (1 << (index % 8))) // Check if the flag is set
        return true;
    else return false;
}

// Helper function to get an inode from an inode number
struct wfs_inode *get_inode_from_num(int inode_num)
{
    struct wfs_inode *inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + (inode_num * BLOCK_SIZE));
    return inode;
}

// Helper function to get a directory entry from a block number
struct wfs_dentry *get_directory_entry(int block_number)
{
    struct wfs_dentry *dentry = (struct wfs_dentry *)((char *)disk_mmap[0] + block_number);
    return dentry;
}


// Helper function to create a new entry in the filesystem
static int create_new_entry(const char *path, mode_t mode)
{
    printf("Creating new entry with path: %s\n", path); // Debugging statement

    // Get the parent's directory inode index
    char parent_path[strlen(path) + 1];
    strcpy(parent_path, path);
    char *final_slash = strrchr(parent_path, '/');

    if (final_slash == NULL)
        return -EINVAL; // Invalid path
    
    *final_slash = '\0'; // NULL terminate the parent path
    int parent_inode = get_inode_num(parent_path);
    if (parent_inode < 0)
        return -ENOENT; // Parent inode does not exist

    // Ensure that the directory is writable
    struct wfs_inode *parent_inode_ptr = get_inode_from_num(parent_inode);
    if (!(parent_inode_ptr->mode & S_IWUSR))
        return -EACCES; // No write permission
    
    printf("Trying to find an empty inode for new entry\n");
    // Find an empty inode 
    int new_inode_num = -1;
    for (size_t i = 0; i < superblock->num_inodes; i++)
    {
        if (!is_bitmap_bit_set((char *)((off_t)disk_mmap[0] + superblock->i_bitmap_ptr), i))
        {
            set_bitmap_bit((char *)((off_t)disk_mmap[0] + superblock->i_bitmap_ptr), i);
            new_inode_num = i;
            break;
        }
    }
    printf("Found empty inode: %d\n", new_inode_num);

    if (new_inode_num < 0)
        return -ENOSPC; // No space left on device

    // Create a new inode for the directory
    struct wfs_inode *new_inode = get_inode_from_num(new_inode_num);
    new_inode->num = new_inode_num;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;    // Initially zero because there's nothing in it
    new_inode->nlinks = 1;  // Initially one because it's the first link
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);

    printf("Trying to add new inode to parent directory\n");
    // Now add the inode to the directory
    int found = 0;
    for (int i = 0; i < N_BLOCKS; i++)
    {
        // Handle an existing block
        if (parent_inode_ptr->blocks[i] != 0)
        {
            printf("Parent inode has existing block\n");
            struct wfs_dentry *dentries = get_directory_entry(parent_inode_ptr->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentries[j].num == 0)
                {
                    if (snprintf(dentries[j].name, MAX_NAME, "%s", final_slash + 1) >= MAX_NAME)
                        return -ENAMETOOLONG; // Name too long
                    dentries[j].num = new_inode_num;
                    parent_inode_ptr->mtim = time(NULL);
                    found = 1 ;
                    break;
                    printf("Added new inode to parent directory for existing block\n");
                }
            }
            if (found)
                break;
        }
        // Otherwise allocate a new data block for the inode
        else
        {
            printf("Parent inode does not have existing block\n");
            printf("Allocating new data block for parent inode\n");

            off_t block_num = allocate_data_block();
            if (block_num < 0)
                return -ENOSPC; // No space left on device
            printf("Allocated new data block for parent inode: %ld\n", block_num);

            parent_inode_ptr->blocks[i] = block_num;
            struct wfs_dentry *dentries = get_directory_entry(block_num);
            if (snprintf(dentries[0].name, MAX_NAME, "%s", final_slash + 1) >= MAX_NAME)
                return -ENAMETOOLONG; // Name too long
            dentries[0].num = new_inode_num;
            parent_inode_ptr->mtim = time(NULL);
            found = 1;
            printf("Added new inode to parent directory for new block\n");
        }
        if (found)
            break;
    }

    if (found < 0)
        return -ENOSPC; // No space left on device
    return 0;
}



int get_inode_num(const char *path)
{
    if (strlen(path) == 0)
    {
        return 0;
    }

    if (strcmp(path, "/") == 0)
    { // Root inode
        printf("returned root\n");
        return 0;
    }

    // Traverse the path to find the corresponding inode
    char *token;
    char *path_copy = strdup(path);
    if (path_copy == NULL)
    {
        printf("Memory allocation failed for path copy.\n");
        return -ENOMEM; // Use standard error code for no memory
    }
    int final_found = 0;
    int found = 0;
    int inodeIndex = 0; // Start from the root inode
                        //   int subInodes[D_BLOCK] = {0};
    char *last_slash = strrchr(path, '/');
    struct wfs_inode *inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr);
    while ((token = strtok_r(path_copy, "/", &path_copy)))
    {
        found = 0;
        printf("Processing token: %s\n", token);
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (inode->blocks[i] != 0)
            {
                struct wfs_dentry *entry = get_directory_entry(inode->blocks[i]);
                for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
                {
                    printf("Comparing Entry:%s Token:%s\n", entry[j].name, token);
                    if (strcmp(entry[j].name, token) == 0)
                    {
                        if (strcmp(last_slash + 1, entry[j].name) == 0)
                        {
                            final_found = 1;
                        }
                        printf("Match found for entry: %s\n", entry[j].name);
                        inode = get_inode_from_num(entry[j].num);
                        inodeIndex = entry[j].num;
                        found = 1;
                        break;
                    }
                }
            }
            if (found == 1)
                break;
        }
        if (final_found == 1)
            break;
        
    }
    if (found == 0 || final_found == 0 || !is_bitmap_bit_set((char *)(superblock->i_bitmap_ptr + (off_t)disk_mmap[0]), inode->num))
    {
        printf("Inode not found or invalid.\n");
        return -ENOENT;
    }

    printf("Inode details:\n");
    printf("Inode number: %d\n", inode->num);
    printf("Inode mode: %d\n", inode->mode);
    printf("Inode links: %d\n", inode->nlinks);
    printf("Inode size: %ld\n", inode->size);
    // free(path_copy);
    return inodeIndex;
}


//////////////////////////////
// END OF HELPER FUNCTIONS //
////////////////////////////

/////////////////////
// FUSE FUNCTIONS //
///////////////////

/* 
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. 
 * For the given pathname, this should fill in the elements of the "stat" structure. 
 * - st_uid
 * - st_gid
 * - st_atime
 * - st_mtime
 * - st_mode
 * - st_size
 */
static int wfs_getattr(const char* path, struct stat* stbuf)
{
    printf("wfs_getattr called with path: %s\n", path); // Debugging statement  

    printf("Getting inode number for path: %s\n", path);

    int inode_num = get_inode_num(path);
    if (inode_num < 0)
        return -ENOENT; // Inode does not exist
    
    printf("Successfully got inode number: %d\n", inode_num);

    // Get inode from the number
    struct wfs_inode *inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + (inode_num * BLOCK_SIZE));

    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_mode = inode->mode;
    stbuf->st_size = inode->size;

    return 0;
}

/* 
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. 
 * This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems. 
 */
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    printf("wfs_mknod called with path: %s\n", path);       // Debugging statement

    int res = create_new_entry(path, mode);
    if (res < 0)
        return res;

    allocate_raid1(disk_mmap);
    
    return 0;
}

/* 
 * Create a directory with the given name. 
 * Directories may contain blank entries up to the size as marked in the directory inode. 
 * That is, a directory with size 512 bytes will use one data block, both of the following entry layouts would be legal -- 15 blank entries followed by a valid directory entry, 
 *      or a valid directory entry followed by 15 blank entries.
 * You should free all directory data blocks with rmdir, but you do not need to free directory data blocks when unlinking files in a directory.
 * A valid file/directory name consists of letters (both uppercase and lowercase), numbers, and underscores (_). 
 * Path names are always separated by forward-slash. You do not need to worry about escape sequences for other characters.
 */
static int wfs_mkdir(const char* path, mode_t mode)
{
    printf("wfs_mkdir called with path: %s\n", path);       // Debugging statement

    
    printf("Attempting to create new directory: %s\n", path);
    int res = create_new_entry(path, mode | S_IFDIR);
    if (res < 0)
        return res;
    printf("Successfully created new directory: %s\n", path);

    allocate_raid1(disk_mmap);

    return 0;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, 
 * unlink only deletes the data when the last hard link is removed. See unlink(2) for details. 
 */
static int wfs_unlink(const char* path)
{
    printf("wfs_unlink called with path: %s\n", path);      // Debugging statement

    int file_inode = get_inode_num(path);
    if (file_inode < 0)
        return -ENOENT; // File not found
    

    struct wfs_inode *file_inode_ptr = get_inode_from_num(file_inode);
    // Ensure that the file is writable
    if (!(file_inode_ptr->mode & S_IWUSR))    
        return -EACCES; // File not writable

    // Release data blocks associated with the file inode
    for (size_t i = 0; i < D_BLOCK; i++)
    {
        if (file_inode_ptr->blocks[i] != 0)
        {
            // Clear the data block from the data bitmap
            memset((char *)file_inode_ptr->blocks[i] + (off_t)disk_mmap[0], 0, BLOCK_SIZE);

            clear_bitmap_bit((char *)superblock->d_bitmap_ptr + (off_t)disk_mmap[0], (file_inode_ptr->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);
            // Reset the data block in the inode
            file_inode_ptr->blocks[i] = 0;
        }
    }
    
    // Clear the inode from the inode bitmap
    if (file_inode_ptr->blocks[IND_BLOCK] != 0)
    {
        off_t *indirect = (off_t *)(file_inode_ptr->blocks[IND_BLOCK] + (off_t)disk_mmap[0]);

        // Release data blocks associated with the file inode
        for (size_t i = 0; i < BLOCK_SIZE / sizeof(off_t); i++)
        {
            if (indirect[i] == 0)
                continue;   // Skip empty blocks

            clear_bitmap_bit((char *)superblock->d_bitmap_ptr + (off_t)disk_mmap[0], (indirect[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);
            memset((char *)indirect[i] + (off_t)disk_mmap[0], 0, BLOCK_SIZE);
        }

        // Clear the indirect block from the data bitmap
        memset(indirect, 0, BLOCK_SIZE);
        clear_bitmap_bit((char *)superblock->d_bitmap_ptr + (off_t)disk_mmap[0], (file_inode_ptr->blocks[IND_BLOCK] - superblock->d_blocks_ptr) / BLOCK_SIZE);
    }

    allocate_raid1(disk_mmap);
    return 0;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
 */
static int wfs_rmdir(const char* path)
{
    printf("wfs_rmdir called with given directory: %s\n", path);       // Debugging statement

    char *final_slash = strrchr(path, '/');
    if (final_slash != NULL && strcmp(final_slash + 1, ".") == 0)
        return -EINVAL; // Cannot remove current directory

    // Get the inode index of the directory to remove
    int dir_inode = get_inode_num(path);
    if (dir_inode < 0)
        return -ENOENT; // Directory does not exist

    // Get the inode pointer from the inode number
    struct wfs_inode *dir_inode_ptr = get_inode_from_num(dir_inode);

    // Ensure that the directory is writable
    if (!(dir_inode_ptr->mode & S_IWUSR))
        return -EACCES; // No write permission

    // Iterate through data blocks of the directory, and clear any bitmap bits that are set
    for (int i = 0; i < D_BLOCK; i++)
    {
        if (dir_inode_ptr->blocks[i] != 0)
            clear_bitmap_bit((char *)superblock->d_bitmap_ptr + (off_t)disk_mmap[0], (dir_inode_ptr->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);
    }

    // Now remove the directory from the parent directory

    // Get the parent path, and find the last last object (last slash)
    char parent_path[strlen(path) + 1]; 
    strcpy(parent_path, path);
    char *last_slash = strrchr(parent_path, '/');
    
    if (last_slash == NULL)
        return -EINVAL; // Invalid path

    *last_slash = '\0'; // NULL terminate the parent path

    int parent_inode = get_inode_num(parent_path);
    if (parent_inode < 0)
        return -ENOENT; // Parent inode does not exist

    struct wfs_inode *parent_inode_ptr = get_inode_from_num(parent_inode);
    if (!(parent_inode_ptr->mode & S_IWUSR))
        return -EACCES; // No write permission

    int found = 0;
    for (size_t i = 0; i < D_BLOCK; i++)
    {
        struct wfs_dentry *entries = get_directory_entry(parent_inode_ptr->blocks[i]);

        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (entries[j].num != 0)
            {
                if (strcmp(entries[j].name, last_slash + 1) == 0)
                {
                    entries[j].num = 0;
                    memset(entries[j].name, 0, MAX_NAME);
                    found = 1;
                    break;
                }
            }
        }
        if (found) break; // Exit the loop if the entry is found
    }

    if (!found)
        return -ENOENT; // Entry not found

    // Finally, clear the bitmap bit of the directory inode
    clear_bitmap_bit((char *)superblock->i_bitmap_ptr + (off_t)disk_mmap[0], dir_inode);

    allocate_raid1(disk_mmap);

    return 0;
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. 
 * See read(2) for full details. Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. 
 * Required for any sensible filesystem.
 */
static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_read called with path: %s\n", path);      // Debugging statement

    // Get the file inode index from the path
    int inode_index = get_inode_num(path);
    if (inode_index < 0)
        return -ENOENT; // File does not exist
    
    // get the inode pointer from the number
    struct wfs_inode *inode_ptr = get_inode_from_num(inode_index);
    
    // Ensure that the file is a regular file
    if (!(inode_ptr->mode & S_IFREG))
        return -EISDIR; // Not a regular file
    
    // Ensure the offset is within the size of the disk
    if (offset >= inode_ptr->size)
        return 0; // Offset is at or beyond the end of the file
    
    // May have to adjust size
    size_t bytes_to_read = size;
    if (offset + size > inode_ptr->size)
        bytes_to_read = inode_ptr->size - offset;

    // Now read data from the file (in bytes)
    size_t bytes_read = 0;
    size_t remaining_bytes = bytes_to_read;
    off_t curr_offset = offset;

    while (bytes_read < bytes_to_read)
    {
        // Calculate the block index and offset within the block
        off_t block_index = curr_offset / BLOCK_SIZE;
        off_t block_offset = curr_offset % BLOCK_SIZE;

        // Check if we need to read from an indirect block
        off_t *block_ptr = get_block_ptr(inode_ptr, block_index, 0);
        if (block_ptr == NULL)
            return -EIO; // Error, block pointer was not allocated

        // Read data from the block
        size_t bytes_to_copy = BLOCK_SIZE - block_offset;
        if (bytes_to_copy > remaining_bytes)
            bytes_to_copy = remaining_bytes;

        memcpy(buf + bytes_read, ((char *)block_ptr + block_offset), bytes_to_copy);

        // Update the counters
        bytes_read += bytes_to_copy;
        remaining_bytes -= bytes_to_copy;
        curr_offset += bytes_to_copy;
    }

    if (raid_mode == RAID_1)
        allocate_raid1(disk_mmap);

    return bytes_read;
}

/* 
 * As for read above, except that it can't return 0.
 */
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_write called with path: %s\n", path);     // Debugging statement

    // Get the file inode index from the path
    int inode_index = get_inode_num(path);
    if (inode_index < 0)
        return -ENOENT; // File does not exist
    
    // get the inode pointer from the number
    struct wfs_inode *inode_ptr = get_inode_from_num(inode_index);
    
    // Ensure that the file is a regular file
    if (!(inode_ptr->mode & S_IFREG))
        return -EISDIR; // Not a regular file
    
    // Ensure the offset is within the size of the disk
    if (offset > inode_ptr->size)
        return -EFBIG; // Offset exceeds the file size
    
    // Calculate the new size of the file
    off_t new_size = offset + size;
    if (new_size > inode_ptr->size)
        inode_ptr->size = new_size;

    // Now write data to the file (in bytes)
    size_t bytes_writen = 0;
    size_t remaining_bytes = size;
    off_t curr_offset = offset;

    printf("Writing data to file: %s\n", path);
    while (bytes_writen < size)
    {
        // Calculate the block index and offset within the block
        off_t block_index = curr_offset / BLOCK_SIZE;
        off_t block_offset = curr_offset % BLOCK_SIZE;

        // Make sure file is not too small to write to
        if (block_index >= N_BLOCKS + BLOCK_SIZE / sizeof(off_t))
            return -EFBIG; // File too larg

        // Check if we need to read from an indirect block
        off_t *block_ptr = get_block_ptr(inode_ptr, block_index, 1);
        if (block_ptr == NULL)
            return -EIO; // Error, block pointer was not allocated

        // Write data from the block
        size_t bytes_to_copy = BLOCK_SIZE - block_offset;
        if (bytes_to_copy > remaining_bytes)
            bytes_to_copy = remaining_bytes;

        printf("Writing data to block at index: %ld\n", block_index);
        memcpy(((char *)block_ptr + block_offset), buf + bytes_writen, bytes_to_copy);

        // Update the counters
        bytes_writen += bytes_to_copy;
        remaining_bytes -= bytes_to_copy;
        curr_offset += bytes_to_copy;
    }

    inode_ptr->mtim = time(NULL);
    
    allocate_raid1(disk_mmap);

    return bytes_writen;
}

/* 
 * The readdir function is somewhat like read, in that it starts at a given offset and returns results in a caller-supplied buffer. However, the offset not a byte offset, and the results are a series of struct dirents rather than being uninterpreted bytes. 
 * To make life easier, FUSE provides a "filler" function that will help you put things into the buffer.
 * The general plan for a complete and correct readdir is:
 *      1. Find the first directory entry following the given offset (see below).
 *      2. Optionally, create a struct stat that describes the file as for getattr (but FUSE only looks at st_ino and the file-type bits of st_mode).
 *      3. Call the filler function with arguments of buf, the null-terminated filename, the address of your struct stat (or NULL if you have none), and the offset of the next directory entry.
 *      4. If filler returns nonzero, or if there are no more files, return 0.
 *      5. Find the next file in the directory.
 *      6. Go back to step 2.
 * From FUSE's point of view, the offset is an uninterpreted off_t (i.e., an unsigned integer). You provide an offset when you call filler, and it's possible that such an offset might come back to you as an argument later. 
 * Typically, it's simply the byte offset (within your directory layout) of the directory entry, but it's really up to you.
 * It's also important to note that readdir can return errors in a number of instances; in particular it can return -EBADF if the file handle is invalid, or -ENOENT if you use the path argument and the path doesn't exist.
 */
static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_readdir called with path: %s\n", path);   // Debugging statement

    // Get the inode index associated with the path
    int inode_num = get_inode_num(path);
    if (inode_num <0)
        return -ENOENT; // Path does not exist

    // Ensure given path is a directory
    if (inode_num < 0 || inode_num >= superblock->num_inodes)
        return -ENOENT; // Path does not exist

    struct wfs_inode *inode = get_inode_from_num(inode_num);
    if (!(inode->mode & S_IFDIR))
        return -ENOTDIR; // Not a directory

    // Now get the parent
    struct wfs_inode *parent_inode = get_inode_from_num(inode_num);
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (parent_inode->blocks[i] != 0)
        {
            struct wfs_dentry *dentries = get_directory_entry(parent_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentries[j].num != 0)
                {
                    filler(buf, dentries[j].name, NULL, 0);
                    printf("Added directory entry: %s\n", dentries[j].name);
                }
            }
        }
    }

    if (raid_mode == RAID_1)
        allocate_raid1(disk_mmap);
    return 0;
}


////////////////////////////
// END OF FUSE FUNCTIONS //
//////////////////////////

// FUSE operations structure
static struct fuse_operations ops = {
  .getattr = wfs_getattr,
  .mknod   = wfs_mknod,
  .mkdir   = wfs_mkdir,
  .unlink  = wfs_unlink,
  .rmdir   = wfs_rmdir,
  .read    = wfs_read,
  .write   = wfs_write,
  .readdir = wfs_readdir,
};

/*
 * ./wfs disk1 disk2 [FUSE options] mount_point
 * You need to pass [FUSE options] along with the mount_point to fuse_main as argv. You may assume -s is always passed to wfs as a FUSE option to disable multi-threading. 
 * We recommend testing your program using the -f option, which runs FUSE in the foreground. With FUSE running in the foreground, 
 * you will need to open a second terminal to test your filesystem. In the terminal running FUSE, printf messages will be printed to the screen. 
 * You might want to have a printf at the beginning of every FUSE callback so you can see which callbacks are being run.
 */
int main(int argc, char *argv[]) 
{
    printf("Starting main function...\n");

    // Need at least 2 disks and a mount point
    if (argc < 4)
    {
        printf("Usage: %s disk1 disk2 [FUSE options] mount_point\n", argv[0]);
        return 1;
    }
    
    // Count disks by checking if arguments are disk files
    disk_count = 0;
    int fuse_args_start = 1;
    printf("Counting disks...\n");
    while (fuse_args_start < argc && disk_count < MAX_DISKS)
    {
        // Check if argument starts with "-" (FUSE option)
        if (argv[fuse_args_start][0] == '-')
            break;  // Ignore FUSE options
        
        // Ignore mnt point
        if (strcmp(argv[fuse_args_start], argv[argc - 1]) == 0)
            break;

        // Store disk option
        disk_files[disk_count++] = argv[fuse_args_start++];
    }
    printf("Counting disk successful\n");

    // Open and mmap all disk files
    printf("Attempting to open and mmap disk files...\n");
    for (int i = 0; i < disk_count; i++)
    {
        // Open the disk for writing or reading
        int fd = open(disk_files[i], O_RDWR);
        if (fd == -1)
        {
            printf("Error opening file: %s\n", disk_files[i]);
            return 1;
        }

        // Get size of file for mmapping
        struct stat st;
        if (fstat(fd, &st) == -1)
        {
            printf("Error getting file size for file: %s\n", disk_files[i]);
            close(fd);
            return 1;
        }

        // Map the disk file into memory
        disk_size[i] = st.st_size;
        disk_mmap[i] = mmap(NULL, disk_size[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_mmap[i] == MAP_FAILED)
        {
            printf("Error mapping file into memory: %s\n", disk_files[i]);
            close(fd);
            return 1;
        }

        close(fd);  // Close file descriptor after mapping
    }
    printf("Disks opened and mmaped successfully\n");

    // Superblocks are mirrored across the disks, so just open from the first disk
    superblock = (struct wfs_sb *)disk_mmap[0];

    // Verify correct number of disks mounted
    if (disk_count != superblock->num_disks)
    {
        printf("Error: Wrong number of disks. Expected %d, got %d\n", superblock->num_disks, disk_count);
        return 1;
    }

    // Set RAID mode from superblock
    raid_mode = superblock->raid_mode;

    // Create new argv array for FUSE
    int fuse_argc = argc - fuse_args_start + 1; // +1 for argv[0]
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv)
    {
        printf("Error allocating memory for fuse_argv\n");
        return 1;
    }

    fuse_argv[0] = argv[0]; // Set the program name as the first argument

    // Copy FUSE arguments from argv to fuse_argv
    for (int i = fuse_args_start; i < argc; i++)
        fuse_argv[i - fuse_args_start + 1] = argv[i]; // +1 to skip argv[0]

    // Debug output
    printf("Superblock loaded:\n");
    printf("    RAID Mode: %d\n", superblock->raid_mode);
    printf("    # Inodes: %ld\n", superblock->num_inodes);
    printf("    # Data Blocks: %ld\n", superblock->num_data_blocks);
    printf("    Inode Bitmap Offset: %ld\n", superblock->i_bitmap_ptr);
    printf("    Data Bitmap Offset: %ld\n", superblock->d_bitmap_ptr);
    printf("    Inode Blocks Offset: %ld\n", superblock->i_blocks_ptr);
    printf("    Data Blocks Offset: %ld\n", superblock->d_blocks_ptr);
    printf("Mounting filesystem: Disks = %d, Mount point = %s\n", disk_count, argv[argc - 1]);


    // Start FUSE
    printf("Starting FUSE filesystem...\n");
    int ret = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    // Free the dynamically allocate fuse_argv
    free(fuse_argv);

    return ret;
}
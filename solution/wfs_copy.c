#define FUSE_USE_VERSION 30
#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <libgen.h>

#define MAX_DISKS 10  // Maximum number of disks supported

// RAID modes
typedef enum {
    RAID_UNKNOWN = -1,
    RAID_0 = 0,
    RAID_1 = 1,
    RAID_1V = 2
} raid_mode_t;


// Global variables for disks
int raid_mode = -1;
int disk_count = 0;
char *disk_files[MAX_DISKS];
size_t disk_size[MAX_DISKS];
void *disk_mmap[MAX_DISKS];

// Superblock
struct wfs_sb *superblock = NULL;

///////////////////////
// HELPER FUNCTIONS //
/////////////////////
int get_inode_from_path(const char *path);
struct wfs_dentry *get_directory_entry(const int block_num);
struct wfs_inode *get_inode(const int inode_num);
bool is_bitmap_bit_set(char *bitmap, off_t index);
static int create_new_entry(const char *path, mode_t mode);
int get_empty_inode();
void set_bitmap_bit(char *bitmap, off_t index);
static int add_directory_entry(struct wfs_inode *inode_ptr, const char *name, int inode_num, time_t curr_time);
static int allocate_data_block();
static int is_directory(int inode_idx);
static int release_data_blocks(struct wfs_inode *inode);
static void clear_bitmap_bit(char *bitmap, off_t bit_offset);
static int remove_directory_entry(const char *path);
static off_t *get_block_ptr(struct wfs_inode *inode_ptr, off_t block_index, int allocate);

/* Given a pointer to the inode, block index, and allocate flag, get the block pointer for the block index */
static off_t *get_block_ptr(struct wfs_inode *inode_ptr, off_t block_index, int allocate)
{
    // Direct block access
    if (block_index < D_BLOCK)
    {
        if (inode_ptr->blocks[block_index] == 0 && allocate)
        {
            int data_block_num = allocate_data_block();
            if (data_block_num < 0)
                return NULL;

            inode_ptr->blocks[block_index] = data_block_num;
        }
        else if (inode_ptr->blocks[block_index] == 0)
            return NULL;

        char *block_ptr = (char *)disk_mmap[0] + superblock->d_blocks_ptr + inode_ptr->blocks[block_index] * BLOCK_SIZE;
        return (off_t *)block_ptr;
    }
    else
    {
        // Indirect block handling...
        off_t indirect_block_index = block_index - D_BLOCK;

        if (indirect_block_index >= BLOCK_SIZE / sizeof(off_t))
            return NULL;

        if (inode_ptr->blocks[IND_BLOCK] == 0)
        {
            if (!allocate)
                return NULL;

            int indirect_block_num = allocate_data_block();
            if (indirect_block_num < 0)
                return NULL;

            inode_ptr->blocks[IND_BLOCK] = indirect_block_num;
            // Zero out the indirect block
            char *indirect_block_ptr = (char *)disk_mmap[0] + superblock->d_blocks_ptr + indirect_block_num * BLOCK_SIZE;
            memset(indirect_block_ptr, 0, BLOCK_SIZE);
        }

        // Get pointer to indirect block
        off_t *indirect_block_ptr = (off_t *)((char *)disk_mmap[0] + superblock->d_blocks_ptr + inode_ptr->blocks[IND_BLOCK] * BLOCK_SIZE);

        if (indirect_block_ptr[indirect_block_index] == 0 && allocate)
        {
            int data_block_num = allocate_data_block();
            if (data_block_num < 0)
                return NULL;

            indirect_block_ptr[indirect_block_index] = data_block_num;
        }
        else if (indirect_block_ptr[indirect_block_index] == 0)
            return NULL;

        char *block_ptr = (char *)disk_mmap[0] + superblock->d_blocks_ptr + indirect_block_ptr[indirect_block_index] * BLOCK_SIZE;
        return (off_t *)block_ptr;
    }
}
/* Remove the directory entry */
static int remove_directory_entry(const char *path)
{
    char parent_path[strlen(path) + 1]; // Make parent_path
    strcpy(parent_path, path);          // Assign path as parent
    char *final_slash = strrchr(parent_path, '/'); // Find the final slash

    if (final_slash == NULL)
        return -EINVAL; // Error if no slash found

    int parent_inode = get_inode_from_path(parent_path); // Get the parent inode
    if (parent_inode < 0)
        return -ENOENT; // Return error if parent inode is invalid
    
    struct wfs_inode *parent_inode_ptr = get_inode(parent_inode);
    if (!(parent_inode_ptr->mode & S_IWUSR))
        return -EACCES; // Return error if parent directory is not writable
    
    // Find the target entry
    int found = 0;
    for (size_t i = 0; i < D_BLOCK; i++)
    {
        struct wfs_dentry *entries = get_directory_entry(parent_inode_ptr->blocks[i]);

        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (entries[j].num == 0)
                continue;   // Skip if empty
            
            if (strcmp(entries[j].name, final_slash + 1) == 0)
            {
                // If found, delete it
                entries[j].num = 0; // Reset the entry
                memset(entries[j].name, 0, MAX_NAME);
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }
    if (!found)
        return -ENOENT; // Return error if not found
    return 0;   // Success otherwise
}


/* Clear the bitmap bit */
void clear_bitmap_bit(char *bitmap, off_t index)
{
    bitmap[index / 8] &= ~(1 << (index % 8));
}

/* Release data blocks associated with inode */
static int release_data_blocks(struct wfs_inode *inode)
{
    for (size_t i = 0; i < D_BLOCK; i++)
    {
        if (inode->blocks[i] == 0)
            continue;   // Skip if unused
        
        // Clear the data block from data bitmap
        memset((char *)inode->blocks[i] + superblock->d_blocks_ptr, 0, BLOCK_SIZE);
        clear_bitmap_bit((char *)disk_mmap[0] + superblock->d_bitmap_ptr, (inode->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);

        // Reset data blocks in the inode
        inode->blocks[i] = 0;
    }
    
    // Find any indirect blocks
    if (inode->blocks[IND_BLOCK] != 0)  
    {
        off_t *indirect = (off_t *)((char *)disk_mmap[0] + inode->blocks[IND_BLOCK]);

        for (size_t i = 0; i < BLOCK_SIZE / sizeof(off_t); i++)
        {
            if (indirect[0] == 0)
                continue;   // Skip if unused   
            
            // Clear the bitmap bit for indirect blocks
            clear_bitmap_bit((char *)disk_mmap[0] + superblock->d_bitmap_ptr, (indirect[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);

            // Reset to 0
            memset((char *)indirect[i] + superblock->d_blocks_ptr, 0, BLOCK_SIZE);
        }

        memset(indirect, 0, BLOCK_SIZE);
        clear_bitmap_bit((char *)disk_mmap[0] + superblock->d_bitmap_ptr, (inode->blocks[IND_BLOCK] - superblock->d_blocks_ptr) / BLOCK_SIZE);
    }

    return 0; // Success
}


/* Check if the given inode from the inode index is a directory or other file */
static int is_directory(int inode_idx)
{
    if (inode_idx < 0 || inode_idx >= superblock->num_inodes)
        return 0;   // Not a directory

    struct wfs_inode *inode = get_inode(inode_idx);
    return (inode->mode & S_IFDIR) != 0;
}

/* Given a block, allocate date for it */
static int allocate_data_block()
{
    char * d_bitmap = (char *)disk_mmap[0] + superblock->d_bitmap_ptr;

    for (size_t i = 0; i < superblock->num_data_blocks; i++)
    {
        if (!is_bitmap_bit_set(d_bitmap, i))
        {
            set_bitmap_bit(d_bitmap, i);

            // Zero out the allocated block
            int block_num = (int)i;
            char *block_ptr = (char *)disk_mmap[0] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE;
            memset(block_ptr, 0, BLOCK_SIZE);

            return block_num;  // Return block number
        }   
    }

    return -ENOSPC; // No space
}



/* Given an inode, name, inode number, and the current time, add a directory entry */
static int add_directory_entry(struct wfs_inode *inode_ptr, const char *name, int inode_num, time_t curr_time)
{
    // Check if name length exceeds MAX_NAME
    if (strlen(name) >= MAX_NAME)
        return -ENAMETOOLONG;  // Name too long

    // Iterate over the blocks of the inode
    for (int i = 0; i < N_BLOCKS; i++)
    {   
        if (inode_ptr->blocks[i] != 0)
        {
            struct wfs_dentry *entries = get_directory_entry(inode_ptr->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (entries[j].num == 0)
                {
                    // Found an empty entry
                    strncpy(entries[j].name, name, MAX_NAME);
                    entries[j].name[MAX_NAME - 1] = '\0';  // Ensure null termination
                    entries[j].num = inode_num;
                    inode_ptr->mtim = curr_time;    // Updated modified time
                    return 0;   // Success
                }
            }
        }
        else
        {   
            // Allocate a new data block
            int block_num = allocate_data_block();
            if (block_num < 0)
                return -ENOSPC; // No space

            inode_ptr->blocks[i] = block_num;  // Assign the block number

            struct wfs_dentry *entries = get_directory_entry(block_num);
            // Entries should already be zeroed out due to the zeroing in allocate_data_block

            // Add the new entry
            strncpy(entries[0].name, name, MAX_NAME);
            entries[0].name[MAX_NAME - 1] = '\0';  // Ensure null termination
            entries[0].num = inode_num;
            inode_ptr->mtim = curr_time;    // Updated modified time
            return 0;   // Success
        }
    }

    // Failure if no space
    return -ENOSPC;
}

/* Set the bitmap bit */
void set_bitmap_bit(char *bitmap, off_t index)
{
    bitmap[index / 8] |= (1 << (index % 8));
}

/* Find an empty inode to use for a new entry */
int get_empty_inode()
{   
    // Iterate through inodes in superblock to find an empty inode
    for (size_t i = 0; i < superblock->num_inodes; i++)
    {
        // Inode node not set???
        if (!is_bitmap_bit_set((char *)((char *)disk_mmap[0] + superblock->i_bitmap_ptr), i))
        {
            // Then set it
            set_bitmap_bit((char *)((char *)disk_mmap[0] + superblock->i_bitmap_ptr), i);
            return i;   // Success
        }
    }
    return -1;  // Failure
}

/* Given the path and mode, create a new entry*/
static int create_new_entry(const char *path, mode_t mode) 
{
    if (path == NULL || strlen(path) == 0) {
        fprintf(stderr, "Error: Path is empty\n");
        return -EINVAL;
    }

    char *path_copy = strdup(path);
    if (!path_copy) {
        return -ENOMEM;
    }

    char *parent_path = strdup(path);
    if (!parent_path) {
        free(path_copy);
        return -ENOMEM;
    }

    char *dir_name = dirname(parent_path);
    char *base_name = basename(path_copy);

    if (strlen(base_name) >= MAX_NAME) {
        free(path_copy);
        free(parent_path);
        return -ENAMETOOLONG;
    }

    int parent_inode = get_inode_from_path(dir_name);
    if (parent_inode < 0) {
        free(path_copy);
        free(parent_path);
        return parent_inode;  // Error code already set
    }

    struct wfs_inode *parent_inode_ptr = get_inode(parent_inode);
    if (!(parent_inode_ptr->mode & S_IFDIR)) {
        free(path_copy);
        free(parent_path);
        return -ENOTDIR;
    }

    if (!(parent_inode_ptr->mode & S_IWUSR)) {
        free(path_copy);
        free(parent_path);
        return -EACCES;
    }

    int new_inode_num = get_empty_inode();
    if (new_inode_num < 0) {
        free(path_copy);
        free(parent_path);
        return -ENOSPC;
    }

    struct wfs_inode *new_inode = get_inode(new_inode_num);
    memset(new_inode, 0, sizeof(struct wfs_inode));
    new_inode->num = new_inode_num;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;
    new_inode->nlinks = 1;

    time_t current_time = time(NULL);
    new_inode->atim = current_time;
    new_inode->mtim = current_time;
    new_inode->ctim = current_time;

    int res = add_directory_entry(parent_inode_ptr, base_name, new_inode_num, current_time);
    if (res < 0) {
        // Free the inode bitmap bit if failed to add directory entry
        clear_bitmap_bit((char *)disk_mmap[0] + superblock->i_bitmap_ptr, new_inode_num);
    }

    free(path_copy);
    free(parent_path);
    return res;
}


/* Given the index and bitmap, check if the bitmap is non-null */
bool is_bitmap_bit_set(char *bitmap, off_t index)
{
    return ((bitmap[index / 8] & (1 << (index % 8))) != 0);
}

/* From a block number, get the directory entry */
struct wfs_dentry *get_directory_entry(int block_num)
{
    return (struct wfs_dentry *)((char *)disk_mmap[0] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE);
}

/* Given a path, get the inode number*/
int get_inode_from_path(const char *path) {
    if (path == NULL || strlen(path) == 0) {
        fprintf(stderr, "Error: Path is empty\n");
        return -ENOENT;
    }

    if (strcmp(path, "/") == 0) {
        return 0; // Root inode number
    }

    char *path_copy = strdup(path);
    if (!path_copy) {
        return -ENOMEM;
    }

    int current_inode_num = 0; // Start from root inode
    struct wfs_inode *current_inode = get_inode(current_inode_num);

    char *token;
    char *saveptr;

    // Skip leading '/' in path
    while (*path_copy == '/') path_copy++;

    for (token = strtok_r(path_copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        int found = 0;

        // Iterate over blocks of current directory
        for (int i = 0; i < N_BLOCKS; i++) {
            if (current_inode->blocks[i] == 0)
                continue;

            struct wfs_dentry *entries = get_directory_entry(current_inode->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (entries[j].num == 0)
                    continue;

                if (strcmp(entries[j].name, token) == 0) {
                    current_inode_num = entries[j].num;
                    current_inode = get_inode(current_inode_num);

                    if ((current_inode->mode & S_IFDIR) == 0 && strtok_r(NULL, "/", &saveptr) != NULL) {
                        // Trying to navigate into a non-directory
                        free(path_copy);
                        fprintf(stderr, "Error: %s is not a directory\n", token);
                        return -ENOTDIR;
                    }

                    found = 1;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found) {
            free(path_copy);
            fprintf(stderr, "Error: Entry %s not found\n", token);
            return -ENOENT;
        }
    }
    free(path_copy);
    return current_inode_num;
}

/* Get the inode from the inode_number */
struct wfs_inode * get_inode(const int inode_num)
{
    // Calculate inode offset based on BLOCK_SIZE
    return (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + inode_num * BLOCK_SIZE);
}

//////////////////////////////
// END OF HELPER FUNCTIONS //
////////////////////////////

/*
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. For the given pathname, this should fill in the elements of the "stat" structure. 
 * If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf) {
    printf("wfs_getattr called with path: %s\n", path);

    memset(stbuf, 0, sizeof(struct stat));

    // Check if root directory is being requested
    if (strcmp(path, "/") == 0) 
    {
        struct wfs_inode *root_inode = get_inode(0); // Root inode number is 0
        if (root_inode == NULL) 
            return -ENOENT;
        
        stbuf->st_mode = root_inode->mode;
        stbuf->st_nlink = root_inode->nlinks;
        stbuf->st_uid = root_inode->uid;
        stbuf->st_gid = root_inode->gid;
        stbuf->st_size = root_inode->size;
        stbuf->st_atime = root_inode->atim;
        stbuf->st_mtime = root_inode->mtim;
        stbuf->st_ctime = root_inode->ctim;

        return 0;
    }

    // Retrieve inode number from path
    int inode_num = get_inode_from_path(path);
    if (inode_num < 0) {
        printf("wfs_getattr: Inode not found for path %s\n", path);
        return -ENOENT;
    }

    // Get inode
    struct wfs_inode *inode = get_inode(inode_num);
    if (!inode) {
        printf("wfs_getattr: Failed to get inode for inode number %d\n", inode_num);
        return -EIO;
    }

    // Fill `stbuf` with inode information
    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    printf("wfs_getattr: Attributes set for path %s\n", path);
    return 0;
}

/*
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems.
 *
*/ 
static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) 
{
    printf("mknod: %s\n", path);
    int res;

    // Some sort of error in creating entry, pipeline error code
    if ((res = create_new_entry(path, mode)) != 0)
        return res;

    return 0;   // Success otherwise
}

/*
 * Create a directory with the given name. The directory permissions are encoded in mode. See mkdir(2) for details. This function is needed for any reasonable read/write filesystem.
*/
static int wfs_mkdir(const char *path, mode_t mode) 
{
    printf("mkdir: %s\n", path);
    int res;

    // Set directory flag
    if ((res = create_new_entry(path, S_IFDIR | mode)) != 0)
        return res;

    return 0;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, unlink only deletes the data when the last hard link is removed. See unlink(2) for details.
*/
static int wfs_unlink(const char *path) 
{
    printf("unlink: %s\n", path);
    // Find the file inode to delete
    int inode_num = get_inode_from_path(path);
    if (inode_num < 0)
        return -ENOENT; // Not found

    // Get the inode from number
    struct wfs_inode *inode_ptr = get_inode(inode_num);

    // Make sure it's writabble
    if (!(inode_ptr->mode & S_IWUSR))
        return -EACCES; // Not writable
    
    int res = release_data_blocks(inode_ptr);  
    if (res < 0)
        return res; // Return error if data blocks not released
    
    res = remove_directory_entry(path);
    if (res < 0)
        return res; // Return error if directory entry not removed
    
    // After removing data blocks, and the inode fromt the directory, clear the bitmap bit
    clear_bitmap_bit((char *)superblock->i_bitmap_ptr + (off_t)(inode_num / 8), inode_num % 8);


    return -ENOSYS;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
*/
static int wfs_rmdir(const char *path) 
{
    printf("rmdir: %s\n", path);
    // Check that path is not current directory (. and ..)
    char *final_slash = strchr(path, '/');
    if (final_slash != NULL && strcmp(final_slash + 1, ".") == 0)
        return -EINVAL; // Error if path is current directory
    
    // Get the directory inode index to remove
    int directory_inode_num = get_inode_from_path(path);

    if (directory_inode_num < 0)
        return -ENOENT; // Not found

    // Get the inode pointer for directory
    struct wfs_inode *directory_inode_ptr = get_inode(directory_inode_num);

    // Make sure that the directory is writable before editing
    if (!(directory_inode_ptr->mode & S_IWUSR))
        return -EACCES; // Not writable
    
    // Iterate through data blocks to find the directory entries to remove
    for (int i = 0; i < D_BLOCK; i++)
    {
        if (directory_inode_ptr->blocks[i] == 0)
            continue;   // Skip if unused
        
        clear_bitmap_bit((char *)disk_mmap[0] + superblock->d_bitmap_ptr, (directory_inode_ptr->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE);
    }

    // Remove directory from parent directory
    int res = remove_directory_entry(path);
    if (res < 0)
        return res; // Return error if directory entry not removed
    
    clear_bitmap_bit((char *)disk_mmap[0] + superblock->i_bitmap_ptr, directory_inode_num);


    return 0;   // Success
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details. 
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) 
{
    printf("read: %s\n", path);

    // Get the file inode from path
    int inode_num = get_inode_from_path(path);
    if (inode_num < 0)
        return -ENOENT; // Not found

    // Get the inode pointer from the inode number
    struct wfs_inode *inode_ptr = get_inode(inode_num);

    // Check if regular file
    if (!(inode_ptr->mode & S_IFREG))
        return -EISDIR; // Not a regular file

    // Make sure offset is within file size
    if (offset >= inode_ptr->size)
        return 0;   // Offset is at or beyond the end of the file

    // Adjust size
    size_t bytes_to_read = size;
    if (offset + size > inode_ptr->size)
        bytes_to_read = inode_ptr->size - offset;
    
    // Now read data from the file
    size_t bytes_read = 0;
    size_t bytes_remaining = bytes_to_read;
    off_t curr_offset = offset;

    while (bytes_read < bytes_to_read)
    {
        // Caclulate the block number and offset within the block
        off_t block_index = curr_offset / BLOCK_SIZE;
        off_t block_offset = curr_offset % BLOCK_SIZE;

        // Check if block is an indirect block
        off_t *block_ptr = get_block_ptr(inode_ptr, block_index, 0);
        if (block_ptr == NULL)
            return -EIO; // Error if block is not found

        // Read the data from the block
        size_t bytes_to_copy = BLOCK_SIZE - block_offset;
        if (bytes_to_copy > bytes_remaining)
            bytes_to_copy = bytes_remaining;

        memcpy(buf + bytes_read, ((char *)block_ptr + block_offset), bytes_to_copy);

        // Update the variables
        bytes_read += bytes_to_copy;
        bytes_remaining -= bytes_to_copy;
        curr_offset += bytes_to_copy;
    }
    return bytes_read;
}

/*
 * As for read above, except that it can't return 0.
*/
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) 
{
    printf("write: %s\n", path);
    
    // Get the inodex index from the path
    int inode_num = get_inode_from_path(path);
    if (inode_num < 0)
        return -ENOENT; // Not found

    // Get the inode pointer from the inode number
    struct wfs_inode *inode_ptr = get_inode(inode_num);

    // Check if regular file
    if (!(inode_ptr->mode & S_IFREG))
        return -EISDIR; // Not a regular file

    // Check that offset is within size of inode
    if (offset > inode_ptr->size)
        return -EFBIG; // Invalid offset
    
    // Calculate new size of file after writing
    off_t new_size = offset + size;
    if (new_size > inode_ptr->size)
        inode_ptr->size = new_size;
    
    // Write data to new file
    size_t bytes_written = 0;
    size_t bytes_remaining = size;
    off_t curr_offset = offset;

    while (bytes_written < size)
    {
        // Calculate the block number and offset within the block
        off_t block_index = curr_offset / BLOCK_SIZE;
        off_t block_offset = curr_offset % BLOCK_SIZE;

        // Allocate new data block if nessecary 
        if (block_index >= N_BLOCKS + BLOCK_SIZE / sizeof(off_t))
            return -EFBIG; // File too large

        // Get the block pointer
        off_t *block_ptr = get_block_ptr(inode_ptr, block_index, 1);
        if (block_ptr == NULL)
            return -EIO; // Error if block is not found
        
        // Write data to the block
        size_t bytes_to_copy = BLOCK_SIZE - block_offset;
        if (bytes_to_copy > bytes_remaining)
            bytes_to_copy = bytes_remaining;

        memcpy((char *)block_ptr + block_offset, buf + bytes_written, bytes_to_copy);

        // Update the variables
        bytes_written += bytes_to_copy;
        bytes_remaining -= bytes_to_copy;
        curr_offset += bytes_to_copy;
    }

    return bytes_written;   // Success
}

/*
 * The readdir function is somewhat like read, in that it starts at a given offset and returns results in a caller-supplied buffer. 
 * However, the offset not a byte offset, and the results are a series of struct dirents rather than being uninterpreted bytes. To make life easier, FUSE provides a "filler" function that will help you put things into the buffer.

    1. Find the first directory entry following the given offset (see below).
    2. Optionally, create a struct stat that describes the file as for getattr (but FUSE only looks at st_ino and the file-type bits of st_mode).
    3. Call the filler function with arguments of buf, the null-terminated filename, the address of your struct stat (or NULL if you have none), and the offset of the next directory entry.
    4. If filler returns nonzero, or if there are no more files, return 0.
    5. Find the next file in the directory.
    6. Go back to step 2.

 * From FUSE's point of view, the offset is an uninterpreted off_t (i.e., an unsigned integer). You provide an offset when you call filler, and it's possible that such an offset might come back to you as an argument later. 
 * Typically, it's simply the byte offset (within your directory layout) of the directory entry, but it's really up to you.
 * It's also important to note that readdir can return errors in a number of instances; in particular it can return -EBADF if the file handle is invalid, or -ENOENT if you use the path argument and the path doesn't exist.
*/
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) 
{
    printf("readdir");

    // Find first directory
    int inode_idx = get_inode_from_path(path);
    if (inode_idx < 0)
        return -ENOENT; // Not found
    if (!is_directory(inode_idx))
        return -ENOTDIR; // Not a directory

    // Call filler function with the directory entries
    struct wfs_inode *inode = get_inode(inode_idx);
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // Iterate over the blocks of the directory
    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (inode->blocks[i] == 0)
            continue;   // Skip if unused

        struct wfs_dentry *entries = get_directory_entry(inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (entries[j].num == 0)
                continue;   // Skip if empty

            filler(buf, entries[j].name, NULL, 0);
        }   
    }

    return 0;   // Success
}


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

int main(int argc, char *argv[]) 
{
    printf("Starting main function\n");

    // Need at least 2 disks and a mount point
    if (argc < 4) {
        fprintf(stderr, "Usage: %s disk1 disk2 [FUSE options] mount_point\n", argv[0]);
        return 1;
    }

    // Count disks by checking if arguments are disk files
    disk_count = 0;
    int fuse_args_start = 1;
    while (fuse_args_start < argc && disk_count < MAX_DISKS) {
        // Check if argument starts with "-" (FUSE option)
        if (argv[fuse_args_start][0] == '-') {
            break;
        }
        disk_files[disk_count++] = argv[fuse_args_start++];
    }

    printf("Number of disks: %d\n", disk_count);

    // Open and mmap all disk files
    for (int i = 0; i < disk_count; i++) {
        int fd = open(disk_files[i], O_RDWR);
        if (fd == -1) {
            perror("Error opening file");
            return 1;
        }

        struct stat st;
        if (fstat(fd, &st) == -1) {
            perror("Error getting file size");
            close(fd);
            return 1;
        }

        disk_size[i] = st.st_size;
        disk_mmap[i] = mmap(NULL, disk_size[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_mmap[i] == MAP_FAILED) {
            perror("Error mapping file into memory");
            close(fd);
            return 1;
        }
        close(fd); // Can close fd after mmap
    }

    printf("Disks mapped successfully\n");

    // Get superblock from first disk
    superblock = (struct wfs_sb *)disk_mmap[0];
    
    // Verify correct number of disks mounted
    if (disk_count != superblock->num_disks) {
        fprintf(stderr, "Error: Wrong number of disks. Expected %d, got %d\n", 
                superblock->num_disks, disk_count);
        return 1;
    }

    // Set RAID mode from superblock
    raid_mode = superblock->raid_mode;

    // Create new argv array for FUSE
    int fuse_argc = argc - fuse_args_start + 1; // +1 for argv[0]
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "Error allocating memory for fuse_argv\n");
        return 1;
    }

    // Set the program name as the first argument
    fuse_argv[0] = argv[0];

    // Copy FUSE arguments from argv to fuse_argv
    for (int i = fuse_args_start; i < argc; i++) {
        fuse_argv[i - fuse_args_start + 1] = argv[i]; // Adjust index by +1
    }

    // Debug output
    printf("\n\n\nSuperblock loaded:\n");
    printf("    RAID Mode: %d\n", superblock->raid_mode);
    printf("    # Inodes: %ld\n", superblock->num_inodes);
    printf("    # Data Blocks: %ld\n", superblock->num_data_blocks);
    printf("    Inode Bitmap Offset: %ld\n", superblock->i_bitmap_ptr);
    printf("    Data Bitmap Offset: %ld\n", superblock->d_bitmap_ptr);
    printf("    Inode Blocks Offset: %ld\n", superblock->i_blocks_ptr);
    printf("    Data Blocks Offset: %ld\n\n\n", superblock->d_blocks_ptr);
    printf("Mounting filesystem: Disks = %d, Mount point = %s\n", disk_count, argv[argc - 1]);

    // Print arguments going into fuse_main
    printf("Arguments to fuse_main:\n");
    for (int i = 0; i < fuse_argc; i++) {
        printf("  fuse_argv[%d] = %s\n", i, fuse_argv[i]);
    }

    printf("Starting FUSE\n");

    // Initialize FUSE
    int ret = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    // Clean up
    free(fuse_argv);

    return ret;
}

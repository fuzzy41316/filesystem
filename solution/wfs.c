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
char *disk_mmap[MAX_DISKS]; // Mapped disk files
int current_disk = 0; // Current disk being accessed for RAID_0

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
struct wfs_inode *allocate_inode(char *i_bitmap_ptr);
void set_inode_bitmap(char *inode_bitmap_ptr, off_t inode_num);
bool is_inode_set(char *inode_bitmap_ptr, off_t inode_num);
void clear_inode_bitmap_bit(char *inode_bitmap_ptr, off_t inode_num);
char** split_path(const char* path, int* count);
struct wfs_inode *get_inode_from_path(const char* path);
static int update_parent_directory(struct wfs_inode *parent_inode, const char* file_name, struct wfs_inode *child_inode);
static int allocate_data_block(int disk_index);
static int create_new_file(const char* path, mode_t mode);
static int remove_file(const char *path);
void disk_stats();
void mirror_raid();

void mirror_raid()
{

    printf("Before mirroring metadata...\n");
    disk_stats();
    // Allocate this inode for all disks
    for (int i = 1; i < disk_count; i++)
    {
        // Mirror the inode_bitmap
        memcpy(disk_mmap[i] + superblock->i_bitmap_ptr, 
            disk_mmap[0] + superblock->i_bitmap_ptr, superblock->num_inodes / 8);

        // Mirror the actual inode blockss
        memcpy(disk_mmap[i] + superblock->i_blocks_ptr, 
            disk_mmap[0] + superblock->i_blocks_ptr, superblock->num_inodes * BLOCK_SIZE);

        if (raid_mode == RAID_1)
        {
            // Mirror the data_bitmap
            memcpy(disk_mmap[i] + superblock->d_bitmap_ptr, 
                disk_mmap[0] + superblock->d_bitmap_ptr, superblock->num_data_blocks / 8);

            // Mirror the data blocks
            memcpy(disk_mmap[i] + superblock->d_blocks_ptr, 
                disk_mmap[0] + superblock->d_blocks_ptr, superblock->num_data_blocks * BLOCK_SIZE);
        }
    }
    printf("After mirroring metadata...\n");
    disk_stats();
}

// Debugging function
void disk_stats()
{
    printf("Disk Stats: \n");
    printf("----------------\n");
    for (int i = 0; i < disk_count; i++)
    {
        printf("Disk %d: %s\n", i + 1, disk_files[i]);
        printf("Disk Location: %p\n", disk_mmap[i]);

        // Print inode bitmap stats
        printf("Inode Bitmap Stats:\n");
        for (size_t j = 0; j < superblock->num_inodes; j++) 
        {
            printf("-->%02x<-- ", (unsigned char)disk_mmap[i][superblock->i_bitmap_ptr + j]);
            if ((j + 1) % 16 == 0) printf("\n");
        }
        printf("\n");
        // Print inode block stats
        /*
        for (size_t j = 0; j < superblock->num_inodes; j++)
        {
            struct wfs_inode *inode = (struct wfs_inode *)(disk_mmap[i] + superblock->i_blocks_ptr + j * BLOCK_SIZE);
            printf("Inode %ld:\n", j);
            printf("    Num: %d\n", inode->num);
            printf("    Mode: %d\n", inode->mode);
            printf("    UID: %d\n", inode->uid);
            printf("    GID: %d\n", inode->gid);
            printf("    Size: %ld\n", inode->size);
            printf("    Nlinks: %d\n", inode->nlinks);
            printf("    Atime: %ld\n", inode->atim);
            printf("    Mtime: %ld\n", inode->mtim);
            printf("    Ctime: %ld\n", inode->ctim);
        }
        */

        // Print data block bitmap stats
        printf("Data Block Bitmap Stats:\n");
        for (size_t k = 0; k < superblock->num_data_blocks; k++) 
        {
            printf("-->%02x<-- ", (unsigned char)disk_mmap[i][superblock->d_bitmap_ptr + k]);
            if ((k + 1) % 16 == 0) printf("\n");
        }
        printf("\n");
    }
    printf("----------------\n");
}

static int remove_file(const char *path)
{
    struct wfs_inode *file = get_inode_from_path(path);
    if (file == NULL)
        return -ENOENT; // File doesn't exist
    
    if ((file->mode & S_IWUSR) != S_IWUSR)
        return -EACCES; // File is not writable
    
    if ((file->mode & S_IFDIR) == S_IFDIR && file->nlinks > 2)
        return -ENOTEMPTY; // Directory is not empty

    // Remove the current file from the parent directory
    printf("Acquiring parent directory to remove current file from...\n");
    char parent_path[strlen(path) + 1]; 
    strcpy(parent_path, path);
    char *last_slash = strrchr(parent_path, '/');     // Find the final occurrence of '/' in the path
    if (last_slash == NULL)
        return -EINVAL; // Invalid path
    if (last_slash == parent_path)
        strcpy(parent_path, "/"); // Root directory
    else
        *last_slash = '\0'; // Terminate the parent path
    printf("Acquired parent directory: %s\n", parent_path);
    printf("Child path: %s\n", path);

    // Find the parent directory inode from the parent path, and ensure it's writable and a valid directory
    struct wfs_inode *parent_directory_inode_ptr = get_inode_from_path(parent_path);
    if (parent_directory_inode_ptr == NULL)
        return -ENOENT; // Parent directory doesn't exist
    if ((parent_directory_inode_ptr->mode & S_IFDIR) != S_IFDIR)
        return -ENOTDIR; // Parent is not a directory
    if ((parent_directory_inode_ptr->mode & S_IWUSR) != S_IWUSR)
        return -EACCES; // Parent is not writable

    // Remove the directory entry from the parent directory
    int found = 0;
    for (size_t i = 0; i < D_BLOCK; i++)    // Directory entries are stored in the direct blocks
    {
        struct wfs_dentry *wfs_dentry_ptr = (struct wfs_dentry *)(disk_mmap[0] + parent_directory_inode_ptr->blocks[i]);

        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            // Compare the inode number in the parent directory, and if it matches the file inode number, remove it
            if (wfs_dentry_ptr[j].num == file->num)
            {
                printf("Removing directory entry of file from parent directory...\n");
                wfs_dentry_ptr[j].num = 0;
                memset(wfs_dentry_ptr[j].name, 0, MAX_NAME);
                parent_directory_inode_ptr->mtim = time(NULL);
                printf("Removed directory entry of file from parent directory\n");

                found = 1;
                break;
            }
        }
        if (found)
            break;
    }

    parent_directory_inode_ptr->nlinks--; // Decrement the number of links in the parent directory

    // Deallocate direct blocks
    for (size_t i = 0; i < D_BLOCK; i++)
    {
        if (file->blocks[i] != 0)
        {
            printf("Deallocating data block %ld...\n", file->blocks[i]);
            char *data_bitmap_ptr = disk_mmap[0] + superblock->d_bitmap_ptr;
            off_t block_num = (file->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE;
            clear_inode_bitmap_bit(data_bitmap_ptr, block_num);
            memset(disk_mmap[0] + file->blocks[i], 0, BLOCK_SIZE); // Clear data block
            file->blocks[i] = 0;
            printf("Deallocated data block\n");
        }
    }

    // Deallocate indirect blocks
    if (file->blocks[IND_BLOCK] != 0)
    {
        printf("Deallocating indirect block...\n");
        off_t *indirect_block = (off_t *)(disk_mmap[0] + file->blocks[IND_BLOCK]);
        for (size_t i = 0; i < BLOCK_SIZE; i++) // Indirect block can store BLOCK_SIZE/sizeof(off_t) data block pointers
        {
            if (indirect_block[i] != 0)
            {
                printf("Deallocating data block in indirect block %ld...\n", indirect_block[i]);
                char *data_bitmap_ptr = disk_mmap[0] + superblock->d_bitmap_ptr;
                off_t block_num = (indirect_block[i] - superblock->d_blocks_ptr) / BLOCK_SIZE;
                clear_inode_bitmap_bit(data_bitmap_ptr, block_num);
                memset(disk_mmap[0] + indirect_block[i], 0, BLOCK_SIZE); // Clear data block
                indirect_block[i] = 0;
                printf("Deallocated data block in indirect block\n");
            }
        }
        // Deallocate the indirect block itself
        char *data_bitmap_ptr = disk_mmap[0] + superblock->d_bitmap_ptr;
        off_t block_num = (file->blocks[IND_BLOCK] - superblock->d_blocks_ptr) / BLOCK_SIZE;
        clear_inode_bitmap_bit(data_bitmap_ptr, block_num);

        printf("Deallocating indirect block %ld...\n", file->blocks[IND_BLOCK]);  
        memset(indirect_block, 0, BLOCK_SIZE); // Clear indirect block
        file->blocks[IND_BLOCK] = 0;
        printf("Deallocated indirect block\n");
    }

    // Clear the inode bitmap
    char *inode_bitmap_ptr = disk_mmap[0] + superblock->i_bitmap_ptr;
    off_t inode_num = file->num;
    clear_inode_bitmap_bit(inode_bitmap_ptr, inode_num);

    // Clear the inode itself
    memset(disk_mmap[0] + superblock->i_blocks_ptr + inode_num * BLOCK_SIZE, 0, BLOCK_SIZE);
  

    mirror_raid();  // Mirror the RAID after removing the file

    if (found) return 0;
    // Entry otherwise was not found
    else return -ENOENT;
}

// Used by wfs_mkdir and wfs_mknod, due to their redundancies
static int create_new_file(const char* path, mode_t mode)
{
    printf("create_new_file called with path: %s\n", path);       // Debugging statement

    printf("Checking that directory doesn't already exist...\n");
    // Ensure that the directory doesn't already exist
    struct wfs_inode *curr_inode = get_inode_from_path(path);
    if (curr_inode != NULL)
        return -EEXIST; // Directory already exists
    printf("Directory doesn't already exist\n");

    printf("Checking that parent exists and is writable...\n");
    // Ensure parent exists and is writable
    char parent_path[strlen(path) + 1];
    strcpy(parent_path, path);

    // Find the final occurence of '/' in the path
    char *last_slash = strrchr(parent_path, '/');
    char child_name[MAX_NAME];
    strcpy(child_name, last_slash + 1);
    if (last_slash == NULL)
        return -EINVAL; // Invalid path

    if (last_slash == parent_path)
        strcpy(parent_path, "/"); // Root directory
    else
        *last_slash = '\0'; // Terminate the parent path

    printf("Parent path: %s\n", parent_path);

    struct wfs_inode *parent_inode = get_inode_from_path(parent_path);
    if (parent_inode == NULL)
        return -ENOENT; // Parent directory doesn't exist

    // Check if parent is a directory
    if ((parent_inode->mode & S_IFDIR) != S_IFDIR)
        return -ENOTDIR; // Parent is not a directory
    
    // Check if parent is writable  
    if ((parent_inode->mode & S_IWUSR) != S_IWUSR)
        return -EACCES; // Parent is not writable
    printf("Parent exists and is writable\n");

    printf("Allocating new inode for directory...\n");

    // Allocate a new inode for the directory (using the inode bitmap)
    struct wfs_inode *new_inode = allocate_inode(disk_mmap[0] + superblock->i_bitmap_ptr);  //Metadata shared across disks

    if (new_inode == NULL)
        return -ENOSPC; // No more inodes available
    printf("Allocated new inode for directory\n");

    // Initialize the new inode
    new_inode->num = new_inode->num;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;
    new_inode->nlinks = 2; // . and ..
    new_inode->atim = time(NULL);
    new_inode->mtim = new_inode->atim;
    new_inode->ctim = new_inode->atim;

    // Print out the inode stats for debugging purposes
    printf("Inode stats:\n");
    printf("----------------\n");
    printf("    Num: %d\n", new_inode->num);
    printf("    Mode: %d\n", new_inode->mode);
    printf("    UID: %d\n", new_inode->uid);
    printf("    GID: %d\n", new_inode->gid);
    printf("    Size: %ld\n", new_inode->size);
    printf("    Nlinks: %d\n", new_inode->nlinks);
    printf("    Atime: %ld\n", new_inode->atim);
    printf("    Mtime: %ld\n", new_inode->mtim);
    printf("    Ctime: %ld\n", new_inode->ctim);
    printf("----------------\n");

    // Update the parent directory
    printf("Updating parent directory...\n");
    if (update_parent_directory(parent_inode, child_name, new_inode) < 0)
        return -ENOSPC; // No more space in parent directory
    printf("Parent directory updated\n");
    return 0;
}

// Allocates an inode using a bitmap, and returns the pointer to a new inode, or returns an error if there are no more nodes available
struct wfs_inode *allocate_inode(char *i_bitmap_ptr)
{
    // Check if there's any available inodes
    for (size_t i = 0; i < superblock->num_inodes; i++)
    {
        // Find an empty inode
        if (!is_inode_set(i_bitmap_ptr, (off_t)i))
        {
            // Set the inode in the bitmap
            off_t inode_offset = superblock->i_blocks_ptr + (off_t)(i * BLOCK_SIZE);
            struct wfs_inode *inode = (struct wfs_inode *)(disk_mmap[0] + inode_offset);   
            inode->num = (int)i;
            set_inode_bitmap(i_bitmap_ptr, (off_t)i);
            return inode;
        }
    }
    return NULL; // No available inodes
}

// Allocate a data block, and return the block number, or return an error if there are no more data blocks available
static int allocate_data_block(int disk_index)
{
    char *data_bitmap_ptr = disk_mmap[disk_index] + superblock->d_bitmap_ptr;
    // Check if there's any available data blocks
    for (size_t i = 0; i < superblock->num_data_blocks; i++)
    {
        // Find an empty data block
        if (!is_inode_set(data_bitmap_ptr, i))
        {
            // Set the data block in the bitmap
            set_inode_bitmap(data_bitmap_ptr, i);
            return (off_t)(i * BLOCK_SIZE) + (superblock->d_blocks_ptr); // Return the block number
        }
    }
    return -ENOSPC; // No available data blocks
}


// Given a pointer to a bitmap, clear the bit at the given offset
void clear_inode_bitmap_bit(char *inode_bitmap_ptr, off_t inode_num)
{
    off_t byte_num = inode_num / 8;
    off_t bit_num = inode_num % 8;
    inode_bitmap_ptr[byte_num] &= ~(1 << bit_num);
}

// Given a parent and newly-created child inode, update the parent directory to include the new child, directory or regular file
static int update_parent_directory(struct wfs_inode *parent_inode, const char* file_name, struct wfs_inode *child_inode)
{
    printf("update_parent_directory called with file_name: %s\n", file_name); // Debugging statement

    printf("Parent inode details:\n");
    printf("----------------\n");
    printf("    Num: %d\n", parent_inode->num);
    printf("----------------\n");
    
    // Check each block block of the parent inode to find an empty directory entry
    for (int i = 0; i < D_BLOCK; i++)
    {   
    
        if (parent_inode->blocks[i] != 0)
        {
            // Check the direct block for empty directory entries
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk_mmap[0] + parent_inode->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentry[j].num == 0)
                {
                    if (strlen(file_name) > MAX_NAME)
                        return -ENAMETOOLONG; // File name too long
                    dentry[j].num = child_inode->num;
                    strcpy(dentry[j].name, file_name);
                    
                    // Update parent inode modification time
                    parent_inode->mtim = time(NULL);

                    return 1;
                }
            }
        }
        // If there's an empty direct block, allocate it and fill with dentries
        else 
        {
            off_t inode_block = allocate_data_block(0);   // Inodes shared, use the first disk and mirror later
            if (inode_block < 0)
                return -ENOSPC; // No data blocks

            parent_inode->blocks[i] = inode_block;

            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk_mmap[0] + parent_inode->blocks[i]);
            
            // Add entry to directory entries
            if (strlen(file_name) > MAX_NAME)
                return -ENAMETOOLONG; // File name too long
            dentry[0].num = child_inode->num;
            strcpy(dentry[0].name, file_name);
            
            // Update parent inode modification time
            parent_inode->mtim = time(NULL);

            return 1;
        }
    }
    
    // If no space on direct blocks, check indirect block
    off_t * indirect_block_ptr = (off_t *)(disk_mmap[0] + parent_inode->blocks[IND_BLOCK]);

    // If there's an indirect block, index through the data blocks for space for the new directory entry
    if (indirect_block_ptr != 0)
    {
        for (off_t i = 0; i < BLOCK_SIZE; i++)
        {
            if (indirect_block_ptr[i] != 0)
            {
                struct wfs_dentry *dentry = (struct wfs_dentry *)(disk_mmap[0] + indirect_block_ptr[i]);

                for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
                {
                    if (dentry[j].num == 0)
                    {
                        if (strlen(file_name) > MAX_NAME)
                            return -ENAMETOOLONG; // File name too long
                        dentry[j].num = child_inode->num;
                        strcpy(dentry[j].name, file_name);
                        
                        // Update parent inode modification time
                        parent_inode->mtim = time(NULL);

                        return 1;
                    }
                }   
            }
        }
    }

    return -ENOSPC; // No more space in parent directory
}

// Given the ptr to the inode_bitmap, set the bit in the inode_bitmap corresponding to the inode_num
void set_inode_bitmap(char *inode_bitmap_ptr, off_t inode_num)
{
    off_t byte_num = inode_num / 8;
    off_t bit_num = inode_num % 8;
    inode_bitmap_ptr[byte_num] |= 1 << bit_num;
}

// Given the ptr to the inode_bitmap, check if the bit in the inode_bitmap corresponding to the inode_num is set
bool is_inode_set(char *inode_bitmap_ptr, off_t inode_num)
{
    off_t byte_num = inode_num / 8;
    off_t bit_num = inode_num % 8;
    return inode_bitmap_ptr[byte_num] & (1 << bit_num);
}

// Given a file path, traverse it to find the inode corresponding to the file
struct wfs_inode *get_inode_from_path(const char* path)
{
    printf("get_inode_from_path called with path: %s\n", path); // Debugging statement

    // Check if path is root
    if (strcmp(path, "/") == 0)
    {
        printf("Path is root\n");
        return (struct wfs_inode *)(disk_mmap[0] + superblock->i_blocks_ptr); // metadata is the same on every disk
    }

    // Want to split the path into its components, so copy because strtok modifies the string
    char *path_copy = strdup(path);

    printf("Starting at root node and traversing path: %s\n", path); 
    // Start at the root inode and traverse the path
    struct wfs_inode *current_inode = (struct wfs_inode *)(disk_mmap[0] + superblock->i_blocks_ptr); 
    struct wfs_inode *ret = NULL;
    char *token = strtok(path_copy, "/"); // Get the first token
    int i = 0;
    while(token != NULL)
    {
        printf("Checking directory entries for directory number %d from path %s\n", i++, path); // Debugging statements
        ret = NULL; // Reset for each token

        // Check each block of the current_inode
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (current_inode->blocks[i] != 0)
            {
                printf("Checking non-empty block: %d\n", i);
                // Check the directory entry of the block, and search for the file
                struct wfs_dentry *dentry = (struct wfs_dentry *)(disk_mmap[0] + current_inode->blocks[i]);

                for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
                {
                    printf("Directory entry name: %s\n", dentry[j].name);
                    if (strcmp(dentry[j].name, token) == 0)
                    {
                        // Found the file, get the inode
                        int inode_num = dentry[j].num;
                        ret = (struct wfs_inode *)(disk_mmap[0] + superblock->i_blocks_ptr + inode_num * BLOCK_SIZE);
                        break;
                    }
                }
                if (ret != NULL)
                    break;
            }
            else printf("Empty block: %d\n", i);
        }
        if (ret == NULL)
        {
            free(path_copy);
            return NULL; // File not found
        }
        current_inode = ret;    // move to the next inode
        token = strtok(NULL, "/");
    }
    free(path_copy);
    return ret; // File not found
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

    printf("Trying to get inode from the given path...\n");
    struct wfs_inode *inode = get_inode_from_path(path);
    if (inode == NULL)
        return -ENOENT; // File not found
    printf("Got inode from the given path\n");

    // Fill in the stat structure
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_mode = inode->mode;
    stbuf->st_size = inode->size;

    // Print out the inode stats for debugging purposes
    printf("Inode stats:\n");
    printf("----------------\n");
    printf("    Num: %d\n", inode->num);    
    printf("    UID: %d\n", inode->uid);
    printf("    GID: %d\n", inode->gid);
    printf("    Atime: %ld\n", inode->atim);
    printf("    Mtime: %ld\n", inode->mtim);
    printf("    Ctime: %ld\n", inode->ctim);
    printf("    Mode: %d\n", inode->mode);
    printf("    Size: %ld\n", inode->size);
    printf("----------------\n");

    return 0;
}

/* 
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. 
 * This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems. 
 */
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    printf("wfs_mknod called with path: %s\n", path);       // Debugging statement

    if (create_new_file(path, S_IFREG | mode) < 0)
        return -ENOSPC; // No more space in parent directory

    mirror_raid();

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

    if (create_new_file(path, S_IFDIR | mode) < 0)
        return -ENOSPC; // No more space in parent directory

    mirror_raid();
    return 0;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, 
 * unlink only deletes the data when the last hard link is removed. See unlink(2) for details. 
 */
static int wfs_unlink(const char* path)
{
    printf("wfs_unlink called with path: %s\n", path);      // Debugging statement

    if (remove_file(path) < 0)
        return -ENOENT; // File doesn't exist

    return 0;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
 */
static int wfs_rmdir(const char* path)
{
    printf("wfs_rmdir called with given directory: %s\n", path);       // Debugging statement

    // Make sure that the path is not the current directory
    if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0)
        return -EINVAL; 

    if (remove_file(path) < 0)
        return -ENOENT; // Directory doesn't exist

    return 0;
}

/* 
 * To read from a file, find the data block corresponding to the offset being read from, and copy data from the data block(s) to the read buffer. 
 * As with writes, reads may be split across data blocks, or span multiple data blocks.
 */
static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_read called with path: %s\n", path);      // Debugging statement
    
   // Make sure file exists
   struct wfs_inode *file_inode_ptr = get_inode_from_path(path);
   if (file_inode_ptr == NULL)
       return -ENOENT; // File doesn't exist
    
    // Ensure the file is regular
    if ((file_inode_ptr->mode & S_IFREG) != S_IFREG)
        return -EISDIR; // File is not regular
    
    // Read inode for list of current data blocks and how large file is
    size_t bytes_read = 0;
    size_t bytes_remaining = size;
    off_t current_offset = offset;

    // Cannot read beyond the end of the file
    if (current_offset >= file_inode_ptr->size)
        return 0;
    
    // Adjust the size if necessary, based on the current file size
    if (current_offset + size > file_inode_ptr->size)
        bytes_remaining = file_inode_ptr->size - current_offset; 

    while (bytes_remaining > 0)
    {
        // Calculate the block index and offset within the block
        int block_index = current_offset / BLOCK_SIZE;
        int block_offset = current_offset % BLOCK_SIZE;

        // Calculate the disk index and disk offset for RAID 0
        int disk_index = (current_offset / BLOCK_SIZE) % disk_count;
        off_t disk_offset = (current_offset / disk_count) * BLOCK_SIZE + (current_offset % BLOCK_SIZE);
        
        // Pointer to the block to read from, start from block index, assuming direct blocks at first
        printf("Initial block index: %d\n", block_index);
        off_t block_ptr = file_inode_ptr->blocks[block_index];

        // Determine if we need to check the indirect block
        if (block_index >= IND_BLOCK)
        {   
            printf("Block index is greater than or equal to IND_BLOCK\n");
            if (file_inode_ptr->blocks[IND_BLOCK] == 0)
                    return -ENOSPC; // Indirect block was not allocated
                
            // Get the pointer to the indirect block
            off_t *indirect_block_ptr = (off_t *)(file_inode_ptr->blocks[IND_BLOCK] + (off_t)(disk_mmap[disk_index]));

            // Search through the indirect block for the data block
            if (indirect_block_ptr[block_index - IND_BLOCK] == 0)
                    return -ENOSPC; // Data block was not allocated
            printf("Data block exists in indirect block: %ld\n", indirect_block_ptr[block_index - IND_BLOCK]);

            block_ptr = indirect_block_ptr[block_index - IND_BLOCK];
        }
        // Otherwise keep accessing the data blocks
        else if (file_inode_ptr->blocks[block_index] == 0)
                    return -ENOSPC; // Data block was not allocated

        // Calculate the amount of data to read in the current block
        size_t read_size = BLOCK_SIZE - block_offset;
        if (read_size > bytes_remaining)
            read_size = bytes_remaining;

        // Read the data from the block
        char *raid_0_block_ptr = disk_mmap[disk_index] + block_ptr + disk_offset;
        char *data_block_ptr = disk_mmap[0] + block_ptr + block_offset;
        if (raid_mode == RAID_0)
            memcpy(buf + bytes_read, raid_0_block_ptr, read_size);
        else    
            memcpy(buf + bytes_read, data_block_ptr, read_size);

        // Update the counters
        bytes_read += read_size;
        bytes_remaining -= read_size;
        current_offset += read_size;
    }
    // Update the access time of the file
    file_inode_ptr->atim = time(NULL);
    
    return bytes_read;
}

/* 
 * To write to a file, find the data block corresponding to the offset being written to, and copy data from the write buffer into the data block(s). 
 * Note that writes may be split across data blocks, or span multiple data blocks. New data blocks should be allocated using the data block bitmap.
 * Cannot return 0; must return the number of bytes written, or an error code.
 */
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_write called with path: %s\n", path);     // Debugging statement

    /* Write to to foo/bar
     * 1. Make sure file exists
     * 2. read bar inode for list of current data blocks and how large file is
     * 3. If there's space in current data block, then use it, otherwise allocate a new data block
     *  3a. use the data_bitmap to allocate a new datablock
     * 4. Write bar data to data block
     * 5. Update timestamp and address of new data block inside the bar inode
    */

   // Make sure file exists
   struct wfs_inode *file_inode_ptr = get_inode_from_path(path);
   if (file_inode_ptr == NULL)
       return -ENOENT; // File doesn't exist
    
    // Ensure the file is regular
    if ((file_inode_ptr->mode & S_IFREG) != S_IFREG)
        return -EISDIR; // File is not regular
    
    // Read inode for list of current data blocks and how large file is
    size_t bytes_written = 0;
    size_t byte_remaining = size;
    off_t current_offset = offset;

    // If there's space in the current data block, then use it, otherwise allocate a new data block
    while (byte_remaining > 0)
    {
        int block_index = current_offset / BLOCK_SIZE;
        int block_offset = current_offset % BLOCK_SIZE;

        // Calculated for data striping for RAID_0
        int disk_index = (current_offset / BLOCK_SIZE) % disk_count;
        if (raid_mode == RAID_1)
            disk_index = 0;
        off_t disk_offset = (current_offset / BLOCK_SIZE) * BLOCK_SIZE + (current_offset % BLOCK_SIZE);
        
        off_t block_ptr = file_inode_ptr->blocks[block_index]; // Pointer to the block to write to

        // Check if we need to check the indirect block
        if (block_index >= IND_BLOCK)
        {   
            printf("Block index is greater than or equal to IND_BLOCK\n");
            // Indirect block was not allocated, allocate it
            if (file_inode_ptr->blocks[IND_BLOCK] == 0)
            {
                printf("Indirect block was not allocated\n");
                block_ptr = allocate_data_block(disk_index);
                if (block_ptr < 0)
                    return -ENOSPC; // No more space in data blocks
                
                // Assign the indirect block to 512 bytes, which can index into BLOCK_SIZE/sizeof(off_t) data blocks
                file_inode_ptr->blocks[IND_BLOCK] = block_ptr;
                printf("Allocated new data block for indirect block: %ld\n", block_ptr);
                // Clear the indirect block when allocated for the first time
                memset((void *)(file_inode_ptr->blocks[IND_BLOCK] + disk_mmap[disk_index]), 0, BLOCK_SIZE);
                            // Indirect block was allocated, so get the pointer to the indirect block
                printf("Allocated indirect block: %ld\n", file_inode_ptr->blocks[IND_BLOCK]); 
            }
            // Check if indirect block has maxxed out entries
            if (block_index - IND_BLOCK >= BLOCK_SIZE / sizeof(off_t))
                return -ENOSPC; // No more space in indirect block
            printf("Indirect block has space for entries\n");

            // Get the pointer to the indirect block
            off_t *indirect_block_ptr = (off_t *)(file_inode_ptr->blocks[IND_BLOCK] + disk_mmap[disk_index]);

            // Allocate a data block for the indirect block
            if (indirect_block_ptr[block_index - IND_BLOCK] == 0)
            {
                printf("Trying to allocate data block in indirect block\n");
                block_ptr = allocate_data_block(disk_index);
                if (block_ptr < 0)
                    return -ENOSPC; // No more space in data blocks

                indirect_block_ptr[block_index - IND_BLOCK] = block_ptr;
                printf("Allocated new data block in indirect block: %ld\n", block_ptr);
            }
            // Get the data block pointer from the indirect block entry
            block_ptr = indirect_block_ptr[block_index - IND_BLOCK];
        }
        // Access direct blocks still, allocate if needed
        else if (file_inode_ptr->blocks[block_index] == 0)
        {
            printf("Block index is less than IND_BLOCK\n");
            printf("Trying to allocate data block\n");
            block_ptr = allocate_data_block(disk_index);
            if (block_ptr < 0)
                return -ENOSPC; // No more space in data blocks
            file_inode_ptr->blocks[block_index] = block_ptr;
            printf("Allocated new data block: %ld\n", block_ptr);
        }

        // Calculate the amount of data to write in the current block
        size_t write_size = BLOCK_SIZE - block_index;
        if (write_size > byte_remaining)
            write_size = byte_remaining;    
        
        // Write the data to the block
        char *raid_0_block_ptr = disk_mmap[disk_index] + block_ptr + disk_offset;
        char *data_block_ptr = disk_mmap[0] + block_ptr + block_offset;
        if (raid_mode == RAID_0)
            memcpy(raid_0_block_ptr, buf + bytes_written, write_size);
        else
            memcpy(data_block_ptr, buf + bytes_written, write_size);

        // Update the counters
        bytes_written += write_size;
        byte_remaining -= write_size;
        current_offset += write_size;
    }
    // Update timestamp of the new data block inside the inode
    file_inode_ptr->mtim = time(NULL);
    file_inode_ptr->size = offset + size;   // New size of file

    mirror_raid();
    return bytes_written;    
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

    // Get the current inode from path
    struct wfs_inode *inode = get_inode_from_path(path);
    if (!inode)
        return -ENOENT; // Path doesn't exist

    if ((inode->mode & S_IFDIR) != S_IFDIR)
        return -ENOTDIR; // Not a directory

    // Now get the parents (. and ..) and add them to the buffer
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // Iterate through the blocks of the inode
    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (inode->blocks[i] != 0)
        {
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk_mmap[0] + inode->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentry[j].num != 0)
                    filler(buf, dentry[j].name, NULL, 0);
            }
        }
    }

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
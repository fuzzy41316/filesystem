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
    // Get the data bitmap
    char * d_bitmap = (char *)((char *)disk_mmap[0] + superblock->d_bitmap_ptr);

    // Iterate through the superblocks number of data blocks
    for (size_t i = 0; i < superblock->num_data_blocks; i++)
    {
        if (!is_bitmap_bit_set(d_bitmap, i))
        {
            // Set the bitmap bit
            set_bitmap_bit(d_bitmap, i);
            return (int)(i * BLOCK_SIZE) + (int)(superblock->d_blocks_ptr);
        }   
    }

    return -ENOSPC; // No space
}



/* Given an inode, name, inode number, and the current time, add a directory entry */
static int add_directory_entry(struct wfs_inode *inode_ptr, const char *name, int inode_num, time_t curr_time)
{
    // Iterate over the blocks of the inode
    for (int i = 0; i < N_BLOCKS; i++)
    {   
        // If block is non-zero, then fill
        if (inode_ptr->blocks[i] != 0)
        {
            // Find the directory entry
            struct wfs_dentry *entries = get_directory_entry(inode_ptr->blocks[i]);

            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (entries[j].num == 0)
                {
                    // Found an empty entry
                    if (strlen(entries[i].name) >= MAX_NAME)
                        return -ENAMETOOLONG;  // Name too long

                    // Assign the name, nunber, and modified time
                    strcpy(entries[j].name, name);
                    entries[j].num = inode_num;
                    inode_ptr->mtim = curr_time;    // Modified time
                    return 0;   // Success
                }
            }
        }
        else
        {   
            // Otherwise allocate date for the block
            off_t block_num = allocate_data_block();

            if (block_num == -ENOSPC)
                return -ENOSPC; // No space in the block

            inode_ptr->blocks[i] = block_num;   // Assign the block number
            struct wfs_dentry *entries = get_directory_entry(block_num);

            if (strlen(entries[0].name) >= MAX_NAME)
                return -ENAMETOOLONG;  // Name too long

            entries[0].num = inode_num;
            inode_ptr->mtim = curr_time;    // Modified time
            return 0;   // SUccess
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
    // Get the parent directory's inode index
    char parent_path[strlen(path) + 1]; // Make parent_path
    strcpy(parent_path, path);          // Assign path as parent
    char *final_slash = strchr(parent_path, '/'); // Find the final slash

    if (final_slash == NULL)
        return -EINVAL; // Error if no slash found

    *final_slash = '\0'; // Null terminate the parent path
    int parent_inode = get_inode_from_path(parent_path); // Get the parent inode

    if (parent_inode < 0)
        return -ENOENT; // Return error if parent inode is invalid
    
    // Ensure parent directory is writable
    struct wfs_inode *parent_inode_ptr = get_inode(parent_inode);
    if (!(parent_inode_ptr->mode & S_IWUSR))
        return -EACCES; // Return error if parent directory is not writable
    
    // Now find an empty inode to store
    int new_inode_num = get_empty_inode();
    if (new_inode_num < 0)
        return -ENOSPC; // Return error if no empty inode found
    
    // Create a new inode for file / directory
    struct wfs_inode *new_inode = get_inode(new_inode_num);
    new_inode->num = new_inode_num;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;    // Initially empty
    new_inode->nlinks = 1;  // Initially one link

    // Get the current time for last access, modification, and status change 
    time_t current_time = time(NULL);
    new_inode->atim = current_time;
    new_inode->mtim = current_time;
    new_inode->ctim = current_time; 

    // Now add to the directory
    if (add_directory_entry(parent_inode_ptr, final_slash + 1, new_inode_num, current_time) < 0)
        return -ENOSPC; // Return error if no space in directory
    
    return 0; // Success
}


/* Given the index and bitmap, check if the bitmap is non-null */
bool is_bitmap_bit_set(char *bitmap, off_t index)
{
    return ((bitmap[index / 8] & (1 << (index % 8))) != 0);
}

/* From a block number, get the directory entry */
struct wfs_dentry *get_directory_entry(const int block_num)
{
    return (struct wfs_dentry *)((char *)disk_mmap[0] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE);
}

/* Given a path, get the inode number*/
int get_inode_from_path(const char *path)
{
    // Make sure path is non-zero
    if (strlen(path) == 0)
    {
        printf("Error: Path is empty\n");
        return -ENOENT;
    } 

    // Make sure path is not the root
    if(strcmp(path, "/") == 0)
    {
        printf("Error: Path is root\n");
        return -EEXIST;
    } 

    // Duplicate path to tokenize without modifying original
    char *path_copy = strdup(path);

    // Find the final slash to identify the final file/directory
    char *final_slash = strrchr(path_copy, '/');

    // Seperate parent path and targetname
    if (final_slash == path_copy)
        *(final_slash + 1) = '\0';  // Parent is the root directory
    else
        *final_slash = '\0';

    char *parent_path = path_copy;
    char *target_name = final_slash + 1;

    // Initialize variables to traverse the path
    struct wfs_inode *current_inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr);
    int current_inode_num = 0;  // Start from root inode

    // Tokenize the parent path, and then traverse each component
    char *token;
    char *rest = parent_path;
    for (token = __strtok_r(rest, "/", &rest); token != NULL; token = __strtok_r(NULL, "/", &rest))
    {
        int found = 0;

        // Iterate over blocks of current directory
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (current_inode->blocks[i] == 0)
                continue;   // Skip if unused
            
            // Otherwise get directory entries in the current block
            struct wfs_dentry *entries = get_directory_entry(current_inode->blocks[i]);
            
            // Skip over emppty directory entries
            if (entries == NULL)
                continue;

            // Now iterate over each directory in the block
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (entries[j].num == 0)
                    continue;   // Skip if empty directory entry
                
                // Compares entry with token
                if (strcmp(entries[j].name, token) == 0)
                {
                    // Found the entry
                    current_inode_num = entries[j].num;
                    current_inode = get_inode(current_inode_num);

                    if (current_inode == NULL)
                    {
                        free(path_copy);
                        printf("Error: Inode number %d is invalid\n", current_inode_num);
                        return -ENOENT;
                    }

                    // Found the matching entry, exit loop
                    found = 1;  
                    break;
                }
            }

            // Exit block loop, entry found
            if (found)
                break;
        }
    }

    // After traversing the parent path, search for the target entry
    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (current_inode->blocks[i] == 0)
            continue;   // Skip if unused

        struct wfs_dentry *entries = get_directory_entry(current_inode->blocks[i]);

        if (entries == NULL)
            continue;   // Skip if empty
        
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (entries[j].num == 0)
                continue;   // Skip if empty

            if (strcmp(entries[j].name, target_name) == 0)
            {
                // Found the target entry
                current_inode_num = entries[j].num;

                // Make sure inode is allocated 
                char *inode_bitmap = (char *)((char *)disk_mmap[0] + superblock->i_bitmap_ptr);

                if (!is_bitmap_bit_set(inode_bitmap, current_inode_num))
                {
                    free(path_copy);
                    printf("Error: Inode number %d is not allocated\n", current_inode_num);
                    return -ENOENT;
                }

                free(path_copy);
                return current_inode_num; // Success return inode number
            }   
        }
    }

    // If target entry is not found
    free(path_copy);
    printf("Error: Target entry %s not found\n", target_name);  
    return -ENOENT;

}
/* Get the inode from the inode_number */
struct wfs_inode * get_inode(const int inode_num)
{
    return (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + inode_num * sizeof(struct wfs_inode));
}

//////////////////////////////
// END OF HELPER FUNCTIONS //
////////////////////////////

/*
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. For the given pathname, this should fill in the elements of the "stat" structure. 
 * If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf) 
{
    // Translate the path to an inode number (wfs_inode->num)
    int inode_num = get_inode_from_path(path);
    if (inode_num == -ENOENT)
        return -ENOENT;

    // with the inode number, get the correct inode
    struct wfs_inode *inode = get_inode(inode_num);

    // Fil the stat structure
    stbuf->st_mode = inode->mode; 
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return 0;
}

/*
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems.
 *
*/ 
static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) 
{
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
    // Implement directory deletion logic here
    return -ENOSYS;
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details. 
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) 
{
    // Implement file read logic here
    return -ENOSYS;
}

/*
 * As for read above, except that it can't return 0.
*/
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) 
{
    // Implement file write logic here
    return -ENOSYS;
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
    // Extract disk files from arguments
    int fuse_argc = 0;
    char *fuse_argv[argc + 1]; // +1 for NULL terminator
    disk_count = 0;

    // Assume the first arguments are disk files until we encounter a FUSE option (starting with '-')
    int i = 1; // argv[0] is program name
    while (i < argc && argv[i][0] != '-') 
    {
        if (disk_count < MAX_DISKS) disk_files[disk_count++] = argv[i++];
        else 
        {
            fprintf(stderr, "Error: Too many disks.\n");
            exit(1);
        }
    }

    if (disk_count < 2) 
    {
        fprintf(stderr, "Error: Not enough disks.\n");
        exit(1);
    }

    // Prepare FUSE arguments
    fuse_argv[fuse_argc++] = argv[0]; // Program name
    while (i < argc) fuse_argv[fuse_argc++] = argv[i++];

    // Null-terminate the fuse_argv array
    fuse_argv[fuse_argc] = NULL;

    // Call fuse_main with FUSE options and mount point
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
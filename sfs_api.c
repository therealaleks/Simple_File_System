#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include "disk_emu.h"
#include "sfs_api.h"
#include <stdbool.h>

#define DISK_NAME "sfs_disk"
#define NUMBER_OF_INODES 100
#define NUMBER_OF_BLOCKS 2000
#define BLOCK_SIZE 1024
#define MAGIC_NUMBER 0xACBD0005
#define ROOT_INODE 0
#define SUPERBLOCK_BLOCK 0
#define FIRST_INODE_BLOCK 1
#define MAX_FILENAME_LENGTH 20
#define MAX_FILE_BLOCKS (12+256)

typedef struct inode {
    bool free;
    int mode;
    int link_cnt;
    int uid;
    int gid;
    int size;
    int pointer[12];
    int indirect;
} inode;

typedef struct fd{
    bool free;
    int inodeNumber;
    int rPointer;
    int wPointer;
} fd;

typedef struct super_block{
    int magick;
    int blockSize;
    int fsSize;
    int inodeTableLength;
    int rootInode;
} super_block;

typedef struct directory{
    bool free;
    int inodeNumber;
    //filename is maxlength + 1 to account for null terminator
    char fileName[MAX_FILENAME_LENGTH+1];
}directory;

inode inodeTable[NUMBER_OF_INODES];

fd fdTable[NUMBER_OF_INODES];

directory directoryTable[NUMBER_OF_INODES];

super_block superBlock;
/*Very simplistic implementation of a bitmap here. It takes a bit more space than it needs. You can make it into
 * an equivalent array of bools instead. The rest of the code will support it. But since it only halves the size, in the
 * context of the sfs, it's of negligeable impact*/
int bitmap[NUMBER_OF_BLOCKS];

//location of the getnextfile thing
int loc = 0;

//function to flush system cache
void flush(){
    //empty buffer of 0's
    char buffer[20000];
    memset(buffer, 0, sizeof(buffer));

    //flush superBlock
    memcpy(buffer, &superBlock, sizeof(superBlock));
    write_blocks(0, 1, buffer);

    //clear buffer
    memset(buffer, 0, sizeof(buffer));

    //flush inode table
    memcpy(buffer, inodeTable, sizeof(inodeTable));
    write_blocks(1, superBlock.inodeTableLength, buffer);

    //clear buffer
    memset(buffer, 0, sizeof(buffer));

    //flush directory table
    memcpy(buffer, directoryTable, sizeof(directoryTable));
    write_blocks(inodeTable[superBlock.rootInode].pointer[0], 12, buffer);

    //clear buffer
    memset(buffer, 0, sizeof(buffer));

    //flush bitmap
    memcpy(buffer, bitmap, sizeof(bitmap));
    write_blocks(NUMBER_OF_BLOCKS-(sizeof(bitmap)/BLOCK_SIZE + 1), (sizeof(bitmap)/BLOCK_SIZE + 1), buffer);

}

void init_mem(int fresh){
    if(fresh){
        //if creating a fresh disk
        //give initial values to inodetable, fdtable, directory table
        for(int i = 0; i < NUMBER_OF_INODES; i++){
            inodeTable[i].free = true;
            fdTable[i].free = true;
            directoryTable[i].free = true;
        }
        //mark all blocks as free
        for(int i = 0; i < NUMBER_OF_BLOCKS; i++){
            bitmap[i] = 1;
        }
        //initiate superblock with compile time values
        superBlock.blockSize = BLOCK_SIZE;
        superBlock.fsSize = NUMBER_OF_BLOCKS;
        superBlock.inodeTableLength = sizeof(inodeTable)/BLOCK_SIZE + 1;//make this more exact
        superBlock.magick = MAGIC_NUMBER;
        superBlock.rootInode = ROOT_INODE;
    }else{
        //if initializing existing disk
        //initiate fd table
        for(int i = 0; i < NUMBER_OF_INODES; i++){
            fdTable[i].free = true;
        }
        //obtain the rest from the disk
        char buffer[20000];
        memset(buffer, 0, sizeof(buffer));

        read_blocks(0, 1, buffer);
        memcpy(&superBlock, buffer, sizeof(superBlock));

        read_blocks(1, superBlock.inodeTableLength, buffer);
        memcpy(inodeTable, buffer, sizeof(inodeTable));

        read_blocks(inodeTable[superBlock.rootInode].pointer[0], 12, buffer);
        memcpy(directoryTable, buffer, sizeof(directoryTable));

        read_blocks(NUMBER_OF_BLOCKS-(sizeof(bitmap)/BLOCK_SIZE + 1), (sizeof(bitmap)/BLOCK_SIZE + 1), buffer);
        memcpy(bitmap, buffer, sizeof(bitmap));

    }
}

void mksfs(int fresh){
    if(fresh){
        init_fresh_disk(DISK_NAME, BLOCK_SIZE, NUMBER_OF_BLOCKS);
        init_mem(fresh);
        //allocating superBlock block
        bitmap[0] = 0;
        // allocating bitmap block
        bitmap[NUMBER_OF_BLOCKS-1] = 0;

        //allocation bitmap blocks
        for(int i = NUMBER_OF_BLOCKS-(sizeof(bitmap)/BLOCK_SIZE+1); i < NUMBER_OF_BLOCKS ; i++){
            bitmap[i] = 0;
        }

        //allocating inode blocks
        for(int i = 1; i <= superBlock.inodeTableLength ; i++){
            bitmap[i] = 0;
        }
        //allocating inode to root
        inodeTable[superBlock.rootInode].free = false;
        //allocating 12 consecutive blocks for root inode
        int toAllocate = 0;
        for(int i = superBlock.inodeTableLength+1; i <= superBlock.inodeTableLength+12; i++){
            bitmap[i] = 0;
            inodeTable[superBlock.rootInode].pointer[toAllocate] = i;
            toAllocate++;
            if(toAllocate == 12) break;
        }
        inodeTable[superBlock.rootInode].indirect = -1;
        inodeTable[superBlock.rootInode].link_cnt = 12;
        inodeTable[superBlock.rootInode].size = sizeof(directoryTable);
        inodeTable[superBlock.rootInode].mode = 0; //what is this for?

        flush();
    }else{
        init_disk(DISK_NAME, BLOCK_SIZE, NUMBER_OF_BLOCKS);
        init_mem(0);
    }
}

int sfs_getnextfilename(char *fname){

    for(int i = loc; i < NUMBER_OF_INODES; i++){
        if(!directoryTable[i].free){

            strcpy(fname, directoryTable[i].fileName);

            i++;
            loc = i;
            return 1;
        }
    }
    loc = 0;
    return 0;
}

int sfs_getfilesize(const char *path){

    for(int i = 0 ; i < NUMBER_OF_INODES; i++){
        if(!directoryTable[i].free){
            if(strcmp(directoryTable[i].fileName, path) == 0){//or do they really mean path?
                return inodeTable[directoryTable[i].inodeNumber].size;
            }
        }
    }
    return -1;
}

int addFD(int inodeNum){
    for(int i = 0; i < NUMBER_OF_INODES; i++){
        if(fdTable[i].free){

            fdTable[i].free = false;
            fdTable[i].inodeNumber = inodeNum;
            fdTable[i].rPointer = 0;
            fdTable[i].wPointer = inodeTable[inodeNum].size;

            int fileID = i;
            return fileID;
        }
    }
    return -1;
}

int sfs_fopen(char *name){
    //check if file name too long
    if(strlen(name) > MAX_FILENAME_LENGTH) return -1;

    for(int i = 0; i < NUMBER_OF_INODES; i++){
        if(!directoryTable[i].free){
            if(strcmp(directoryTable[i].fileName, name) == 0){
                //make sure file isn't already opened
                for(int z = 0; z < NUMBER_OF_INODES; z++){
                    if(!fdTable[z].free){
                        if(fdTable[z].inodeNumber == directoryTable[i].inodeNumber) return -1;
                    }
                }
                //add inode to FD table
                return addFD(directoryTable[i].inodeNumber);
            }
        }
    }

    //if file doesn't exist, we create it
    for(int i = 0; i < NUMBER_OF_INODES; i++){
        //obtain free directory entry
        if(directoryTable[i].free){
            //obtain free inode
            for(int z = 0; z < NUMBER_OF_INODES; z++) {
                if (inodeTable[z].free) {

                    //allocate inode
                    inodeTable[z].free = false;

                    inodeTable[z].size = 0;

                    inodeTable[z].link_cnt = 0;

                    inodeTable[z].mode = 1;

                    //allocate directory entry
                    directoryTable[i].free = false;

                    strcpy(directoryTable[i].fileName, name);

                    directoryTable[i].inodeNumber = z;

                    flush();

                    return addFD(directoryTable[i].inodeNumber);
                }
            }
        }
    }
    //if no room for new file, exit
    return -1;
}

int sfs_fclose(int fileID){

    if(!fdTable[fileID].free){
        fdTable[fileID].free = true;
        return 0;
    }

    return -1;
}

int sfs_frseek(int fileID, int loc){
    if(loc >= 0 && loc <= inodeTable[fdTable[fileID].inodeNumber].size){
        fdTable[fileID].rPointer = loc;
        return 0;
    }else return -1;
}

int sfs_fwseek(int fileID, int loc){

    if(loc >= 0 && loc <= inodeTable[fdTable[fileID].inodeNumber].size){
        fdTable[fileID].wPointer = loc;
        return 0;
    }else return -1;
}

int sfs_fread(int fileID, char *buf, int length) {

    //check if file is open
    if(fdTable[fileID].free) return 0;

    //cache the inode to keep things nice and easy
    inode inodeCache = inodeTable[fdTable[fileID].inodeNumber];
    //obtain read pointer
    int pointer = fdTable[fileID].rPointer;
    //where reading starts
    int StartBlock = pointer / BLOCK_SIZE;
    int StartByte = pointer % BLOCK_SIZE;
    //Exact number of bytes we can read
    int ByteNum = length;
    if (pointer + length > inodeCache.size) {
        ByteNum -= pointer + length - inodeCache.size;
    }

    //initialize an all null buffer the size of all the file's blocks.
    char aBuffer[inodeCache.link_cnt * BLOCK_SIZE];
    memset(aBuffer, 0, sizeof(aBuffer));

    //Read whole file into aBuffer
    for (int i = 0; i < inodeCache.link_cnt; i++ ){
        if(i < 12) {
            read_blocks(inodeCache.pointer[i], 1, aBuffer + (i * BLOCK_SIZE));
        }else{
            //for blocks through indirect pointer
            int indirectPointers[256];
            read_blocks(inodeCache.indirect, 1, indirectPointers);

            for(int z = i; z < inodeCache.link_cnt; z++){
                read_blocks(indirectPointers[z-12], 1, aBuffer + (z * BLOCK_SIZE));
            }
            break;
        }
    }

    //write into buffer
    memcpy(buf, aBuffer + (StartBlock*BLOCK_SIZE) + StartByte, ByteNum);

    //update read pointer
    fdTable[fileID].rPointer += ByteNum;

    //flush system cache
    flush();

    return ByteNum;
}
//Function to allocate blocks
int allocate(int fileID, int numOfBlocks) {
    //allocated blocks counter
    int allocated = 0;
    //cache inode
    inode inodeCache = inodeTable[fdTable[fileID].inodeNumber];

    for (int i = inodeCache.link_cnt; i < inodeCache.link_cnt + numOfBlocks; i++) {
        //for each block, we allocate a pointer and cross off the bitmap bit
        for (int block = 0; block < NUMBER_OF_BLOCKS; block++) {
            if (bitmap[block] == 1) {
                bitmap[block] = 0;
                //check whether we're allocating direct or indirect pointers
                if (i < 12) {
                    inodeCache.pointer[i] = block;

                    allocated++;
                } else {
                    //if there isnt an indirect pointer already allocated, we allocate one
                    bool indirect = false;
                    if(inodeCache.link_cnt + allocated == 12){
                        for (int indBlock = 0; indBlock < NUMBER_OF_BLOCKS; indBlock++) {
                            if (bitmap[indBlock] == 1) {
                                //cross off block on bitmap
                                bitmap[indBlock] = 0;
                                //assign block to indirect
                                inodeCache.indirect = indBlock;
                                //store indirect pointers array
                                int indirectPointers[256];
                                write_blocks(inodeCache.indirect, 1, indirectPointers);

                                indirect = true;
                                break;
                            }
                        }
                    }else indirect = true;
                    //if no indirect pointer, we do not proceed
                    if (indirect == false) break;
                    //we then add block to indirect pointers array
                    //obtain array
                    int indirectPointers[256];
                    read_blocks(inodeCache.indirect, 1, indirectPointers);

                    indirectPointers[i-12] = block;
                    allocated++;

                    write_blocks(inodeCache.indirect, 1, indirectPointers);

                }
                break;
            }
        }
    }

    inodeCache.link_cnt += allocated;
    //flush function cache
    inodeTable[fdTable[fileID].inodeNumber] = inodeCache;
    //flush file system cache
    flush();

    return allocated;
}

int sfs_fwrite(int fileID, char *buf, int length){

    //check if file is open
    if(fdTable[fileID].free) return 0;

    //cache the inode to keep things clean
    inode inodeCache = inodeTable[fdTable[fileID].inodeNumber];
    //obtain pointer
    int pointer = fdTable[fileID].wPointer;
    //where writing starts
    int StartBlock = pointer/BLOCK_SIZE;
    int StartByte = pointer%BLOCK_SIZE;
    //Exact number of bytes to be written
    int ByteNum = length;
    if (length + pointer > MAX_FILE_BLOCKS*BLOCK_SIZE){
        ByteNum -= length + pointer - (MAX_FILE_BLOCKS*BLOCK_SIZE);
    }

    //calculating number of extra blocks needed
    int NumberOfExtraBlocks = 0;
    int temp = (pointer + ByteNum)/BLOCK_SIZE;
    if ((pointer + ByteNum)%BLOCK_SIZE != 0) temp++;
    if(temp >= inodeCache.link_cnt){
        NumberOfExtraBlocks = temp - inodeCache.link_cnt;
    }

    //allocate extra blocks needed
    int actual = allocate(fileID, NumberOfExtraBlocks);
    //update cache
    inodeCache = inodeTable[fdTable[fileID].inodeNumber];
    //if not enough blocks were allocated, update bytenum accordingly
    if(NumberOfExtraBlocks > actual){
        NumberOfExtraBlocks = actual;
        ByteNum -= length + pointer - (inodeCache.link_cnt*BLOCK_SIZE);
    }

    //initialize an all null buffer the size of all blocks occupied after write
    char aBuffer[(inodeCache.link_cnt)*BLOCK_SIZE];
    memset(aBuffer, 0, sizeof(aBuffer));

    //Read whole file into aBuffer
    for (int i = 0; i < inodeCache.link_cnt; i++ ){
        if(i < 12) {
            read_blocks(inodeCache.pointer[i], 1, aBuffer + (i * BLOCK_SIZE));
        }else{
            //for blocks through indirect pointer
            int indirectPointers[256];
            read_blocks(inodeCache.indirect, 1, indirectPointers);

            for(int z = i; z < inodeCache.link_cnt; z++){
                read_blocks(indirectPointers[z-12], 1, aBuffer + (z * BLOCK_SIZE));
            }
            break;
        }
    }

    //write into buffer at right location
    memcpy(aBuffer+(StartBlock*BLOCK_SIZE)+StartByte, buf, ByteNum);

    //Write back the buffer into the appropriate blocks
    for (int i = 0; i < inodeCache.link_cnt; i++ ){
        if(i < 12){

            write_blocks(inodeCache.pointer[i], 1, aBuffer + (i*BLOCK_SIZE));
        }else{
            //for blocks through indirect pointer
            int indirectPointers[256];
            read_blocks(inodeCache.indirect, 1, indirectPointers);

            for(int z = i; z < inodeCache.link_cnt; z++){
                write_blocks(indirectPointers[z-12], 1, aBuffer + (z * BLOCK_SIZE));
            }
            break;
        }
    }

    //update file size if necessary and flush inode cache
    if(pointer + ByteNum > inodeCache.size){
        inodeCache.size = pointer + ByteNum;
    }
    inodeTable[fdTable[fileID].inodeNumber] = inodeCache;
    //update write pointer
    fdTable[fileID].wPointer += ByteNum;

    //flush system cache
    flush();

    return ByteNum;
}

int sfs_remove(char *file){

    inode inodeCache;
    inodeCache.free = true;

    for(int i = 0; i < NUMBER_OF_INODES; i++){
        if(!directoryTable[i].free){
            if(strcmp(directoryTable[i].fileName, file) == 0){
                //check if file is open
                for(int z = 0; z < NUMBER_OF_INODES; z++){
                    if(!fdTable[z].free){
                        //if not, exit
                        if(fdTable[z].inodeNumber == directoryTable[i].inodeNumber) return -1;
                    }
                }
                //cache inode of file to be removed
                inodeCache = inodeTable[directoryTable[i].inodeNumber];
                //free inode
                inodeTable[directoryTable[i].inodeNumber].free = true;
                //free directory entry
                directoryTable[i].free = true;
                flush();
                break;
            }
        }
    }
    //if nothing has be cached, then no such file has been found. exit
    if(inodeCache.free) return -1;
    //deallocate blocks.
    for(int i = 0; i < inodeCache.link_cnt ; i++){
        if(i<12){
            bitmap[inodeCache.pointer[i]] = 1;
        }else{
            //free indirect pointer block
            bitmap[inodeCache.indirect] = 1;

            int indirectPointers[256];
            read_blocks(inodeCache.indirect, 1, indirectPointers);

            for(int z = i; z < inodeCache.link_cnt; z++){
                bitmap[indirectPointers[z-12]] = 1;
            }
            break;
        }
    }
    flush();
    return 0;
}

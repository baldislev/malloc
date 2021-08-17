#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <iostream>
#include <cstring>
#include <assert.h>

#define META_SIZE sizeof(MallocMetadata)
#define MMETA_SIZE sizeof(MallocMetadata)
#define LARGE_ENOUGH 128
#define MMAP_THRESHOLD 128*1024
#define ROUND_SIZE    8

using namespace std;

struct MallocMetadata {
    size_t size;
    bool isFree;
    MallocMetadata *next;
    MallocMetadata *prev;
};

typedef enum{
    size, count
}countFlag;

typedef enum{
    heap, memmap
}listName;

MallocMetadata* memList = nullptr;
MallocMetadata* lastMetadata = nullptr;

MallocMetadata*   mmapList = nullptr;
MallocMetadata*   lastMMap = nullptr;

bool isInMMapList(void* p){
    MallocMetadata* curr = mmapList;
    while(curr){
        if ((void *)((char*)curr + MMETA_SIZE) == p){
            return true;
        }
        curr = curr->next;
    }
    return false;
}

MallocMetadata* find_free_block(size_t size){
    MallocMetadata* current = memList;
    while(current){
        if(current->isFree && current->size >= size){
            current->isFree = false;
            return current;
        }
        current = current->next;
    }
    return current;
}

void split(MallocMetadata* metaData, size_t reqSize){
    int newSize = metaData->size - reqSize - META_SIZE;// size of new allocated
    if (newSize >= LARGE_ENOUGH){
        MallocMetadata* newMeta = (MallocMetadata*)((char*)metaData + META_SIZE + reqSize);
        newMeta->size   = newSize;
        metaData->size  = reqSize;
        newMeta->isFree = true;
        newMeta->prev   = metaData;
        newMeta->next   = metaData->next;
        metaData->next  = newMeta;
        if (newMeta->next) {
            newMeta->next->prev = newMeta;
        }
        else{
            lastMetadata = newMeta;
        }

    }
}

void merge(MallocMetadata* metaData){
    MallocMetadata* newBlock;
    MallocMetadata* prev = metaData->prev;
    MallocMetadata* next = metaData->next;
    if (prev && prev->isFree){// add prev to new block
        prev->size += META_SIZE + metaData->size;
        prev->next = metaData->next;
        newBlock = prev;
        if (next){
            next->prev = prev;
            if (next->isFree){// add next to new block
                prev->size += META_SIZE + next->size;
                prev->next = next->next;
                if (next->next){
                    next->next->prev = prev;
                }
            }
        }
        if (!newBlock->next){
            lastMetadata = newBlock;
        }
    }
    else if (next && next->isFree) {
        newBlock = metaData;
        metaData->size += META_SIZE + next->size;
        metaData->next = next->next;
        if (next->next){
            next->next->prev = metaData;
        } else{// next is last
            lastMetadata = newBlock;
        }
    }
}

MallocMetadata* sreallocMerge(MallocMetadata* metaData, size_t reqSize){
    MallocMetadata* prev = metaData->prev;
    MallocMetadata* next = metaData->next;

    if (prev && prev->isFree && prev->size + META_SIZE + metaData->size >= reqSize){// merge only with prev
        prev->size+= META_SIZE + metaData->size;
        prev->next = next;
        if(next){
            next->prev = prev;
        }
        else{//new last
            lastMetadata = prev;
        }
        return prev;
    }
    if (next && next->isFree && next->size + META_SIZE + metaData->size >= reqSize){//merge only with next
        metaData->size+= META_SIZE + next->size;
        metaData->next = next->next;
        if(next->next){
            next->next->prev = metaData;
        }
        else{//new last
            lastMetadata = metaData;
        }
        return metaData;
    }
    if (prev && prev->isFree &&
        next && next->isFree &&
        prev->size + META_SIZE + metaData->size +  META_SIZE + next->size >= reqSize){
        merge(metaData);
        return prev;
    }
    return nullptr;
}
void mmapSwap(MallocMetadata* oldMetadata, MallocMetadata* newMetadata) { //frees old and adds new at the tail
    if (oldMetadata->prev) {
        oldMetadata->prev->next = oldMetadata->next;
    } else {
        mmapList = oldMetadata->next;
    }
    if (oldMetadata->next) {
        oldMetadata->next->prev =  oldMetadata->prev;
    }
    newMetadata->prev = lastMMap;
    lastMMap = newMetadata;
    newMetadata->next = nullptr;
}
void removeMetadata(listName lname, MallocMetadata* oldMetadata) {
    if (oldMetadata->prev) {
        oldMetadata->prev->next = oldMetadata->next;
    } else {
        if (lname == heap){
            memList = oldMetadata->next;
        }
        else {
            mmapList = oldMetadata->next;
        }
    }
    if (oldMetadata->next) {
        oldMetadata->next->prev = oldMetadata->prev;
    } else{// new last
        lastMMap = oldMetadata->prev;
    }
}

size_t countOfMem(countFlag what, listName lname, bool isFree){
    MallocMetadata* curr = memList;
    if( lname == memmap){
        curr = mmapList;
        isFree = false;
    }
    if (!curr) return 0;
    size_t count = 0;
    while(curr){
        if (!isFree || curr->isFree){
            count += (what == size ? curr->size : 1);
        }
        curr = curr->next;
    }
    return count;
}

size_t _num_free_blocks(){
    return countOfMem(count, heap, true);
}

size_t _num_free_bytes(){
    return countOfMem(size, heap, true);
}

size_t _num_allocated_blocks(){
    return countOfMem(count, heap, false)
           + countOfMem(count, memmap, false);
}

size_t _num_allocated_bytes(){
    return countOfMem(size, heap, false)
           + countOfMem(size, memmap, false);
}

size_t _size_meta_data(){
    return META_SIZE;
}

size_t _num_meta_data_bytes(){
    return _size_meta_data()*_num_allocated_blocks();
}

size_t sround(size_t size) {
	return size + (ROUND_SIZE - size % ROUND_SIZE)%ROUND_SIZE;
}



void* smalloc(size_t size) {

    if(size == 0 || size > 100000000) return nullptr;
    listName lname = heap;
    size = sround(size);//align
    if (size >= MMAP_THRESHOLD){ //MMMMMMMMMMAP
        lname = memmap;

    }
    /* first try to find a free block in the list of allocated memories: */

    if (lname == heap){
        MallocMetadata* foundBlock = find_free_block(size);
        if(foundBlock){
            /* return the address of the first byte in a block excluding metadata: */
            split(foundBlock, size);// split if necessary
            foundBlock->isFree = false;
            return (void*)((char*)foundBlock + META_SIZE);
        }
        // we need to make a new block
        if (lastMetadata && lastMetadata->isFree){//wilderness check
            if(sbrk(size - lastMetadata->size) == (void*)-1){
                return nullptr;
            }
            lastMetadata->size = size;
            lastMetadata->isFree = false;
            return (void*)((char*)lastMetadata + META_SIZE);
        }
    }
    MallocMetadata* currentMetadata;
    if(lname == heap){
        /* A free block wasnt found in the list, need to allocate a new block upon the current program break: */
        void* currProgBreak = sbrk(0);

        if(currProgBreak == (void*)-1){
            return nullptr;
        }
        // align:
        size_t delta =  ( ROUND_SIZE- (size_t)currProgBreak % ROUND_SIZE)%ROUND_SIZE;
        currProgBreak = sbrk(delta);

        if(currProgBreak == (void*)-1){
            return nullptr;
        }
        currentMetadata = (MallocMetadata*) currProgBreak;

        /*Allocating space for metadata and requested memory: */
        void* newProgBreak = sbrk(META_SIZE + size);
        if(newProgBreak == (void*)-1){
            return nullptr;
        }
    }
    else{//mmap
        void* allocatedMem = mmap(nullptr, META_SIZE + size,
                                  PROT_READ | PROT_WRITE , MAP_PRIVATE | MAP_ANONYMOUS , -1, 0);
        if(allocatedMem == (void*)-1){
            return nullptr;
        }
        currentMetadata = (MallocMetadata*)allocatedMem;

    }
    /* setting relevant values in fields of metadata for current memory allocation: */
    currentMetadata->size = size;
    currentMetadata->isFree = false;
    currentMetadata->next = nullptr;
    currentMetadata->prev = (lname == heap ? lastMetadata : lastMMap);

    /*Initiating the first node in memory list: */
    if(lname == heap){
        if(!memList) {
            memList = currentMetadata;
        }
        else {
            lastMetadata->next = currentMetadata;
        }
        lastMetadata = currentMetadata;
    }
    else {//mmap
        if (!mmapList) {
            mmapList = currentMetadata;
        } else {
            lastMMap->next = currentMetadata;
        }
        lastMMap = currentMetadata;
    }
    /* returning an address of the first byte of allocated memory excluding metadata: */
    return (void*)((char*)currentMetadata + META_SIZE);
}

void* scalloc(size_t num, size_t size){
    size_t totalSize = num*size;
    void* allocatedMem = smalloc(totalSize);
    if(!allocatedMem) return nullptr;
    std::memset(allocatedMem, 0, totalSize);
    return allocatedMem;
}

void* srealloc(void* oldp, size_t size){
    if (size == 0) return  nullptr;
    size = sround(size);//align
    void* allocatedMem = nullptr;
    if(!oldp){
        allocatedMem = smalloc(size);
        return allocatedMem;
    }
    MallocMetadata* oldMetadata = (MallocMetadata*)((char*)oldp - META_SIZE);
    size_t oldSize = oldMetadata->size;
    if (oldSize == size){// no work to do
        return oldp;
    }
    if(size >= MMAP_THRESHOLD){//MMMMMAP
        void* newmmap = smalloc(size);// will use mmap()
        if (!newmmap){//mmap() == -1
            return nullptr;
        }
        memmove(newmmap, oldp, ( size > oldSize ? oldSize : size));//avoid overcopying or undercopying
        removeMetadata(memmap, oldMetadata);
        munmap(oldMetadata, META_SIZE + oldMetadata->size);
        return newmmap;
    }


    if(size < oldSize){ //a
        split(oldMetadata, size);
        return oldp;
    }
    //now wilderness
    if(oldMetadata == lastMetadata){
        if(sbrk(size - oldMetadata->size) != (void*)-1){
            oldMetadata->size = size;
            return (void*)((char*)oldMetadata + META_SIZE);
        }
    }

    MallocMetadata* mergedMetadata = sreallocMerge(oldMetadata, size);
    if (mergedMetadata){//b, c, d
        mergedMetadata->isFree = false;
        allocatedMem = (void*)((char*)mergedMetadata + META_SIZE);
        split(mergedMetadata, size);
    }
    else{// cannot merge or not enough
        allocatedMem = smalloc(size);
        if (!allocatedMem) return nullptr;
        oldMetadata->isFree = true;
    }
    std::memcpy(allocatedMem, oldp, oldSize);
    return allocatedMem;
}

void sfree(void* p){
    if(!p) return;
    MallocMetadata* metadata = (MallocMetadata*)((char*)p - META_SIZE);
    if(isInMMapList(p)){
        removeMetadata(memmap, metadata);
        munmap(metadata, MMETA_SIZE + metadata->size);
        return;
    }
    // on heap
    if(metadata->isFree) return;
    merge(metadata);
    metadata->isFree = true;
}
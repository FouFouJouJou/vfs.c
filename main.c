#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define BLOCK_SIZE 4096
#define TOTAL_BLOCK_SECTORS 32
#define SECTOR_SIZE BLOCK_SIZE/TOTAL_BLOCK_SECTORS
#define FILENAME_SIZE 120
#define DISK_SIZE 1000000
#define TOTAL_BLOCKS DISK_SIZE/BLOCK_SIZE

// 1mb disk
uint8_t disk[DISK_SIZE]={0};

struct node_t {
  uint64_t addr;
  struct node_t *next;
};

struct list_t {
  struct node_t *head;
  struct node_t *tail;
  int count;
};

enum file_type_t {
  _FILE
  ,DIRECTORY
};

struct inode_t {
  uint64_t i_number, size, subfiles;
  enum file_type_t type;
  uint64_t first_block;
};

struct filesystem_metadata_t {
  uint64_t block_size, sector_size, node_index;
  uint8_t super_size, i_bmap_size, d_bmap_size, inodes_size;
  uint64_t root_inode;
};

// TODO: store metadata in super block
// i.e: size of each region, size of individual sector and block units
struct entry_t {
  char name[FILENAME_SIZE];
  uint64_t i_number;
};

struct filesystem_t {
  uint8_t disk[DISK_SIZE];
  struct filesystem_metadata_t metadata;
  uint8_t *super, *i_bmap, *d_bmap, *inodes, *data;
  struct list_t free_blocks, free_inodes;
};

const struct filesystem_metadata_t init_metadata(struct filesystem_metadata_t *const metadata) {
  *metadata=(struct filesystem_metadata_t){
    .block_size=BLOCK_SIZE
    ,.sector_size=SECTOR_SIZE
    ,.super_size=1
    ,.i_bmap_size=1
    ,.d_bmap_size=1
    ,.inodes_size=5
    ,.root_inode=0
    ,.node_index=0
  };
  return *metadata;
}

void init_regions(struct filesystem_t *const fs) {
  fs->super=fs->disk;
  fs->i_bmap=fs->super+(fs->metadata.super_size*fs->metadata.block_size);
  fs->d_bmap=fs->i_bmap+(fs->metadata.i_bmap_size*fs->metadata.block_size);
  fs->inodes=fs->d_bmap+(fs->metadata.d_bmap_size*fs->metadata.block_size);
  fs->data=fs->inodes+(fs->metadata.inodes_size*fs->metadata.block_size);
}


void write_metadata(struct filesystem_t *const fs) {
  memcpy(fs->super, &(fs->metadata), sizeof(struct filesystem_metadata_t));
}

void set_inode_bit(uint8_t *const bit_map, const uint64_t i_number) {
  const uint64_t byte_offset=i_number/8;
  const uint64_t bit_offset=i_number%8;
  *(bit_map+byte_offset)|=(1 << bit_offset);
}

void unset_inode_bit(uint8_t *const bit_map, const uint64_t i_number) {
  const uint64_t byte_offset=i_number/8;
  const uint64_t bit_offset=i_number%8;
  *(bit_map+byte_offset)&=((1 << bit_offset)^0xFF);
}
const uint8_t get_inode_bit(const uint8_t *const bit_map, const uint64_t i_number) {
  const uint64_t byte_offset=i_number/8;
  const uint64_t bit_offset=i_number%8;
  return (*(bit_map+byte_offset) >> bit_offset) & 0x01;
}

// * inode creation in steps:
//    - generate inode number
//    - write 1 to inode bitmap
//    * if dir:
//      - allocate first block

void list_free(struct list_t *list, uint64_t addr) {
  struct node_t *node=calloc(1, sizeof(struct node_t));
  node->addr=addr;
  if(list->head == 0) {
    list->head=node;
  }
  else if(list->tail == 0) {
    list->tail=node;
    list->head->next=node;
  }
  else {
    list->tail->next=node;
    list->tail=list->tail->next;
  }
  list->count++;
}

const uint64_t list_first(struct list_t *list) {
  if(list->head == 0) exit(80);
  const uint64_t addr=list->head->addr;
  struct node_t *const old_head=list->head;
  list->head=old_head->next;
  free(old_head);
  list->count--;
  return addr;
}

void write_bit(uint8_t *const bit_map, const uint64_t i_number, const uint8_t bit_value) {
  switch(bit_value) {
  case 1:
    set_inode_bit(bit_map, i_number);
    break;
  default:
    unset_inode_bit(bit_map, i_number);
  }
}

void init_free_inodes(struct filesystem_t *const fs) {
  for(
      const uint8_t *inode=fs->inodes
      ;inode != fs->inodes+(fs->metadata.inodes_size*fs->metadata.block_size)
      ; inode+=fs->metadata.sector_size
  ) {
    const uint64_t inode_offset=(inode-fs->inodes)/fs->metadata.sector_size;
    const uint8_t bitmap_bit=get_inode_bit(fs->i_bmap, inode_offset);
    if(!bitmap_bit) list_free(&fs->free_inodes, inode_offset);
  }
}

void init_free_data_blocks(struct filesystem_t *const fs) {
  for(const uint8_t *block=fs->data; block < fs->disk+DISK_SIZE; block+=fs->metadata.block_size) {
    const uint64_t block_offset=(block-fs->data)/fs->metadata.block_size;
    const uint8_t bitmap_bit=get_inode_bit(fs->d_bmap, block_offset);
    if(!bitmap_bit) list_free(&fs->free_blocks, block_offset);
  }
}

void init_free_lists(struct filesystem_t *const fs) {
  init_free_inodes(fs);
  init_free_data_blocks(fs);
}

void write_inode_data(uint8_t *const inodes, const struct inode_t *const inode, const uint64_t sector_size) {
  const uint64_t i_number=inode->i_number;
  memcpy(inodes+(inode->i_number*sector_size), inode, sizeof(struct inode_t));
}

void write_block_data(uint8_t *const blocks, uint8_t block[], uint64_t block_number, uint64_t block_size) {
  memcpy(blocks+(block_number*block_size), block, block_size);
}

void mkdir() {}

const struct inode_t create_inode(struct filesystem_t *fs) {
  const uint64_t i_number=list_first(&fs->free_inodes);
  const uint64_t block=list_first(&fs->free_blocks);
  write_bit(fs->i_bmap, i_number, 1);
  write_bit(fs->d_bmap, block, 1);
  struct inode_t inode=(struct inode_t){
    .i_number=i_number
    ,.size=0
    ,.subfiles=0
    ,.type=_FILE
    ,.first_block=block
  };
  write_inode_data(fs->inodes, &inode, fs->metadata.sector_size);
  return inode;
}

const struct inode_t mount_root(struct filesystem_t *const fs) {
  const uint64_t i_number=list_first(&fs->free_inodes);
  const uint64_t block=list_first(&fs->free_blocks);
  write_bit(fs->i_bmap, i_number, 1);
  write_bit(fs->d_bmap, block, 1);
  struct inode_t inode=(struct inode_t) {
    .i_number=i_number
    ,.size=1
    ,.subfiles=0
    ,.type=DIRECTORY
    ,.first_block=block
  };
  write_inode_data(fs->inodes, &inode, fs->metadata.sector_size);
  return inode;
}

struct inode_t read_inode(uint8_t *const inodes, const uint64_t i_number, const uint64_t sector_size) {
  struct inode_t inode;
  memcpy(&inode, inodes+(i_number*sector_size), sizeof(struct inode_t));
  return inode;
}

void read_block(uint8_t *const block, const uint8_t *const blocks, const uint64_t block_number, const uint64_t block_size) {
  memcpy(block, blocks+(block_number*block_size), block_size);
}

struct filesystem_t *mount(char *disk_file) {
  return 0;
}

void write_entry_data(uint8_t *const block, struct entry_t *const entry, uint64_t entry_number) {
  memcpy(block+(entry_number*sizeof(struct entry_t)), entry, sizeof(struct entry_t));
}

struct entry_t create_entry(const uint64_t i_number, char *name) {
  struct entry_t entry;
  entry.i_number=i_number;
  strncpy(entry.name, name, strlen(name)+1);
  return entry;
}

void touch(struct filesystem_t *fs, char *name) {
  if(strlen(name) > FILENAME_SIZE) exit(81);
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  // if read_inode returns value then remove and adding the variable block breaks data
  // TODO: definitely investigate later
  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, root_inode.first_block, fs->metadata.block_size);
  struct inode_t inode=create_inode(fs);
  // for some reason retruning a value rather than a pointer segfaults
  // TODO: definitely investigate later
  struct entry_t entry=create_entry(inode.i_number, name);
  write_entry_data(block, &entry, root_inode.subfiles);
  write_block_data(fs->data, block, root_inode.first_block, fs->metadata.block_size);
  root_inode.subfiles++;

  write_inode_data(fs->inodes, &root_inode, fs->metadata.sector_size);
}

void ls(struct filesystem_t *const fs, const char *const name) {
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, root_inode.first_block, fs->metadata.block_size);
  struct entry_t entry;
  memcpy(&entry, block, sizeof(struct entry_t));
  printf("%s %d\n", entry.name, entry.i_number);
}

struct filesystem_t *fs_format() {
  struct filesystem_t *const fs=calloc(1, sizeof(struct filesystem_t));
  memset(fs->disk, 0, DISK_SIZE);

  init_metadata(&fs->metadata);
  init_regions(fs);
  init_free_lists(fs);

  mount_root(fs);
  return fs;
}

int main(int argc, char **argv) {
  struct filesystem_t *const fs=fs_format();
  touch(fs, "main");
  ls(fs, "main");
  return EXIT_SUCCESS;
}
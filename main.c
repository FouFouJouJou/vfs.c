#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>

#define BLOCK_SIZE 4096
#define COMMAND_BUFF 1024
#define SLASH "/"
#define DISK_FILE "fs.disk"
#define TOTAL_BLOCK_SECTORS 32
#define SECTOR_SIZE BLOCK_SIZE/TOTAL_BLOCK_SECTORS
#define FILENAME_SIZE 120
#define DISK_SIZE 1000072
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
  uint64_t i_number, size, sub_entries;
  enum file_type_t type;
  uint64_t first_block;
};

struct filesystem_metadata_t {
  uint64_t block_size, sector_size, node_index;
  uint64_t super_size, i_bmap_size, d_bmap_size, inodes_size, data_size;
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

struct filesystem_metadata_t init_metadata(struct filesystem_t *fs) {
  fs->metadata=(struct filesystem_metadata_t){
    .block_size=BLOCK_SIZE
    ,.sector_size=SECTOR_SIZE
    ,.super_size=1
    ,.i_bmap_size=1
    ,.d_bmap_size=1
    ,.inodes_size=5
    ,.root_inode=0
    ,.node_index=0
  };
  fs->metadata.data_size=DISK_SIZE-(
    fs->metadata.super_size
    +fs->metadata.i_bmap_size
    +fs->metadata.d_bmap_size
    +fs->metadata.inodes_size
  )*fs->metadata.block_size;
  return fs->metadata;
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
uint8_t get_inode_bit(const uint8_t *const bit_map, const uint64_t i_number) {
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

uint64_t list_first(struct list_t *list) {
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
  memcpy(inodes+(inode->i_number*sector_size), inode, sizeof(struct inode_t));
}

void write_block_data(uint8_t *const blocks, uint8_t block[], uint64_t block_number, uint64_t block_size) {
  memcpy(blocks+(block_number*block_size), block, block_size);
}

void mkdir() {}

struct inode_t create_inode(struct filesystem_t *const fs) {
  const uint64_t i_number=list_first(&fs->free_inodes);
  const uint64_t block=list_first(&fs->free_blocks);
  write_bit(fs->i_bmap, i_number, 1);
  write_bit(fs->d_bmap, block, 1);
  struct inode_t inode=(struct inode_t){
    .i_number=i_number
    ,.size=0
    ,.sub_entries=0
    ,.type=_FILE
    ,.first_block=block
  };
  write_inode_data(fs->inodes, &inode, fs->metadata.sector_size);
  return inode;
}

struct inode_t mount_root(struct filesystem_t *const fs) {
  const uint64_t i_number=list_first(&fs->free_inodes);
  const uint64_t block=list_first(&fs->free_blocks);
  write_bit(fs->i_bmap, i_number, 1);
  write_bit(fs->d_bmap, block, 1);
  struct inode_t inode=(struct inode_t) {
    .i_number=i_number
    ,.size=1
    ,.sub_entries=0
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

// TODO: add number of bytes to read param
void read_block(uint8_t *const block, const uint8_t *const blocks, const uint64_t block_number, const uint64_t block_size) {
  memcpy(block, blocks+(block_number*block_size), block_size);
}


void write_entry_data(uint8_t *const block, struct entry_t *const entry, uint64_t entry_number) {
  memcpy(block+(entry_number*sizeof(struct entry_t)), entry, sizeof(struct entry_t));
}

struct entry_t create_entry(const uint64_t i_number, const char *const name) {
  struct entry_t entry;
  entry.i_number=i_number;
  strncpy(entry.name, name, strlen(name)+1);
  return entry;
}

void read_entries(struct entry_t entries[], uint8_t *block, const uint64_t total_entries) {
  memcpy(entries, block, sizeof(struct entry_t)*total_entries);
}

void read_root_entries(const struct filesystem_t *const fs, struct entry_t entries[]) {
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, root_inode.first_block, fs->metadata.block_size);
  read_entries(entries, block, root_inode.sub_entries);
}

struct entry_t *entry_exists(const char name[], struct entry_t *entries, const uint64_t total_entries) {
  for(uint64_t i=0; i<total_entries; ++i) {
    if(!strncmp(entries[i].name, name, strlen(entries[i].name))) {
      return entries+i;
    }
  }
  return 0;
}

const struct entry_t *entry_exists_(const struct filesystem_t *fs, const char name[]) {
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  // if read_inode returns value then remove and adding the variable block breaks data
  // TODO: definitely investigate later
  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, root_inode.first_block, fs->metadata.block_size);

  struct entry_t entries[root_inode.sub_entries];
  read_entries(entries, block, root_inode.sub_entries);
  return entry_exists(name, entries, root_inode.sub_entries);
}

struct entry_t *entry_exists__(const char name[], struct entry_t entries[], const uint64_t total_entries) {
  for(uint64_t i=0; i<total_entries; ++i) {
    if(!strncmp(entries[i].name, name, strlen(entries[i].name))) {
      return entries+i;
    }
  }
  return 0;
}


void touch(struct filesystem_t *const fs, const char *const name) {
  printf("# touch %s", name);
  if(strlen(name) > FILENAME_SIZE) exit(81);

  if(entry_exists_(fs, name)) {
    printf(": File or directory already exists\n");
    return;
  }
  printf("\n");
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  struct inode_t inode=create_inode(fs);

  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, root_inode.first_block, fs->metadata.block_size);
  // for some reason retruning a value rather than a pointer segfaults
  // TODO: definitely investigate later
  struct entry_t entry=create_entry(inode.i_number, name);
  write_entry_data(block, &entry, root_inode.sub_entries);
  write_block_data(
    fs->data+(root_inode.first_block*fs->metadata.block_size)
    ,block
    ,root_inode.first_block
    ,fs->metadata.block_size
  );
  root_inode.sub_entries++;
  write_inode_data(fs->inodes, &root_inode, fs->metadata.sector_size);
}

void ls(struct filesystem_t *const fs, const char *const name) {
  if(!strncmp(name, SLASH, 1)) {
    printf("\n");
    struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
    struct entry_t entries[root_inode.sub_entries];
    read_root_entries(fs, entries);
    for(uint64_t i=0; i<root_inode.sub_entries; ++i) {
      printf("%s - %ld\n", entries[i].name, entries[i].i_number);
    }
    return;
  }

  const struct entry_t *entry_addr=entry_exists_(fs, name);
  if(entry_addr==0) {
    printf(": No such file or directory\n");
    return;
  }
  printf(": file %s\n", entry_addr->name);
}

void cat(struct filesystem_t *const fs, const char *const name) {
  const struct entry_t *entry_addr=entry_exists_(fs, name);
  if(entry_addr==0) {
    printf(": No such file or directory\n");
    return;
  }
  struct inode_t inode=read_inode(fs->inodes, entry_addr->i_number, fs->metadata.sector_size);
  if(inode.size == 0) {
    printf("\n");
    return;
  }
  printf("\n");
  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, inode.first_block, fs->metadata.block_size);
  for(uint64_t i=0; i<inode.size; ++i) printf("%c", block[i]);
  printf("\n");
}

void echo(struct filesystem_t *const fs, const char *const name, const char data[], const uint64_t size) {
  const struct entry_t *entry_addr=entry_exists_(fs, name);
  if(entry_addr==0) {
    printf(": No such file or directory\n");
    return;
  }
  printf("\n");
  struct inode_t inode=read_inode(fs->inodes, entry_addr->i_number, fs->metadata.sector_size);

  uint8_t block[fs->metadata.block_size];
  read_block(block, fs->data, inode.first_block, fs->metadata.block_size);
  memcpy(block, data, size);
  write_block_data(fs->data, block, inode.first_block, fs->metadata.block_size);
  // TODO: adjust size to already existing data
  inode.size=size;
  write_inode_data(fs->inodes, &inode, fs->metadata.sector_size);
}

void rm(struct filesystem_t *const fs, const char *const name) {
  struct inode_t root_inode=read_inode(fs->inodes, fs->metadata.root_inode, fs->metadata.sector_size);
  struct entry_t entries[fs->metadata.block_size];
  read_root_entries(fs, entries);
  struct entry_t *const entry_addr=entry_exists__(name, entries, root_inode.sub_entries);
  if(entry_addr==0) {
    printf(": No such file or directory\n");
    return;
  }
  printf("\n");
  struct inode_t inode=read_inode(fs->inodes, entry_addr->i_number, fs->metadata.sector_size);
  memset(fs->data+(inode.first_block*fs->metadata.block_size), 0, fs->metadata.block_size);
  memset(fs->inodes+(inode.i_number*fs->metadata.sector_size), 0, fs->metadata.sector_size);
  list_free(&fs->free_blocks, inode.first_block);
  list_free(&fs->free_inodes, inode.i_number);

  for(uint64_t i=0; i<root_inode.sub_entries; ++i) {
    if(entry_addr == entries+i) {
      memset(entry_addr, 0, sizeof(struct entry_t));
      for(uint64_t k=i+1; k<root_inode.sub_entries; ++k) {
	entries[k-1]=entries[k];
      }
    }
  }
  root_inode.sub_entries--;
  write_inode_data(fs->inodes, &root_inode, fs->metadata.block_size);
  write_block_data(fs->data, (uint8_t*)entries, root_inode.first_block, root_inode.sub_entries*sizeof(struct entry_t));
}

struct filesystem_t *fs_format() {
  struct filesystem_t *const fs=calloc(1, sizeof(struct filesystem_t));
  memset(fs->disk, 0, DISK_SIZE);
  init_metadata(fs);
  init_regions(fs);
  init_free_lists(fs);
  write_metadata(fs);
  mount_root(fs);
  return fs;
}

void save(struct filesystem_t*const fs, char *disk_file) {
  int fd=open(disk_file, O_WRONLY|O_CREAT, 0644);
  write(fd, fs->disk, DISK_SIZE);
  close(fd);
}

void simulate(struct filesystem_t *fs) {
  touch(fs, "main1");
  touch(fs, "main2");
  touch(fs, "main3");
  touch(fs, "main4");

  ls(fs, "main");
  ls(fs, "main1");
  ls(fs, "/");

  const char *const data="C is king";
  echo(fs, "main1", data, strlen(data));
  cat(fs, "main1");
  rm(fs, "main2");
  ls(fs, "/");
}

struct filesystem_t *mount(char *disk_file) {
  int fd=open(disk_file, O_RDONLY);
  if (fd == -1) {
    printf("nope\n");
    return fs_format();
  }

  struct filesystem_t *fs=calloc(1, sizeof(struct filesystem_t));
  size_t size=lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  read(fd, fs->disk, size);
  memcpy(&(fs->metadata), fs->disk, sizeof(struct filesystem_metadata_t));
  close(fd);
  init_regions(fs);
  init_free_lists(fs);
  return fs;
}

ssize_t split_args(char *args, char *args_list[]) {
  char *delims=" \0";
  ssize_t argc=0, count=0;
  while(*args != '\0') {
    count=strcspn(args, delims);
    *(args_list+argc)=calloc(count+1, sizeof(char));
    memcpy(*(args_list+argc), args, count);
    (*(args_list+argc))[count]='\0';
    args+=count+1;
    argc++;
  }
  return argc;
}
// TODO: proper lexing at least, raw-dawging it for now.
// TODO: seperate by spaces at least
uint8_t interpret(struct filesystem_t *const fs, const char *const command, ssize_t size) {
  char args[COMMAND_BUFF];
  if(!strncmp(command, "ls", 2)) {
    memcpy(args, command+3, size-3);
    ls(fs, args);
    goto zero;
  }

  if(!strncmp(command, "touch", 5)) {
    memcpy(args, command+6, size-6);
    touch(fs, args);
    goto zero;
  }

  if(!strncmp(command, "rm", 2)) {
    memcpy(args, command+3, size-3);
    rm(fs, args);
    goto zero;
  }

  if(!strncmp(command, "cat", 3)) {
    memcpy(args, command+4, size-4);
    cat(fs, args);
    goto zero;
  }

  if(!strncmp(command, "echo", 4)) {
    memcpy(args, command+5, size-4);
    char *echo_args[COMMAND_BUFF]={0};
    ssize_t argc=split_args(args, echo_args);
    if(argc < 2) {
      printf("Not enough arguments\n");
      goto zero;
    }
    char *file_name=echo_args[0];
    echo(fs, file_name, args+strlen(echo_args[0])+1, strlen(args+strlen(echo_args[0])));
    goto zero;
  }

  if(!strncmp(command, "exit", size)) {
    goto one;
  }

  printf("No such command\n");

  zero:
    return 0;

  one:
    return 1;
}

ssize_t prompt(char *command) {
  char prompt[4]=">> ";
  write(1, prompt, strlen(prompt));
  ssize_t read_bytes=read(0, command, COMMAND_BUFF);
  command[read_bytes-1]='\0';
  fflush(stdin);
  return read_bytes;
}

//TODO: alternative api somehow
// size=4;
// function_pointer_list[size]={ls, cat, rm, echo, 0};
//char *command=prompt();
//[function_call_pointer, ...args]=interpret( command,function_pointer_list, size);
//function_call_pointer(ffs, ...args);
int main() {
  char command[COMMAND_BUFF];
  struct filesystem_t *ffs=mount(DISK_FILE);
  uint8_t done=0;
  while(!done) {
    ssize_t size=prompt(command);
    done=interpret(ffs, command, size);
    memset(command, 0, COMMAND_BUFF);
  }
  save(ffs, DISK_FILE);
  free(ffs);
  return EXIT_SUCCESS;
}

//Fuse nofs filesystem.
//Copyright (C) Rob J Meijer 2014  <pibara@gmail.com>
//Copyright (C) KLPD 2006  <ocfa@dnpa.nl>
//
//This library is free software; you can redistribute it and/or
//modify it under the terms of the GNU Lesser General Public
//License as published by the Free Software Foundation; either
//version 2.1 of the License, or (at your option) any later version.
//
//This library is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//Lesser General Public License for more details.
//
//You should have received a copy of the GNU Lesser General Public
//License along with this library; if not, write to the Free Software
//Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <syslog.h>
struct nofshandles {
   int dd;
   int newdata;
   int index;
   int events;
};

static struct nofshandles nofs_handles;

//This function basically does a stat on the file or directory.
static int nofs_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));  //Start of with an empty zeroed out stat buffer.
    if(strcmp(path, "/") == 0) {
	//The top directory is a simple worls readable directory.
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 3;
        return 0;
    }
    //The full image as raw data file
    else if(strcmp(path, "/nofs.dd") == 0) {
        //Secure the atime/mtime/ctime from the stat of the newdata file.
        time_t atime;
        time_t mtime;
        time_t ctime;
        fstat(nofs_handles.newdata,stbuf);
        atime=stbuf->st_atime;
        mtime=stbuf->st_mtime;
        ctime=stbuf->st_ctime;
        //Use the stat of the dd file as basis.
        fstat(nofs_handles.dd,stbuf);
        //Update the atime/mtime/ctime using the saved times from the newdata file.
        stbuf->st_atime=atime;
        stbuf->st_mtime=mtime;
        stbuf->st_ctime=ctime;
        return 0;
    }
    //Other entities do not exist. 
    else {
       return -ENOENT;
    }
}

//Readdir for just the readable '/' directory, other dirs are non readable.
static int nofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    if(strcmp(path, "/") != 0) {
          return -ENOENT;
    }
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, "nofs.dd", NULL, 0); //The full image as a raw dd.
    return 0;
}

static int nofs_open(const char *path, struct fuse_file_info *fi)
{
    if(strcmp(path, "/nofs.dd") == 0) 
	return 0;
    if(strcmp(path, "/") == 0)
        return -EPERM;
    return -ENOENT;
}

static int nofs_write(const char *path,const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    //We assume for now that all writes start off at a 512 byte boundary, otherwise we need more logic here.
    if (offset % 512) {
        syslog(LOG_ERR,"Write on unexpected offset.\n");
        return -EFAULT;
    }
    //We assume for now that all reads are a multiple of 512 bytes, otherwise we need more logic here.
    if (size % 512) {
        syslog(LOG_ERR,"Write of unexpected length.\n");
        return -EFAULT;
    }
    //Calculate the block number of the first 512 byte block to write.
    off_t blockno = offset/512;
    //Calculate the total number of 512 byte blocks to write.
    off_t blockcount = size/512;
    off_t i;
    for (i=0;i<blockcount;i++) {
        //Determine the index in the newdata file where we are about to write our block to.
        off_t newdata_end=lseek(nofs_handles.newdata,0,SEEK_END);
        //get a pointer to the 512 byte of our next block and write that block out to the newdata file.
        const char * block_buf=buf+512*i;
        write(nofs_handles.newdata,block_buf,512);
        //Write the index of the just written data to the end of our eventlog file.
        write(nofs_handles.events,&blockno,sizeof(off_t));
        //Seek the virtual possition of our newly written data and write that posittion to the index file.
        lseek(nofs_handles.index,(blockno)*sizeof(off_t),SEEK_SET);
        write(nofs_handles.index,&newdata_end,sizeof(off_t));
        blockno++;
    }    
    return size;
}

//This function reads a chunk of data from a file.
static int nofs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    //We assume for now that all writes start off at a 512 byte boundary, otherwise we need more logic here.
    if (offset % 512) {
        syslog(LOG_ERR,"Read on unexpected offset.\n");
        return -EFAULT;
    }
    //We assume for now that all reads are a multiple of 512 bytes, otherwise we need more logic here.
    if (size % 512) {
        syslog(LOG_ERR,"Read of unexpected length.\n");
        return -EFAULT;
    }
    //Calculate the block number of the first 512 byte block to read.
    off_t blockno = offset/512;
    //Calculate the total number of 512 byte blocks to read.
    off_t blockcount = size/512;
    off_t i;
    for (i=0;i<blockcount;i++) {
        //Variable to store the offset from the index file in.
        off_t data_offset;
        //get a pointer to the 512 byte of our next block and write that block out to the newdata file.
        char * block_buf=buf+512*i;
        //Seek the virtual possition of our data and fetch the offset value from the index file.
        lseek(nofs_handles.index,(blockno)*sizeof(off_t),SEEK_SET);
        read(nofs_handles.index,&data_offset,sizeof(off_t));
        //Check if its anything other than the default FFF
        if (data_offset == 0xFFFFFFFFFFFFFFFF) {
            //Read the data from the original dd file.
            lseek(nofs_handles.dd,offset + 512*i,SEEK_SET);
            read(nofs_handles.dd,block_buf,512);
        } else {
            lseek(nofs_handles.newdata,data_offset,SEEK_SET);
            read(nofs_handles.newdata,block_buf,512);
        }
        blockno++;
    }
    return size;
}

static void nofs_destroy(void *handle) 
{
   close(nofs_handles.dd);
   close(nofs_handles.newdata);
   close(nofs_handles.index);
   close(nofs_handles.events); 
}

//The nofs_init function returns a global. It should be possible to do this more cleanly in the future.
static void * nofs_init(void) 
{
  return (void *) &nofs_handles;    
}

//This structure binds the filesystem callbacks to the above functions.
static struct fuse_operations nofs_oper = {
    .getattr	= nofs_getattr,
    .readdir	= nofs_readdir,
    .open	= nofs_open,
    .read	= nofs_read,
    .write	= nofs_write,
    .init       = nofs_init,
    .destroy    = nofs_destroy,
};


int open_files(char *ddpath) {
  char *newdatafile = calloc(strlen(ddpath)+10,1);
  char *dataindexfile = calloc(strlen(ddpath)+10,1);
  char *eventfile = calloc(strlen(ddpath)+10,1);
  sprintf(newdatafile,  "%s.newdata",ddpath);
  sprintf(dataindexfile,"%s.index",ddpath);
  sprintf(eventfile,    "%s.event",ddpath);
  nofs_handles.dd = open(ddpath, O_RDONLY);
  if (nofs_handles.dd == -1) {
    fprintf(stderr,"Problem opening dd file: %s\n",ddpath);
    return 0;
  }
  off_t block_count = lseek(nofs_handles.dd,0,SEEK_END)/512;
  nofs_handles.newdata = open(newdatafile, O_CREAT | O_RDWR,0600);
  if (nofs_handles.newdata == -1) {
     fprintf(stderr,"Problem opening or creating newdata file: %s\n",newdatafile);
     return 0;
  }
  off_t new_block_count = lseek(nofs_handles.newdata,0,SEEK_END)/512;
  nofs_handles.index = open(dataindexfile, O_CREAT | O_RDWR,0600);
  if (nofs_handles.index == -1) {
    fprintf(stderr,"Problem opening or creating dataindex file: %s\n",dataindexfile);
    return 0;
  }
  off_t dataindex_count = lseek(nofs_handles.index,0,SEEK_END)/sizeof(off_t);
  if (dataindex_count == 0) {
      if (new_block_count > 0) {
          fprintf(stderr,"ERROR, size of dataindex (%s) should not be zero if newdata (%s) > 0\n",dataindexfile,newdatafile);
          return 0;
      }
      lseek(nofs_handles.index,0,SEEK_SET);
      off_t invalid = 0xFFFFFFFFFFFFFFFF;
      off_t i;
      for(i=0;i<block_count;i++) {
         write(nofs_handles.index,(void *) &invalid,sizeof(off_t));
      }
  }
  nofs_handles.events = open(eventfile, O_CREAT | O_WRONLY| O_APPEND,0600);
  if (nofs_handles.events == -1) {
      fprintf(stderr,"Problem opening evenf file for append: %s\n",eventfile);
      return 0;
  }
  return 1;
}

int usage() {
   fprintf(stderr,"usage:\n nofs [-d] <mountpoint> <dd file>\n");
   return 1;
}

int main(int argc, char *argv[])
{
    char *mountpoint;
    char *ddpath;
    int failure=0;
    int fuseargcount=1;
    char *fuseargv[6]; //Up to 4 arguments in the fuse arguments array.
    fuseargv[0]=argv[0];
    fuseargv[1]=argv[1];        //This might be a -d, otherwise it will be overwritten.
    if ((argc < 3)||(argc > 4)) {
      exit(usage());
    }
    if ((argv[1][0] == '-') &&(argv[1][1] == 'd')) {
      if (argc < 4) {
          exit(usage());
      }
      fuseargcount=2;
      mountpoint = argv[2];
      ddpath = argv[3];
    } else {
      if (argc > 3) {
          exit(usage());
      }
      mountpoint = argv[1];
      ddpath = argv[2];
    }
    fuseargv[fuseargcount]="-o";     //After the argv[0] and optionaly a -d argument set some options:
    fuseargv[fuseargcount+1]="allow_other";  // allow all users to access the filesystem.
    fuseargv[fuseargcount+2]="-s";           //Run the filesystem as single threaded.
    fuseargv[fuseargcount+3]=mountpoint;      //Set the proper mountpoint for the filesystem.
    //Open the syslog facility to send our debug and error messages to.
    openlog("nofs",LOG_PID | LOG_NDELAY, LOG_DAEMON);
    //Nor run the filesystem.
    if (!open_files(ddpath)) {
        exit(1);
    }
    int rval= fuse_main(4+fuseargcount, fuseargv, &nofs_oper);
    fprintf(stderr,"errno:%d\n",rval);
    return 1;
}



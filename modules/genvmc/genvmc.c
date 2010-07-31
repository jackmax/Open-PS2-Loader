/*
  Copyright 2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include <stdio.h>
#include <loadcore.h>
#include <ioman.h>
#include "ioman_add.h"
#include <io_common.h>
#include <intrman.h>
#include <thsemap.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>
#include <errno.h>

#include "genvmc.h"

#define MODNAME "genvmc"
IRX_ID(MODNAME, 1, 1);

// driver ops protypes
int genvmc_dummy(void);
int genvmc_init(iop_device_t *dev);
int genvmc_deinit(iop_device_t *dev);
int genvmc_devctl(iop_file_t *f, const char *name, int cmd, void *args, u32 arglen, void *buf, u32 buflen);

// driver ops func tab
void *genvmc_ops[27] = {
	(void*)genvmc_init,
	(void*)genvmc_deinit,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_devctl,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy,
	(void*)genvmc_dummy
};

// driver descriptor
static iop_ext_device_t genvmc_dev = {
	"genvmc", 
	IOP_DT_FS | IOP_DT_FSEXT,
	1,
	"genvmc",
	(struct _iop_ext_device_ops *)&genvmc_ops
};

// from cdvdman
typedef struct {
	u8 stat;  			
	u8 second; 			
	u8 minute; 			
	u8 hour; 			
	u8 week; 			
	u8 day; 			
	u8 month; 			
	u8 year; 			
} cd_clock_t;

int sceCdRC(cd_clock_t *rtc);		 // #51

// mc file attributes
#define SCE_STM_R                     0x01
#define SCE_STM_W                     0x02
#define SCE_STM_X                     0x04
#define SCE_STM_C                     0x08
#define SCE_STM_F                     0x10
#define SCE_STM_D                     0x20
#define sceMcFileAttrReadable         SCE_STM_R
#define sceMcFileAttrWriteable        SCE_STM_W
#define sceMcFileAttrExecutable       SCE_STM_X
#define sceMcFileAttrDupProhibit      SCE_STM_C
#define sceMcFileAttrFile             SCE_STM_F
#define sceMcFileAttrSubdir           SCE_STM_D
#define sceMcFileCreateDir            0x0040
#define sceMcFileAttrClosed           0x0080
#define sceMcFileCreateFile           0x0200
#define sceMcFile0400	              0x0400
#define sceMcFileAttrPDAExec          0x0800
#define sceMcFileAttrPS1              0x1000
#define sceMcFileAttrHidden           0x2000
#define sceMcFileAttrExists           0x8000

// SONY superblock magic & version
static char SUPERBLOCK_MAGIC[]   = "Sony PS2 Memory Card Format ";
static char SUPERBLOCK_VERSION[] = "1.2.0.0";

// superblock struct
typedef struct { 			// size = 384
	u8  magic[28];			// Superblock magic, on PS2 MC : "Sony PS2 Memory Card Format "
	u8  version[12];		// Version number of the format used, 1.2 indicates full support for bad_block_list
	s16 pagesize;			// size in bytes of a memory card page
	u16 pages_per_cluster;		// number of pages in a cluster
	u16 blocksize;			// number of pages in an erase block
	u16 unused;			// unused
	u32 clusters_per_card;		// total size in clusters of the memory card
	u32 alloc_offset;		// Cluster offset of the first allocatable cluster. Cluster values in the FAT and directory entries are relative to this. This is the cluster immediately after the FAT
	u32 alloc_end;			// The cluster after the highest allocatable cluster. Relative to alloc_offset. Not used
	u32 rootdir_cluster;		// First cluster of the root directory. Relative to alloc_offset. Must be zero
	u32 backup_block1;		// Erase block used as a backup area during programming. Normally the the last block on the card, it may have a different value if that block was found to be bad
	u32 backup_block2;		// This block should be erased to all ones. Normally the the second last block on the card
	u8  unused2[8];
	u32 ifc_list[32];		// List of indirect FAT clusters. On a standard 8M card there's only one indirect FAT cluster
	int bad_block_list[32];		// List of erase blocks that have errors and shouldn't be used
	u8  cardtype;			// Memory card type. Must be 2, indicating that this is a PS2 memory card
	u8  cardflags;			// Physical characteristics of the memory card 
	u16 unused3;
	u32 cluster_size;
	u32 FATentries_per_cluster;
	u32 clusters_per_block;
	int cardform;
	u32 rootdir_cluster2;
	u32 unknown1;
	u32 unknown2;
	u32 max_allocatable_clusters;
	u32 unknown3;
	u32 unknown4;
	int unknown5;
} MCDevInfo;

typedef struct _sceMcStDateTime {
	u8  Resv2;
	u8  Sec;
	u8  Min;
	u8  Hour;
	u8  Day;
	u8  Month;
	u16 Year;
} sceMcStDateTime;

typedef struct {			// size = 512
	u16 mode;			// 0
	u16 unused;			// 2	
	u32 length;			// 4
	sceMcStDateTime created;	// 8
	u32 cluster;			// 16
	u32 dir_entry;			// 20
	sceMcStDateTime modified;	// 24
	u32 attr;			// 32
	u32 unused2[7];			// 36
	char name[32];			// 64
	u8  unused3[416];		// 96
} McFsEntry;

static MCDevInfo devinfo __attribute__((aligned(64)));
static u8 cluster_buf[16384] __attribute__((aligned(64)));

static int genvmc_io_sema = -1;
static int genvmc_thread_sema = -1;
static int genvmc_thid = -1;
static int genvmc_fh = -1;

static statusVMCparam_t genvmc_stats;

//-------------------------------------------------------------- 
static void long_multiply(u32 v1, u32 v2, u32 *HI, u32 *LO)
{
	register long a, b, c, d;
	register long x, y;

	a = (v1 >> 16) & 0xffff;
	b = v1 & 0xffff;
	c = (v2 >> 16) & 0xffff;
	d = v2 & 0xffff;

	*LO = b * d;   
	x = a * d + c * b;
	y = ((*LO >> 16) & 0xffff) + x;

	*LO = (*LO & 0xffff) | ((y & 0xffff) << 16);
	*HI = (y >> 16) & 0xffff;

	*HI += a * c;     
}

//--------------------------------------------------------------
static int mc_getmcrtime(sceMcStDateTime *time)
{
	register int retries;
	cd_clock_t cdtime;

	retries = 64;

	do {
		if (sceCdRC(&cdtime))
			break;
	} while (--retries > 0);

	if (cdtime.stat & 128) {
		*((u16 *)&cdtime.month) = 0x7d0;
		cdtime.day = 3;
		cdtime.week = 4;
		cdtime.hour = 0;
		cdtime.minute = 0;
		cdtime.second = 0;
		cdtime.stat = 0;
	}

	time->Resv2 = 0;
	time->Sec = ((((cdtime.second >> 4) << 2) + (cdtime.second >> 4)) << 1) + (cdtime.second & 0xf);
	time->Min = ((((cdtime.minute >> 4) << 2) + (cdtime.minute >> 4)) << 1) + (cdtime.minute & 0xf);
	time->Hour = ((((cdtime.hour >> 4) << 2) + (cdtime.hour >> 4)) << 1) + (cdtime.hour & 0xf);
	time->Day = ((((cdtime.day >> 4) << 2) + (cdtime.day >> 4)) << 1) + (cdtime.day & 0xf);

	if ((cdtime.month & 0x10) != 0)
		time->Month = (cdtime.month & 0xf) + 0xa;
	else	
		time->Month = cdtime.month & 0xf;

	time->Year = ((((cdtime.year >> 4) << 2) + (cdtime.year >> 4)) << 1) + ((cdtime.year & 0xf) | 0x7d0);

	return 0;	
}

//--------------------------------------------------------------
static int mc_writecluster(int fd, int cluster, void *buf, int dup)
{
	register int r, size;
	MCDevInfo *mcdi = (MCDevInfo *)&devinfo;

	lseek(fd, cluster * mcdi->cluster_size, SEEK_SET);
	size = mcdi->cluster_size * dup;
	r = write(fd, buf, size);
	if (r != size)
		return -1;

	return 0;
}

//--------------------------------------------------------------
static int vmc_format(char *filename, int size_kb, int blocksize, int *progress, char *msg)
{
	register int i, r, b, ifc_index, fat_index;
	register int ifc_length, fat_length, alloc_offset;
	register int ret, j = 0, z = 0;
	int oldstate;
	MCDevInfo *mcdi = (MCDevInfo *)&devinfo;

	strcpy(msg, "Creating VMC file...");
	genvmc_fh = open(filename, O_RDWR|O_CREAT|O_TRUNC);
	if (genvmc_fh < 0)
		return -101;

	// set superblock magic & version
	memset((void *)&mcdi->magic, 0, sizeof (mcdi->magic) + sizeof (mcdi->version));
	strcpy((char *)&mcdi->magic, SUPERBLOCK_MAGIC);
	strcat((char *)&mcdi->magic, SUPERBLOCK_VERSION);

	// set mc specs
	mcdi->cluster_size = 1024; 	// size in KB of clusters
	mcdi->blocksize = blocksize;	// how many pages in a block of data
	mcdi->pages_per_cluster = 2;	// how many pages in a cluster
	mcdi->pagesize = mcdi->cluster_size / mcdi->pages_per_cluster;
	mcdi->clusters_per_block = mcdi->blocksize / mcdi->pages_per_cluster;
	mcdi->clusters_per_card = (size_kb*1024) / mcdi->cluster_size;
	mcdi->cardtype = 0x02;		// PlayStation2 card type
	mcdi->cardflags = 0x2b;
	mcdi->cardform = -1;
	mcdi->FATentries_per_cluster = mcdi->cluster_size / sizeof(u32);

	// clear bad blocks list
	for (i=0; i<32; i++)
		mcdi->bad_block_list[i] = -1;

	// erase all clusters
	strcpy(msg, "Erasing VMC clusters...");
	memset(cluster_buf, 0xff, sizeof(cluster_buf));
	for (i=0; i<mcdi->clusters_per_card; i+=16) {
		*progress = i / (mcdi->clusters_per_card / 99);
		r = mc_writecluster(genvmc_fh, i, cluster_buf, 16);
		if (r < 0) {
			r = -102;
			goto err_out;
		}
	}

	// calculate fat & ifc length
	fat_length = (((mcdi->clusters_per_card << 2) - 1) / mcdi->cluster_size) + 1; 	// get length of fat in clusters
	ifc_length = (((fat_length << 2) - 1) / mcdi->cluster_size) + 1; 		// get number of needed ifc clusters

	if (!(ifc_length <= 32)) {
		ifc_length = 32;
		fat_length = mcdi->FATentries_per_cluster << 5;
	}

	// clear ifc list
	for (i=0; i<32; i++)
		mcdi->ifc_list[i] = -1;
	ifc_index = mcdi->blocksize / 2;
	i = ifc_index;
	for (j=0; j<ifc_length; j++, i++)
		mcdi->ifc_list[j] = i;

	// keep fat cluster index
	fat_index = i;

	// allocate memory for ifc clusters
	CpuSuspendIntr(&oldstate);
	u8 *ifc_mem = AllocSysMemory(ALLOC_FIRST, (ifc_length * mcdi->cluster_size)+0XFF, NULL);
	CpuResumeIntr(oldstate);
	if (!ifc_mem) {
		r = -103;
		goto err_out;
	}
	memset(ifc_mem, 0, ifc_length * mcdi->cluster_size);

	// build ifc clusters
	u32 *ifc = (u32 *)ifc_mem;
	for (j=0; j<fat_length; j++, i++) {
		// just as security...
		if (i >= mcdi->clusters_per_card) {
			CpuSuspendIntr(&oldstate);
			FreeSysMemory(ifc_mem);
			CpuResumeIntr(oldstate);
			r = -104;
			goto err_out;
		}
		ifc[j] = i;
	}

	// write ifc clusters
	strcpy(msg, "Writing ifc clusters...");
	for (z=0; z<ifc_length; z++) {
		r = mc_writecluster(genvmc_fh, mcdi->ifc_list[z], &ifc_mem[z * mcdi->cluster_size], 1);
		if (r < 0) {
			// freeing ifc clusters memory
			CpuSuspendIntr(&oldstate);
			FreeSysMemory(ifc_mem);
			CpuResumeIntr(oldstate);
			r = -105;
			goto err_out;
		}
	}

	// freeing ifc clusters memory
	CpuSuspendIntr(&oldstate);
	FreeSysMemory(ifc_mem);
	CpuResumeIntr(oldstate);

	// set alloc offset
	alloc_offset = i;

	// set backup blocks
	mcdi->backup_block1 = (mcdi->clusters_per_card / mcdi->clusters_per_block) - 1;
	mcdi->backup_block2 = (mcdi->clusters_per_card / mcdi->clusters_per_block) - 2;

	// calculate number of allocatable clusters per card
	u32 hi, lo, temp;
	long_multiply(mcdi->clusters_per_card, 0x10624dd3, &hi, &lo);
	temp = (hi >> 6) - (mcdi->clusters_per_card >> 31);
	mcdi->max_allocatable_clusters = (((((temp << 5) - temp) << 2) + temp) << 3) + 1;
	j = alloc_offset;

	// building/writing FAT clusters
	strcpy(msg, "Writing fat clusters...");
	i = (mcdi->clusters_per_card / mcdi->clusters_per_block) - 2; // 2 backup blocks
	for (z=0; j < (i * mcdi->clusters_per_block); j+=mcdi->FATentries_per_cluster) {

		memset(cluster_buf, 0, mcdi->cluster_size);
		u32 *fc = (u32 *)cluster_buf;
		int sz_u32 = (i * mcdi->clusters_per_block) - j;
		if (sz_u32 > mcdi->FATentries_per_cluster)
			sz_u32 = mcdi->FATentries_per_cluster;
		for (b=0; b<sz_u32; b++)
			fc[b] = 0x7fffffff; // marking free cluster

		if (z == 0) {
			mcdi->alloc_offset = j;
			mcdi->rootdir_cluster = 0;
			fc[0] = 0xffffffff; // marking rootdir end
		}
		z+=sz_u32;

		r = mc_writecluster(genvmc_fh, fat_index++, cluster_buf, 1);
		if (r < 0) {
			r = -107;
			goto err_out;
		}
	}

	// calculate alloc_end
	mcdi->alloc_end = (i * mcdi->clusters_per_block) - mcdi->alloc_offset;

	// just a security...
	if (z < mcdi->clusters_per_block) {
		r = -108;
		goto err_out;
	}

	mcdi->unknown1 = 0;
	mcdi->unknown2 = 0;
	mcdi->unknown5 = -1;
	mcdi->rootdir_cluster2 = mcdi->rootdir_cluster;

	// build root directory
	McFsEntry *rootdir_entry[2];
	sceMcStDateTime time;

	mc_getmcrtime(&time);
	rootdir_entry[0] = (McFsEntry *)&cluster_buf[0];
	rootdir_entry[1] = (McFsEntry *)&cluster_buf[sizeof(McFsEntry)];
	memset((void *)rootdir_entry[0], 0, sizeof(McFsEntry));
	memset((void *)rootdir_entry[1], 0, sizeof(McFsEntry));
	rootdir_entry[0]->mode = sceMcFileAttrExists | sceMcFile0400 | sceMcFileAttrSubdir | sceMcFileAttrReadable | sceMcFileAttrWriteable | sceMcFileAttrExecutable;
	rootdir_entry[0]->length = 2;
	memcpy((void *)&rootdir_entry[0]->created, (void *)&time, sizeof(sceMcStDateTime));
	memcpy((void *)&rootdir_entry[0]->modified, (void *)&time, sizeof(sceMcStDateTime));
	rootdir_entry[0]->cluster = 0;
	rootdir_entry[0]->dir_entry = 0;
	strcpy(rootdir_entry[0]->name, ".");
	rootdir_entry[1]->mode = sceMcFileAttrExists | sceMcFileAttrHidden | sceMcFile0400 | sceMcFileAttrSubdir | sceMcFileAttrWriteable | sceMcFileAttrExecutable;
	rootdir_entry[1]->length = 2;
	memcpy((void *)&rootdir_entry[1]->created, (void *)&time, sizeof(sceMcStDateTime));
	memcpy((void *)&rootdir_entry[1]->modified, (void *)&time, sizeof(sceMcStDateTime));
	rootdir_entry[1]->cluster = 0;
	rootdir_entry[1]->dir_entry = 0;
	strcpy(rootdir_entry[1]->name, "..");

	// write root directory cluster
	strcpy(msg, "Writing root directory cluster...");
	r = mc_writecluster(genvmc_fh, mcdi->alloc_offset + mcdi->rootdir_cluster, cluster_buf, 1);
	if (r < 0) {
		r = -109;
		goto err_out;
	}

	// set superblock formatted flag
	mcdi->cardform = 1;

	// finally write superblock
	strcpy(msg, "Writing superblock...");
	memset(cluster_buf, 0xff, mcdi->cluster_size);
	memcpy(cluster_buf, (void *)mcdi, sizeof(MCDevInfo));
	r = mc_writecluster(genvmc_fh, 0, cluster_buf, 1);
	if (r < 0) {
		r = -110;
		goto err_out;
	}

	r = close(genvmc_fh);
	if (r < 0)
		return -111;
	genvmc_fh = -1;

	*progress = 100;

	return 0;


err_out:
	ret = close(genvmc_fh);
	if (!(ret < 0))
		genvmc_fh = -1;

	return r;
}

//-------------------------------------------------------------- 
int genvmc_dummy(void)
{
	return -EPERM;
}

//-------------------------------------------------------------- 
int genvmc_init(iop_device_t *dev)
{
	genvmc_io_sema = CreateMutex(IOP_MUTEX_UNLOCKED);
	genvmc_thread_sema = CreateMutex(IOP_MUTEX_UNLOCKED);

	return 0;
}

//-------------------------------------------------------------- 
int genvmc_deinit(iop_device_t *dev)
{
	DeleteSema(genvmc_io_sema);
	DeleteSema(genvmc_thread_sema);

	return 0;
}

//-------------------------------------------------------------- 
static void VMC_create_thread(void *args)
{
	register int r;
	createVMCparam_t *param = (createVMCparam_t *)args;

	WaitSema(genvmc_thread_sema);

	genvmc_stats.VMC_status = GENVMC_STAT_BUSY;
	genvmc_stats.VMC_error = 0;
	genvmc_stats.VMC_progress = 0;
	strcpy(genvmc_stats.VMC_msg, "Initializing...");

	r = vmc_format(param->VMC_filename, param->VMC_size_mb * 1024, param->VMC_blocksize, &genvmc_stats.VMC_progress, genvmc_stats.VMC_msg);
	if (r < 0) {
		genvmc_stats.VMC_status = GENVMC_STAT_AVAIL;
		genvmc_stats.VMC_error = r;
		strcpy(genvmc_stats.VMC_msg, "Failed to format VMC file");
		goto exit;
	}

	genvmc_stats.VMC_status = GENVMC_STAT_AVAIL;
	genvmc_stats.VMC_error = 0;
	genvmc_stats.VMC_progress = 100;
	strcpy(genvmc_stats.VMC_msg, "VMC file created");

exit:
	SignalSema(genvmc_thread_sema);
	genvmc_thid = -1;
	ExitDeleteThread();
}

//-------------------------------------------------------------- 
static int vmc_create(createVMCparam_t *param)
{
	register int r;
	iop_thread_t thread_param;

	thread_param.attr = TH_C;
	thread_param.option = 0;
	thread_param.thread = (void *)VMC_create_thread;
	thread_param.stacksize = 0x2000;
	thread_param.priority = (param->VMC_thread_priority < 0x0f) ? 0x0f : param->VMC_thread_priority;

	// creating VMC create thread
	genvmc_thid = CreateThread(&thread_param);
	if (genvmc_thid < 0)
		return -1;

	// starting VMC create thread
	r = StartThread(genvmc_thid, (void *)param);
	if (r < 0)
		return -2;

	return 0;
}

//-------------------------------------------------------------- 
static int vmc_abort(void)
{
	register int r;

	if (genvmc_thid >= 0) {
		// terminate VMC create thread
		r = TerminateThread(genvmc_thid);
		if (r < 0)
			return -1;

		// delete VMC create thread
		r = DeleteThread(genvmc_thid);
		if (r < 0)
			return -2;

		// try to close VMC file
		if (genvmc_fh >= 0)
			close(genvmc_fh);

		// adjusting stats
		genvmc_stats.VMC_status = GENVMC_STAT_AVAIL;
		genvmc_stats.VMC_error = -201;
		strcpy(genvmc_stats.VMC_msg, "Aborted...");
	}

	return 0;
}

//-------------------------------------------------------------- 
static int vmc_status(statusVMCparam_t *param)
{
	// copy global genvmc stats to output param
	memcpy((void *)param, (void *)&genvmc_stats, sizeof(statusVMCparam_t));

	return 0;
}

//-------------------------------------------------------------- 
int genvmc_devctl(iop_file_t *f, const char *name, int cmd, void *args, u32 arglen, void *buf, u32 buflen)
{
	register int r = 0;

	if (!name)
		return -ENOENT;

	WaitSema(genvmc_io_sema);

	switch (cmd) {

		// VMC file creation request command
		case GENVMC_DEVCTL_CREATE_VMC:
			r = vmc_create((createVMCparam_t *)args);
			if (r < 0)
				r = -EIO;
			break;

		// VMC file creation abort command
		case GENVMC_DEVCTL_ABORT:
			r = vmc_abort();
			if (r < 0)
				r = -EIO;
			break;

		// VMC file creation status command
		case GENVMC_DEVCTL_STATUS:
			r = vmc_status((statusVMCparam_t *)buf);
			if (r < 0)
				r = -EIO;
			break;

		default:
			r = -EINVAL;
	}

	SignalSema(genvmc_io_sema);

	return r;
}

//-------------------------------------------------------------- 
int _start(int argc, char** argv)
{
	DelDrv("genvmc");

	if (AddDrv((iop_device_t *)&genvmc_dev) < 0)
		return MODULE_NO_RESIDENT_END;

	return MODULE_RESIDENT_END;
}

//--------------------------------------------------------------
DECLARE_IMPORT_TABLE(cdvdman, 1, 1)
DECLARE_IMPORT(51, sceCdRC)
END_IMPORT_TABLE

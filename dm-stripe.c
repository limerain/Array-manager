/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include <linux/device-mapper.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/dm-io.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

#define DM_MSG_PREFIX "striped"
#define DM_IO_ERROR_THRESHOLD 15
#define minor_shift 4

struct reverse_nodes{
	sector_t index;
	unsigned char dirty;
};

struct flag_nodes{
	sector_t msector;
	unsigned int wp;
};

struct flag_set{
	struct flag_nodes** table;
	struct kmem_cache* node_buf;
	struct reverse_nodes** reverse_table;
};

struct gc_set{
	unsigned int tp;//device Target pointer
	unsigned int gp;//gc device pointer
	struct reverse_nodes** reverse_table;
	struct flag_nodes** table;
	unsigned int phase;
	unsigned char io_flag;
	sector_t cur_sector;
	sector_t tp_io_sector;
	unsigned long long index;
	unsigned int ptr_ovflw_size;
	char *kijil_map;
	unsigned long long kijil_size;
	char *block_buffer;
	struct dm_io_client *io_client;
	struct mutex *lock;
	unsigned int vms;
};

struct vm {
	struct dm_dev *dev;
	sector_t physical_start;
	sector_t end_sector;
	unsigned int main_dev;

	atomic_t error_count;
};

struct vm_c {
	uint32_t vms;
	int vms_shift;

	/* The size of this target / num. stripes */
	sector_t vm_width;

	uint32_t chunk_size;
	int chunk_size_shift;

	/* Needed for handling events */
	struct dm_target *ti;

	/* Work struct used for triggering events*/
	struct work_struct trigger_event;
	/* volume manager variable*/
	unsigned int wp;//device Write pointer
	unsigned char *gp_list; //need do gc device
	unsigned int mp;//device migration pointer
	unsigned long long *ws;//in device Write sector pointer
	unsigned long long *d_num;
	unsigned long long num_entry;// number of table's entry
	unsigned char mig_flag;
	struct task_struct *th_id;
	struct flag_set* fs;
	struct gc_set* gs;
	struct mutex lock;
	struct dm_io_client *io_client;

	struct vm vm[0];
};

static int bgrnd_job(struct dm_target *);
static struct flag_nodes* vm_lfs_map_sector(struct vm_c *vc, sector_t target_sector,
		unsigned int wp, sector_t *write_sector, struct block_device **bdev, unsigned long bi_rw, unsigned int size);
/*
 * An event is triggered whenever a drive
 * drops out of a stripe volume.
 */

static int atom(const char *name){//ascii to minor number
	int val;
	for(;;name++){
		if(*name == ':'){
			name++;
			break;
		}
	}
	for(val=0;;name++){
		switch(*name){
			case '0'...'9':
				val = 10 *val + (*name - '0');
				break;
			default:
				return val;
		}
	}

	return val;
}

static void trigger_event(struct work_struct *work)
{
	struct vm_c *vc = container_of(work, struct vm_c,
					   trigger_event);
	dm_table_event(vc->ti->table);
}

static inline struct vm_c *alloc_context(unsigned int vms)
{
	size_t len;

	if (dm_array_too_big(sizeof(struct vm_c), sizeof(struct vm),
			     vms))
		return NULL;

	len = sizeof(struct vm_c) + (sizeof(struct vm) * vms);

	//return kmalloc(len, GFP_NOFS);
	return kmalloc(len, GFP_KERNEL);
}

/*
 * Parse a single <dev> <sector> pair
 */
static int get_vm(struct dm_target *ti, struct vm_c *vc,
		      unsigned int vm, char **argv)
{
	unsigned long long start;
	char dummy;

	if (sscanf(argv[1], "%llu%c", &start, &dummy) != 1)
		return -EINVAL;

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			  &vc->vm[vm].dev))
		return -ENXIO;

	vc->vm[vm].physical_start = start;

	return 0;
}

/*
 * Construct a striped mapping.
 * <number of stripes> <chunk size> [<dev_path> <offset>]+
 */
static int vm_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct vm_c *vc;
	sector_t width, tmp_len;
	uint32_t vms;
	uint32_t chunk_size;
	int r;
	unsigned long long i;

	if (argc < 2) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}
	/*for(i=0; i<argc; i++){
		printk("argv %d = %s\n", i, argv[i]);
	}*/

	if (kstrtouint(argv[0], 10, &vms) || !vms) {
		ti->error = "Invalid stripe count";
		return -EINVAL;
	}

	if (kstrtouint(argv[1], 10, &chunk_size) || !chunk_size) {
		ti->error = "Invalid chunk_size";
		return -EINVAL;
	}

	width = ti->len;
	if (sector_div(width, vms)) {
		ti->error = "Target length not divisible by "
		    "number of stripes";
		return -EINVAL;
	}

	tmp_len = width;
	if (sector_div(tmp_len, chunk_size)) {
		ti->error = "Target length not divisible by "
		    "chunk size";
		return -EINVAL;
	}

	/*
	 * Do we have enough arguments for that many stripes ?
	 */
	if (argc != (2 + 2 * vms)) {
		ti->error = "Not enough destinations "
			"specified";
		return -EINVAL;
	}

	vc = alloc_context(vms);
	if (!vc) {
		ti->error = "Memory allocation for striped context "
		    "failed";
		return -ENOMEM;
	}

	INIT_WORK(&vc->trigger_event, trigger_event);

	/* Set pointer to dm target; used in trigger_event */
	vc->ti = ti;
	vc->vms = vms;
	vc->vm_width = width;

	if (vms & (vms - 1))
		vc->vms_shift = -1;
	else
		vc->vms_shift = __ffs(vms);

	r = dm_set_target_max_io_len(ti, chunk_size);
	if (r) {
		kfree(vc);
		return r;
	}

	ti->num_flush_bios = vms;
	ti->num_discard_bios = vms;
	ti->num_write_same_bios = vms;

	vc->chunk_size = chunk_size;
	if (chunk_size & (chunk_size - 1))
		vc->chunk_size_shift = -1;
	else
		vc->chunk_size_shift = __ffs(chunk_size);

	/*
	 * Get the stripe destinations.
	 */
	for (i = 0; i < vms; i++) {
		argv += 2;

		r = get_vm(ti, vc, i, argv);
		if (r < 0) {
			ti->error = "Couldn't parse stripe destination";
			while (i--)
				dm_put_device(ti, vc->vm[i].dev);
			kfree(vc);
			return r;
		}
		atomic_set(&(vc->vm[i].error_count), 0);
	}

	/*volume manager initialize*/
	vc->wp = 0;
	vc->ws = kmalloc(sizeof(unsigned long long) * vc->vms, GFP_KERNEL);
	for(i = 0; i<vc->vms; i++)
		vc->ws[i] = 0;
	vc->mp = 0;
	vc->gp_list = kmalloc(sizeof(char) * vc->vms, GFP_KERNEL);
	vc->gs = NULL;
	vc->io_client = dm_io_client_create();
	for(i=0; i<vc->vms; i++)
		vc->gp_list[i] = 0;//0 is clean
	{
		unsigned long long tem, disk_size;
		
		tem = 0;
		for(i = 0; i<vms; i++){
			struct block_device *cur_bdev = vc->vm[i].dev->bdev;
			vc->vm[i].end_sector = i_size_read(cur_bdev->bd_inode)>>9;//unit of sector
			printk("vm%llu start_sector %llu, end_sector %llu, target_offset %llu\n", i, (unsigned long long) vc->vm[i].physical_start, (unsigned long long) vc->vm[i].end_sector, (unsigned long long)dm_target_offset(ti, vc->ws[i]));
			disk_size = vc->vm[i].end_sector * 512;
			do_div(disk_size, (unsigned long long) vc->vm[i].dev->bdev->bd_block_size);
			tem += disk_size;
		}
		vc->num_entry = tem;//num entry is blk num
	}
	printk("num entry is %llu, node size is %lu, req mem is %llu\n", vc->num_entry, sizeof(struct flag_nodes), sizeof(struct flag_nodes) * vc->num_entry);
	
	//flag set initialize
	vc->fs = (struct flag_set *) kmalloc(sizeof(struct flag_set), GFP_KERNEL);
	vc->fs->node_buf = kmem_cache_create("dirty_data_buf", sizeof(struct flag_nodes),
			0, (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD), NULL);

	vc->fs->table = (struct flag_nodes **)vmalloc(sizeof(struct flag_nodes*) * vc->num_entry);
	for(i=0; i<vc->num_entry; i++){
		vc->fs->table[i] = kmem_cache_alloc(vc->fs->node_buf, GFP_KERNEL);
		vc->fs->table[i]->msector = -1;
		vc->fs->table[i]->wp = -1;
	}

	vc->fs->reverse_table = vmalloc(sizeof(struct reverse_nodes*) * vc->vms);
	for(i=0; i<vc->vms; i++){
		unsigned long long r_table_size = (vc->vm[i].end_sector+7 - vc->vm[i].physical_start);
		unsigned long long j;
		//unsigned long long r_table_size = (vc->vm[i].end_sector );
		do_div(r_table_size, 8);
		//printk("size is %llu\n", r_table_size);
		vc->fs->reverse_table[i] = vmalloc(sizeof(struct reverse_nodes) * r_table_size);
		for(j=0; j<r_table_size; j++)
			vc->fs->reverse_table[i][j].dirty = 1;
	}

	vc->d_num = kmalloc(sizeof(unsigned long long) * vc->vms, GFP_KERNEL);
	for(i=0; i<vc->vms; i++){
		vc->d_num[i] = 0;
	}

	for(i=0; i<vc->vms; i++){
		unsigned int minor = atom(vc->vm[i].dev->name);
		vc->vm[i].main_dev = minor >> minor_shift;
	}

	vc->mig_flag = 0;
	mutex_init(&vc->lock);

	vc->th_id = kthread_run((void*)bgrnd_job, ti, "striped");
	ti->private = vc;

	/*for(i=0; i<vc->vms; i++){///all discard
		int err = blkdev_issue_discard(vc->vm[i].dev->bdev,
				vc->vm[i].physical_start, vc->vm[i].end_sector-1 - vc->vm[i].physical_start, GFP_NOFS, 0);
	}*/
	
	return 0;
}

static void vm_dtr(struct dm_target *ti)
{
	unsigned int i;
	struct vm_c *vc = (struct vm_c *) ti->private;

	for (i = 0; i < vc->vms; i++)
		dm_put_device(ti, vc->vm[i].dev);

	flush_work(&vc->trigger_event);

	/*for(i=0;i<vc->vms;i++){
		temp = vc->fs->wp[i];
		if(temp == NULL) continue;
		do{
			struct flag_node* del_node = temp;
			temp = temp->next;
			flag_erase(vc->fs, del_node);
		}while(temp->next != NULL);
		vc->fs->wp[i] = NULL;
	}*/
	/*if(vc->th_id[0]){
		kthread_stop(vc->th_id[0]);
		vc->th_id[0] = NULL;
	}
	if(vc->th_id[1]){
		kthread_stop(vc->th_id[1]);
		vc->th_id[1] = NULL;
	}*/
	/*if(vc->th_id){
		kthread_stop(vc->th_id);
		vc->th_id = NULL;
	}*/
	vfree(vc->fs->table);

	kfree(vc);
}

inline char check_range_over(struct vm_c* vc){
	if(vc->vm[vc->wp].end_sector < vc->vm[vc->wp].physical_start + vc->ws[vc->wp]){
		///need to implement for ws 0 is valid by cold valid data
		/*unsigned int next_point = (vc->wp+1) % vc->vms;
		  if(vc->gs != NULL){
		  while(vc->vm[next_point].main_dev == vc->vm[vc->gs->tp].main_dev){//next_point == vc->gs->tp)
		  next_point = (next_point+1) %vc->vms;
				}
			}*/
		unsigned int next_point;
		if(vc->wp == 0)
			next_point = 3;
		else
			next_point = (vc->wp+1) %vc->vms;
		/*unsigned int next_point = 0;//
		  unsigned int i;
		  for(i=1; i<vc->vms; i++){
		  if(vc->wp == i) continue;
		  if(vc->gp_list[next_point] > vc->gp_list[i])
		  next_point = i;
		  }*/
		
		printk("big!! next wp is %s\n", vc->vm[next_point].dev->name);
		/*if(bio->bi_iter.bi_sector + bio_sectors(bio) > vc->vm[vc->wp].end_sector){
			unsigned long long ws;
			printk("bi_sector %llu, +sectors %llu\n", (unsigned long long)bio->bi_iter.bi_sector, (unsigned long long)bio->bi_iter.bi_sector + bio_sectors(bio));
			
			ws = temp->msector - vc->vm[temp->wp].physical_start;
			do_div(ws, 8);
			vc->fs->reverse_table[temp->wp][ws].dirty = 1;
			temp->msector = -1;
			
			vc->gp_list[vc->wp] = 2;//2 is dirty
			vc->wp = next_point;
			
			/*vm_lfs_map_sector(vc, backup_sector,
					vc->wp, &bio->bi_iter.bi_sector,
					&bio->bi_bdev, bio_rw(bio), 1);
		}*/
		//if(!vc->mig_flag) vc->mig_flag = 1;
		return 1;
	}
	return 0;
}

inline void do_kijil(struct vm_c* vc){
	//unsigned long long disk_block_size = vc->vm[vc->gs->gp].end_sector;// - vc->vm[vc->gs->gp].physical_start;//initialize for do_div
	unsigned long long disk_block_size = vc->vm[vc->gs->gp].end_sector+7 - vc->vm[vc->gs->gp].physical_start;//initialize for do_div
	signed char num_count = 0;
	unsigned long long i;
	char* kijil_map = vmalloc(disk_block_size);
	struct reverse_nodes* gp_reverse_table = vc->gs->reverse_table[vc->gs->gp];

	do_div(disk_block_size, 8);
	if(kijil_map == NULL) printk("unknown kijil_map error\n");
	///why kijil grain is 1 byte?? more coars grain??

	/*for(i=0; i<disk_block_size; i++){
		printk("%llu:%u ", i, gp_reverse_table[i].dirty);
		if(i !=0 && i%30 == 0)
			printk("\n");
	}
	printk("rv table print end\n");*/

	if(gp_reverse_table[0].dirty == 0)		num_count = 1;
	else if(gp_reverse_table[0].dirty == 1)	num_count = -1;
	for(i = 1; i<disk_block_size; i++){///already check 0 index
		if(num_count > 0){
			if(num_count == 127){//range over
				kijil_map[vc->gs->kijil_size] = num_count;
				vc->gs->kijil_size++;
				num_count = 0;
			}
			if(gp_reverse_table[i].dirty == 0) num_count++; //continuous valid blk
			else{//valid is end
				kijil_map[vc->gs->kijil_size] = num_count;
				vc->gs->kijil_size++;
				num_count = -1;
			}
		}
		else if(num_count < 0){
			if(num_count == -127){//range over
				kijil_map[vc->gs->kijil_size] = num_count;//recording count
				vc->gs->kijil_size++;
				//printk("recording invalid count... count %d\n", num_count);
				num_count = 0;
			}
			if(gp_reverse_table[i].dirty == 1) num_count--;//continuous invalid blk
			else{//invalid is end
				kijil_map[vc->gs->kijil_size] = num_count;
				vc->gs->kijil_size++;
				num_count = 1;
			}
		}
		else printk("unknown else error\n");
	}
	kijil_map[vc->gs->kijil_size++] = num_count;
	//end doing kijil
	/*for(i=0; i<vc->gs->kijil_size; i++){//Printing kijil_map
		printk("%llu:%d ", i, kijil_map[i]);
		if(i != 0 && i%30 == 0)
			printk("\n");
	}*/
	vc->gs->kijil_map = vmalloc(vc->gs->kijil_size);
	memcpy(vc->gs->kijil_map, kijil_map, vc->gs->kijil_size);
	vfree(kijil_map); kijil_map = NULL;
}

inline char point_targeting(struct vm_c *vc){
	unsigned int tp, i, wp_main_dev, min, min_weight;
	struct gc_set *gs = vc->gs;

	//rcu_read_lock();
	for(i=0; i<vc->vms; i++){////need to replace point_targeting
		if(vc->gp_list[i] == 2){
			gs->gp = i;
			break;
		}
		else if(i == vc->vms-1 && vc->gp_list[i] !=2){
			printk("not gc ssd\n");
			vc->mig_flag = 0;
			vfree(gs->block_buffer);
			gs->block_buffer = NULL;
			kfree(gs);
			vc->gs = NULL;
			return 0;
		}
	}
	tp = gs->gp;
	wp_main_dev = vc->vm[vc->wp].main_dev;
	min = tp; min_weight = -1;

	for(i=0; i<vc->vms; i++){
		unsigned weight = 0;
		tp = (tp + 1) % vc->vms;
		if(vc->vm[tp].main_dev == wp_main_dev)
			weight = 5;
		weight += vc->gp_list[tp];
		if(min_weight > weight){//search target device by minimal weight
			min = tp;
			min_weight = weight;
		}
		//printk("gp_list[tp] %u\n", vc->gp_list[tp]);
	}
	gs->tp = min;
	//gs->tp = 3;//////read collision
	//gs->tp = 5;/////write collision
	//rcu_read_unlock();
	
	return 1;
}

inline char weathering_check(struct vm_c *vc){
	if(vc->gs == NULL){
	//if(vc->wp >= 4){
		printk("mig is start\n");
		vc->gs = kmalloc(sizeof(struct gc_set), GFP_KERNEL);

		vc->gs->io_flag = 0;
		vc->gs->cur_sector = -1;
		vc->gs->index = 0;
		vc->gs->block_buffer = vmalloc(4096*127);//maximum size
		vc->gs->kijil_map = NULL;
		vc->gs->kijil_size = 1;
		vc->gs->phase = 2;
		vc->gs->io_client = vc->io_client;
		vc->gs->lock = &vc->lock;
		vc->gs->ptr_ovflw_size = 0;
		vc->gs->vms = vc->vms;


		if(point_targeting(vc) == 0){
			vfree(vc->gs->block_buffer);
			vc->gs->block_buffer = NULL;
			kfree(vc->gs);
			vc->gs = NULL;
			return 0;//failed point targeting
		}

		//gp_list's weight is judge to selecting pointer. policy is avoid to high weight
		//target ptr is write intensive job. gc ptr is read intensive job.
		vc->gp_list[vc->gs->gp] = 3;//3 is garbage collecting...
		vc->gp_list[vc->gs->tp] = 4;//4 is targeting...

		vc->gs->table = vc->fs->table;
		vc->gs->reverse_table = vc->fs->reverse_table;
		do_kijil(vc);////kijil_mapping
		printk("gp %u, tp %u, kijil_size %llu\n", vc->gs->gp, vc->gs->tp, vc->gs->kijil_size);

		vc->gs->phase = 0;
		return 1;
	}
	return 0;
}

static void read_callback(unsigned long error, void* context){
	struct gc_set *gs = (struct gc_set*) context;
	gs->io_flag = 2;
}

static void write_callback(unsigned long error, void* context){
	struct gc_set *gs = (struct gc_set*) context;
	unsigned int size = gs->kijil_map[gs->index] - gs->ptr_ovflw_size;
	struct reverse_nodes* tp_reverse_table = gs->reverse_table[gs->tp];
	unsigned int i;
	if(gs->kijil_map[gs->index] < 0)printk("unknown write_callback's invalid error\n");

	rcu_read_lock();
	for(i=0; i<size; i++){
		struct reverse_nodes* rn = &(rcu_dereference(tp_reverse_table)[gs->tp_io_sector + i]);
		rcu_dereference(gs->table)[rn->index]->msector += (gs->tp_io_sector * 8) + (i*8);//want to block scale
	}
	rcu_read_unlock();

	gs->cur_sector += size * 8;
	gs->index++;
	if(gs->ptr_ovflw_size != 0){// target pointer overflow occur!!!!!!
		gs->index--;
		gs->kijil_map[gs->index] = size;
		gs->ptr_ovflw_size = 0;
		gs->tp = (gs->tp + 1) % gs->vms;
	}
	if(gs->index < gs->kijil_size)
		gs->io_flag = 0;
	else{
		gs->io_flag = 3;
		gs->phase = 1;
		printk("index %llu, size %llu, phase 0 is finished\n", gs->index, gs->kijil_size);
	}
}

static int bgrnd_job(struct dm_target *ti){
	struct vm_c *vc = ti->private;
	
	while(1){
		if(vc->mig_flag == 1){
			if(vc->gs != NULL || weathering_check(vc)){
				struct gc_set *gs;
				rcu_read_lock();
				gs = rcu_dereference(vc->gs);
				rcu_read_unlock();
				if(gs->phase == 0){
					if(gs->kijil_size == 0){
						gs->io_flag = 3;
						gs->phase = 1;//??? 2??
					}
					if(unlikely(gs->cur_sector == -1)){
						gs->cur_sector = vc->vm[gs->gp].physical_start;
						gs->index = 0;
					}
					if(gs->io_flag == 0){
						//printk("1. index %llu, cur_sector %llu, size %d\n", gs->index, (unsigned long long)gs->cur_sector, gs->kijil_map[gs->index]);
						struct dm_io_region io;
						struct dm_io_request io_req;

						while(gs->kijil_map[gs->index] < 0){//if invalid
							gs->cur_sector -= (gs->kijil_map[gs->index] * 8);
							gs->index++;//index and sector increase
							//printk("2. index %llu, cur_sector %llu, size %d\n", gs->index, (unsigned long long)gs->cur_sector, gs->kijil_map[gs->index]);
						}
						
						io_req.bi_rw = READ; io_req.mem.type = DM_IO_VMA;
						io_req.mem.ptr.vma = gs->block_buffer;
						io_req.notify.fn = read_callback; io_req.notify.context = gs; io_req.client = gs->io_client;

						//rcu_read_lock();
						io.bdev = vc->vm[gs->gp].dev->bdev;
						io.sector = gs->cur_sector;
						io.count = (gs->kijil_map[gs->index] * 8);
						//rcu_read_unlock();

						if(io.count != 0 && io.sector + io.count > vc->vm[gs->gp].end_sector){
							/*printk("unknown range over error!\n");
							printk("index %llu, io_sector %llu, io_count %llu\n", gs->index, (unsigned long long)io.sector, (unsigned long long)io.count);*/
							gs->io_flag = 3;
							gs->phase = 1;
						}
						else{
							gs->io_flag = 1;
							dm_io(&io_req, 1, &io, NULL);
						}
					}
					else if(gs->io_flag == 2){
						unsigned int size = gs->kijil_map[gs->index];
						struct dm_io_region io;
						struct dm_io_request io_req;
						struct reverse_nodes* tp_reverse_table = gs->reverse_table[gs->tp];
						struct reverse_nodes* gp_reverse_table = gs->reverse_table[gs->gp];

							unsigned int i;
							unsigned long long cur_sector = gs->cur_sector - vc->vm[gs->gp].physical_start;
							gs->tp_io_sector = vc->ws[gs->tp];///??? What is this?
							do_div(gs->tp_io_sector, 8);//for reduce division op
							do_div(cur_sector, 8);//scaling to block number

							rcu_read_lock();{
							mutex_lock(&vc->lock);
							for(i=0; i<size; i++){
								struct flag_nodes* fn;
								struct reverse_nodes* rn;

								if(vc->ws[gs->tp] + vc->vm[gs->tp].physical_start + 8 > vc->vm[gs->tp].end_sector){
									//printk("over range by ws ws %llu, total %llu\n", vc->ws[gs->tp], vc->ws[gs->tp] + vc->vm[gs->tp].physical_start + 8);
									gs->ptr_ovflw_size = i;
									vc->gp_list[gs->tp] = 2;
									break;
								}

								tp_reverse_table[gs->tp_io_sector+i].index = gp_reverse_table[cur_sector+i].index;//need by block scale
								tp_reverse_table[gs->tp_io_sector+i].dirty = 0;

								rn = &(rcu_dereference(tp_reverse_table)[gs->tp_io_sector+i]);//&tp_reverse_table[gs->tp_io_sector+i];
								fn = rcu_dereference(gs->table)[rn->index];//gs->table[rn->index];
								//rcu_dereference(gs->table)[tp_reverse_table[gs->tp_io_sector+i].index]->msector = rcu_dereference(vc)->vm[gs->tp].physical_start;
								fn->msector = rcu_dereference(vc)->vm[gs->tp].physical_start;
								//if(gs->tp_io_sector+i + 8 > vc->vm[gs->tp].end_sector)printk("over range by tp_io sector, tp_io_sector %llu, total %llu\n", gs->tp_io_sector+i, gs->tp_io_sector+i+8);

								vc->ws[gs->tp] += 8;
							}
							mutex_unlock(&vc->lock);
						}rcu_read_unlock();

						io_req.bi_rw = WRITE; io_req.mem.type = DM_IO_VMA;
						io_req.mem.ptr.vma = gs->block_buffer; io_req.notify.context = gs;
						io_req.notify.fn = write_callback; io_req.client = gs->io_client;

						io.bdev = vc->vm[gs->tp].dev->bdev;
						io.sector = vc->vm[gs->tp].physical_start + (gs->tp_io_sector*8);
						io.count = (gs->kijil_map[gs->index] - gs->ptr_ovflw_size) * 8;

						if(io.count != 0 && io.sector + io.count > vc->vm[gs->tp].end_sector){
							printk("unknown range over error!\n");
						}

						gs->io_flag = 3;
						dm_io(&io_req, 1, &io, NULL);
					}
				}
				if(gs->phase == 1){
					unsigned int i;
					int err = blkdev_issue_discard(vc->vm[gs->gp].dev->bdev,
							vc->vm[gs->gp].physical_start, vc->vm[gs->gp].end_sector-1 - vc->vm[gs->gp].physical_start, GFP_NOFS, 0);
					//vc->vm[gs->gp].end_sector+7 -8 - vc->vm[gs->gp].physical_start					if(err != 0) printk("discard error, %d\n", err);
					printk("dirty_num is %llu\n", vc->d_num[gs->gp]);
					vc->d_num[gs->gp] = 0;
					if(err != 0) printk("unknown discard error %d\n", err);
					printk("end discard\n");
					vfree(gs->kijil_map);
					gs->io_client = NULL;
					gs->phase = 2;
					if(gs->tp != gs->gp)
						vc->ws[gs->gp] = 0;
					vc->gp_list[gs->tp] = 1;//1 is targeted
					vc->gp_list[gs->gp] = 0;//0 is clean

					kfree(gs);
					vc->gs = NULL;
					
					vc->mig_flag = 0;
					for(i=0; i<vc->vms; i++){
						if(vc->gp_list[i] == 2){
							vc->mig_flag = 1;
							break;
						}
					}
				}
			}
			else{//wait for weathering...
				ssleep(1);
			}
		}
		else if(vc->mig_flag == 0){//0 is wait
			ssleep(1);
		}
		//msleep(1);
	}
	return 0;
}

static void vm_map_range_sector(struct vm_c *vc, sector_t sector,
				    uint32_t target_vm, sector_t *result)
{
	uint32_t vm = 0;
	sector_t cur_sector;

	for(cur_sector = vc->vm[vm].physical_start +  sector;
			cur_sector > vc->vm[vm].end_sector;
			cur_sector -= vc->vm[vm].end_sector, vm++);

	if (vm == target_vm)
		return;
}

static int vm_map_range(struct vm_c *vc, struct bio *bio,
			    uint32_t target_vm)
{
	sector_t begin=0, end=0;
	vm_map_range_sector(vc, bio->bi_iter.bi_sector,
			target_vm, &begin);
	vm_map_range_sector(vc, bio_end_sector(bio),/////end sector is start sect + size
			target_vm, &end);

	if (begin < end) {
		bio->bi_bdev = vc->vm[target_vm].dev->bdev;
		bio->bi_iter.bi_sector = begin +
			vc->vm[target_vm].physical_start;
		bio->bi_iter.bi_size = to_bytes(end - begin);
		return DM_MAPIO_REMAPPED;
	} else {
		// The range doesn't map to the target stripe 
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}
}

static inline struct flag_nodes* vm_lfs_map_sector(struct vm_c *vc,
		sector_t target_sector,	unsigned int wp,
		sector_t *write_sector,	struct block_device **bdev,
		unsigned long bi_rw, unsigned int size){
	struct flag_set *fs = vc->fs;
	unsigned long long index = target_sector;
	unsigned int remainder = 0;
	unsigned long long ws;
	remainder = do_div(index, 8);

	//rcu_read_lock();
	mutex_lock(&vc->lock);
	if(fs->table[index]->msector == -1){
		sector_t return_sector;
		//printk("target_sector %llu, index %llu, mapped sector %llu, ws %llu, wp %u\n", (unsigned long long)target_sector, (unsigned long long)index*nnode_per_page, (unsigned long long)return_sector, (unsigned long long)vc->ws[wp], fs->table[index][sect].wp);

		return_sector = vc->vm[wp].physical_start + vc->ws[wp];
		fs->table[index]->msector = return_sector;
		fs->table[index]->wp = wp;
		
		ws = vc->ws[wp];
		do_div(ws, 8);
		//fs->reverse_table[wp][ws].lsector = target_sector;////????
		fs->reverse_table[wp][ws].index = index;
		fs->reverse_table[wp][ws].dirty = 0;

		*bdev = vc->vm[wp].dev->bdev;
		*write_sector = return_sector + remainder;
		vc->ws[wp] += 8;
	}
	else{
		if(bi_rw == WRITE){//write
			//and... new alloc mapped sector
			
			ws = fs->table[index]->msector - vc->vm[fs->table[index]->wp].physical_start;
			do_div(ws, 8);
			fs->reverse_table[wp][ws].dirty = 1;
			vc->d_num[wp]++;

			rcu_read_lock();
			rcu_dereference(fs->table[index])->msector = vc->vm[wp].physical_start + rcu_dereference(vc->ws)[wp];
			rcu_read_unlock();
			fs->table[index]->wp = wp;
			//printk("target_sector %llu, index %llu, mapped sector %llu, ws %llu, wp %u, flag %u\n", (unsigned long long)target_sector, (unsigned long long)index*nnode_per_page, (unsigned long long)fs->table[index][sect].msector, (unsigned long long)vc->ws[wp], fs->table[index][sect].wp, fs->table[index][sect].flag);

			*bdev = vc->vm[fs->table[index]->wp].dev->bdev;
			*write_sector = fs->table[index]->msector + remainder;
			vc->ws[wp] += 8;
		}
		else{//read
			*bdev = vc->vm[fs->table[index]->wp].dev->bdev;
			*write_sector = fs->table[index]->msector + remainder;
		}
	}
	mutex_unlock(&vc->lock);
	//rcu_read_unlock();

	return fs->table[index];
}///////////lfs1

static inline void vm_lfs_map_bio(struct dm_target *ti, struct bio *bio){
	struct vm_c *vc = ti->private;
	struct flag_nodes* temp;
	sector_t backup_sector = bio->bi_iter.bi_sector;
	
	//printk("lfs_map\n");
	if(bio_sectors(bio)){
		temp = vm_lfs_map_sector(vc, backup_sector,
				vc->wp, &bio->bi_iter.bi_sector,
				&bio->bi_bdev, bio_rw(bio), 1);
		/*if(check_range_over(vc)){
			vm_lfs_map_sector(vc, backup_sector,
					vc->wp, &bio->bi_iter.bi_sector,
					&bio->bi_bdev, bio_rw(bio), 1);
			if(!vc->mig_flag) vc->mig_flag = 1;
		}*/
		///need to implement for ws 0 is valid by cold valid data
		if(bio->bi_iter.bi_sector + bio_sectors(bio) > vc->vm[vc->wp].end_sector){
			unsigned long long ws;
			unsigned int next_point;
			unsigned int gp_main_dev;
			unsigned int min;
			unsigned int min_weight;
			unsigned int i;
			printk("ws %llu, start %llu, bi_sector %llu, end_sector %llu, sectors %u\n", (unsigned long long)vc->ws[vc->wp], (unsigned long long)vc->vm[vc->wp].physical_start, (unsigned long long)bio->bi_iter.bi_sector, (unsigned long long)vc->vm[vc->wp].end_sector, bio_sectors(bio));

			/*if(vc->wp == 0)
				//next_point = 1;////read collision
				next_point = 2;////write collision
			else
				next_point = (vc->wp+1) %vc->vms;*/

			vc->gp_list[vc->wp] = 2;//2 is dirty
		
			next_point = vc->wp;
			gp_main_dev = vc->vm[vc->wp].main_dev;
			min = next_point; min_weight = -1;

			for(i=0; i<vc->vms; i++){
				unsigned weight = 0;
				next_point = (next_point + 1) % vc->vms;
				if(vc->vm[next_point].main_dev == gp_main_dev)
					weight = 5;
				weight += vc->gp_list[next_point];
				if(min_weight > weight){//search target device by minimal weight
					min = next_point;
					min_weight = weight;
				}
				//printk("gp_list[tp] %u\n", vc->gp_list[tp]);
			}
			//vc->wp = min;
			
			printk("big!! next wp is %s\n", vc->vm[min].dev->name);
			//printk("bi_sector %llu, +sectors %llu\n", (unsigned long long)bio->bi_iter.bi_sector, (unsigned long long)bio->bi_iter.bi_sector + bio_sectors(bio));
			
			mutex_lock(&vc->lock);
			ws = temp->msector - vc->vm[temp->wp].physical_start;
			do_div(ws, 8);
			vc->fs->reverse_table[temp->wp][ws].dirty = 1;
			temp->msector = -1;
			
			vc->wp = min;
			mutex_unlock(&vc->lock);
			
			vm_lfs_map_sector(vc, backup_sector,
					vc->wp, &bio->bi_iter.bi_sector,
					&bio->bi_bdev, bio_rw(bio), 1);
		
			if(!vc->mig_flag) vc->mig_flag = 1;
		}
	}
}

static int vm_map(struct dm_target *ti, struct bio *bio){
	struct vm_c *vc = ti->private;
	//uint32_t vm;
	unsigned target_bio_nr;

	//printk("vm_map\n");
	if(bio->bi_rw & REQ_FLUSH){
		//printk("flush\n");
		target_bio_nr = dm_bio_get_target_bio_nr(bio);
		BUG_ON(target_bio_nr >= vc->vms);
		bio->bi_bdev = vc->vm[target_bio_nr].dev->bdev;
		return DM_MAPIO_REMAPPED;
	}
	if(unlikely(bio->bi_rw & REQ_DISCARD) ||
			unlikely(bio->bi_rw & REQ_WRITE_SAME)){
		//printk("discard or write same\n");
		target_bio_nr = dm_bio_get_target_bio_nr(bio);
		BUG_ON(target_bio_nr >= vc->vms);
		return vm_map_range(vc, bio, target_bio_nr);
	}

	//if(flags & REQ_FAILFAST_TRANSPORT) printk("failfast_transport\n");//
	//if(flags & REQ_FLUSH_SEQ) printk("flush_seq\n");//
	//if(flags & REQ_IO_STAT) printk("io_stat\n");
	//if(flags & REQ_MIXED_MERGE) printk("mixed_merge\n");//
	//if(flags & REQ_PM) printk("pm\n");//
	if(bio->bi_rw & REQ_THROTTLED){
		printk("REQ_THROTTLED\n");
	}
	vm_lfs_map_bio(ti, bio);

	return DM_MAPIO_REMAPPED;
}

/*
 * Stripe status:
 *
 * INFO
 * #stripes [stripe_name <stripe_name>] [group word count]
 * [error count 'A|D' <error count 'A|D'>]
 *
 * TABLE
 * #stripes [stripe chunk size]
 * [stripe_name physical_start <stripe_name physical_start>]
 *
 */

static void vm_status(struct dm_target *ti, status_type_t type,
			  unsigned status_flags, char *result, unsigned maxlen)
{
	struct vm_c *vc = (struct vm_c *) ti->private;
	char buffer[vc->vms + 1];
	unsigned int sz = 0;
	unsigned int i;

	//printk("vm_status\n");
	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%d ", vc->vms);
		for (i = 0; i < vc->vms; i++)  {
			DMEMIT("%s ", vc->vm[i].dev->name);
			buffer[i] = atomic_read(&(vc->vm[i].error_count)) ?
				'D' : 'A';
		}
		buffer[i] = '\0';
		DMEMIT("1 %s", buffer);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%d %llu", vc->vms,
			(unsigned long long)vc->chunk_size);
		for (i = 0; i < vc->vms; i++)
			DMEMIT(" %s %llu", vc->vm[i].dev->name,
			    (unsigned long long)vc->vm[i].physical_start);
		break;
	}
}

static int vm_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	unsigned i;
	char major_minor[16];
	struct vm_c *vc = ti->private;

	//printk("vm_end_io\n");

	if (!error)
		return 0; /* I/O complete */
	//printk("What?\n");

	if ((error == -EWOULDBLOCK) && (bio->bi_rw & REQ_RAHEAD))
		return error;

	if (error == -EOPNOTSUPP)
		return error;

	memset(major_minor, 0, sizeof(major_minor));
	sprintf(major_minor, "%d:%d",
		MAJOR(disk_devt(bio->bi_bdev->bd_disk)),
		MINOR(disk_devt(bio->bi_bdev->bd_disk)));

	/*
	 * Test to see which stripe drive triggered the event
	 * and increment error count for all stripes on that device.
	 * If the error count for a given device exceeds the threshold
	 * value we will no longer trigger any further events.
	 */
	for (i = 0; i < vc->vms; i++)
		if (!strcmp(vc->vm[i].dev->name, major_minor)) {
			atomic_inc(&(vc->vm[i].error_count));
			if (atomic_read(&(vc->vm[i].error_count)) <
			    DM_IO_ERROR_THRESHOLD)
				schedule_work(&vc->trigger_event);
		}

	return error;
}

/*static int vm_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct vm_c *vc = ti->private;
	int ret = 0;
	unsigned i = 0;

	printk("vm_iterate_devices\n");
	do {
		ret = fn(ti, vc->vm[i].dev,
			 vc->vm[i].physical_start,
			 vc->vm_width, data);
	} while (!ret && ++i < vc->vms);

	return ret;
}*/

static struct target_type vm_target = {
	.name   = "striped",
	.version = {1, 5, 1},
	.module = THIS_MODULE,
	.ctr    = vm_ctr,
	.dtr    = vm_dtr,
	.map    = vm_map,
	.end_io = vm_end_io,
	.status = vm_status,
	//.iterate_devices = vm_iterate_devices,
	//.io_hints = vm_io_hints,
};

int __init dm_stripe_init(void)
{
	int r;

	r = dm_register_target(&vm_target);
	if (r < 0) {
		DMWARN("target registration failed");
		return r;
	}

	return r;
}

void dm_stripe_exit(void)
{
	dm_unregister_target(&vm_target);
}

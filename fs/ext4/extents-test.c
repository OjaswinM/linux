// SPDX-License-Identifier: GPL-2.0
/*
 * Written by Ojaswin Mujoo <ojaswin@linux.ibm.com> (IBM)
 *
 * These Kunit tests are designed to test the functionality of
 * extent split and conversion in ext4.
 *
 * Currently, ext4 can split extents in 2 ways:
 * 1. By splitting the extents in the extent tree and optionally converting them
 *    to written or unwritten based on flags passed.
 * 2. In case 1 encounters an error, ext4 instead zerooes out the unwritten
 *    areas of the extent and marks the complete extent written.
 *
 * The primary function that handles this is ext4_split_convert_extents().
 *
 * We test both of the methods of split. The behavior we try to enforce is:
 * 1. When passing EXT4_GET_BLOCKS_CONVERT flag to ext4_split_convert_extents(),
 *    the split extent should be converted to initialized.
 * 2. When passing EXT4_GET_BLOCKS_CONVERT_UNWRITTEN flag to
 *    ext4_split_convert_extents(), the split extent should be converted to
 *    uninitialized.
 * 3. In case we use the zeroout method, then we should correctly write zeroes
 *    to the unwritten areas of the extent and we should not corrupt/leak any
 *    data.
 *
 * Enforcing 1 and 2 is straight forward, we just setup a minimal inode with
 * extent tree, call ext4_split_convert_extents() and check the final state of
 * the extent tree.
 *
 * For zeroout testing, we maintain a separate buffer which represents the disk
 * data corresponding to the extents. We then override ext4's zeroout functions
 * to instead write zeroes to our buffer. Then, we override
 * ext4_ext_insert_extent() to return -ENOSPC, which triggers the zeroout.
 * Finally, we check the state of the extent tree and zeroout buffer to confirm
 * everything went well.
 */

#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/fs_context.h>
#include <linux/gfp_types.h>
#include <linux/stddef.h>

#include "ext4.h"
#include "ext4_extents.h"

#define EXT_DATA_PBLK 100
#define EXT_DATA_LBLK 10
#define EXT_DATA_LEN 3

struct kunit_ctx {
	/*
	 * Ext4 inode which has only 1 unwrit extent
	 */
	struct ext4_inode_info *k_ei;
	/*
	 * Represents the underlying data area (used for zeroout testing)
	 */
	char *k_data;
} k_ctx;

/*
 * describes the state of an expected extent in extent tree.
 */
struct kunit_ext_state {
	ext4_lblk_t ex_lblk;
	ext4_lblk_t ex_len;
	ext4_fsblk_t ex_pblk;
	bool is_unwrit;
};

/*
 * describes the state of the data area of a writ extent. Used for testing
 * correctness of zeroout.
 */
struct kunit_ext_data_state {
	char exp_char;
	ext4_lblk_t off_blk;
	ext4_lblk_t len_blk;
};

enum kunit_test_types {
	/*
	 * Tests unwritten to written split-convert via endio path -
	 * ext4_convert_unwritten_extents_endio()
	 */
	TEST_UNWRIT_TO_WRIT_ENDIO,

	/*
	 * Tests unwritten to written split-convert via non endio path -
	 * ext4_ext_convert_to_initialized()
	 */
	TEST_UNWRIT_TO_WRIT_NON_ENDIO,

	/*
	 * Tests written to unwritten split-convert -
	 * convert_initialized_extent()
	 */
	TEST_WRIT_TO_UNWRIT,
};

enum kunit_test_subtypes {
	/* Test the path for actual extent split */
	TEST_EXTENT_SPLIT,

	/* Test when actual extent split fails and we fallback to zeroout */
	TEST_ZEROOUT_FALLBACK
};

/*
 * initial extent(s) to be created in the inode.
 */
static struct kunit_ext_state kunit_ext_single_unwrit[] = {
	{
		.ex_lblk = EXT_DATA_LBLK,
		.ex_len = EXT_DATA_LEN,
		.ex_pblk = EXT_DATA_PBLK,
		.is_unwrit = true,
	},
	{ }
};

static struct kunit_ext_state kunit_ext_single_writ[] = {
	{
		.ex_lblk = EXT_DATA_LBLK,
		.ex_len = EXT_DATA_LEN,
		.ex_pblk = EXT_DATA_PBLK,
		.is_unwrit = false,
	},
	{ }
};

struct kunit_ext_test_param {
	/* description of test */
	char *desc;

	/* determines which function will be tested */
	int type;

	/* determines the subtype of test */
	int subtype;

	/*
	 * The initial state of extents before the test begins.
	 * If NULL then a single written extent is added.
	 */
	struct kunit_ext_state *init_state;

	/* flags to pass while splitting */
	int split_flags;

	/* map describing range to split */
	struct ext4_map_blocks split_map;

	/* no of extents expected after split */
	int nr_exp_ext;

	/*
	 * expected state of extents after split. We will never split into more
	 * than 3 extents
	 */
	struct kunit_ext_state exp_ext_state[3];

	/* Below fields used for TEST_ZEROOUT_FALLBACK subtype tests */

	/*
	 * no of expected data segments (zeroout tests). Example, if we expect
	 * data to be 4kb 0s, followed by 8kb non-zero, then nr_exp_data_segs==2
	 */
	int nr_exp_data_segs;

	/*
	 * expected state of data area after zeroout.
	 */
	struct kunit_ext_data_state exp_data_state[3];
};

static void ext_kill_sb(struct super_block *sb)
{
	generic_shutdown_super(sb);
}

static int ext_init_fs_context(struct fs_context *fc)
{
	return 0;
}

static int ext_set(struct super_block *sb, struct fs_context *fc)
{
	return 0;
}

static struct file_system_type ext_fs_type = {
	.name		 = "extents test",
	.init_fs_context = ext_init_fs_context,
	.kill_sb	 = ext_kill_sb,
};

static void extents_kunit_exit(struct kunit *test)
{
	struct ext4_sb_info *sbi;

	if (!k_ctx.k_ei)
		return;

	sbi = k_ctx.k_ei->vfs_inode.i_sb->s_fs_info;
	ext4_es_unregister_shrinker(sbi);
	deactivate_super(sbi->s_sb);
	kfree(sbi);
	kfree(k_ctx.k_ei);
	kfree(k_ctx.k_data);
}

static int __ext4_ext_dirty_stub(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct ext4_ext_path *path)
{
	return 0;
}

static struct ext4_ext_path *
ext4_ext_insert_extent_stub(handle_t *handle, struct inode *inode,
			    struct ext4_ext_path *path,
			    struct ext4_extent *newext, int gb_flags)
{
	return ERR_PTR(-ENOSPC);
}

/*
 * We will zeroout the equivalent range in the data area
 */
static int ext4_ext_zeroout_stub(struct inode *inode, struct ext4_extent *ex)
{
	ext4_lblk_t ee_block, off_blk;
	loff_t ee_len;
	loff_t off_bytes;
	struct kunit *test = kunit_get_current_test();

	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);

	KUNIT_EXPECT_EQ_MSG(test, 1, ee_block >= EXT_DATA_LBLK, "ee_block=%d",
			    ee_block);
	KUNIT_EXPECT_EQ(test, 1,
			ee_block + ee_len <= EXT_DATA_LBLK + EXT_DATA_LEN);

	off_blk = ee_block - EXT_DATA_LBLK;
	off_bytes = off_blk << inode->i_sb->s_blocksize_bits;
	memset(k_ctx.k_data + off_bytes, 0,
	       ee_len << inode->i_sb->s_blocksize_bits);

	return 0;
}

static int ext4_issue_zeroout_stub(struct inode *inode, ext4_lblk_t lblk,
				   ext4_fsblk_t pblk, ext4_lblk_t len)
{
	ext4_lblk_t off_blk;
	loff_t off_bytes;
	struct kunit *test = kunit_get_current_test();

	kunit_log(KERN_ALERT, test,
		  "%s: lblk=%u pblk=%llu len=%u", __func__, lblk, pblk, len);
	KUNIT_EXPECT_EQ(test, 1, lblk >= EXT_DATA_LBLK);
	KUNIT_EXPECT_EQ(test, 1, lblk + len <= EXT_DATA_LBLK + EXT_DATA_LEN);
	KUNIT_EXPECT_EQ(test, 1, lblk - EXT_DATA_LBLK == pblk - EXT_DATA_PBLK);

	off_blk = lblk - EXT_DATA_LBLK;
	off_bytes = off_blk << inode->i_sb->s_blocksize_bits;
	memset(k_ctx.k_data + off_bytes, 0,
	       len << inode->i_sb->s_blocksize_bits);

	return 0;
}

/*
 * Right now, the extent split path is pretty nuanced with various constraints
 * and optimizations thrown around. This holds all the test specific tuning
 * logic needed to unit test various paths.
 */
static void extents_kunit_tune(struct kunit *test) {
	struct kunit_ext_test_param *param =
		(struct kunit_ext_test_param *)(test->param_value);
	struct ext4_sb_info *sbi = k_ctx.k_ei->vfs_inode.i_sb->s_fs_info;

	if (param->subtype == TEST_ZEROOUT_FALLBACK) {
		/*
		 * Force zeroout fallback by making ext4_ext_insert_extent
		 * return ENOSPC
		 */
		kunit_activate_static_stub(test, ext4_ext_insert_extent,
					   ext4_ext_insert_extent_stub);

		/*
		 * To test zeroout fallback for written to unwritten conversion,
		 * s_extent_max_zeroout_kb must be set. See ext4_split_extent_at().
		 */
		if (param->type == TEST_WRIT_TO_UNWRIT)
			sbi->s_extent_max_zeroout_kb = 32;

	}
}

static int extents_kunit_init(struct kunit *test)
{
	struct ext4_extent_header *eh = NULL;
	struct ext4_inode_info *ei;
	struct inode *inode;
	struct super_block *sb;
	struct fs_context *fc;
	struct ext4_sb_info *sbi = NULL;
	struct kunit_ext_test_param *param =
		(struct kunit_ext_test_param *)(test->param_value);
	int err;


	if (!param)
		KUNIT_FAIL(test,
			   "All tests expect ->param to be set.");

	sbi = kzalloc_obj(struct ext4_sb_info);
	if (sbi == NULL)
		return -ENOMEM;

	fc = fs_context_for_mount(&ext_fs_type, 0);
	if (IS_ERR(fc)) {
		kfree(sbi);
		return PTR_ERR(fc);
	}
	sb = sget_fc(fc, NULL, ext_set);
	put_fs_context(fc);
	if (IS_ERR(sb)) {
		kfree(sbi);
		return PTR_ERR(sb);
	}

	sbi->s_sb = sb;
	sb->s_fs_info = sbi;

	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;

	err = ext4_es_register_shrinker(sbi);
	if (err)
		goto out_deactivate;

	/* setup the mock inode */
	k_ctx.k_ei = kzalloc_obj(struct ext4_inode_info);
	if (k_ctx.k_ei == NULL) {
		err = -ENOMEM;
		goto out;
	}
	ei = k_ctx.k_ei;
	inode = &ei->vfs_inode;

	ext4_es_init_tree(&ei->i_es_tree);
	rwlock_init(&ei->i_es_lock);
	INIT_LIST_HEAD(&ei->i_es_list);
	ei->i_es_all_nr = 0;
	ei->i_es_shk_nr = 0;
	ei->i_es_shrink_lblk = 0;

	ei->i_disksize = (EXT_DATA_LBLK + EXT_DATA_LEN + 10)
			 << sb->s_blocksize_bits;
	ei->i_flags = 0;
	ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
	inode->i_sb = sb;

	k_ctx.k_data = kzalloc(EXT_DATA_LEN * 4096, GFP_KERNEL);
	if (k_ctx.k_data == NULL) {
		err = -ENOMEM;
		goto out;
	}

	/*
	 * set the data area to a junk value
	 */
	memset(k_ctx.k_data, 'X', EXT_DATA_LEN * 4096);

	/* create a tree with depth 0 */
	eh = (struct ext4_extent_header *)k_ctx.k_ei->i_data;

	/* Fill extent header */
	eh = ext_inode_hdr(&k_ctx.k_ei->vfs_inode);
	eh->eh_depth = 0;
	eh->eh_entries = cpu_to_le16(1);
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = cpu_to_le16(ext4_ext_space_root_idx_test(
					&k_ctx.k_ei->vfs_inode, 0));
	eh->eh_generation = 0;

	/*
	 * Add extent(s) in leaf node using the global extent configuration
	 */
	struct kunit_ext_state *ext_config_array;
	int nr_extents;
	int i;
	struct ext4_extent *ex;

	/* Select config array based on test parameter */
	if (!param || !param->init_state)
		ext_config_array = kunit_ext_single_writ;
	else
		ext_config_array = param->init_state;

	/*
	 * the state array must always end with NULL entry hence ex_len != 0
	 * check works
	 */
	for (i = 0; i < 4 && ext_config_array[i].ex_len != 0; i++)
		nr_extents++;

	if (nr_extents > 4) {
		kunit_skip(test, "More than 4 extents can't be stored inline in the inode, hence not supported");
		err = -EINVAL;
		goto out;
	}

	eh->eh_entries = cpu_to_le16(nr_extents);

	/* Add each extent from the init state array to the inode */
	for (i = 0; i < nr_extents; i++) {
		ex = EXT_FIRST_EXTENT(eh) + i;
		ex->ee_block = cpu_to_le32(ext_config_array[i].ex_lblk);
		ex->ee_len = cpu_to_le16(ext_config_array[i].ex_len);
		ext4_ext_store_pblock(ex, ext_config_array[i].ex_pblk);
		if (ext_config_array[i].is_unwrit)
			ext4_ext_mark_unwritten(ex);

		ext4_es_insert_extent(inode, ext_config_array[i].ex_lblk,
				      ext_config_array[i].ex_len,
				      ext_config_array[i].ex_pblk,
				      ext_config_array[i].is_unwrit ?
					      EXTENT_STATUS_UNWRITTEN :
					      EXTENT_STATUS_WRITTEN,
				      0);
	}

	/* Add stubs */
	kunit_activate_static_stub(test, __ext4_ext_dirty,
				   __ext4_ext_dirty_stub);
	kunit_activate_static_stub(test, ext4_ext_zeroout, ext4_ext_zeroout_stub);
	kunit_activate_static_stub(test, ext4_issue_zeroout,
				   ext4_issue_zeroout_stub);

	/* Test specific tuning */
	extents_kunit_tune(test);

	up_write(&sb->s_umount);

	return 0;

out:
	kfree(k_ctx.k_ei);
	k_ctx.k_ei = NULL;

	kfree(k_ctx.k_data);
	k_ctx.k_data = NULL;

	ext4_es_unregister_shrinker(sbi);
out_deactivate:
	deactivate_locked_super(sb);
	kfree(sbi);

	return err;
}

/*
 * Return 1 if all bytes in the buf equal to c, else return the offset of first mismatch
 */
static int check_buffer(char *buf, int c, int size)
{
	void *ret = NULL;

	ret = memchr_inv(buf, c, size);
	if (ret  == NULL)
		return 0;

	kunit_log(KERN_ALERT, kunit_get_current_test(),
		  "# %s: wrong char found at offset %u (expected:%d got:%d)", __func__,
		  (u32)((char *)ret - buf), c, *((char *)ret));
	return 1;
}

/*
 * Simulate a map block call by first calling ext4_map_query_blocks() to
 * correctly populate map flags and pblk and then call the
 * ext4_map_create_blocks() to do actual split and conversion. This is easier
 * than calling ext4_map_blocks() because that needs mocking a lot of unrelated
 * functions.
 */
static void ext4_map_create_blocks_helper(struct kunit *test,
					  struct inode *inode,
					  struct ext4_map_blocks *map,
					  int flags)
{
	int retval = 0;

	retval = ext4_map_query_blocks(NULL, inode, map, flags);
	if (retval < 0) {
		KUNIT_FAIL(test,
			   "ext4_map_query_blocks() failed. Cannot proceed\n");
		return;
	}

	ext4_map_create_blocks(NULL, inode, map, flags);
}

static void test_split_convert(struct kunit *test)
{
	struct ext4_ext_path *path;
	struct inode *inode = &k_ctx.k_ei->vfs_inode;
	struct ext4_extent *ex;
	struct ext4_map_blocks map;
	const struct kunit_ext_test_param *param =
		(const struct kunit_ext_test_param *)(test->param_value);
	int blkbits = inode->i_sb->s_blocksize_bits;

	path = ext4_find_extent(inode, EXT_DATA_LBLK, NULL, EXT4_EX_NOCACHE);
	ex = path->p_ext;
	KUNIT_EXPECT_EQ(test, EXT_DATA_LBLK, le32_to_cpu(ex->ee_block));
	KUNIT_EXPECT_EQ(test, EXT_DATA_LEN, ext4_ext_get_actual_len(ex));
	if (param->subtype == TEST_ZEROOUT_FALLBACK)
		KUNIT_EXPECT_EQ(test, 0,
				check_buffer(k_ctx.k_data, 'X',
					     EXT_DATA_LEN << blkbits));

	map.m_lblk = param->split_map.m_lblk;
	map.m_len = param->split_map.m_len;

	ext4_map_create_blocks_helper(test, inode, &map, param->split_flags);

	path = ext4_find_extent(inode, EXT_DATA_LBLK, NULL, EXT4_EX_NOCACHE);
	ex = path->p_ext;

	for (int i = 0; i < param->nr_exp_ext; i++) {
		struct kunit_ext_state exp_ext = param->exp_ext_state[i];
		struct extent_status es;
		int contains_ex, ex_end, es_end, es_pblk;

		KUNIT_EXPECT_EQ(test, exp_ext.ex_lblk,
				le32_to_cpu(ex->ee_block));
		KUNIT_EXPECT_EQ(test, exp_ext.ex_len,
				ext4_ext_get_actual_len(ex));
		KUNIT_EXPECT_EQ(test, exp_ext.is_unwrit,
				ext4_ext_is_unwritten(ex));
		/*
		 * Confirm extent cache is in sync. Note that es cache can be
		 * merged even when on-disk extents are not so take that into
		 * account.
		 *
		 * Also, ext4_split_convert_extents() forces EXT4_EX_NOCACHE hence
		 * es status are ignored for that case.
		 */
		ext4_es_lookup_extent(inode, le32_to_cpu(ex->ee_block), NULL,
				      &es, NULL);

		ex_end = exp_ext.ex_lblk + exp_ext.ex_len;
		es_end = es.es_lblk + es.es_len;
		contains_ex = es.es_lblk <= exp_ext.ex_lblk && es_end >= ex_end;
		es_pblk = ext4_es_pblock(&es) + (exp_ext.ex_lblk - es.es_lblk);

		KUNIT_EXPECT_EQ(test, contains_ex, 1);
		KUNIT_EXPECT_EQ(test, ext4_ext_pblock(ex), es_pblk);
		KUNIT_EXPECT_EQ(test, 1,
				(exp_ext.is_unwrit &&
				 ext4_es_is_unwritten(&es)) ||
					(!exp_ext.is_unwrit &&
					 ext4_es_is_written(&es)));

		/* Only printed on failure */
		kunit_log(KERN_INFO, test,
			  "# [extent %d] exp: lblk:%d len:%d unwrit:%d \n", i,
			  exp_ext.ex_lblk, exp_ext.ex_len, exp_ext.is_unwrit);
		kunit_log(KERN_INFO, test,
			  "# [extent %d] got: lblk:%d len:%d unwrit:%d\n", i,
			  le32_to_cpu(ex->ee_block),
			  ext4_ext_get_actual_len(ex),
			  ext4_ext_is_unwritten(ex));
		kunit_log(
			KERN_INFO, test,
			"# [extent %d] es: lblk:%d len:%d pblk:%lld type:0x%x\n",
			i, es.es_lblk, es.es_len, ext4_es_pblock(&es),
			ext4_es_type(&es));
		kunit_log(KERN_INFO, test, "------------------\n");

		ex = ex + 1;
	}

	if (param->subtype != TEST_ZEROOUT_FALLBACK)
		return;

	/*
	 * Check that then data area has been zeroed out correctly
	 */
	for (int i = 0; i < param->nr_exp_data_segs; i++) {
		loff_t off, len;
		struct kunit_ext_data_state exp_data_seg = param->exp_data_state[i];

		off = exp_data_seg.off_blk << blkbits;
		len = exp_data_seg.len_blk << blkbits;
		KUNIT_EXPECT_EQ_MSG(test, 0,
				    check_buffer(k_ctx.k_data + off,
						 exp_data_seg.exp_char, len),
				    "# corruption in byte range [%lld, %lld)",
				    off, len);
	}

	return;
}

/* Tests to trigger ext4_ext_map_blocks() -> convert_initialized_extent() */
static const struct kunit_ext_test_param test_convert_initialized_params[] = {
	/* writ to unwrit splits */
	{ .desc = "convert writ extent to unwrit",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_EXTENT_SPLIT,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .init_state = kunit_ext_single_writ,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 1 } } },
	{ .desc = "split writ extent to 2 extents and convert 1st half unwrit",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_EXTENT_SPLIT,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .init_state = kunit_ext_single_writ,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 0 } } },
	{ .desc = "split writ extent to 2 extents and convert 2nd half unwrit",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_EXTENT_SPLIT,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .init_state = kunit_ext_single_writ,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 1 } } },
	{ .desc = "split writ extent to 2 convert 2nd half unwrit - range extends beyond extent",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_EXTENT_SPLIT,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .init_state = kunit_ext_single_writ,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 1 } } },
	{ .desc = "split writ extent to 3 extents and convert 2nd half to unwrit",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_EXTENT_SPLIT,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .init_state = kunit_ext_single_writ,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 3,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 2,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1 + (EXT_DATA_LEN - 2),
			       .ex_len = 1,
			       .is_unwrit = 0 } } },

	/* writ to unwrit splits (zeroout) */
	{ .desc = "split writ extent to 2 extents and convert 1st half unwrit (zeroout)",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_writ,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split writ extent to 2 extents and convert 2nd half unwrit (zeroout)",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_writ,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 'X', .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 0,
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split writ extent to 2 and convert 2nd half unwrit - range extends beyond extent (zeroout)",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_writ,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN  },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 'X', .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 0,
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split writ extent to 3 extents and convert 2nd half unwrit (zeroout)",
	  .type = TEST_WRIT_TO_UNWRIT,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_writ,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT_UNWRITTEN,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 3,
	  .exp_data_state = { { .exp_char = 'X', .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 0,
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 2 },
			      { .exp_char = 'X',
				.off_blk = EXT_DATA_LEN - 1,
				.len_blk = 1 } } },
};

/* Tests to trigger ext4_ext_map_blocks() -> ext4_ext_handle_unwritten_exntents() */
static const struct kunit_ext_test_param test_handle_unwritten_params[] = {
	/* unwrit to writ splits via endio path */
	{ .desc = "convert unwrit extent to writ (endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 1st half writ (endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 1 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 2nd half writ (endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 2 and convert 2nd half writ - range extends beyond extent (endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 3 extents and convert 2nd half to writ (endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 3,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 2,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1 + (EXT_DATA_LEN - 2),
			       .ex_len = 1,
			       .is_unwrit = 1 } } },

	/* unwrit to writ splits via non-endio path */
	{ .desc = "convert unwrit extent to writ (non endio)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 1st half writ (non endio)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 1 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 2nd half writ (non endio)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 2 and convert 2nd half writ - range extends beyond extent (non endio)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 2,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 1,
			       .is_unwrit = 0 } } },
	{ .desc = "split unwrit extent to 3 extents and convert 2nd half to writ (non endio)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_EXTENT_SPLIT,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 3,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = 1,
			       .is_unwrit = 1 },
			     { .ex_lblk = EXT_DATA_LBLK + 1,
			       .ex_len = EXT_DATA_LEN - 2,
			       .is_unwrit = 0 },
			     { .ex_lblk = EXT_DATA_LBLK + 1 + (EXT_DATA_LEN - 2),
			       .ex_len = 1,
			       .is_unwrit = 1 } } },

	/*
	 * ***** zeroout tests *****
	 */
	/* unwrit to writ splits (endio)*/
	{ .desc = "split unwrit extent to 2 extents and convert 1st half writ (endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 'X', .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 0,
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 2nd half writ (endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 2 and convert 2nd half writ - range extends beyond extent (endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 3 extents and convert 2nd half writ (endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CONVERT,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 3,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 2 },
			      { .exp_char = 0,
				.off_blk = EXT_DATA_LEN - 1,
				.len_blk = 1 } } },

	/* unwrit to writ splits (non-endio)*/
	{ .desc = "split unwrit extent to 2 extents and convert 1st half writ (non-endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK, .m_len = 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 'X', .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 0,
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 2 extents and convert 2nd half writ (non-endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 1 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 2 and convert 2nd half writ - range extends beyond extent (non-endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 2,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 1 } } },
	{ .desc = "split unwrit extent to 3 extents and convert 2nd half writ (non-endio, zeroout)",
	  .type = TEST_UNWRIT_TO_WRIT_NON_ENDIO,
	  .subtype = TEST_ZEROOUT_FALLBACK,
	  .init_state = kunit_ext_single_unwrit,
	  .split_flags = EXT4_GET_BLOCKS_CREATE,
	  .split_map = { .m_lblk = EXT_DATA_LBLK + 1, .m_len = EXT_DATA_LEN - 2 },
	  .nr_exp_ext = 1,
	  .exp_ext_state = { { .ex_lblk = EXT_DATA_LBLK,
			       .ex_len = EXT_DATA_LEN,
			       .is_unwrit = 0 } },
	  .nr_exp_data_segs = 3,
	  .exp_data_state = { { .exp_char = 0, .off_blk = 0, .len_blk = 1 },
			      { .exp_char = 'X',
				.off_blk = 1,
				.len_blk = EXT_DATA_LEN - 2 },
			      { .exp_char = 0,
				.off_blk = EXT_DATA_LEN - 1,
				.len_blk = 1 } } },
};

static void ext_get_desc(struct kunit *test, const void *p, char *desc)

{
	struct kunit_ext_test_param *param = (struct kunit_ext_test_param *)p;

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s\n", param->desc);
}

static int test_convert_initialized_param_init(struct kunit *test)
{
	size_t arr_size = ARRAY_SIZE(test_convert_initialized_params);

	kunit_register_params_array(test, test_convert_initialized_params,
				    arr_size, ext_get_desc);
	return 0;
}

static int test_handle_unwritten_init(struct kunit *test)
{
	size_t arr_size = ARRAY_SIZE(test_handle_unwritten_params);

	kunit_register_params_array(test, test_handle_unwritten_params,
				    arr_size, ext_get_desc);
	return 0;
}

/*
 * Note that we use KUNIT_CASE_PARAM_WITH_INIT() instead of the more compact
 * KUNIT_ARRAY_PARAM() because the later currently has a limitation causing the
 * output parsing to be prone to error. For more context:
 *
 * https://lore.kernel.org/linux-kselftest/aULJpTvJDw9ctUDe@li-dc0c254c-257c-11b2-a85c-98b6c1322444.ibm.com/
 */
static struct kunit_case extents_test_cases[] = {
	KUNIT_CASE_PARAM_WITH_INIT(test_split_convert, kunit_array_gen_params,
				   test_convert_initialized_param_init, NULL),
	KUNIT_CASE_PARAM_WITH_INIT(test_split_convert, kunit_array_gen_params,
				   test_handle_unwritten_init, NULL),
	{}
};

static struct kunit_suite extents_test_suite = {
	.name = "ext4_extents_test",
	.init = extents_kunit_init,
	.exit = extents_kunit_exit,
	.test_cases = extents_test_cases,
};

kunit_test_suites(&extents_test_suite);

MODULE_LICENSE("GPL");

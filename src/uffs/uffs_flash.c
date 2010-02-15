/*
  This file is part of UFFS, the Ultra-low-cost Flash File System.
  
  Copyright (C) 2005-2009 Ricky Zheng <ricky_gz_zheng@yahoo.co.nz>

  UFFS is free software; you can redistribute it and/or modify it under
  the GNU Library General Public License as published by the Free Software 
  Foundation; either version 2 of the License, or (at your option) any
  later version.

  UFFS is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
  or GNU Library General Public License, as applicable, for more details.
 
  You should have received a copy of the GNU General Public License
  and GNU Library General Public License along with UFFS; if not, write
  to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA  02110-1301, USA.

  As a special exception, if other files instantiate templates or use
  macros or inline functions from this file, or you compile this file
  and link it with other works to produce a work based on this file,
  this file does not by itself cause the resulting work to be covered
  by the GNU General Public License. However the source code for this
  file must still be made available in accordance with section (3) of
  the GNU General Public License v2.
 
  This exception does not invalidate any other reasons why a work based
  on this file might be covered by the GNU General Public License.
*/

/** 
 * \file uffs_flash.c
 * \brief UFFS flash interface
 * \author Ricky Zheng, created 17th July, 2009
 */
#include "uffs/uffs_config.h"
#include "uffs/uffs_public.h"
#include "uffs/uffs_ecc.h"
#include "uffs/uffs_flash.h"
#include "uffs/uffs_device.h"
#include "uffs/uffs_badblock.h"
#include <string.h>

#define PFX "Flash: "

#define SPOOL(dev) &((dev)->mem.spare_pool)

#define ECC_SIZE(dev) (3 * (dev)->attr->page_data_size / 256)
#define TAG_STORE_SIZE	(sizeof(struct uffs_TagStoreSt))


static void TagMakeEcc(struct uffs_TagStoreSt *ts)
{
	ts->tag_ecc = 0xFFF;
	ts->tag_ecc = uffs_EccMake8(ts, sizeof(struct uffs_TagStoreSt));
}

static int TagEccCorrect(struct uffs_TagStoreSt *ts)
{
	u16 ecc_store, ecc_read;
	int ret;

	ecc_store = ts->tag_ecc;
	ts->tag_ecc = 0xFFF;
	ecc_read = uffs_EccMake8(ts, sizeof(struct uffs_TagStoreSt));
	ret = uffs_EccCorrect8(ts, ecc_read, ecc_store, sizeof(struct uffs_TagStoreSt));
	ts->tag_ecc = ecc_store;	// restore tag ecc

	return ret;

}

/** setup UFFS spare data & ecc layout */
static void InitSpareLayout(uffs_Device *dev)
{
	u8 s; // status byte offset
	u8 *p;

	s = dev->attr->block_status_offs;

	if (s < TAG_STORE_SIZE) {	/* status byte is within 0 ~ TAG_STORE_SIZE-1 */

		/* spare data layout */
		p = dev->attr->_uffs_data_layout;
		if (s > 0) {
			*p++ = 0;
			*p++ = s;
		}
		*p++ = s + 1;
		*p++ = TAG_STORE_SIZE - s;
		*p++ = 0xFF;
		*p++ = 0;

		/* spare ecc layout */
		p = dev->attr->_uffs_ecc_layout;
		*p++ = TAG_STORE_SIZE + 1;
		*p++ = ECC_SIZE(dev);
		*p++ = 0xFF;
		*p++ = 0;
	}
	else {	/* status byte > TAG_STORE_SIZE-1 */

		/* spare data layout */
		p = dev->attr->_uffs_data_layout;
		*p++ = 0;
		*p++ = TAG_STORE_SIZE;
		*p++ = 0xFF;
		*p++ = 0;

		/* spare ecc layout */
		p = dev->attr->_uffs_ecc_layout;
		if (s < TAG_STORE_SIZE + ECC_SIZE(dev)) {
			if (s > TAG_STORE_SIZE) {
				*p++ = TAG_STORE_SIZE;
				*p++ = s - TAG_STORE_SIZE;
			}
			*p++ = s + 1;
			*p++ = TAG_STORE_SIZE + ECC_SIZE(dev) - s;
		}
		else {
			*p++ = TAG_STORE_SIZE;
			*p++ = ECC_SIZE(dev);
		}
		*p++ = 0xFF;
		*p++ = 0;
	}

	dev->attr->data_layout = dev->attr->_uffs_data_layout;
	dev->attr->ecc_layout = dev->attr->_uffs_ecc_layout;
}

static int CalculateSpareDataSize(uffs_Device *dev)
{
	const u8 *p;
	int ecc_last = 0, tag_last = 0;
	int ecc_size, tag_size;
	int n;

	ecc_size = ECC_SIZE(dev);
	
	p = dev->attr->ecc_layout;
	if (p) {
		while (*p != 0xFF && ecc_size > 0) {
			n = (p[1] > ecc_size ? ecc_size : p[1]);
			ecc_last = p[0] + n;
			ecc_size -= n;
			p += 2;
		}
	}

	tag_size = TAG_STORE_SIZE;
	p = dev->attr->data_layout;
	if (p) {
		while (*p != 0xFF && tag_size > 0) {
			n = (p[1] > tag_size ? tag_size : p[1]);
			tag_last = p[0] + n;
			tag_size -= n;
			p += 2;
		}
	}

	n = (ecc_last > tag_last ? ecc_last : tag_last);
	n = (n > dev->attr->block_status_offs + 1 ? n : dev->attr->block_status_offs + 1);

	return n;
}


/**
 * Initialize UFFS flash interface
 */
URET uffs_FlashInterfaceInit(uffs_Device *dev)
{
	struct uffs_StorageAttrSt *attr = dev->attr;
	uffs_Pool *pool = SPOOL(dev);

	if (dev->mem.spare_pool_size == 0) {
		if (dev->mem.malloc) {
			dev->mem.spare_pool_buf = dev->mem.malloc(dev, UFFS_SPARE_BUFFER_SIZE);
			if (dev->mem.spare_pool_buf)
				dev->mem.spare_pool_size = UFFS_SPARE_BUFFER_SIZE;
		}
	}

	if (UFFS_SPARE_BUFFER_SIZE > dev->mem.spare_pool_size) {
		uffs_Perror(UFFS_ERR_DEAD, PFX"Spare buffer require %d but only %d available.\n", UFFS_SPARE_BUFFER_SIZE, dev->mem.spare_pool_size);
		memset(pool, 0, sizeof(uffs_Pool));
		return U_FAIL;
	}

	uffs_Perror(UFFS_ERR_NOISY, PFX"alloc spare buffers %d bytes.\n", UFFS_SPARE_BUFFER_SIZE);
	uffs_PoolInit(pool, dev->mem.spare_pool_buf, dev->mem.spare_pool_size, UFFS_MAX_SPARE_SIZE, MAX_SPARE_BUFFERS);

	if (dev->attr->layout_opt == UFFS_LAYOUT_UFFS) {
		/* sanity check */
		if ((dev->attr->data_layout && !dev->attr->ecc_layout) ||
			(!dev->attr->data_layout && dev->attr->ecc_layout)) {
			uffs_Perror(UFFS_ERR_SERIOUS, PFX"Please setup data_layout and ecc_layout, or leave them all NULL !\n");
			return U_FAIL;
		}

		if (!attr->data_layout && !attr->ecc_layout)
			InitSpareLayout(dev);
	}

	dev->mem.spare_data_size = CalculateSpareDataSize(dev);

	return U_SUCC;
}

/**
 * Release UFFS flash interface
 */
URET uffs_FlashInterfaceRelease(uffs_Device *dev)
{
	uffs_Pool *pool;
	
	pool = SPOOL(dev);
	if (pool->mem && dev->mem.free) {
		dev->mem.free(dev, pool->mem);
		pool->mem = NULL;
		dev->mem.spare_pool_size = 0;
	}
	uffs_PoolRelease(pool);
	memset(pool, 0, sizeof(uffs_Pool));

	return U_SUCC;
}

/**
 * unload spare to tag and ecc.
 */
static void UnloadSpare(uffs_Device *dev, const u8 *spare, uffs_Tags *tag, u8 *ecc)
{
	u8 *p_tag = (u8 *)&tag->s;
	int tag_size = TAG_STORE_SIZE;
	int ecc_size = ECC_SIZE(dev);
	int n;
	const u8 *p;

	// unload ecc
	p = dev->attr->ecc_layout;
	if (p && ecc) {
		while (*p != 0xFF && ecc_size > 0) {
			n = (p[1] > ecc_size ? ecc_size : p[1]);
			memcpy(ecc, spare + p[0], n);
			ecc_size -= n;
			ecc += n;
			p += 2;
		}
	}

	// unload tag
	if (tag) {
		p = dev->attr->data_layout;
		while (*p != 0xFF && tag_size > 0) {
			n = (p[1] > tag_size ? tag_size : p[1]);
			memcpy(p_tag, spare + p[0], n);
			tag_size -= n;
			p_tag += n;
			p += 2;
		}

		tag->block_status = spare[dev->attr->block_status_offs];
	}
}

/**
 * Read tag and ecc from page spare
 *
 * \param[in] dev uffs device
 * \param[in] block flash block num
 * \param[in] page flash page num
 * \param[out] tag tag to be filled
 * \param[out] ecc ecc to be filled
 *
 * \return	#UFFS_FLASH_NO_ERR: success and/or has no flip bits.
 *			#UFFS_FLASH_IO_ERR: I/O error, expect retry ?
 *			#UFFS_FLASH_ECC_FAIL: spare data has flip bits and ecc correct failed.
 *			#UFFS_FLASH_ECC_OK: spare data has flip bits and corrected by ecc.
*/
int uffs_FlashReadPageSpare(uffs_Device *dev, int block, int page, uffs_Tags *tag, u8 *ecc)
{
	uffs_FlashOps *ops = dev->ops;
	struct uffs_StorageAttrSt *attr = dev->attr;
	u8 * spare_buf;
	int ret = UFFS_FLASH_UNKNOWN_ERR;
	UBOOL is_bad = U_FALSE;

	spare_buf = (u8 *) uffs_PoolGet(SPOOL(dev));
	if (spare_buf == NULL)
		goto ext;

	if (attr->layout_opt == UFFS_LAYOUT_FLASH)
		ret = ops->ReadPageSpareWithLayout(dev, block, page, (u8 *)&tag->s, tag ? TAG_STORE_SIZE : 0, ecc);
	else
		ret = ops->ReadPageSpare(dev, block, page, spare_buf, 0, dev->mem.spare_data_size);


	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		is_bad = U_TRUE;

	if (attr->layout_opt == UFFS_LAYOUT_UFFS)
		UnloadSpare(dev, spare_buf, tag, ecc);

	// copy some raw data
	if (tag) {
		tag->_dirty = tag->s.dirty;
		tag->_valid = tag->s.valid;
	}

	if (UFFS_FLASH_HAVE_ERR(ret))
		goto ext;

	if (tag) {
		if (tag->_valid == 1) //it's not a valid page ? don't need go further
			goto ext;

		// do tag ecc correction
		if (dev->attr->ecc_opt != UFFS_ECC_NONE) {
			ret = TagEccCorrect(&tag->s);
			ret = (ret < 0 ? UFFS_FLASH_ECC_FAIL :
					(ret > 0 ? UFFS_FLASH_ECC_OK : UFFS_FLASH_NO_ERR));

			if (UFFS_FLASH_IS_BAD_BLOCK(ret))
				is_bad = U_TRUE;

			if (UFFS_FLASH_HAVE_ERR(ret))
				goto ext;
		}
	}

ext:
	if (is_bad) {
		uffs_BadBlockAdd(dev, block);
		uffs_Perror(UFFS_ERR_NORMAL, PFX"A new bad block (%d) is detected.\n", block);
	}

	if (spare_buf)
		uffs_PoolPut(SPOOL(dev), spare_buf);

	return ret;
}

/**
 * Read page data to page buf and calculate ecc.
 * \param[in] dev uffs device
 * \param[in] block flash block num
 * \param[in] page flash page num of the block
 * \param[out] buf holding the read out data
 *
 * \return	#UFFS_FLASH_NO_ERR: success and/or has no flip bits.
 *			#UFFS_FLASH_IO_ERR: I/O error, expect retry ?
 *			#UFFS_FLASH_ECC_FAIL: spare data has flip bits and ecc correct failed.
 *			#UFFS_FLASH_ECC_OK: spare data has flip bits and corrected by ecc.
 */
int uffs_FlashReadPage(uffs_Device *dev, int block, int page, u8 *buf)
{
	uffs_FlashOps *ops = dev->ops;
	int size = dev->attr->page_data_size;
	u8 ecc_buf[MAX_ECC_SIZE];
	u8 ecc_store[MAX_ECC_SIZE];
	UBOOL is_bad = U_FALSE;

	int ret;

	// if ecc_opt is HW or HW_AUTO, flash driver should do ecc correction.
	ret = ops->ReadPageData(dev, block, page, buf, size, ecc_buf);
	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		is_bad = U_TRUE;

	if (UFFS_FLASH_HAVE_ERR(ret))
		goto ext;

	if (dev->attr->ecc_opt == UFFS_ECC_SOFT) {
		uffs_EccMake(buf, size, ecc_buf);
		ret = uffs_FlashReadPageSpare(dev, block, page, NULL, ecc_store);
		if (UFFS_FLASH_IS_BAD_BLOCK(ret))
			is_bad = U_TRUE;

		if (UFFS_FLASH_HAVE_ERR(ret))
			goto ext;

		ret = uffs_EccCorrect(buf, size, ecc_store, ecc_buf);
		ret = (ret < 0 ? UFFS_FLASH_ECC_FAIL :
				(ret > 0 ? UFFS_FLASH_ECC_OK : UFFS_FLASH_NO_ERR));

		if (UFFS_FLASH_IS_BAD_BLOCK(ret))
			is_bad = U_TRUE;

		if (UFFS_FLASH_HAVE_ERR(ret))
			goto ext;
	}

ext:
	if (is_bad) {
		uffs_BadBlockAdd(dev, block);
	}

	return ret;
}

/**
 * make spare from tag and ecc
 */
static void MakeSpare(uffs_Device *dev, uffs_TagStore *ts, u8 *ecc, u8* spare)
{
	u8 *p_ts = (u8 *)ts;
	int ts_size = TAG_STORE_SIZE;
	int ecc_size = ECC_SIZE(dev);
	int n;
	const u8 *p;

	memset(spare, 0xFF, dev->mem.spare_data_size);	// initialize as 0xFF.

	// load ecc
	p = dev->attr->ecc_layout;
	if (p && ecc) {
		while (*p != 0xFF && ecc_size > 0) {
			n = (p[1] > ecc_size ? ecc_size : p[1]);
			memcpy(spare + p[0], ecc, n);
			ecc_size -= n;
			ecc += n;
			p += 2;
		}
	}

	p = dev->attr->data_layout;
	while (*p != 0xFF && ts_size > 0) {
		n = (p[1] > ts_size ? ts_size : p[1]);
		memcpy(spare + p[0], p_ts, n);
		ts_size -= n;
		p_ts += n;
		p += 2;
	}
}

/**
 * write the whole page, include data and tag
 *
 * \param[in] dev uffs device
 * \param[in] block
 * \param[in] page
 * \param[in] buf contains data to be wrote
 * \param[in] tag tag to be wrote
 *
 * \return	#UFFS_FLASH_NO_ERR: success.
 *			#UFFS_FLASH_IO_ERR: I/O error, expect retry ?
 *			#UFFS_FLASH_BAD_BLK: a new bad block detected.
 */
int uffs_FlashWritePageCombine(uffs_Device *dev, int block, int page, u8 *buf, uffs_Tags *tag)
{
	uffs_FlashOps *ops = dev->ops;
	int size = dev->attr->page_data_size;
	u8 ecc_buf[MAX_ECC_SIZE];
	u8 *spare;
	int ret = UFFS_FLASH_UNKNOWN_ERR;
	UBOOL is_bad = U_FALSE;
	uffs_TagStore local_ts;

	uffs_Buf *verify_buf;

	spare = (u8 *) uffs_PoolGet(SPOOL(dev));
	if (spare == NULL)
		goto ext;

	// setp 1: write only the dirty bit to the spare
	memset(&local_ts, 0xFF, sizeof(local_ts));
	local_ts.dirty = TAG_DIRTY;	//!< set dirty mark

	if (dev->attr->layout_opt == UFFS_LAYOUT_UFFS) {
		MakeSpare(dev, &local_ts, NULL, spare);
		ret = ops->WritePageSpare(dev, block, page, spare, 0, dev->mem.spare_data_size);
	}
	else {
		ret = ops->WritePageSpareWithLayout(dev, block, page, (u8 *)&local_ts, 1, NULL);
	}

	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		is_bad = U_TRUE;

	if (UFFS_FLASH_HAVE_ERR(ret))
		goto ext;

	// setp 2: write page data
	if (dev->attr->ecc_opt == UFFS_ECC_SOFT)
		uffs_EccMake(buf, size, ecc_buf);

	ret = ops->WritePageData(dev, block, page, buf, size, ecc_buf);
	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		is_bad = U_TRUE;

	if (UFFS_FLASH_HAVE_ERR(ret))
		goto ext;

	// step 3: write full tag to spare, with ECC
	tag->s.dirty = TAG_DIRTY;		//!< set dirty bit
	tag->s.valid = TAG_VALID;		//!< set valid bit
	if (dev->attr->ecc_opt != UFFS_ECC_NONE)
		TagMakeEcc(&tag->s);
	else
		tag->s.tag_ecc = 0xFFFF;

	if (dev->attr->layout_opt == UFFS_LAYOUT_UFFS) {
		if (dev->attr->ecc_opt == UFFS_ECC_SOFT ||
			dev->attr->ecc_opt == UFFS_ECC_HW) {
			MakeSpare(dev, &tag->s, ecc_buf, spare);
		}
		else
			MakeSpare(dev, &tag->s, NULL, spare);

		ret = ops->WritePageSpare(dev, block, page, spare, 0, dev->mem.spare_data_size);
	}
	else {
		ret = ops->WritePageSpareWithLayout(dev, block, page, (u8 *)(&tag->s), TAG_STORE_SIZE, ecc_buf);
	}

	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		is_bad = U_TRUE;

#ifdef CONFIG_PAGE_WRITE_VERIFY
	if (!UFFS_FLASH_HAVE_ERR(ret)) {
		verify_buf = uffs_BufClone(dev, NULL);
		if (verify_buf) {
			ret = uffs_FlashReadPage(dev, block, page, verify_buf->data);
			if (!UFFS_FLASH_HAVE_ERR(ret))
				if (memcmp(buf, verify_buf->data, dev->attr->page_data_size) != 0) {
					uffs_Perror(UFFS_ERR_NORMAL, PFX"Page write verify fail (block %d page %d)\n", block, page);
					ret = UFFS_FLASH_BAD_BLK;
				}
			uffs_BufFreeClone(dev, verify_buf);
		}
	}
#endif
ext:
	if (is_bad)
		uffs_BadBlockAdd(dev, block);

	if (spare)
		uffs_PoolPut(SPOOL(dev), spare);

	return ret;
}

/** Mark this block as bad block */
URET uffs_FlashMarkBadBlock(uffs_Device *dev, int block)
{
	u8 status = 0;
	int ret;

	if (dev->ops->MarkBadBlock)
		return dev->ops->MarkBadBlock(dev, block) == 0 ? U_SUCC : U_FAIL;

	if (dev->ops->WritePageSpare == NULL) {
		uffs_Perror(UFFS_ERR_SERIOUS, PFX"flash driver must provide 'WritePageSpare' function!\n");
		return U_FAIL;
	}

	ret = dev->ops->EraseBlock(dev, block);
	if (ret == UFFS_FLASH_NO_ERR)
		ret = dev->ops->WritePageSpare(dev, block, 0, &status, dev->attr->block_status_offs, 1);

	return ret == UFFS_FLASH_NO_ERR ? U_SUCC : U_FAIL;
}

/** Is this block a bad block ? */
UBOOL uffs_FlashIsBadBlock(uffs_Device *dev, int block)
{
	u8 status = 0xFF;

	if (dev->ops->IsBadBlock) /* if flash driver provide 'IsBadBlock' function, then use it. */
		return dev->ops->IsBadBlock(dev, block) == 1 ? U_TRUE : U_FALSE;

	/* otherwise we check the 'status' byte of spare */
	if (dev->ops->ReadPageSpare == NULL) {
		uffs_Perror(UFFS_ERR_SERIOUS, PFX"flash driver must provide 'ReadPageSpare' function!\n");
		return U_FALSE;
	}

	dev->ops->ReadPageSpare(dev, block, 0, &status, dev->attr->block_status_offs, 1);

	if (status == 0xFF) {
		dev->ops->ReadPageSpare(dev, block, 1, &status, dev->attr->block_status_offs, 1);
		if (status == 0xFF)
			return U_FALSE;
	}

	return U_TRUE;
}

/** Erase flash block */
URET uffs_FlashEraseBlock(uffs_Device *dev, int block)
{
	int ret;

	ret = dev->ops->EraseBlock(dev, block);

	if (UFFS_FLASH_IS_BAD_BLOCK(ret))
		uffs_BadBlockAdd(dev, block);

	return UFFS_FLASH_HAVE_ERR(ret) ? U_FAIL : U_SUCC;
}


/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "#%d " fmt "\n", __LINE__

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_i2c.h>
#include <linux/debugfs.h>
#include <linux/msm-sps.h>
#include <linux/msm-bus.h>
#include <linux/msm-bus-board.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include "i2c-msm-v2.h"

#ifdef DEBUG
static const enum msm_i2_debug_level DEFAULT_DBG_LVL = MSM_DBG;
#else
static const enum msm_i2_debug_level DEFAULT_DBG_LVL = MSM_ERR;
#endif

static const char * const i2c_msm_mode_str_tbl[] = {
	"FIFO", "BAM", "None",
};

static const char * const i2c_msm_qup_mode_str_tbl[] = {
	"FIFO", "Block", "Reserved", "BAM",
};

static const u32 i2c_msm_mode_to_reg_tbl[] = {
	0x0, 
	0x3  
};

#define RECOVER_LIMIT 5
static int error_times[7];

static bool i2c_msm_xfer_next_buf(struct i2c_msm_ctrl *ctrl);
static int  i2c_msm_xfer_wait_for_completion(struct i2c_msm_ctrl *ctrl);
static int  i2c_msm_bam_xfer(struct i2c_msm_ctrl *ctrl);
static int  i2c_msm_fifo_xfer(struct i2c_msm_ctrl *ctrl);
static void i2c_msm_pm_resume_adptr(struct i2c_msm_ctrl *ctrl);
static void i2c_msm_pm_suspend_adptr(struct i2c_msm_ctrl *ctrl);
static int  i2c_msm_qup_init(struct i2c_msm_ctrl *ctrl);
static int  i2c_msm_pm_resume_impl(struct device *dev);

static struct i2c_msm_xfer_mode_bam *i2c_msm_bam_get_struct(
						struct i2c_msm_ctrl *ctrl)
{
	return (struct i2c_msm_xfer_mode_bam *)
				ctrl->ver.xfer_mode[I2C_MSM_XFER_MODE_BAM];
}

static void i2c_msm_bam_set_struct(struct i2c_msm_ctrl *ctrl,
					  struct i2c_msm_xfer_mode_bam *bam)
{
	ctrl->ver.xfer_mode[I2C_MSM_XFER_MODE_BAM] =
					(struct i2c_msm_xfer_mode *) bam;
}

static struct i2c_msm_xfer_mode_fifo *i2c_msm_fifo_get_struct(
						struct i2c_msm_ctrl *ctrl)
{
	return (struct i2c_msm_xfer_mode_fifo *)
				ctrl->ver.xfer_mode[I2C_MSM_XFER_MODE_FIFO];
}

static void i2c_msm_fifo_set_struct(struct i2c_msm_ctrl *ctrl,
					  struct i2c_msm_xfer_mode_fifo *fifo)
{
	ctrl->ver.xfer_mode[I2C_MSM_XFER_MODE_FIFO] =
					(struct i2c_msm_xfer_mode *) fifo;
}

static const char * const i2c_msm_mini_core_str_tbl[] = {
	"null", "SPI", "I2C", "reserved",
};

static const u32 i2c_msm_fifo_block_sz_tbl[] = {16, 16 , 32, 0};
static const char * const i2c_msm_fifo_sz_str_tbl[]
		= {"x2 blk sz", "x4 blk sz" , "x8 blk sz", "x16 blk sz"};
static const char * const i2c_msm_fifo_block_sz_str_tbl[]
						= {"16", "16" , "32", "0"};

static u32 i2c_msm_reg_io_modes_out_blk_sz(u32 qup_io_modes)
{
	return i2c_msm_fifo_block_sz_tbl[qup_io_modes & 0x3];
}

static u32 i2c_msm_reg_io_modes_in_blk_sz(u32 qup_io_modes)
{
	return i2c_msm_fifo_block_sz_tbl[BITS_AT(qup_io_modes, 5, 2)];
}

static const u32 i2c_msm_fifo_sz_table[] = {2, 4 , 8, 16};

static void i2c_msm_qup_fifo_calc_size(struct i2c_msm_ctrl *ctrl)
{
	u32 reg_data, output_fifo_size, input_fifo_size;
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);

	
	if (fifo->input_fifo_sz && fifo->output_fifo_sz)
		return;

	reg_data = readl_relaxed(ctrl->rsrcs.base + QUP_IO_MODES);
	output_fifo_size  = BITS_AT(reg_data, 2, 2);
	input_fifo_size   = BITS_AT(reg_data, 7, 2);

	fifo->input_fifo_sz = i2c_msm_reg_io_modes_in_blk_sz(reg_data) *
					i2c_msm_fifo_sz_table[input_fifo_size];
	fifo->output_fifo_sz = i2c_msm_reg_io_modes_out_blk_sz(reg_data) *
					i2c_msm_fifo_sz_table[output_fifo_size];

	i2c_msm_dbg(ctrl, MSM_PROF, "QUP input-sz:%zu, input-sz:%zu",
			fifo->input_fifo_sz, fifo->output_fifo_sz);

}

static const char * const i2c_msm_reg_qup_state_to_str[] = {
	"Reset", "Run", "Clear", "Pause"
};

struct i2c_msm_qup_reg_fld {
	const char * const name;
	int                bit_idx;
	int                n_bits;
	const char * const *to_str_tbl;
};

static const char *i2c_msm_dbg_qup_reg_flds_to_str(
	u32 val, char *buf, int len, const struct i2c_msm_qup_reg_fld *fld)
{
	char *ptr = buf;
	int str_len;
	int str_len_sum = 0;
	int rem_len     = len;
	u32 field_val;
	for ( ; fld->name && (rem_len > 0) ; ++fld) {

		if (fld->n_bits == 1) {
			field_val = BIT_IS_SET(val, fld->bit_idx);
			if (!field_val)
				continue;

			str_len = snprintf(ptr, rem_len, "%s ", fld->name);
		} else {
			field_val = BITS_AT(val, fld->bit_idx, fld->n_bits);

			if (!field_val)
				continue;

			if (fld->to_str_tbl)
				str_len = snprintf(ptr, rem_len, "%s:%s ",
				   fld->name, fld->to_str_tbl[field_val]);
			else
				str_len = snprintf(ptr, rem_len, "%s:0x%x ",
				   fld->name, field_val);
		}

		if (str_len > rem_len) {
			pr_err("%s insufficient buffer space\n", __func__);
			
			buf[len - 1] = 0;
			return buf;
		}

		rem_len     -= str_len;
		ptr         += str_len;
		str_len_sum += str_len;
	}

	
	buf[len - 1] = 0;
	return buf;
}

static struct i2c_msm_qup_reg_fld i2c_msm_qup_config_fields_map[] = {
	{ "N",               0,   5},
	{ "MINI_CORE",       8,   2, i2c_msm_mini_core_str_tbl},
	{ "NO_OUTPUT",       6,   1},
	{ "NO_INPUT",        7,   1},
	{ "EN_EXT_OUT",     16,   1},
	{ NULL,              0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_op_fields_map[] = {
	{ "OUT_FF_N_EMPTY",  4,   1},
	{ "IN_FF_N_EMPTY",   5,   1},
	{ "OUT_FF_FUL",      6,   1},
	{ "IN_FF_FUL",       7,   1},
	{ "OUT_SRV_FLG",     8,   1},
	{ "IN_SRV_FLG",      9,   1},
	{ "MX_OUT_DN",      10,   1},
	{ "MX_IN_DN",       11,   1},
	{ "OUT_BLK_WR",     12,   1},
	{ "IN_BLK_WR",      13,   1},
	{ "DONE_TGL",       14,   1},
	{ "NWD",            15,   1},
	{ NULL,              0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_i2c_stat_fields_map[] = {
	{ "BUS_ERR",        2,   1},
	{ "NACK",           3,   1},
	{ "ARB_LOST",       4,   1},
	{ "INVLD_WR",       5,   1},
	{ "FAIL",           6,   2},
	{ "BUS_ACTV",       8,   1},
	{ "BUS_MSTR",       9,   1},
	{ "DAT_STATE",     10,   3},
	{ "CLK_STATE",     13,   3},
	{ "O_FSM_STAT",    16,   3},
	{ "I_FSM_STAT",    19,   3},
	{ "INVLD_TAG",     23,   1},
	{ "INVLD_RD_ADDR", 24,   1},
	{ "INVLD_RD_SEQ",  25,   1},
	{ "SDA",           26,   1},
	{ "SCL",           27,   1},
	{ NULL,             0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_err_flags_fields_map[] = {
	{ "IN_OVR_RUN",        2,   1},
	{ "OUT_UNDR_RUN",      3,   1},
	{ "IN_UNDR_RUN",       4,   1},
	{ "OUT_OVR_RUN",       5,   1},
	{ NULL,                0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_op_mask_fields_map[] = {
	{ "OUT_SRVC_MASK",     8,   1},
	{ "IN_SRVC_MASK",      9,   1},
	{ NULL,                0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_master_clk_fields_map[] = {
	{ "FS_DIV",            0,   8},
	{ "HS_DIV",            8,   3},
	{ "HI_TM_DIV",        16,   8},
	{ "SCL_NS_RJCT",      24,   2},
	{ "SDA_NS_RJCT",      26,   2},
	{ "SCL_EXT_FRC_L",    28,   1},
	{ NULL,                0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_state_fields_map[] = {
	{ "STATE",             0,   2, i2c_msm_reg_qup_state_to_str},
	{ "VALID",             2,   1},
	{ "MAST_GEN",          4,   1},
	{ "WAIT_EOT",          5,   1},
	{ "FLUSH",             6,   1},
	{ NULL,                0,   1},
};

static struct i2c_msm_qup_reg_fld i2c_msm_qup_io_modes_map[] = {
	{ "IN_BLK_SZ",         5,   2, i2c_msm_fifo_block_sz_str_tbl},
	{ "IN_FF_SZ",          7,   3, i2c_msm_fifo_sz_str_tbl},
	{ "OUT_BLK_SZ",        0,   2, i2c_msm_fifo_block_sz_str_tbl},
	{ "OUT_FF_SZ",         2,   3, i2c_msm_fifo_sz_str_tbl},
	{ "UNPACK",           14,   1},
	{ "PACK",             15,   1},
	{ "INP_MOD",          12,   2, i2c_msm_qup_mode_str_tbl},
	{ "OUT_MOD",          10,   2, i2c_msm_qup_mode_str_tbl},
	{ NULL,                0,   1},
};

struct i2c_msm_qup_reg_dump {
	u32                          offset;
	const char                  *name;
	struct i2c_msm_qup_reg_fld  *field_map;
};

static const struct i2c_msm_qup_reg_dump i2c_msm_qup_reg_dump_map[] = {
{QUP_CONFIG,             "QUP_CONFIG",   i2c_msm_qup_config_fields_map    },
{QUP_STATE,              "QUP_STATE",    i2c_msm_qup_state_fields_map     },
{QUP_IO_MODES,           "QUP_IO_MDS",   i2c_msm_qup_io_modes_map         },
{QUP_ERROR_FLAGS,        "QUP_ERR_FLGS", i2c_msm_qup_err_flags_fields_map },
{QUP_OPERATIONAL,        "QUP_OP",       i2c_msm_qup_op_fields_map        },
{QUP_OPERATIONAL_MASK,   "QUP_OP_MASK",  i2c_msm_qup_op_mask_fields_map   },
{QUP_I2C_STATUS,         "QUP_I2C_STAT", i2c_msm_qup_i2c_stat_fields_map  },
{QUP_I2C_MASTER_CLK_CTL, "QUP_MSTR_CLK", i2c_msm_qup_master_clk_fields_map},
{QUP_IN_DEBUG,           "QUP_IN_DBG"  },
{QUP_OUT_DEBUG,          "QUP_OUT_DBG" },
{QUP_IN_FIFO_CNT,        "QUP_IN_CNT"  },
{QUP_OUT_FIFO_CNT,       "QUP_OUT_CNT" },
{QUP_MX_READ_COUNT,      "MX_RD_CNT"   },
{QUP_MX_WRITE_COUNT,     "MX_WR_CNT"   },
{0,                       NULL          },
};

static void i2c_msm_dbg_qup_reg_dump(struct i2c_msm_ctrl *ctrl)
{
	u32 val;
	char buf[I2C_MSM_REG_2_STR_BUF_SZ];
	void __iomem *base = ctrl->rsrcs.base;
	const struct i2c_msm_qup_reg_dump *itr = i2c_msm_qup_reg_dump_map;

	for (; itr->name ; ++itr) {
		val = readl_relaxed(base + itr->offset);

		if (itr->field_map)
			i2c_msm_dbg_qup_reg_flds_to_str(val, buf, sizeof(buf),
								itr->field_map);
		else
			buf[0] = 0;

		dev_info(ctrl->dev, "%-12s:0x%08x %s\n", itr->name, val, buf);
	};
}

static void i2c_msm_dbg_xfer_dump(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);
	struct i2c_msm_xfer *xfer = &ctrl->xfer;

	void __iomem *base = ctrl->rsrcs.base;
	u32          i2c_status = 0;
	int scl_val, sda_val;

	if (!xfer->msgs) {
		dev_info(ctrl->dev, "No active transfer\n");
		return;
	}

	i2c_status = readl_relaxed(base + QUP_I2C_STATUS);

	dev_info(ctrl->dev,
		"n-msgs:%d mode:%s in-bc:%zu out-bc:%zu in-ovrhd:%zu "
		"out-ovrhd:%zu timeout:%dmsec msg-idx:%d is_rx:%d "
		"ff-tx-bc:%zu ff-rx-bc:%zu\n",
		xfer->msg_cnt, i2c_msm_mode_str_tbl[xfer->mode_id],
		xfer->rx_cnt, xfer->tx_cnt, xfer->rx_ovrhd_cnt,
		xfer->tx_ovrhd_cnt, jiffies_to_msecs(xfer->timeout),
		xfer->cur_buf.msg_idx, xfer->cur_buf.is_rx, fifo->tx_bc,
		fifo->rx_bc);

	dev_info(ctrl->dev,
		"SL-AD = 0x%x, buf[0] = 0x%x, i2c_status = 0x%X, "
		"ctrl->adapter.nr = %d, error_times = %d\n",
		xfer->msgs->addr, xfer->msgs->buf[0], i2c_status,
		ctrl->adapter.nr, error_times[ctrl->adapter.nr]);

	if (ctrl->sda_gpio > 0) {
		scl_val = gpio_get_value(ctrl->scl_gpio);
		sda_val = gpio_get_value(ctrl->sda_gpio);

		dev_info(ctrl->dev, "sda = %d, scl = %d\n", sda_val, scl_val);
	}

	if ((ctrl->sda_gpio > 0) && ((sda_val == 0) || (scl_val == 0)) &&
	    (error_times[ctrl->adapter.nr] >= RECOVER_LIMIT)) {
		dev_err(ctrl->dev, "[i2c-msm-v2] Error for %d times,"
			" goto ramdump!!\n",
			error_times[ctrl->adapter.nr]);
		msleep(1000);
		BUG();
	}
	error_times[ctrl->adapter.nr] ++;
}

static const char * const i2c_msm_dbg_tag_val_to_str_tbl[] = {
	"NOP_WAIT",		
	"START",		
	"DATAWRITE",		
	"DATAWRT_and_STOP",	
	NULL,			
	"DATAREAD",		
	"DATARD_and_NACK",	
	"DATARD_and_STOP",	
	"STOP_TAG",		
	NULL,			
	NULL,			
	NULL,			
	NULL,			
	NULL,			
	NULL,			
	NULL,			
	"NOP_MARK",		
	"NOP_ID",		
	"TIME_STAMP",		
	"INPUT_EOT",		
	"INPUT_EOT_FLUSH",	
	"NOP_LOCAL",		
	"FLUSH STOP",		
};

static const char *i2c_msm_dbg_tag_val_to_str(u8 tag_val)
{
	if ((tag_val < 0x80) || (tag_val > 0x96) || (tag_val == 0x84) ||
	   ((tag_val > 0x88) && (0x90 > tag_val)))
		return "Invalid_tag";

	return i2c_msm_dbg_tag_val_to_str_tbl[tag_val - 0x80];
}

static u8 *i2c_msm_tag_byte(struct i2c_msm_tag *tag, int byte_n)
{
	return ((u8 *)tag) + byte_n;
}

static const char *i2c_msm_dbg_tag_to_str(const struct i2c_msm_tag *tag,
						char *buf, size_t buf_len)
{
	
	struct i2c_msm_tag *t = (struct i2c_msm_tag *) tag;
	switch (tag->len) {
	case 6:
		snprintf(buf, buf_len, "val:0x%012llx %s:0x%x %s:0x%x %s:%d",
			tag->val,
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 0)),
			*i2c_msm_tag_byte(t, 1),
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 2)),
			*i2c_msm_tag_byte(t, 3),
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 4)),
			*i2c_msm_tag_byte(t, 5));
		break;
	case 4:
		snprintf(buf, buf_len, "val:0x%08llx %s:0x%x %s:%d",
			(tag->val & 0xffffffff),
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 0)),
			*i2c_msm_tag_byte(t, 1),
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 2)),
			*i2c_msm_tag_byte(t, 3));
		break;
	default: 
		snprintf(buf, buf_len, "val:0x%04llx %s:%d",
			(tag->val & 0xffff),
			i2c_msm_dbg_tag_val_to_str(*i2c_msm_tag_byte(t, 0)),
			*i2c_msm_tag_byte(t, 1));
	}

	return buf;
}

static const char *
i2c_msm_dbg_bam_tag_to_str(const struct i2c_msm_bam_tag *bam_tag, char *buf,
								size_t buf_len)
{
	const char *ret;
	u32        *val;
	struct i2c_msm_tag tag;

	val = phys_to_virt(bam_tag->buf);
	if (!val) {
		pr_err("Failed phys_to_virt(0x%llx)", (u64) bam_tag->buf);
		return "Error";
	}

	tag = (struct i2c_msm_tag) {
		.val = *val,
		.len = bam_tag->len,
	};

	ret = i2c_msm_dbg_tag_to_str(&tag, buf, buf_len);
	return ret;
}

static u8 *i2c_msm_buf_to_ptr(struct i2c_msm_xfer_buf *buf)
{
	struct i2c_msm_xfer *xfer =
				container_of(buf, struct i2c_msm_xfer, cur_buf);
	struct i2c_msg *msg = xfer->msgs + buf->msg_idx;
	return msg->buf + buf->byte_idx;
}

/*
 * i2c_msm_prof_evnt_add: pushes event into end of event array
 *
 * @dump_now log a copy immediately to kernel log
 *
 * Implementation of i2c_msm_prof_evnt_add().When array overflows, the last
 * entry is overwritten as many times as it overflows.
 */
static void i2c_msm_prof_evnt_add(struct i2c_msm_ctrl *ctrl,
				enum msm_i2_debug_level dbg_level,
				i2c_msm_prof_dump_func_func_t dump_func,
				u64 data0, u32 data1, u32 data2)
{
	struct i2c_msm_xfer       *xfer  = &ctrl->xfer;
	struct i2c_msm_prof_event *event;
	int idx;

	if (ctrl->dbgfs.dbg_lvl < dbg_level)
		return;

	atomic_add_unless(&xfer->event_cnt, 1, I2C_MSM_PROF_MAX_EVNTS - 1);
	idx = atomic_read(&xfer->event_cnt) - 1;
	if (idx > (I2C_MSM_PROF_MAX_EVNTS - 1))
		dev_err(ctrl->dev, "error event index:%d max:%d\n",
						idx, I2C_MSM_PROF_MAX_EVNTS);
	event = &xfer->event[idx];

	getnstimeofday(&event->time);
	event->dump_func = dump_func;
	event->data0 = data0;
	event->data1 = data1;
	event->data2 = data2;
}

void i2c_msm_prof_dump_xfer_beg(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev,
		"-->.%03zums XFER_BEG msg_cnt:%llx addr:0x%x\n",
		usec, event->data0, event->data1);
}

static const char * const i2c_msm_err_str_tbl[] = {
	"NONE", "NACK", "ARB_LOST" , "ARB_LOST + NACK", "BUS_ERR",
	"BUS_ERR + NACK", "BUS_ERR + ARB_LOST", "BUS_ERR + ARB_LOST + NACK",
	"TIMEOUT", "TIMEOUT + NACK", "TIMEOUT + ARB_LOST",
	"TIMEOUT + ARB_LOST + NACK", "TIMEOUT + BUS_ERR",
	"TIMEOUT + BUS_ERR + NACK" , "TIMEOUT + BUS_ERR + ARB_LOST",
	"TIMEOUT + BUS_ERR + ARB_LOST + NACK",
};

void i2c_msm_prof_dump_xfer_end(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	int ret = event->data0;
	int err = event->data1;
	int bc  = ctrl->xfer.rx_cnt + ctrl->xfer.rx_ovrhd_cnt +
		  ctrl->xfer.tx_cnt + ctrl->xfer.tx_ovrhd_cnt;
	int bc_sec = (bc * 1000000) / (msec * 1000 + usec);
	const char *status = (!err && (ret == ctrl->xfer.msg_cnt)) ?
								"OK" : "FAIL";

	dev_info(ctrl->dev,
		"%3zu.%03zums XFER_END "
		"ret:%d err:[%s] msgs_sent:%d BC:%d B/sec:%d i2c-stts:%s\n" ,
		msec, usec, ret, i2c_msm_err_str_tbl[err], event->data2,
		bc, bc_sec, status);
}

void i2c_msm_prof_dump_irq_begn(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev, "%3zu.%03zums  IRQ_BEG irq:%lld\n",
						msec, usec, event->data0);
}

void i2c_msm_prof_dump_irq_end(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	char str[I2C_MSM_REG_2_STR_BUF_SZ];
	u32 mstr_stts = event->data0;
	u32 qup_oper  = event->data1;
	u32 err_flgs  = event->data2;
	dev_info(ctrl->dev,
		"%3zu.%03zums  IRQ_END "
		"MSTR_STTS:0x%x QUP_OPER:0x%x ERR_FLGS:0x%x\n",
		msec, usec, mstr_stts, qup_oper, err_flgs);

	 
	if (mstr_stts & QUP_MSTR_STTS_ERR_MASK) {
		i2c_msm_dbg_qup_reg_flds_to_str(
				mstr_stts, str, sizeof(str),
				i2c_msm_qup_i2c_stat_fields_map);

		dev_info(ctrl->dev, "            |->MSTR_STTS:0x%llx %s\n",
						event->data0, str);
	}
	
	if (qup_oper &
	   (QUP_OUTPUT_SERVICE_FLAG | QUP_INPUT_SERVICE_FLAG)) {

		i2c_msm_dbg_qup_reg_flds_to_str(
				qup_oper, str, sizeof(str),
				i2c_msm_qup_op_fields_map);

		dev_info(ctrl->dev, "            |-> QUP_OPER:0x%x %s\n",
						event->data1, str);
	}
	
	if (err_flgs) {
		i2c_msm_dbg_qup_reg_flds_to_str(
				err_flgs, str, sizeof(str),
				i2c_msm_qup_err_flags_fields_map);

		dev_info(ctrl->dev, "            |-> ERR_FLGS:0x%x %s\n",
						event->data2, str);
	}
}

void i2c_msm_prof_dump_next_buf(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	struct i2c_msg *msg = ctrl->xfer.msgs + event->data0;
	dev_info(ctrl->dev,
		"%3zu.%03zums XFER_BUF msg[%lld] pos:%d adr:0x%x "
		"len:%d is_rx:0x%x last:0x%x\n",
		msec, usec, event->data0, event->data1, msg->addr, msg->len,
		(msg->flags & I2C_M_RD),
		event->data0 == (ctrl->xfer.msg_cnt - 1));

}

void i2c_msm_prof_dump_scan_sum(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	u32 bc_rx       = (event->data0 & 0xff);
	u32 bc_rx_ovrhd = (event->data0 >> 16);
	u32 bc_tx       = (event->data1 & 0xff);
	u32 bc_tx_ovrhd = (event->data1 >> 16);
	u32 timeout     = (event->data2 & 0xfff);
	u32 mode        = (event->data2 >> 24);
	u32 bc      = bc_rx + bc_rx_ovrhd + bc_tx + bc_tx_ovrhd;
	dev_info(ctrl->dev,
		"%3zu.%03zums SCN_SMRY BC:%u rx:%u+ovrhd:%u tx:%u+ovrhd:%u "
		"timeout:%umsec mode:%s\n",
		msec, usec, bc, bc_rx, bc_rx_ovrhd, bc_tx, bc_tx_ovrhd,
		jiffies_to_msecs(timeout), i2c_msm_mode_str_tbl[mode]);
}

void i2c_msm_prof_dump_cmplt_ok(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev,
		"%3zu.%03zums  DONE_OK timeout-used:%umsec time_left:%umsec\n",
		msec, usec, jiffies_to_msecs(event->data0),
		jiffies_to_msecs(event->data1));
}

void i2c_msm_prof_dump_cmplt_fl(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev,
		"%3zu.%03zums  TIMEOUT-error timeout-used:%umsec. "
		"Check GPIOs configuration\n",
		msec, usec, jiffies_to_msecs(event->data0));
}

void i2c_msm_prof_dump_vlid_end(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	int  ret        = (int)(event->data0 & 0xff);
	enum i2c_msm_qup_state state = ((event->data0 << 16) & 0xf);
	u32  status     = event->data2;

	dev_info(ctrl->dev,
	"%3zu.%03zums SET_STTE set:%s ret:%d rd_cnt:%u reg_val:0x%x vld:%d\n",
	msec, usec, i2c_msm_reg_qup_state_to_str[state], ret,
	event->data1, status, BIT_IS_SET(status, 2));
}

void i2c_msm_prof_dump_actv_end(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev,
	    "%3zu.%03zums ACTV_END ret:%lld jiffies_left:%u/%u read_cnt:%u\n",
	    msec, usec, event->data0, event->data1,
	    I2C_MSM_MAX_POLL_MSEC, event->data2);
}

void i2c_msm_prof_dump_bam_flsh(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev, "%3zu.%03zums  BAM_FLSH\n", msec, usec);
}

void i2c_msm_prof_dump_pip_dscn(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	struct i2c_msm_bam_pipe *pipe =
			(struct i2c_msm_bam_pipe *) ((ulong) event->data0);
	int ret = event->data1;
	dev_info(ctrl->dev,
		"%3zu.%03zums PIP_DCNCT sps_disconnect(hndl:0x%p %s):%d\n",
		msec, usec, pipe->handle, pipe->name, ret);
}

void i2c_msm_prof_dump_pip_cnct(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	struct i2c_msm_bam_pipe *pipe =
			(struct i2c_msm_bam_pipe *) ((ulong) event->data0);
	int ret = event->data1;
	dev_info(ctrl->dev,
		"%3zu.%03zums PIP_CNCT sps_connect(hndl:0x%p %s):%d\n",
		msec, usec, pipe->handle, pipe->name, ret);
}

void i2c_msm_prof_reset(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_prof_event *event, size_t msec, size_t usec)
{
	dev_info(ctrl->dev, "%3zu.%03zums  QUP_RSET\n", msec, usec);
}

static void i2c_msm_prof_evnt_dump(struct i2c_msm_ctrl *ctrl)
{
	size_t                     cnt   = atomic_read(&ctrl->xfer.event_cnt);
	struct i2c_msm_prof_event *event = ctrl->xfer.event;
	struct timespec            time0 = event->time;
	struct timespec            time_diff;
	size_t                     diff_usec;
	size_t                     diff_msec;

	for (; cnt ; --cnt, ++event) {
		time_diff = timespec_sub(event->time, time0);
		diff_usec = time_diff.tv_sec  * USEC_PER_SEC +
			    time_diff.tv_nsec / NSEC_PER_USEC;
		diff_msec  = diff_usec / USEC_PER_MSEC;
		diff_usec -= diff_msec * USEC_PER_MSEC;

		(event->dump_func)(ctrl, event, diff_msec, diff_usec);
	}
}

static const struct i2c_msm_tag tag_lookup_table[2][2][2][2] = {
	{{{{QUP_TAG2_DATA_WRITE                                   , 2},
	   {QUP_TAG2_DATA_READ_N_STOP                             , 2} },
	
	  {{QUP_TAG2_DATA_WRITE_N_STOP                            , 2},
	   {QUP_TAG2_DATA_READ_N_STOP                             , 2} } },
	
	 {{{QUP_TAG2_START | (QUP_TAG2_DATA_WRITE           << 16), 4},
	   {QUP_TAG2_START | (QUP_TAG2_DATA_READ_N_STOP     << 16), 4} },
	
	  {{QUP_TAG2_START | (QUP_TAG2_DATA_WRITE_N_STOP    << 16), 4},
	   {QUP_TAG2_START | (QUP_TAG2_DATA_READ_N_STOP     << 16), 4} } } },
	
	{{{{QUP_TAG2_DATA_WRITE                                   , 2},
	   {QUP_TAG2_DATA_READ_N_STOP                             , 2} },
	
	  {{QUP_TAG2_DATA_WRITE_N_STOP                            , 2},
	   {QUP_TAG2_DATA_READ_N_STOP                             , 2} } },
	
	 {{{QUP_TAG2_START_HS | (QUP_TAG2_DATA_WRITE        << 32), 6},
	   {QUP_TAG2_START_HS | (QUP_TAG2_DATA_READ_N_STOP  << 32), 6} },
	
	  {{QUP_TAG2_START_HS | (QUP_TAG2_DATA_WRITE_N_STOP << 32), 6},
	   {QUP_TAG2_START_HS | (QUP_TAG2_DATA_READ_N_STOP  << 32), 6} } } },
};

static struct i2c_msm_tag i2c_msm_tag_create(bool is_high_speed,
	bool is_new_addr, bool is_last_buf, bool is_rx, u8 buf_len,
	u8 slave_addr)
{
	struct i2c_msm_tag tag;

	is_new_addr = is_new_addr ? 1 : 0;
	is_last_buf = is_last_buf ? 1 : 0;
	is_rx    = is_rx    ? 1 : 0;

	tag = tag_lookup_table[is_high_speed][is_new_addr][is_last_buf][is_rx];
	
	switch (tag.len) {
	case 6:
		*i2c_msm_tag_byte(&tag, 3) = slave_addr;
		*i2c_msm_tag_byte(&tag, 5) = buf_len;
		break;
	case 4:
		*i2c_msm_tag_byte(&tag, 1) = slave_addr;
		*i2c_msm_tag_byte(&tag, 3) = buf_len;
		break;
	default:
		*i2c_msm_tag_byte(&tag, 1) = buf_len;
	};

	return tag;
}

static void i2c_msm_dbg_inp_fifo_dump(struct i2c_msm_ctrl *ctrl)
{
	void __iomem * const base = ctrl->rsrcs.base;
	u32  qup_op;
	u32  data;
	bool fifo_empty;
	char str[256] = {0};
	int len = 0;
	int cnt;
	int ret;

	for (cnt = 0; ; ++cnt) {
		qup_op     = readl_relaxed(base + QUP_OPERATIONAL);
		fifo_empty = !(qup_op & QUP_INPUT_FIFO_NOT_EMPTY);

		if (fifo_empty)
			break;

		data = readl_relaxed(base + QUP_IN_FIFO_BASE);
		ret  = snprintf(str + len, sizeof(str) - len, "0x%08x\n", data);
		udelay(USEC_PER_MSEC);
		if (ret > (sizeof(str) - len))
			break;

		len += ret;
	}
	dev_info(ctrl->dev, "INPUT FIFO: cnt:%d\n%s\n", cnt, str);
}

static int
i2c_msm_qup_state_wait_valid(struct i2c_msm_ctrl *ctrl,
			enum i2c_msm_qup_state state, bool only_valid)
{
	u32 status;
	void __iomem  *base     = ctrl->rsrcs.base;
	unsigned long  start   = jiffies;
	unsigned long  timeout = start +
				 msecs_to_jiffies(I2C_MSM_MAX_POLL_MSEC);
	int ret      = 0;
	int read_cnt = 0;

	do {
		status = readl_relaxed(base + QUP_STATE);
		++read_cnt;

		if (status & QUP_STATE_VALID) {
			if (only_valid)
				goto poll_valid_end;
			else if ((state & QUP_I2C_MAST_GEN) &&
					(status & QUP_I2C_MAST_GEN))
				goto poll_valid_end;
			else if ((status & QUP_STATE_MASK) == state)
				goto poll_valid_end;
		}

	} while (time_before_eq(jiffies, timeout));

	ret = -ETIMEDOUT;
	dev_err(ctrl->dev,
		"error timeout on polling for valid state. check core_clk\n");

poll_valid_end:
	if (!only_valid)
		i2c_msm_prof_evnt_add(ctrl, MSM_DBG, i2c_msm_prof_dump_vlid_end,
				
				(((-ret) & 0xff) | ((state & 0xf) << 16)),
				read_cnt, status);

	return ret;
}

static int i2c_msm_qup_state_set(struct i2c_msm_ctrl *ctrl,
						enum i2c_msm_qup_state state)
{
	if (i2c_msm_qup_state_wait_valid(ctrl, 0, true))
		return -EIO;

	writel_relaxed(state, ctrl->rsrcs.base + QUP_STATE);

	if (i2c_msm_qup_state_wait_valid(ctrl, state, false))
		return -EIO;

	return 0;
}

static int i2c_msm_qup_sw_reset(struct i2c_msm_ctrl *ctrl)
{
	int ret;

	writel_relaxed(1, ctrl->rsrcs.base + QUP_SW_RESET);
	/*
	 * Ensure that QUP that reset state is written before waiting for a the
	 * reset state to be valid.
	 */
	wmb();
	ret = i2c_msm_qup_state_wait_valid(ctrl, QUP_STATE_RESET, false);
	if (ret)
		dev_err(ctrl->dev, "error on issuing QUP software-reset\n");

	return ret;
}

static void
i2c_msm_qup_xfer_init_reset_state(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	void __iomem * const base = ctrl->rsrcs.base;
	u32  in_cnt        = 0;
	u32  out_cnt       = 0;
	u32  no_input      = 0;
	u32  no_output     = 0;
	u32  input_mode    = i2c_msm_mode_to_reg_tbl[xfer->mode_id] << 12;
	u32  output_mode   = i2c_msm_mode_to_reg_tbl[xfer->mode_id] << 10;
	u32  config_reg;
	u32  io_modes_reg;
	u32  op_mask;

	if (xfer->mode_id == I2C_MSM_XFER_MODE_FIFO) {
		in_cnt        = xfer->rx_cnt  + xfer->rx_ovrhd_cnt;
		out_cnt       = xfer->tx_cnt + xfer->tx_ovrhd_cnt;
		no_input      = in_cnt  ? 0 : QUP_NO_INPUT;
		no_output     = out_cnt ? 0 : QUP_NO_OUPUT;
	}

	
	writel_relaxed(0, base + QUP_MX_INPUT_COUNT);
	writel_relaxed(0, base + QUP_MX_OUTPUT_COUNT);

	
	writel_relaxed(in_cnt , base + QUP_MX_READ_COUNT);
	writel_relaxed(out_cnt, base + QUP_MX_WRITE_COUNT);

	config_reg = readl_relaxed(base + QUP_CONFIG);
	config_reg &=
	      ~(QUP_NO_INPUT | QUP_NO_OUPUT | QUP_N_MASK | QUP_MINI_CORE_MASK);
	config_reg |= (no_input | no_output | QUP_N_VAL |
							QUP_MINI_CORE_I2C_VAL);
	writel_relaxed(config_reg, base + QUP_CONFIG);

	io_modes_reg = readl_relaxed(base + QUP_IO_MODES);
	io_modes_reg &=
	   ~(QUP_INPUT_MODE | QUP_OUTPUT_MODE | QUP_PACK_EN | QUP_UNPACK_EN
	     | QUP_OUTPUT_BIT_SHIFT_EN);
	io_modes_reg |=
	   (input_mode | output_mode | QUP_PACK_EN | QUP_UNPACK_EN);
	writel_relaxed(io_modes_reg, base + QUP_IO_MODES);

	op_mask = (xfer->mode_id == I2C_MSM_XFER_MODE_BAM) ?
		    (QUP_INPUT_SERVICE_MASK | QUP_OUTPUT_SERVICE_MASK) : 0 ;
	writel_relaxed(op_mask, base + QUP_OPERATIONAL_MASK);
	/* Ensure that QUP configuration is written before leaving this func */
	wmb();
}

bool i2c_msm_xfer_is_high_speed(struct i2c_msm_ctrl *ctrl)
{
	return ctrl->rsrcs.clk_freq_out > I2C_MSM_CLK_FAST_MAX_FREQ;
}

static void i2c_msm_qup_xfer_init_run_state(struct i2c_msm_ctrl *ctrl)
{
	void __iomem *base = ctrl->rsrcs.base;
	u32 val = 0;

	if (i2c_msm_xfer_is_high_speed(ctrl)) {
		val = I2C_MSM_SCL_NOISE_REJECTION(val, ctrl->noise_rjct_scl);
		val = I2C_MSM_SDA_NOISE_REJECTION(val, ctrl->noise_rjct_sda);
		val = I2C_MSM_CLK_DIV(val, ctrl->rsrcs.clk_freq_in,
					ctrl->rsrcs.clk_freq_out, true);
	} else {
		val = I2C_MSM_SCL_NOISE_REJECTION(val, ctrl->noise_rjct_scl);
		val = I2C_MSM_SDA_NOISE_REJECTION(val, ctrl->noise_rjct_sda);
		val = I2C_MSM_CLK_DIV(val, ctrl->rsrcs.clk_freq_in,
					ctrl->rsrcs.clk_freq_out, false);
	}

	writel_relaxed(val, base + QUP_I2C_MASTER_CLK_CTL);

	/* Ensure that QUP configuration is written before leaving this func */
	wmb();

	if (ctrl->dbgfs.dbg_lvl == MSM_DBG) {
		dev_info(ctrl->dev,
			"QUP state after programming for next transfers\n");
		i2c_msm_dbg_qup_reg_dump(ctrl);
	}
}

static void i2c_msm_fifo_destroy_struct(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);
	kfree(fifo);
	i2c_msm_fifo_set_struct(ctrl, NULL);
}

static void i2c_msm_fifo_wr_word(struct i2c_msm_ctrl *ctrl, u32 data)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);

	writel_relaxed(data, ctrl->rsrcs.base + QUP_OUT_FIFO_BASE);
	i2c_msm_dbg(ctrl, MSM_DBG, "OUT-FIFO:0x%08x", data);
	fifo->tx_bc += 4;
}

static u32 i2c_msm_fifo_rd_word(struct i2c_msm_ctrl *ctrl, u32 *data)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);
	u32 val;

	val = readl_relaxed(ctrl->rsrcs.base + QUP_IN_FIFO_BASE);
	i2c_msm_dbg(ctrl, MSM_DBG, "IN-FIFO :0x%08x", val);
	fifo->rx_bc += 4;

	if (data)
		*data = val;

	return val;
}

static void i2c_msm_fifo_wr_buf_flush(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);
	u32 *word;

	if (!fifo->out_buf_idx)
		return;

	word = (u32 *) fifo->out_buf;
	i2c_msm_fifo_wr_word(ctrl, *word);
	fifo->out_buf_idx = 0;
	*word = 0;
}

/*
 * i2c_msm_fifo_wr_buf:
 *
 * @len buf size (in bytes)
 * @return number of bytes from buf which have been processed (written to
 *         FIFO or kept in out buffer and will be written later)
 */
static size_t
i2c_msm_fifo_wr_buf(struct i2c_msm_ctrl *ctrl, u8 *buf, size_t len)
{
	struct i2c_msm_xfer_mode_fifo *fifo = i2c_msm_fifo_get_struct(ctrl);
	int i;

	for (i = 0 ; i < len; ++i, ++buf) {

		fifo->out_buf[fifo->out_buf_idx] = *buf;
		++fifo->out_buf_idx;

		if (fifo->out_buf_idx == 4) {
			u32 *word = (u32 *) fifo->out_buf;

			i2c_msm_fifo_wr_word(ctrl, *word);
			fifo->out_buf_idx = 0;
			*word = 0;
		}
	}
	return i;
}

static size_t i2c_msm_fifo_xfer_wr_tag(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf = &ctrl->xfer.cur_buf;
	size_t len = 0;

	if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
		char str[I2C_MSM_REG_2_STR_BUF_SZ];
		dev_info(ctrl->dev, "tag.val:0x%llx tag.len:%d %s\n",
			buf->out_tag.val, buf->out_tag.len,
			i2c_msm_dbg_tag_to_str(&buf->out_tag, str,
								sizeof(str)));
	}

	if (buf->out_tag.len) {
		len = i2c_msm_fifo_wr_buf(ctrl, (u8 *) &buf->out_tag.val,
							buf->out_tag.len);

		if (len < buf->out_tag.len)
			goto done;

		buf->out_tag = (struct i2c_msm_tag) {0};
	}
done:
	return len;
}

static void i2c_msm_fifo_read_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf = &ctrl->xfer.cur_buf;
	struct i2c_msg          *msg = ctrl->xfer.msgs + buf->msg_idx;
	u8 *p_tag_val   = (u8 *) &buf->in_tag.val;
	int buf_need_bc = msg->len - buf->byte_idx;
	u8  word[4];
	int copy_bc;
	int word_idx;
	int word_bc;

	if (!buf->is_rx)
		return;

	while (buf_need_bc || buf->in_tag.len) {
		i2c_msm_fifo_rd_word(ctrl, (u32 *) word);
		word_bc  = sizeof(word);
		word_idx = 0;

		if (buf->in_tag.len) {
			copy_bc = min_t(int, word_bc, buf->in_tag.len);

			memcpy(p_tag_val + buf->in_tag.len, word, copy_bc);

			word_idx        += copy_bc;
			word_bc         -= copy_bc;
			buf->in_tag.len -= copy_bc;

			if ((ctrl->dbgfs.dbg_lvl >= MSM_DBG) &&
							!buf->in_tag.len) {
				char str[64];
				dev_info(ctrl->dev, "%s\n",
					i2c_msm_dbg_tag_to_str(&buf->in_tag,
							str, sizeof(str)));
			}
		}

		
		copy_bc = min_t(int, word_bc, buf_need_bc);
		memcpy(msg->buf + buf->byte_idx, word + word_idx, copy_bc);

		buf->byte_idx += copy_bc;
		buf_need_bc   -= copy_bc;
	}
}

static void i2c_msm_fifo_write_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf  = &ctrl->xfer.cur_buf;
	size_t len;
	size_t tag_len;

	tag_len = buf->out_tag.len;
	len = i2c_msm_fifo_xfer_wr_tag(ctrl);
	if (len < tag_len) {
		dev_err(ctrl->dev, "error on writing tag to out FIFO\n");
		return;
	}

	if (!buf->is_rx) {
		if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
			char str[I2C_MSM_REG_2_STR_BUF_SZ];
			int  offset = 0;
			u8  *p      = i2c_msm_buf_to_ptr(buf);
			int  i;

			for (i = 0 ; i < len; ++i, ++p)
				offset += snprintf(str + offset,
						   sizeof(str) - offset,
						   "0x%x ", *p);
			dev_info(ctrl->dev, "data: %s\n", str);
		}

		len = i2c_msm_fifo_wr_buf(ctrl, i2c_msm_buf_to_ptr(buf),
						buf->len);
		if (len < buf->len)
			dev_err(ctrl->dev, "error on xfering buf with FIFO\n");

		buf->prcsed_bc = len;
	}
}

static int i2c_msm_fifo_xfer_process(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf first_buf = ctrl->xfer.cur_buf;
	int ret;

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_PAUSE);
	if (ret < 0)
		return ret;

	
	while (i2c_msm_xfer_next_buf(ctrl))
		i2c_msm_fifo_write_xfer_buf(ctrl);

	i2c_msm_fifo_wr_buf_flush(ctrl);

	ctrl->xfer.cur_buf = first_buf;

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	
	ret = i2c_msm_xfer_wait_for_completion(ctrl);
	if (ret < 0)
		return ret;

	
	while (i2c_msm_xfer_next_buf(ctrl))
		i2c_msm_fifo_read_xfer_buf(ctrl);

	if ((ctrl->adapter.nr < 7) && (ctrl->adapter.nr >= 0))
		error_times[ctrl->adapter.nr] = 0;
	return 0;
}

static int i2c_msm_fifo_xfer(struct i2c_msm_ctrl *ctrl)
{
	int ret;

	i2c_msm_dbg(ctrl, MSM_DBG, "Starting FIFO transfer");

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_reset_state(ctrl);

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_run_state(ctrl);

	ret = i2c_msm_fifo_xfer_process(ctrl);

	return ret;
}

static void i2c_msm_fifo_teardown(struct i2c_msm_ctrl *ctrl) {}

static int i2c_msm_fifo_create_struct(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo =
					kmalloc(sizeof(*fifo), GFP_KERNEL);
	if (!fifo) {
		dev_err(ctrl->dev,
		  "error on allocating memory for fifo mode. malloc(size:%zu\n)",
		  sizeof(*fifo));
		return -ENOMEM;
	}

	*fifo = (struct i2c_msm_xfer_mode_fifo) {
		.ops = (struct i2c_msm_xfer_mode) {
			.xfer     = i2c_msm_fifo_xfer,
			.teardown = i2c_msm_fifo_teardown,
		},
	};
	i2c_msm_fifo_set_struct(ctrl, fifo);

	return 0;
}

static int i2c_msm_bam_xfer_prepare(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam  = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_xfer_buf      *buf  = &ctrl->xfer.cur_buf;
	struct i2c_msm_bam_pipe      *cons = &bam->pipe[I2C_MSM_BAM_CONS];
	struct i2c_msm_bam_pipe      *prod = &bam->pipe[I2C_MSM_BAM_PROD];
	struct i2c_msm_bam_buf *bam_buf;
	int                     rem_buf_cnt = I2C_MSM_BAM_DESC_ARR_SIZ;
	struct i2c_msg         *cur_msg;
	enum dma_data_direction buf_dma_dirctn;
	struct i2c_msm_dma_mem  data;
	u8        *tag_arr_itr_vrtl_addr;
	dma_addr_t tag_arr_itr_phy_addr;

	cons->desc_cnt_cur    = 0;
	prod->desc_cnt_cur    = 0;
	bam->buf_arr_cnt      = 0;
	bam_buf               = bam->buf_arr;
	tag_arr_itr_vrtl_addr = ((u8 *) bam->tag_arr.vrtl_addr);
	tag_arr_itr_phy_addr  = bam->tag_arr.phy_addr;

	for (; i2c_msm_xfer_next_buf(ctrl) && rem_buf_cnt;
		++bam_buf,
		tag_arr_itr_phy_addr  += sizeof(dma_addr_t),
		tag_arr_itr_vrtl_addr += sizeof(dma_addr_t)) {

		
		cur_msg        = ctrl->xfer.msgs + buf->msg_idx;
		data.vrtl_addr = cur_msg->buf + buf->byte_idx;
		if (buf->is_rx) {
			buf_dma_dirctn  = DMA_FROM_DEVICE;
			prod->desc_cnt_cur += 2; 
			cons->desc_cnt_cur += 1; 
		} else {
			buf_dma_dirctn  = DMA_TO_DEVICE;
			cons->desc_cnt_cur += 2; 
		}

		if ((prod->desc_cnt_cur >= prod->desc_cnt_max) ||
		    (cons->desc_cnt_cur >= cons->desc_cnt_max))
			return -ENOMEM;

		data.phy_addr = dma_map_single(ctrl->dev, data.vrtl_addr,
						buf->len, buf_dma_dirctn);

		if (dma_mapping_error(ctrl->dev, data.phy_addr)) {
			dev_err(ctrl->dev,
			  "error DMA mapping BAM buffers. err:%lld "
			  "buf_vrtl:0x%p data_len:%zu dma_dir:%s\n",
			  (u64) data.phy_addr, data.vrtl_addr, buf->len,
			  ((buf_dma_dirctn == DMA_FROM_DEVICE)
				? "DMA_FROM_DEVICE" : "DMA_TO_DEVICE"));
			return -EFAULT;
		}

		
		*((u64 *)tag_arr_itr_vrtl_addr) =  buf->out_tag.val;

		i2c_msm_dbg(ctrl, MSM_DBG,
			"vrtl:0x%p phy:0x%llx val:0x%llx sizeof(dma_addr_t):%zu",
			tag_arr_itr_vrtl_addr, (u64) tag_arr_itr_phy_addr,
			*((u64 *)tag_arr_itr_vrtl_addr), sizeof(dma_addr_t));

		*bam_buf = (struct i2c_msm_bam_buf) {
			.ptr      = data,
			.len      = buf->len,
			.dma_dir  = buf_dma_dirctn,
			.is_rx    = buf->is_rx,
			.is_last  = buf->is_last,
			.tag      = (struct i2c_msm_bam_tag) {
				.buf = tag_arr_itr_phy_addr,
				.len = buf->out_tag.len,
			},
		};
		buf->prcsed_bc = buf->len;

		++bam->buf_arr_cnt;
		--rem_buf_cnt;
	}
	return 0;
}

static void i2c_msm_bam_xfer_unprepare(struct i2c_msm_ctrl *ctrl)
{
	int i;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_bam_buf *buf_itr;

	buf_itr = bam->buf_arr;
	for (i = 0 ; i < bam->buf_arr_cnt ; ++i, ++buf_itr)
		dma_unmap_single(ctrl->dev, buf_itr->ptr.phy_addr, buf_itr->len,
							buf_itr->dma_dir);
}

static int i2c_msm_bam_xfer_rmv_inp_fifo_tag(struct i2c_msm_ctrl *ctrl, u32 len)
{
	int ret;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_bam_pipe      *prod;

	prod = &bam->pipe[I2C_MSM_BAM_PROD];

	i2c_msm_dbg(ctrl, MSM_DBG, "queuing input tag buf len:%d to prod", len);

	ret = sps_transfer_one(prod->handle, bam->input_tag.phy_addr,
			       len , ctrl, 0);

	if (ret < 0)
		dev_err(ctrl->dev,
			"error on reading BAM input tags len:%d sps-err:%d\n",
			len, ret);

	return ret;
}

static int i2c_msm_bam_xfer_process(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_bam_pipe *cons;
	struct i2c_msm_bam_pipe *prod ;
	struct i2c_msm_bam_buf  *buf_itr;
	struct i2c_msm_bam_pipe *pipe;
	int  i;
	int  ret           = 0;
	u32  bam_flags     = 0; 
	char str[64];
	i2c_msm_dbg(ctrl, MSM_DBG, "Going to enqueue %zu buffers in BAM",
							bam->buf_arr_cnt);

	cons = &bam->pipe[I2C_MSM_BAM_CONS];
	prod = &bam->pipe[I2C_MSM_BAM_PROD];
	buf_itr = bam->buf_arr;

	for (i = 0; i < bam->buf_arr_cnt ; ++i, ++buf_itr) {
		
		i2c_msm_dbg(ctrl, MSM_DBG, "queueing bam tag %s",
			i2c_msm_dbg_bam_tag_to_str(&buf_itr->tag, str,
							ARRAY_SIZE(str)));

		ret = sps_transfer_one(cons->handle, buf_itr->tag.buf,
					   buf_itr->tag.len, ctrl, bam_flags);
		if (ret < 0) {
			dev_err(ctrl->dev,
			     "error on queuing tag in bam. sps-err:%d\n", ret);
			goto bam_xfer_end;
		}

		
		if (buf_itr->is_rx) {
			ret = i2c_msm_bam_xfer_rmv_inp_fifo_tag(ctrl, 2);
			if (ret)
				goto bam_xfer_end;
		}

		
		if (buf_itr->is_last && !ctrl->xfer.last_is_rx)
			bam_flags = (SPS_IOVEC_FLAG_EOT | SPS_IOVEC_FLAG_NWD);

		
		pipe = buf_itr->is_rx ? prod : cons;

		i2c_msm_dbg(ctrl, MSM_DBG,
			"Queue data buf to %s pipe desc(phy:0x%llx len:%zu) "
			"EOT:%d NWD:%d",
			pipe->name, (u64) buf_itr->ptr.phy_addr, buf_itr->len,
			!!(bam_flags & SPS_IOVEC_FLAG_EOT),
			!!(bam_flags & SPS_IOVEC_FLAG_NWD));

		ret = sps_transfer_one(pipe->handle, buf_itr->ptr.phy_addr,
				       buf_itr->len, ctrl, bam_flags);
		if (ret < 0) {
			dev_err(ctrl->dev,
			   "error on queuing data to %s BAM pipe, sps-err:%d\n",
			   pipe->name, ret);
			goto bam_xfer_end;
		}
	}

	if (ctrl->xfer.last_is_rx) {
		i2c_msm_dbg(ctrl, MSM_DBG,
				"Queue input tag to read EOT+FLUSH_STOP ");
		ret = i2c_msm_bam_xfer_rmv_inp_fifo_tag(ctrl, 2);
		if (ret)
			goto bam_xfer_end;

		bam_flags = (SPS_IOVEC_FLAG_EOT | SPS_IOVEC_FLAG_NWD);
		i2c_msm_dbg(ctrl, MSM_DBG,
			"Queue EOT+FLUSH_STOP tags to cons EOT:1 NWD:1");

		
		ret = sps_transfer_one(cons->handle,
				       bam->eot_n_flush_stop_tags.phy_addr, 2,
				       ctrl, bam_flags);
		if (ret < 0) {
			dev_err(ctrl->dev,
			"error on queuing EOT+FLUSH_STOP tags to cons EOT:1 NWD:1\n");
			goto bam_xfer_end;
		}
	}

	ret = i2c_msm_xfer_wait_for_completion(ctrl);

bam_xfer_end:
	return ret;
}

static int i2c_msm_bam_pipe_diconnect(struct i2c_msm_ctrl *ctrl,
						struct i2c_msm_bam_pipe  *pipe)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	int ret = sps_disconnect(pipe->handle);
	if (ret) {
		i2c_msm_prof_evnt_add(ctrl, MSM_ERR, i2c_msm_prof_dump_pip_dscn,
						(ulong) pipe, (u32)ret, 0);
		return ret;
	}
	pipe->is_init = false;
	bam->is_init  = false;
	return 0;
}

static int i2c_msm_bam_pipe_connect(struct i2c_msm_ctrl *ctrl,
		struct i2c_msm_bam_pipe  *pipe, struct sps_connect *config)
{
	int ret;
	struct sps_register_event event  = {
		.mode      = SPS_TRIGGER_WAIT,
		.options   = SPS_O_EOT,
		.xfer_done = &ctrl->xfer.complete,
	};

	ret = sps_connect(pipe->handle, config);
	if (ret) {
		i2c_msm_prof_evnt_add(ctrl, MSM_ERR, i2c_msm_prof_dump_pip_cnct,
						(ulong) pipe, (u32)ret, 0);
		return ret;
	}

	ret = sps_register_event(pipe->handle, &event);
	if (ret) {
		dev_err(ctrl->dev,
			"error sps_register_event(hndl:0x%p %s):%d\n",
			pipe->handle, pipe->name, ret);
		i2c_msm_bam_pipe_diconnect(ctrl, pipe);
		return ret;
	}

	pipe->is_init = true;
	return 0;
}

static void i2c_msm_bam_pipe_teardown(struct i2c_msm_ctrl *ctrl,
				      enum i2c_msm_bam_pipe_dir pipe_dir)
{
	struct i2c_msm_xfer_mode_bam *bam  = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_bam_pipe      *pipe = &bam->pipe[pipe_dir];

	i2c_msm_dbg(ctrl, MSM_DBG, "tearing down the BAM %s pipe. is_init:%d",
				i2c_msm_bam_pipe_name[pipe_dir], pipe->is_init);

	if (!pipe->is_init)
		return;

	i2c_msm_bam_pipe_diconnect(ctrl, pipe);
	dma_free_coherent(ctrl->dev,
			  pipe->config.desc.size,
			  pipe->config.desc.base,
			  pipe->config.desc.phys_base);
	sps_free_endpoint(pipe->handle);
	pipe->handle  = 0;
}

static int i2c_msm_bam_pipe_init(struct i2c_msm_ctrl *ctrl,
				 enum i2c_msm_bam_pipe_dir pipe_dir)
{
	int ret = 0;
	struct sps_pipe          *handle;
	struct i2c_msm_bam_pipe  *pipe;
	struct sps_connect       *config;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);

	pipe   = &bam->pipe[pipe_dir];
	config = &pipe->config;

	i2c_msm_dbg(ctrl, MSM_DBG, "Calling BAM %s pipe init. is_init:%d",
				i2c_msm_bam_pipe_name[pipe_dir], pipe->is_init);

	if (pipe->is_init)
		return 0;

	pipe->name = i2c_msm_bam_pipe_name[pipe_dir];
	pipe->handle = 0;
	handle = sps_alloc_endpoint();
	if (!handle) {
		dev_err(ctrl->dev, "error allocating BAM endpoint\n");
		return -ENOMEM;
	}

	ret = sps_get_config(handle, config);
	if (ret) {
		dev_err(ctrl->dev, "error getting BAM pipe config\n");
		goto config_err;
	}

	if (pipe_dir == I2C_MSM_BAM_CONS) {
		config->source          = SPS_DEV_HANDLE_MEM;
		config->destination     = bam->handle;
		config->mode            = SPS_MODE_DEST;
		config->src_pipe_index  = 0;
		config->dest_pipe_index = ctrl->rsrcs.bam_pipe_idx_cons;
		pipe->desc_cnt_max      = I2C_MSM_BAM_CONS_SZ;
	} else {
		config->source          = bam->handle;
		config->destination     = SPS_DEV_HANDLE_MEM;
		config->mode            = SPS_MODE_SRC;
		config->src_pipe_index  = ctrl->rsrcs.bam_pipe_idx_prod;
		config->dest_pipe_index = 0;
		pipe->desc_cnt_max      = I2C_MSM_BAM_PROD_SZ;
	}
	config->options   = SPS_O_EOT | SPS_O_AUTO_ENABLE;
	config->desc.size = pipe->desc_cnt_max * sizeof(struct sps_iovec);
	config->desc.base = dma_alloc_coherent(ctrl->dev,
					       config->desc.size,
					       &config->desc.phys_base,
					       GFP_KERNEL);
	if (!config->desc.base) {
		dev_err(ctrl->dev, "error allocating BAM pipe memory\n");
		ret = -ENOMEM;
		goto config_err;
	}
	memset(config->desc.base, 0, config->desc.size);

	pipe->handle  = handle;
	ret = i2c_msm_bam_pipe_connect(ctrl, pipe, config);
	if (ret)
		goto connect_err;

	pipe->is_init = true;
	return 0;

connect_err:
	dma_free_coherent(ctrl->dev, config->desc.size,
		config->desc.base, config->desc.phys_base);
config_err:
	sps_free_endpoint(handle);

	return ret;
}

static void i2c_msm_bam_pipe_flush(struct i2c_msm_ctrl *ctrl,
					enum i2c_msm_bam_pipe_dir pipe_dir)
{
	struct i2c_msm_xfer_mode_bam *bam    = i2c_msm_bam_get_struct(ctrl);
	struct i2c_msm_bam_pipe      *pipe   = &bam->pipe[pipe_dir];
	struct sps_connect           config  = pipe->config;
	bool   prev_state = bam->is_init;
	int    ret;

	ret = i2c_msm_bam_pipe_diconnect(ctrl, pipe);
	if (ret)
		return;

	ret = i2c_msm_bam_pipe_connect(ctrl, pipe, &config);
	if (ret)
		return;

	bam->is_init  = prev_state;
}

static void i2c_msm_bam_flush(struct i2c_msm_ctrl *ctrl)
{
	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_dump_bam_flsh,
								0, 0, 0);
	i2c_msm_bam_pipe_flush(ctrl, I2C_MSM_BAM_CONS);
	i2c_msm_bam_pipe_flush(ctrl, I2C_MSM_BAM_PROD);
}

static void i2c_msm_bam_teardown(struct i2c_msm_ctrl *ctrl)
{
	u8         *tags_space_virt_addr;
	dma_addr_t  tags_space_phy_addr;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);

	if (!bam->is_core_init)
		return;

	tags_space_virt_addr = bam->input_tag.vrtl_addr;
	tags_space_phy_addr  = bam->input_tag.phy_addr;

	i2c_msm_bam_pipe_teardown(ctrl, I2C_MSM_BAM_CONS);
	i2c_msm_bam_pipe_teardown(ctrl, I2C_MSM_BAM_PROD);

	if (bam->deregister_required) {
		sps_deregister_bam_device(bam->handle);
		bam->deregister_required = false;
	}

	dma_free_coherent(ctrl->dev, I2C_MSM_BAM_TAG_MEM_SZ,
			tags_space_virt_addr, tags_space_phy_addr);

	bam->is_init      = false;
	bam->is_core_init = false;
}

static int i2c_msm_bam_init_pipes(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	int ret;

	ret = i2c_msm_bam_pipe_init(ctrl, I2C_MSM_BAM_PROD);
	if (ret) {
		dev_err(ctrl->dev, "error Failed to init producer BAM-pipe\n");
		goto pipe_error;
	}

	ret = i2c_msm_bam_pipe_init(ctrl, I2C_MSM_BAM_CONS);
	if (ret)
		dev_err(ctrl->dev, "error Failed to init consumer BAM-pipe\n");

	bam->is_init = true;

pipe_error:
	return ret;
}

static int i2c_msm_bam_reg_dev(struct i2c_msm_ctrl *ctrl, ulong *bam_handle)
{
	int                  ret;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	resource_size_t phy_addr = bam->mem->start;
	size_t          mem_size = resource_size(bam->mem);
	struct sps_bam_props props = {
		.phys_addr = phy_addr,
		.irq       = bam->irq,
		.manage    = SPS_BAM_MGR_LOCAL,
		.summing_threshold = 0x10,
	};

	bam->base = devm_ioremap(ctrl->dev, phy_addr, mem_size);
	if (!bam->base) {
		dev_err(ctrl->dev,
			"error ioremap(bam@0x%lx size:0x%zu) failed\n",
			(ulong) phy_addr, mem_size);

		return -EBUSY;
	}
	i2c_msm_dbg(ctrl, MSM_PROF,
		"ioremap(bam@0x%lx size:0x%zu) mapped to (va)0x%p",
		(ulong) phy_addr, mem_size, bam->base);

	props.virt_addr = bam->base;

	ret = sps_register_bam_device(&props, bam_handle);
	if (ret)
		dev_err(ctrl->dev,
		"error sps_register_bam_device(phy:0x%lx virt:0x%lx irq:%d):%d\n"
		, (ulong) props.phys_addr, (ulong) props.virt_addr, props.irq,
		ret);
	else
		bam->deregister_required = true;

	return ret;
}

static int i2c_msm_bam_init(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	ulong           bam_handle;
	int             ret;
	u8             *tags_space_virt_addr;
	dma_addr_t      tags_space_phy_addr;
	resource_size_t phy_addr = bam->mem->start;

	BUG_ON(unlikely(!bam));

	if (bam->is_init)
		return 0;

	ret = (*ctrl->ver.init)(ctrl);
	if (ret) {
		dev_err(ctrl->dev, "error on initializing QUP registers\n");
		return ret;
	}

	i2c_msm_dbg(ctrl, MSM_DBG, "initializing BAM@0x%p", bam);

	if (bam->is_core_init)
		return i2c_msm_bam_init_pipes(ctrl);

	tags_space_virt_addr = dma_alloc_coherent(
						ctrl->dev,
						I2C_MSM_BAM_TAG_MEM_SZ,
						&tags_space_phy_addr,
						GFP_KERNEL);
	if (!tags_space_virt_addr) {
		dev_err(ctrl->dev,
		  "error alloc %d bytes of DMAable memory for BAM tags space\n",
		  I2C_MSM_BAM_TAG_MEM_SZ);
		ret = -ENOMEM;
		goto bam_init_error;
	}

	
	bam->input_tag.vrtl_addr  = tags_space_virt_addr;
	bam->eot_n_flush_stop_tags.vrtl_addr
				  = tags_space_virt_addr + I2C_MSM_TAG2_MAX_LEN;
	bam->tag_arr.vrtl_addr    = tags_space_virt_addr
						+ (I2C_MSM_TAG2_MAX_LEN * 2);

	
	bam->input_tag.phy_addr   = tags_space_phy_addr;
	bam->eot_n_flush_stop_tags.phy_addr
				  = tags_space_phy_addr + I2C_MSM_TAG2_MAX_LEN;
	bam->tag_arr.phy_addr     = tags_space_phy_addr
						+ (I2C_MSM_TAG2_MAX_LEN * 2);

	
	*((u16 *) bam->eot_n_flush_stop_tags.vrtl_addr) =
				QUP_TAG2_INPUT_EOT | (QUP_TAG2_FLUSH_STOP << 8);

	ret = sps_phy2h(phy_addr, &bam_handle);
	if (ret || !bam_handle) {
		ret = i2c_msm_bam_reg_dev(ctrl, &bam_handle);
		if (ret)
			goto bam_init_error;
	}

	bam->handle       = bam_handle;
	bam->is_core_init = true;

	ret = i2c_msm_bam_init_pipes(ctrl);
	if (ret)
		goto bam_init_error;

	bam->is_init = true;
	return 0;

bam_init_error:
	i2c_msm_bam_teardown(ctrl);
	return ret;
}

static int i2c_msm_bam_xfer(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);

	i2c_msm_dbg(ctrl, MSM_DBG, "Starting BAM transfer");

	if (!bam->is_init) {
		ret = i2c_msm_bam_init(ctrl);
		if (ret)
			return ret;
	}

	if (ctrl->xfer.last_is_rx) {
		ctrl->xfer.rx_ovrhd_cnt += 2; 
		ctrl->xfer.tx_ovrhd_cnt += 2; 
	}

	
	ret = i2c_msm_bam_xfer_prepare(ctrl);
	if (ret < 0) {
		dev_err(ctrl->dev, "error on i2c_msm_bam_xfer_prepare():%d\n",
									ret);
		goto err_bam_xfer;
	}

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
	if (ret < 0)
		goto err_bam_xfer;

	
	i2c_msm_qup_xfer_init_reset_state(ctrl);

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		goto err_bam_xfer;

	
	i2c_msm_qup_xfer_init_run_state(ctrl);

	
	ret = i2c_msm_bam_xfer_process(ctrl);
	if (ret)
		dev_err(ctrl->dev,
			"error i2c_msm_bam_xfer_process(n_bufs:%zu):%d\n",
			bam->buf_arr_cnt, ret);

err_bam_xfer:
	i2c_msm_bam_xfer_unprepare(ctrl);

	return ret;
}

static void i2c_msm_bam_destroy_struct(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);
	kfree(bam);
	i2c_msm_bam_set_struct(ctrl, NULL);
}

static int i2c_msm_bam_create_struct(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = kmalloc(sizeof(*bam), GFP_KERNEL);

	if (!bam) {
		dev_err(ctrl->dev,
		   "error on allocating memory for bam mode. malloc(size:%zu)\n",
		   sizeof(*bam));
		return -ENOMEM;
	}

	*bam = (struct i2c_msm_xfer_mode_bam) {
		.ops = (struct i2c_msm_xfer_mode) {
			.xfer     = i2c_msm_bam_xfer,
			.teardown = i2c_msm_bam_teardown,
		},
	};

	i2c_msm_bam_set_struct(ctrl, bam);
	return 0;
}

static int i2c_msm_qup_rsrcs_init(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_bam *bam = i2c_msm_bam_get_struct(ctrl);

	bam->mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"bam_phys_addr");
	if (!bam->mem) {
		i2c_msm_dbg(ctrl, MSM_PROF,
				"Missing 'qup_phys_addr' resource entry");
		return -ENODEV;
	}

	bam->irq = platform_get_irq_byname(pdev, "bam_irq");
	if (bam->irq < 0) {
		dev_warn(&pdev->dev, "missing 'bam_irq' resource entry");
		return -EINVAL;
	}

	return 0;
}

static bool i2c_msm_qup_slv_holds_bus(struct i2c_msm_ctrl *ctrl)
{
	u32 status = readl_relaxed(ctrl->rsrcs.base + QUP_I2C_STATUS);

	bool slv_holds_bus =	!(status & QUP_I2C_SDA) &&
				(status & QUP_BUS_ACTIVE) &&
				!(status & QUP_BUS_MASTER);
	if (slv_holds_bus)
		dev_info(ctrl->dev,
			"bus lines held low by a slave detected\n");

	return slv_holds_bus;
}

static int i2c_msm_qup_poll_bus_active_unset(struct i2c_msm_ctrl *ctrl)
{
	void __iomem *base    = ctrl->rsrcs.base;
	ulong timeout = jiffies + msecs_to_jiffies(I2C_MSM_MAX_POLL_MSEC);
	int    ret      = 0;
	size_t read_cnt = 0;

	do {
		if (!(readl_relaxed(base + QUP_I2C_STATUS) & QUP_BUS_ACTIVE))
			goto poll_active_end;
		++read_cnt;
	} while (time_before_eq(jiffies, timeout));

	ret = -EBUSY;

poll_active_end:
	
	i2c_msm_prof_evnt_add(ctrl, MSM_DBG, i2c_msm_prof_dump_actv_end,
				ret, (ret ? 0 : (timeout - jiffies)), read_cnt);

	return ret;
}

static void i2c_msm_clk_path_vote(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_path_vote.client_hdl)
		msm_bus_scale_client_update_request(
					ctrl->rsrcs.clk_path_vote.client_hdl,
					I2C_MSM_CLK_PATH_RESUME_VEC);
}

static void i2c_msm_clk_path_unvote(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_path_vote.client_hdl)
		msm_bus_scale_client_update_request(
					ctrl->rsrcs.clk_path_vote.client_hdl,
					I2C_MSM_CLK_PATH_SUSPEND_VEC);
}

static void i2c_msm_clk_path_teardown(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_path_vote.client_hdl) {
		msm_bus_scale_unregister_client(
					ctrl->rsrcs.clk_path_vote.client_hdl);
		ctrl->rsrcs.clk_path_vote.client_hdl = 0;
	}
}

static int i2c_msm_clk_path_init_structs(struct i2c_msm_ctrl *ctrl)
{
	struct msm_bus_vectors *paths    = NULL;
	struct msm_bus_paths   *usecases = NULL;

	i2c_msm_dbg(ctrl, MSM_PROF, "initializes path clock voting structs");

	paths = devm_kzalloc(ctrl->dev, sizeof(*paths) * 2, GFP_KERNEL);
	if (!paths) {
		dev_err(ctrl->dev,
			"error msm_bus_paths.paths memory allocation failed\n");
		return -ENOMEM;
	}

	usecases = devm_kzalloc(ctrl->dev, sizeof(*usecases) * 2, GFP_KERNEL);
	if (!usecases) {
		dev_err(ctrl->dev,
		"error  msm_bus_scale_pdata.usecases memory allocation failed\n");
		goto path_init_err;
	}

	ctrl->rsrcs.clk_path_vote.pdata = devm_kzalloc(ctrl->dev,
				       sizeof(*ctrl->rsrcs.clk_path_vote.pdata),
				       GFP_KERNEL);
	if (!ctrl->rsrcs.clk_path_vote.pdata) {
		dev_err(ctrl->dev,
			"error  msm_bus_scale_pdata memory allocation failed\n");
		goto path_init_err;
	}

	paths[I2C_MSM_CLK_PATH_SUSPEND_VEC] = (struct msm_bus_vectors) {
		.src = ctrl->rsrcs.clk_path_vote.mstr_id,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	};

	paths[I2C_MSM_CLK_PATH_RESUME_VEC]  = (struct msm_bus_vectors) {
		.src = ctrl->rsrcs.clk_path_vote.mstr_id,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = I2C_MSM_CLK_PATH_AVRG_BW(ctrl),
		.ib  = I2C_MSM_CLK_PATH_BRST_BW(ctrl),
	};

	usecases[I2C_MSM_CLK_PATH_SUSPEND_VEC] = (struct msm_bus_paths) {
		.num_paths = 1,
		.vectors   = &paths[I2C_MSM_CLK_PATH_SUSPEND_VEC],
	};

	usecases[I2C_MSM_CLK_PATH_RESUME_VEC] = (struct msm_bus_paths) {
		.num_paths = 1,
		.vectors   = &paths[I2C_MSM_CLK_PATH_RESUME_VEC],
	};

	*ctrl->rsrcs.clk_path_vote.pdata = (struct msm_bus_scale_pdata) {
		.usecase      = usecases,
		.num_usecases = 2,
		.name         = dev_name(ctrl->dev),
	};

	return 0;

path_init_err:
	devm_kfree(ctrl->dev, paths);
	devm_kfree(ctrl->dev, usecases);
	devm_kfree(ctrl->dev, ctrl->rsrcs.clk_path_vote.pdata);
	ctrl->rsrcs.clk_path_vote.pdata = NULL;
	return -ENOMEM;
}

static int i2c_msm_clk_path_postponed_register(struct i2c_msm_ctrl *ctrl)
{
	ctrl->rsrcs.clk_path_vote.client_hdl =
		msm_bus_scale_register_client(ctrl->rsrcs.clk_path_vote.pdata);

	if (ctrl->rsrcs.clk_path_vote.client_hdl) {
		if (ctrl->rsrcs.clk_path_vote.reg_err) {
			
			ctrl->rsrcs.clk_path_vote.reg_err = false;
			dev_err(ctrl->dev,
				"msm_bus_scale_register_client(mstr-id:%d):0x%x (ok)",
				ctrl->rsrcs.clk_path_vote.mstr_id,
				ctrl->rsrcs.clk_path_vote.client_hdl);
		}
	} else {
		
		if (!ctrl->rsrcs.clk_path_vote.reg_err) {
			ctrl->rsrcs.clk_path_vote.reg_err = true;

			dev_info(ctrl->dev,
				"msm_bus_scale_register_client(mstr-id:%d):0 (not a problem)",
				ctrl->rsrcs.clk_path_vote.mstr_id);
		}
	}

	return ctrl->rsrcs.clk_path_vote.client_hdl ? 0 : -EAGAIN;
}

static void i2c_msm_clk_path_init(struct i2c_msm_ctrl *ctrl)
{
	if (!ctrl->rsrcs.clk_path_vote.mstr_id ||
		ctrl->rsrcs.clk_path_vote.client_hdl)
		return;

	
	if (!ctrl->rsrcs.clk_path_vote.pdata &&
					i2c_msm_clk_path_init_structs(ctrl)) {
		ctrl->rsrcs.clk_path_vote.mstr_id = 0;
		return;
	};

	
	if (i2c_msm_clk_path_postponed_register(ctrl))
		return;
}

static irqreturn_t i2c_msm_qup_isr(int irq, void *devid)
{
	struct i2c_msm_ctrl *ctrl = devid;
	void __iomem        *base = ctrl->rsrcs.base;
	u32  i2c_status = 0;
	u32  err_flags  = 0;
	u32  qup_op     = 0;
	u32  clr_flds   = 0;
	bool dump_details    = false;
	bool log_event       = false;
	bool signal_complete = false;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_dump_irq_begn,
								irq, 0, 0);

	if (!atomic_read(&ctrl->is_ctrl_active)) {
		dev_info(ctrl->dev, "irq:%d when PM suspended\n", irq);
		return IRQ_NONE;
	}

	if (!ctrl->xfer.msgs) {
		dev_info(ctrl->dev, "irq:%d when no active transfer\n", irq);
		writel_relaxed(QUP_STATE_RESET, base + QUP_STATE);
		/* Ensure that state is written before ISR exits */
		wmb();
		goto isr_end;
	}

	i2c_status  = readl_relaxed(base + QUP_I2C_STATUS);
	err_flags   = readl_relaxed(base + QUP_ERROR_FLAGS);
	qup_op      = readl_relaxed(base + QUP_OPERATIONAL);
	i2c_msm_dbg(ctrl, MSM_DBG,
	    "IRQ MASTER_STATUS:0x%08x ERROR_FLAGS:0x%08x OPERATIONAL:0x%08x\n",
	    i2c_status, err_flags, qup_op);

	if (err_flags & QUP_ERR_FLGS_MASK) {
		dump_details    = true;
		signal_complete = true;
		log_event       = true;
	}

	if (i2c_status & QUP_MSTR_STTS_ERR_MASK) {
		signal_complete = true;
		log_event       = true;

		if (i2c_status & QUP_PACKET_NACKED) {
			struct i2c_msg *cur_msg = ctrl->xfer.msgs +
						ctrl->xfer.cur_buf.msg_idx;
			dev_err(ctrl->dev,
				"slave:0x%x is not responding (I2C-NACK) ensure the slave is powered and out of reset",
				cur_msg->addr);

			ctrl->xfer.err |= I2C_MSM_ERR_NACK;
			dump_details = true;
		}

		if (i2c_status & QUP_ARB_LOST)
			ctrl->xfer.err |= I2C_MSM_ERR_ARB_LOST;

		if (i2c_status & QUP_BUS_ERROR)
			ctrl->xfer.err |= I2C_MSM_ERR_BUS_ERR;
	}

	if (qup_op & QUP_MAX_INPUT_DONE_FLAG) {
		log_event = true;
		if (ctrl->xfer.last_is_rx)
			signal_complete = true;
	}
	if (qup_op & (QUP_OUTPUT_SERVICE_FLAG | QUP_MAX_OUTPUT_DONE_FLAG)) {
		log_event = true;
		if (!ctrl->xfer.last_is_rx)
			signal_complete = true;
	}

	if (dump_details && (ctrl->dbgfs.dbg_lvl >= MSM_DBG)) {
		dev_info(ctrl->dev,
				"irq:%d transfer details and reg dump\n", irq);
		i2c_msm_dbg_xfer_dump(ctrl);
		i2c_msm_dbg_qup_reg_dump(ctrl);
	}

	clr_flds = i2c_status & QUP_MSTR_STTS_ERR_MASK;
	if (clr_flds)
		writel_relaxed(clr_flds, base + QUP_I2C_STATUS);

	clr_flds = err_flags & QUP_ERR_FLGS_MASK;
	if (clr_flds)
		writel_relaxed(clr_flds,  base + QUP_ERROR_FLAGS);

	clr_flds = qup_op & (QUP_OUTPUT_SERVICE_FLAG | QUP_INPUT_SERVICE_FLAG);
	if (clr_flds)
		writel_relaxed(clr_flds,     base + QUP_OPERATIONAL);

isr_end:
	if (dump_details || log_event || (ctrl->dbgfs.dbg_lvl >= MSM_DBG))
		i2c_msm_prof_evnt_add(ctrl, MSM_PROF,
					i2c_msm_prof_dump_irq_end,
					i2c_status, qup_op, err_flags);

	if (signal_complete)
		complete(&ctrl->xfer.complete);

	return IRQ_HANDLED;
}

static int i2c_msm_qup_mini_core_init(struct i2c_msm_ctrl *ctrl)
{
	void __iomem *base = ctrl->rsrcs.base;
	u32 val = readl_relaxed(base + QUP_STATE);

	if (!(val & QUP_I2C_MAST_GEN))
		dev_err(ctrl->dev,
			"error on verifying HW support (I2C_MAST_GEN=0)\n");

	writel_relaxed(QUP_MINI_CORE_I2C_VAL, base + QUP_CONFIG);
	writel_relaxed(QUP_EN_VERSION_TWO_TAG, base + QUP_I2C_MASTER_CONFIG);

	val = readl_relaxed(base + QUP_CONFIG);
	writel_relaxed(val | QUP_N_VAL, base + QUP_CONFIG);

	return 0;
}

static int i2c_msm_qup_create_struct(struct i2c_msm_ctrl *ctrl)
{
	int ret = i2c_msm_bam_create_struct(ctrl);
	if (ret)
		return ret;

	ret = i2c_msm_fifo_create_struct(ctrl);
	if (ret)
		i2c_msm_bam_destroy_struct(ctrl);

	return ret;
}

static void i2c_msm_qup_destroy_struct(struct i2c_msm_ctrl *ctrl)
{
	i2c_msm_fifo_destroy_struct(ctrl);
	i2c_msm_bam_destroy_struct(ctrl);
}

static int i2c_msm_qup_init(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	void __iomem *base = ctrl->rsrcs.base;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_reset, 0, 0, 0);

	i2c_msm_qup_sw_reset(ctrl);
	i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);

	writel_relaxed(QUP_APP_CLK_ON_EN | QUP_CORE_CLK_ON_EN |
				QUP_FIFO_CLK_GATE_EN,
				base + QUP_CONFIG);

	writel_relaxed(QUP_OUTPUT_OVER_RUN_ERR_EN | QUP_INPUT_UNDER_RUN_ERR_EN
		     | QUP_OUTPUT_UNDER_RUN_ERR_EN | QUP_INPUT_OVER_RUN_ERR_EN,
					base + QUP_ERROR_FLAGS_EN);

	writel_relaxed(QUP_INPUT_SERVICE_MASK | QUP_OUTPUT_SERVICE_MASK,
					base + QUP_OPERATIONAL_MASK);

	writel_relaxed(0, base + QUP_CONFIG);
	writel_relaxed(0, base + QUP_TEST_CTRL);
	writel_relaxed(0, base + QUP_IO_MODES);

	ret = i2c_msm_qup_mini_core_init(ctrl);
	if (ret)
		return ret;

	i2c_msm_qup_fifo_calc_size(ctrl);
	/*
	 * Ensure that QUP configuration is written and that fifo size if read
	 * before leaving this function
	 */
	mb();

	return ret;
}

static bool i2c_msm_qup_do_bus_clear(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	ulong min_sleep_usec;
	dev_info(ctrl->dev, "Executing bus recovery procedure (9 clk pulse)\n");

	
	ret = i2c_msm_qup_init(ctrl);
	if (ret)
		return ret;

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret)
		return ret;

	i2c_msm_qup_xfer_init_run_state(ctrl);

	writel_relaxed(0x1, ctrl->rsrcs.base + QUP_I2C_MASTER_BUS_CLR);

	min_sleep_usec =
	  max_t(ulong, (9 * 10 * USEC_PER_SEC) / ctrl->rsrcs.clk_freq_out, 100);

	usleep_range(min_sleep_usec, min_sleep_usec * 10);

	return 0;
}

static void i2c_msm_qup_teardown(struct i2c_msm_ctrl *ctrl)
{
	int i;
	i2c_msm_dbg(ctrl, MSM_PROF, "Teardown the QUP and BAM");

	for (i = 0; i < I2C_MSM_XFER_MODE_NONE ; ++i)
		(*ctrl->ver.xfer_mode[i]->teardown)(ctrl);
}

static int i2c_msm_qup_post_xfer(struct i2c_msm_ctrl *ctrl, int err)
{
	bool need_reset = false;

	
	if (i2c_msm_qup_poll_bus_active_unset(ctrl)) {
		if ((ctrl->xfer.err & I2C_MSM_ERR_ARB_LOST) ||
		    (ctrl->xfer.err & I2C_MSM_ERR_BUS_ERR)) {
			if (i2c_msm_qup_slv_holds_bus(ctrl))
				i2c_msm_qup_do_bus_clear(ctrl);

			if (!err)
				err = -EIO;
		}
	}

	if (ctrl->xfer.err & I2C_MSM_ERR_TIMEOUT) {
		err = -ETIMEDOUT;
		need_reset = true;
	}

	if (ctrl->xfer.err & I2C_MSM_ERR_NACK) {
		writel_relaxed(QUP_I2C_FLUSH,  ctrl->rsrcs.base + QUP_STATE);
		err = -ENOTCONN;
		need_reset = true;
	}

	if ((ctrl->xfer.err) && (ctrl->xfer.mode_id == I2C_MSM_XFER_MODE_BAM)) {
		i2c_msm_bam_flush(ctrl);
		need_reset = true;
	}

	if (need_reset)
		i2c_msm_qup_init(ctrl);

	return err;
}

static void i2c_msm_qup_choose_mode(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo;
	struct i2c_msm_xfer           *xfer = &ctrl->xfer;
	size_t rx_cnt_sum = xfer->rx_cnt + xfer->rx_ovrhd_cnt;
	size_t tx_cnt_sum = xfer->tx_cnt + xfer->tx_ovrhd_cnt;

	fifo = i2c_msm_fifo_get_struct(ctrl);

	if (ctrl->dbgfs.force_xfer_mode != I2C_MSM_XFER_MODE_NONE)
		xfer->mode_id = ctrl->dbgfs.force_xfer_mode;
	else if (((rx_cnt_sum < fifo->input_fifo_sz) &&
		  (tx_cnt_sum < fifo->output_fifo_sz)) ||
						ctrl->rsrcs.disable_dma)
		xfer->mode_id = I2C_MSM_XFER_MODE_FIFO;
	else
		xfer->mode_id = I2C_MSM_XFER_MODE_BAM;
}

static void i2c_msm_qup_set_version(struct i2c_msm_ctrl *ctrl)
{
	ctrl->ver = (struct i2c_msm_ctrl_ver) {
		.create               = i2c_msm_qup_create_struct,
		.destroy              = i2c_msm_qup_destroy_struct,
		.init                 = i2c_msm_qup_init,
		.reset                = i2c_msm_qup_sw_reset,
		.teardown             = i2c_msm_qup_teardown,
		.init_rsrcs           = i2c_msm_qup_rsrcs_init,
		.choose_mode          = i2c_msm_qup_choose_mode,
		.post_xfer            = i2c_msm_qup_post_xfer,
		.max_rx_cnt           = 0xFFFF,
		.max_tx_cnt           = 0xFFFF,
		.max_buf_size         = 0xFF,
		.msg_ovrhd_bc         = I2C_MSM_TAG2_MAX_LEN,
		.buf_ovrhd_bc         = 2, 
	};
}

static const int
i2c_msm_ctrl_ver_detect_and_set(struct i2c_msm_ctrl *ctrl)
{
	enum i2c_msm_ctrl_ver_num ver_family;
	u32 ver_num = readl_relaxed(ctrl->rsrcs.base + QUP_HW_VERSION);

	if (ver_num < I2C_MSM_CTRL_VER_B_MIN)
		ver_family = I2C_MSM_CTRL_VER_A;
	else if (ver_num < I2C_MSM_CTRL_VER_B_MAX)
		ver_family = I2C_MSM_CTRL_VER_B;
	else
		ver_family = I2C_MSM_CTRL_VER_UNKNOWN;

	if (ver_family ==  I2C_MSM_CTRL_VER_B) {
		i2c_msm_dbg(ctrl, MSM_PROF,
				"B-family HW detected (ver:0x%x)...", ver_num);
		i2c_msm_qup_set_version(ctrl);
		return 0;
	}

	dev_err(ctrl->dev,
		"unsupported hardware version detected ver#:0x%x", ver_num);
	return -ENODEV;
}
static void i2c_msm_xfer_calc_timeout(struct i2c_msm_ctrl *ctrl)
{
	size_t byte_cnt = ctrl->xfer.rx_cnt + ctrl->xfer.tx_cnt;
	size_t bit_cnt  = byte_cnt * 9;
	size_t bit_usec = (bit_cnt * USEC_PER_SEC) / ctrl->rsrcs.clk_freq_out;
	size_t loging_ovrhd_coef = ctrl->dbgfs.dbg_lvl + 1;
	size_t safety_coef   = I2C_MSM_TIMEOUT_SAFTY_COEF * loging_ovrhd_coef;
	size_t xfer_max_usec = (bit_usec * safety_coef) +
						I2C_MSM_TIMEOUT_MIN_USEC;

	ctrl->xfer.timeout = usecs_to_jiffies(xfer_max_usec);
}

static int i2c_msm_xfer_wait_for_completion(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	long  time_left;
	int   ret = 0;

	time_left = wait_for_completion_timeout(&xfer->complete,
							xfer->timeout);
	if (!time_left) {
		i2c_msm_prof_evnt_add(ctrl, MSM_ERR, i2c_msm_prof_dump_cmplt_fl,
					xfer->timeout, time_left, 0);

		i2c_msm_dbg_xfer_dump(ctrl);
		i2c_msm_dbg_qup_reg_dump(ctrl);
		i2c_msm_dbg_inp_fifo_dump(ctrl);
		xfer->err |= I2C_MSM_ERR_TIMEOUT;
		ret = -EIO;
	} else {
		i2c_msm_prof_evnt_add(ctrl, MSM_DBG, i2c_msm_prof_dump_cmplt_ok,
					xfer->timeout, time_left, 0);
	}

	return ret;
}

static u16 i2c_msm_slv_rd_wr_addr(u16 slv_addr, bool is_rx)
{
	return (slv_addr << 1) | (is_rx ? 0x1 : 0x0);
}

static bool i2c_msm_xfer_msg_is_last(struct i2c_msm_ctrl *ctrl)
{
	return ctrl->xfer.cur_buf.msg_idx >= (ctrl->xfer.msg_cnt - 1);
}

static bool i2c_msm_xfer_next_msg(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *cur_buf = &ctrl->xfer.cur_buf;
	struct i2c_msg          *cur_msg = ctrl->xfer.msgs + cur_buf->msg_idx;
	struct i2c_msg          *prv_msg;
	bool is_last_msg  = i2c_msm_xfer_msg_is_last(ctrl);
	bool is_first_msg = !cur_buf->msg_idx;
	bool is_hs        = i2c_msm_xfer_is_high_speed(ctrl);
	bool start_req;
	u16  slv_addr;

	if (cur_buf->is_init) {
		if (is_last_msg) {
			return false;
		} else {
			++cur_buf->msg_idx;
			++cur_msg;
			is_last_msg  = i2c_msm_xfer_msg_is_last(ctrl);
			is_first_msg = false;
		}
	} else {
		cur_buf->is_init = true;
	}
	
	prv_msg = cur_msg - 1;

	
	cur_buf->byte_idx  = 0;
	cur_buf->prcsed_bc = 0;

	cur_buf->is_last  = is_last_msg;
	cur_buf->len      = cur_msg->len;
	cur_buf->is_rx    = (cur_msg->flags & I2C_M_RD);
	start_req         = (is_first_msg || (prv_msg->flags & I2C_M_RD) ||
			    (cur_msg->addr != prv_msg->addr) ||
			    ((cur_msg->flags & I2C_M_RD) !=
					(prv_msg->flags & I2C_M_RD)));
	slv_addr          = i2c_msm_slv_rd_wr_addr(cur_msg->addr,
							cur_buf->is_rx);

	cur_buf->out_tag = i2c_msm_tag_create(is_hs, start_req, is_last_msg,
				cur_buf->is_rx, cur_buf->len, slv_addr);

	cur_buf->in_tag.len = cur_buf->is_rx ? ctrl->ver.buf_ovrhd_bc : 0;

	if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
		char str[I2C_MSM_REG_2_STR_BUF_SZ];

		i2c_msm_dbg_tag_to_str(&cur_buf->out_tag, str, sizeof(str));
		dev_info(ctrl->dev,
			"msg[%d] first:0x%x last:0x%x new_adr:0x%x inp:0x%x "
			"len:%zu adr:0x%x tag:%s\n",
			cur_buf->msg_idx, is_first_msg, is_last_msg,
			start_req, cur_buf->is_rx, cur_buf->len, slv_addr,
			str);
	}

	return  true;
}

static bool i2c_msm_xfer_next_buf(struct i2c_msm_ctrl *ctrl)
{
	bool ret;
	struct i2c_msm_xfer_buf *cur_buf = &ctrl->xfer.cur_buf;

	if (cur_buf->is_init) {
		struct i2c_msg *cur_msg = ctrl->xfer.msgs +
						ctrl->xfer.cur_buf.msg_idx;
		if (cur_msg->len > 0xFF) {
			dev_info(ctrl->dev, "Unsupported message len:%d\n",
								cur_msg->len);
			i2c_msm_dbg_xfer_dump(ctrl);
			return false;
		}
	}

	ret = i2c_msm_xfer_next_msg(ctrl);
	if (ret)
		i2c_msm_prof_evnt_add(ctrl, MSM_DBG, i2c_msm_prof_dump_next_buf,
					cur_buf->msg_idx, cur_buf->byte_idx, 0);

	return ret;
}

static void i2c_msm_xfer_scan(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer     *xfer      = &ctrl->xfer;
	struct i2c_msm_xfer_buf  first_buf = ctrl->xfer.cur_buf;
	struct i2c_msm_xfer_buf *cur_buf   = &ctrl->xfer.cur_buf;
	int msg_num = 0;

	while (i2c_msm_xfer_next_buf(ctrl)) {

		if (cur_buf->is_rx)
			xfer->rx_cnt += cur_buf->len;
		else
			xfer->tx_cnt += cur_buf->len;

		xfer->rx_ovrhd_cnt += cur_buf->in_tag.len;
		xfer->tx_ovrhd_cnt += cur_buf->out_tag.len;

		if (cur_buf->is_last)
			xfer->last_is_rx = cur_buf->is_rx;

		if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
			struct i2c_msg *msg = xfer->msgs + cur_buf->msg_idx;

			dev_info(ctrl->dev,
				"msg[%02d] len:%d is_rx:0x%x addr:0x%x\n",
				msg_num, msg->len, cur_buf->is_rx, msg->addr);
			++msg_num;
		}
	}
	ctrl->xfer.cur_buf = first_buf;
}

static int
i2c_msm_frmwrk_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	int ret = 0;
	struct i2c_msm_ctrl      *ctrl = i2c_get_adapdata(adap);
	struct i2c_msm_xfer      *xfer = &ctrl->xfer;
	struct i2c_msm_xfer_mode *xfer_mode;

	mutex_lock(&ctrl->mlock);
	if (ctrl->pwr_state == MSM_I2C_PM_SYS_SUSPENDED) {
		dev_err(ctrl->dev,
				"slave:0x%x is calling xfer when system is suspended\n",
				msgs->addr);
		mutex_unlock(&ctrl->mlock);
		return -EIO;
	}

	
	xfer->msgs         = msgs;
	xfer->msg_cnt      = num;
	xfer->mode_id      = I2C_MSM_XFER_MODE_NONE;
	xfer->err          = 0;
	xfer->rx_cnt       = 0;
	xfer->tx_cnt       = 0;
	xfer->rx_ovrhd_cnt = 0;
	xfer->tx_ovrhd_cnt = 0;
	atomic_set(&xfer->event_cnt, 0);
	init_completion(&xfer->complete);
	xfer->cur_buf.is_init = false;
	xfer->cur_buf.msg_idx = 0;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_dump_xfer_beg,
							num, msgs->addr, 0);

	i2c_msm_pm_resume_adptr(ctrl);
	
	if (ctrl->pwr_state != MSM_I2C_PM_ACTIVE) {
		dev_info(ctrl->dev, "Runtime PM-callback was not invoked.\n");
		i2c_msm_pm_resume_impl(ctrl->dev);
	}

	i2c_msm_xfer_scan(ctrl);
	i2c_msm_xfer_calc_timeout(ctrl);
	(*ctrl->ver.choose_mode)(ctrl);

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_dump_scan_sum,
		((xfer->rx_cnt & 0xff) | ((xfer->rx_ovrhd_cnt & 0xff) << 16)),
		((xfer->tx_cnt & 0xff) | ((xfer->tx_ovrhd_cnt & 0xff) << 16)),
		((ctrl->xfer.timeout & 0xfff) | ((xfer->mode_id & 0xf) << 24)));

	xfer_mode = ctrl->ver.xfer_mode[xfer->mode_id];
	ret = (*xfer_mode->xfer)(ctrl);
	ret = (*ctrl->ver.post_xfer)(ctrl, ret);

	i2c_msm_pm_suspend_adptr(ctrl);

	
	if (!ret)
		ret = xfer->cur_buf.msg_idx + 1;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, i2c_msm_prof_dump_xfer_end,
				ret, xfer->err, xfer->cur_buf.msg_idx + 1);
	
	if (xfer->err || (ctrl->dbgfs.dbg_lvl >= MSM_PROF))
		i2c_msm_prof_evnt_dump(ctrl);

	
	xfer->msg_cnt	= 0;
	xfer->msgs	= NULL;
	mutex_unlock(&ctrl->mlock);
	return ret;
}

enum i2c_msm_dt_entry_status {
	DT_REQ,  
	DT_SGST, 
	DT_OPT,  
};

enum i2c_msm_dt_entry_type {
	DT_U32,
	DT_BOOL,
	DT_ID,   
	DT_GPIO,
};

struct i2c_msm_dt_to_pdata_map {
	const char                  *dt_name;
	void                        *ptr_data;
	enum i2c_msm_dt_entry_status status;
	enum i2c_msm_dt_entry_type   type;
	int                          default_val;
};

static int i2c_msm_dt_to_pdata_populate(struct i2c_msm_ctrl *ctrl,
					struct platform_device *pdev,
					struct i2c_msm_dt_to_pdata_map *itr)
{
	int  ret, err = 0;
	struct device_node *node = pdev->dev.of_node;
	uint32_t irq_gpio_flags;

	for (; itr->dt_name ; ++itr) {
		switch (itr->type) {
		case DT_U32:
			ret = of_property_read_u32(node, itr->dt_name,
							 (u32 *) itr->ptr_data);
			break;
		case DT_BOOL:
			*((bool *) itr->ptr_data) =
				of_property_read_bool(node, itr->dt_name);
			ret = 0;
			break;
		case DT_ID:
			ret = of_alias_get_id(node, itr->dt_name);
			if (ret >= 0) {
				*((int *) itr->ptr_data) = ret;
				ret = 0;
			}
			break;
		case DT_GPIO:
			ret = of_get_named_gpio_flags(node, itr->dt_name, 0,
						      &irq_gpio_flags);
			if (ret >= 0) {
				*((int *) itr->ptr_data) = ret;
				ret = 0;
			}
			break;
		default:
			dev_err(ctrl->dev,
				"error %d is of unknown DT entry type\n",
				itr->type);
			ret = -EBADE;
		}

		i2c_msm_dbg(ctrl, MSM_PROF, "DT entry ret:%d name:%s val:%d",
				ret, itr->dt_name, *((int *)itr->ptr_data));

		if (ret) {
			*((int *)itr->ptr_data) = itr->default_val;

			if (itr->status < DT_OPT) {
				dev_err(ctrl->dev,
					"error Missing '%s' DT entry\n",
					itr->dt_name);

				
				if (itr->status == DT_REQ && !err)
					err = ret;
			}
		}
	}

	return err;
}


static int i2c_msm_rsrcs_dt_to_pdata(struct i2c_msm_ctrl *ctrl,
					struct platform_device *pdev)
{
	struct i2c_msm_dt_to_pdata_map map[] = {
	{"i2c",				&pdev->id,	DT_REQ,  DT_ID,  -1},
	{"qcom,clk-freq-out",		&ctrl->rsrcs.clk_freq_out,
							DT_REQ,  DT_U32,  0},
	{"qcom,clk-freq-in",		&ctrl->rsrcs.clk_freq_in,
							DT_REQ,  DT_U32,  0},
	{"qcom,bam-pipe-idx-cons",	&(ctrl->rsrcs.bam_pipe_idx_cons),
							DT_OPT,  DT_U32,  0},
	{"qcom,bam-pipe-idx-prod",	&(ctrl->rsrcs.bam_pipe_idx_prod),
							DT_OPT,  DT_U32,  0},
	{"qcom,bam-disable",		&(ctrl->rsrcs.disable_dma),
							DT_OPT,  DT_BOOL, 0},
	{"qcom,master-id",		&(ctrl->rsrcs.clk_path_vote.mstr_id),
							DT_SGST, DT_U32,  0},
	{"qcom,clk-ctl-xfer",		 &(ctrl->rsrcs.clk_ctl_xfer),
							DT_OPT,  DT_BOOL, 0},
	{"qcom,noise-rjct-scl",		&(ctrl->noise_rjct_scl),
							DT_OPT,  DT_U32,  0},
	{"qcom,noise-rjct-sda",		&(ctrl->noise_rjct_sda),
							DT_OPT,  DT_U32,  0},
	{"qcom,sda-gpio",		&(ctrl->sda_gpio),
							DT_OPT,  DT_GPIO,  0},
	{"qcom,scl-gpio",		&(ctrl->scl_gpio),
							DT_OPT,  DT_GPIO,  0},
	{NULL,  NULL,					0,       0,       0},
	};
	return i2c_msm_dt_to_pdata_populate(ctrl, pdev, map);
}

static int i2c_msm_rsrcs_mem_init(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	struct resource *mem_region;

	ctrl->rsrcs.mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qup_phys_addr");
	if (!ctrl->rsrcs.mem) {
		dev_err(ctrl->dev, "error Missing 'qup_phys_addr' resource\n");
		return -ENODEV;
	}

	mem_region = request_mem_region(ctrl->rsrcs.mem->start,
					resource_size(ctrl->rsrcs.mem),
					pdev->name);
	if (!mem_region) {
		dev_err(ctrl->dev,
			"QUP physical memory region already claimed\n");
		return -EBUSY;
	}

	ctrl->rsrcs.base = devm_ioremap(ctrl->dev, ctrl->rsrcs.mem->start,
				   resource_size(ctrl->rsrcs.mem));
	if (!ctrl->rsrcs.base) {
		dev_err(ctrl->dev,
			"error failed ioremap(base:0x%llx size:0x%llx\n)",
			(u64) ctrl->rsrcs.mem->start,
			(u64) resource_size(ctrl->rsrcs.mem));
		release_mem_region(ctrl->rsrcs.mem->start,
						resource_size(ctrl->rsrcs.mem));
		return -ENOMEM;
	}

	return 0;
}

static void i2c_msm_rsrcs_mem_teardown(struct i2c_msm_ctrl *ctrl)
{
	release_mem_region(ctrl->rsrcs.mem->start,
						resource_size(ctrl->rsrcs.mem));
}

static int i2c_msm_rsrcs_irq_init(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	int ret, irq;

	irq = platform_get_irq_byname(pdev, "qup_irq");
	if (irq < 0) {
		dev_err(ctrl->dev, "error reading irq resource\n");
		return irq;
	}

	ret = request_irq(irq, i2c_msm_qup_isr, IRQF_TRIGGER_HIGH,
						"i2c-msm-v2-irq", ctrl);
	if (ret) {
		dev_err(ctrl->dev, "error request_irq(irq_num:%d ) ret:%d\n",
								irq, ret);
		return ret;
	}

	disable_irq(irq);
	ctrl->rsrcs.irq = irq;
	return 0;
}

static void i2c_msm_rsrcs_irq_teardown(struct i2c_msm_ctrl *ctrl)
{
	free_irq(ctrl->rsrcs.irq, ctrl->dev);
}


static struct pinctrl_state *
i2c_msm_rsrcs_gpio_get_state(struct i2c_msm_ctrl *ctrl, const char *name)
{
	struct pinctrl_state *pin_state
			= pinctrl_lookup_state(ctrl->rsrcs.pinctrl, name);

	if (IS_ERR_OR_NULL(pin_state))
		dev_info(ctrl->dev, "note pinctrl_lookup_state(%s) err:%ld\n",
						name, PTR_ERR(pin_state));
	return pin_state;
}

static int i2c_msm_rsrcs_gpio_pinctrl_init(struct i2c_msm_ctrl *ctrl)
{
	ctrl->rsrcs.pinctrl = devm_pinctrl_get(ctrl->dev);
	if (IS_ERR_OR_NULL(ctrl->rsrcs.pinctrl)) {
		dev_err(ctrl->dev, "error devm_pinctrl_get() failed err:%ld\n",
				PTR_ERR(ctrl->rsrcs.pinctrl));
		return PTR_ERR(ctrl->rsrcs.pinctrl);
	}

	ctrl->rsrcs.gpio_state_active =
		i2c_msm_rsrcs_gpio_get_state(ctrl, I2C_MSM_PINCTRL_ACTIVE);

	ctrl->rsrcs.gpio_state_suspend =
		i2c_msm_rsrcs_gpio_get_state(ctrl, I2C_MSM_PINCTRL_SUSPEND);

	return 0;
}

static void i2c_msm_pm_pinctrl_state(struct i2c_msm_ctrl *ctrl,
				bool runtime_active)
{
	struct pinctrl_state *pins_state;
	const char           *pins_state_name;

	if (runtime_active) {
		pins_state      = ctrl->rsrcs.gpio_state_active;
		pins_state_name = I2C_MSM_PINCTRL_ACTIVE;
	} else {
		pins_state      = ctrl->rsrcs.gpio_state_suspend;
		pins_state_name = I2C_MSM_PINCTRL_SUSPEND;
	}

	if (!IS_ERR_OR_NULL(pins_state)) {
		int ret = pinctrl_select_state(ctrl->rsrcs.pinctrl, pins_state);
		if (ret)
			dev_err(ctrl->dev,
			"error pinctrl_select_state(%s) err:%d\n",
			pins_state_name, ret);
	} else {
		dev_err(ctrl->dev,
			"error pinctrl state-name:'%s' is not configured\n",
			pins_state_name);
	}
}

static int i2c_msm_rsrcs_clk_init(struct i2c_msm_ctrl *ctrl)
{
	int ret = 0;

	if ((ctrl->rsrcs.clk_freq_out <= 0) ||
	    (ctrl->rsrcs.clk_freq_out > I2C_MSM_CLK_HIGH_MAX_FREQ)) {
		dev_err(ctrl->dev,
			"error clock frequency %dKHZ is not supported\n",
			(ctrl->rsrcs.clk_freq_out / 1000));
		return -EIO;
	}

	ctrl->rsrcs.core_clk = clk_get(ctrl->dev, "core_clk");
	if (IS_ERR(ctrl->rsrcs.core_clk)) {
		ret = PTR_ERR(ctrl->rsrcs.core_clk);
		dev_err(ctrl->dev, "error on clk_get(core_clk):%d\n", ret);
		return ret;
	}

	ret = clk_set_rate(ctrl->rsrcs.core_clk, ctrl->rsrcs.clk_freq_in);
	if (ret) {
		dev_err(ctrl->dev, "error on clk_set_rate(core_clk, %dKHz):%d\n",
					(ctrl->rsrcs.clk_freq_in / 1000), ret);
		goto err_set_rate;
	}

	ctrl->rsrcs.iface_clk = clk_get(ctrl->dev, "iface_clk");
	if (IS_ERR(ctrl->rsrcs.iface_clk)) {
		ret = PTR_ERR(ctrl->rsrcs.iface_clk);
		dev_err(ctrl->dev, "error on clk_get(iface_clk):%d\n", ret);
		goto err_set_rate;
	}

	return 0;

err_set_rate:
		clk_put(ctrl->rsrcs.core_clk);
		ctrl->rsrcs.core_clk = NULL;
	return ret;
}

static void i2c_msm_rsrcs_clk_teardown(struct i2c_msm_ctrl *ctrl)
{
	clk_put(ctrl->rsrcs.core_clk);
	clk_put(ctrl->rsrcs.iface_clk);
	i2c_msm_clk_path_teardown(ctrl);
}

#ifdef CONFIG_DEBUG_FS
static int i2c_msm_dbgfs_noise_scl_read(void *data, u64 *val)
{
	struct i2c_msm_ctrl *ctrl = data;
	*val = ctrl->noise_rjct_scl;
	return 0;
}

static int i2c_msm_dbgfs_noise_scl_write(void *data, u64 val)
{
	struct i2c_msm_ctrl *ctrl = data;

	if (val < 0 || val > 3) {
		dev_err(ctrl->dev,
			"error dbgfs attempt to set invalid value for noise"
			" reject. Should be 0..3\n");
		return -EINVAL;
	}

	ctrl->noise_rjct_scl = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_noise_scl_fops,
			i2c_msm_dbgfs_noise_scl_read,
			i2c_msm_dbgfs_noise_scl_write,
			"0x%llx");

static int i2c_msm_dbgfs_noise_sda_read(void *data, u64 *val)
{
	struct i2c_msm_ctrl *ctrl = data;
	*val = ctrl->noise_rjct_sda;
	return 0;
}

static int i2c_msm_dbgfs_noise_sda_write(void *data, u64 val)
{
	struct i2c_msm_ctrl *ctrl = data;

	if (val < 0 || val > 3) {
		dev_err(ctrl->dev,
			"error dbgfs attempt to set invalid value for noise"
			" reject. Should be 0..3\n");
		return -EINVAL;
	}

	ctrl->noise_rjct_sda = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_noise_sda_fops,
			i2c_msm_dbgfs_noise_sda_read,
			i2c_msm_dbgfs_noise_sda_write,
			"0x%llx");

static int i2c_msm_dbgfs_reset(void *data, u64 val)
{
	struct i2c_msm_ctrl *ctrl = data;
	(ctrl->ver.teardown)(ctrl);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_reset_fops,
			NULL,
			i2c_msm_dbgfs_reset,
			"0x%llx");

static int i2c_msm_dbgfs_reg_dump(void *data, u64 val)
{
	struct i2c_msm_ctrl *ctrl = data;
	i2c_msm_dbg_qup_reg_dump(ctrl);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_reg_dump_fops,
			NULL,
			i2c_msm_dbgfs_reg_dump,
			"0x%llx");

static int i2c_msm_dbgfs_do_bus_clear(void *data, u64 val)
{
	struct i2c_msm_ctrl *ctrl = data;
	i2c_msm_qup_do_bus_clear(ctrl);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_do_bus_clear_fops,
			NULL,
			i2c_msm_dbgfs_do_bus_clear,
			"0x%llx");

static int i2c_msm_dbgfs_scl_gpio_read(void *data, u64 *val)
{
	struct i2c_msm_ctrl *ctrl = data;
	*val = (ctrl->scl_gpio > 0) ? gpio_get_value(ctrl->scl_gpio) : 1;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_scl_gpio_fops,
			i2c_msm_dbgfs_scl_gpio_read,
			NULL,
			"0x%llx");

static int i2c_msm_dbgfs_sda_gpio_read(void *data, u64 *val)
{
	struct i2c_msm_ctrl *ctrl = data;
	*val = (ctrl->sda_gpio > 0) ? gpio_get_value(ctrl->sda_gpio) : 1;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_msm_dbgfs_sda_gpio_fops,
			i2c_msm_dbgfs_sda_gpio_read,
			NULL,
			"0x%llx");

static const umode_t I2C_MSM_DFS_MD_R  = S_IRUSR | S_IRGRP;
static const umode_t I2C_MSM_DFS_MD_W    = S_IWUSR | S_IWGRP;
static const umode_t I2C_MSM_DFS_MD_RW = S_IRUSR | S_IRGRP |
					   S_IWUSR | S_IWGRP;

enum i2c_msm_dbgfs_file_type {
	I2C_MSM_DFS_U8,
	I2C_MSM_DFS_U32,
	I2C_MSM_DFS_FILE,
};
struct i2c_msm_dbgfs_file {
	const char                   *name;
	const umode_t                 mode;
	enum i2c_msm_dbgfs_file_type  type;
	const struct file_operations *fops;
	u32                          *value_ptr;
};

static void i2c_msm_dbgfs_create(struct i2c_msm_ctrl *ctrl,
				struct i2c_msm_dbgfs_file *itr)
{
	struct dentry *file;

	ctrl->dbgfs.root = debugfs_create_dir(dev_name(ctrl->dev), NULL);
	if (!ctrl->dbgfs.root) {
		dev_err(ctrl->dev, "error on creating debugfs root\n");
		return;
	}

	for ( ; itr->name ; ++itr) {
		switch (itr->type) {
		case I2C_MSM_DFS_FILE:
			file = debugfs_create_file(itr->name,
						   itr->mode,
						   ctrl->dbgfs.root,
						   ctrl, itr->fops);
			break;
		case I2C_MSM_DFS_U8:
			file = debugfs_create_u8(itr->name,
						 itr->mode,
						 ctrl->dbgfs.root,
						 (u8 *) itr->value_ptr);
			break;
		default: 
			file = debugfs_create_u32(itr->name,
						 itr->mode,
						 ctrl->dbgfs.root,
						 (u32 *) itr->value_ptr);
			break;
		}

		if (!file)
			dev_err(ctrl->dev,
				"error on creating debugfs entry:%s\n",
				itr->name);
	}
}

static void i2c_msm_dbgfs_init(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_dbgfs_file i2c_msm_dbgfs_map[] = {
		{"dbg-lvl",         I2C_MSM_DFS_MD_RW, I2C_MSM_DFS_U8,
				NULL, &ctrl->dbgfs.dbg_lvl},
		{"xfer-force-mode", I2C_MSM_DFS_MD_RW, I2C_MSM_DFS_U8,
				NULL, &ctrl->dbgfs.force_xfer_mode},
		{"noise-rjct-scl",  I2C_MSM_DFS_MD_RW, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_noise_scl_fops,     NULL},
		{"noise-rjct-sda",  I2C_MSM_DFS_MD_RW, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_noise_sda_fops,     NULL},
		{"reset",           I2C_MSM_DFS_MD_W, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_reset_fops,         NULL},
		{"dump-regs",       I2C_MSM_DFS_MD_W, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_reg_dump_fops,      NULL},
		{"bus-clear",       I2C_MSM_DFS_MD_W, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_do_bus_clear_fops,  NULL},
		{"freq-out-hz",     I2C_MSM_DFS_MD_RW, I2C_MSM_DFS_U32,
				NULL, &ctrl->rsrcs.clk_freq_out},
		{"scl-gpio",     I2C_MSM_DFS_MD_R, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_scl_gpio_fops, NULL},
		{"sda-gpio",     I2C_MSM_DFS_MD_R, I2C_MSM_DFS_FILE,
				&i2c_msm_dbgfs_sda_gpio_fops, NULL},
		{NULL, 0, 0, NULL , NULL}, 
	};
	return i2c_msm_dbgfs_create(ctrl, i2c_msm_dbgfs_map);
}

static void i2c_msm_dbgfs_teardown(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->dbgfs.root)
		debugfs_remove_recursive(ctrl->dbgfs.root);
}
#else
static void i2c_msm_dbgfs_init(struct i2c_msm_ctrl *ctrl) {}
static void i2c_msm_dbgfs_teardown(struct i2c_msm_ctrl *ctrl) {}
#endif

static void i2c_msm_pm_suspend_clk(struct i2c_msm_ctrl *ctrl)
{
	clk_disable_unprepare(ctrl->rsrcs.core_clk);
	clk_disable_unprepare(ctrl->rsrcs.iface_clk);
}

static void i2c_msm_pm_clk_unvote(struct i2c_msm_ctrl *ctrl)
{
	if (!ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_suspend_clk(ctrl);

	i2c_msm_clk_path_unvote(ctrl);
}

static void i2c_msm_pm_resume_clk(struct i2c_msm_ctrl *ctrl)
{
	int ret;

	ret = clk_prepare_enable(ctrl->rsrcs.iface_clk);
	if (ret) {
		dev_err(ctrl->dev,
			"error on clk_prepare_enable(iface_clk):%d\n", ret);
		return;
	}

	ret = clk_prepare_enable(ctrl->rsrcs.core_clk);
	if (ret)
		dev_err(ctrl->dev,
			"error clk_prepare_enable(core_clk):%d\n", ret);
}

static void i2c_msm_pm_clk_vote(struct i2c_msm_ctrl *ctrl)
{
	i2c_msm_clk_path_init(ctrl);
	i2c_msm_clk_path_vote(ctrl);

	if (!ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_resume_clk(ctrl);
}

static int i2c_msm_pm_suspend_impl(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	if (ctrl->pwr_state == MSM_I2C_PM_SUSPENDED) {
		dev_err(ctrl->dev, "attempt to suspend when suspended\n");
		return 0;
	}
	i2c_msm_dbg(ctrl, MSM_DBG, "suspending...");

	disable_irq(ctrl->rsrcs.irq);
	i2c_msm_pm_clk_unvote(ctrl);
	i2c_msm_pm_pinctrl_state(ctrl, false);
	return 0;
}

static int  i2c_msm_pm_resume_impl(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	if (ctrl->pwr_state == MSM_I2C_PM_ACTIVE)
		return 0;

	i2c_msm_dbg(ctrl, MSM_DBG, "resuming...");

	i2c_msm_pm_pinctrl_state(ctrl, true);
	i2c_msm_pm_clk_vote(ctrl);
	enable_irq(ctrl->rsrcs.irq);
	(*ctrl->ver.init)(ctrl);
	ctrl->pwr_state = MSM_I2C_PM_ACTIVE;
	atomic_set(&ctrl->is_ctrl_active, 1);
	return 0;
}

#ifdef CONFIG_PM
static int i2c_msm_pm_sys_suspend_noirq(struct device *dev)
{
	int ret = 0;
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);
	enum msm_i2c_power_state curr_state = ctrl->pwr_state;
	i2c_msm_dbg(ctrl, MSM_DBG, "pm_sys_noirq: suspending...");

	
	mutex_lock(&ctrl->mlock);
	ctrl->pwr_state = MSM_I2C_PM_SYS_SUSPENDED;
	mutex_unlock(&ctrl->mlock);
	atomic_set(&ctrl->is_ctrl_active, 0);

	if (curr_state == MSM_I2C_PM_ACTIVE) {
		ret = i2c_msm_pm_suspend_impl(dev);
		pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
		pm_runtime_enable(dev);
	}

	return ret;
}

static int i2c_msm_pm_sys_resume_noirq(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);
	i2c_msm_dbg(ctrl, MSM_DBG, "pm_sys_noirq: resuming...");
	ctrl->pwr_state = MSM_I2C_PM_SUSPENDED;
	atomic_set(&ctrl->is_ctrl_active, 0);
	return  0;
}
#endif

#ifdef CONFIG_PM_RUNTIME
static void i2c_msm_pm_rt_init(struct device *dev)
{
	pm_runtime_set_suspended(dev);
	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
}

static int i2c_msm_pm_rt_suspend(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);
	int ret = 0;

	i2c_msm_dbg(ctrl, MSM_DBG, "pm_runtime: suspending...");
	ret = i2c_msm_pm_suspend_impl(dev);
	ctrl->pwr_state = MSM_I2C_PM_SUSPENDED;
	atomic_set(&ctrl->is_ctrl_active, 0);
	return ret;
}

static int i2c_msm_pm_rt_resume(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	i2c_msm_dbg(ctrl, MSM_DBG, "pm_runtime: resuming...");
	return  i2c_msm_pm_resume_impl(dev);
}

static void i2c_msm_pm_resume_adptr(struct i2c_msm_ctrl *ctrl)
{
	pm_runtime_get_sync(ctrl->dev);
	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_resume_clk(ctrl);
}

static void i2c_msm_pm_suspend_adptr(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_suspend_clk(ctrl);
	pm_runtime_mark_last_busy(ctrl->dev);
	pm_runtime_put_autosuspend(ctrl->dev);
}
#else
static void i2c_msm_pm_rt_init(struct device *dev) {}
#define i2c_msm_pm_rt_suspend NULL
#define i2c_msm_pm_rt_resume NULL

static void i2c_msm_pm_resume_adptr(struct i2c_msm_ctrl *ctrl)
{
	i2c_msm_pm_resume_impl(ctrl->dev);
	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_resume_clk(ctrl);
}

static void i2c_msm_pm_suspend_adptr(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_suspend_clk(ctrl);
	i2c_msm_pm_suspend_impl(ctrl->dev);
	ctrl->pwr_state = MSM_I2C_PM_SUSPENDED;
	atomic_set(&ctrl->is_ctrl_active, 0);
}
#endif

static const struct dev_pm_ops i2c_msm_pm_ops = {
	.suspend_noirq		= i2c_msm_pm_sys_suspend_noirq,
	.resume_noirq		= i2c_msm_pm_sys_resume_noirq,
	.runtime_suspend	= i2c_msm_pm_rt_suspend,
	.runtime_resume		= i2c_msm_pm_rt_resume,
};

static u32 i2c_msm_frmwrk_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm i2c_msm_frmwrk_algrtm = {
	.master_xfer	= i2c_msm_frmwrk_xfer,
	.functionality	= i2c_msm_frmwrk_func,
};

static const char const *i2c_msm_adapter_name = "MSM-I2C-v2-adapter";

static int i2c_msm_frmwrk_reg(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	int ret;

	i2c_set_adapdata(&ctrl->adapter, ctrl);
	ctrl->adapter.algo = &i2c_msm_frmwrk_algrtm;
	strlcpy(ctrl->adapter.name, i2c_msm_adapter_name,
						sizeof(ctrl->adapter.name));

	ctrl->adapter.nr = pdev->id;
	ctrl->adapter.dev.parent = &pdev->dev;
	ret = i2c_add_numbered_adapter(&ctrl->adapter);
	if (ret) {
		dev_err(ctrl->dev, "error i2c_add_adapter failed\n");
		return ret;
	}

	if (ctrl->dev->of_node) {
		ctrl->adapter.dev.of_node = pdev->dev.of_node;
		of_i2c_register_devices(&ctrl->adapter);
	}

	return ret;
}

static void i2c_msm_frmwrk_unreg(struct i2c_msm_ctrl *ctrl)
{
	i2c_del_adapter(&ctrl->adapter);
}

static int i2c_msm_probe(struct platform_device *pdev)
{
	struct i2c_msm_ctrl             *ctrl;
	int ret = 0;

	dev_info(&pdev->dev, "probing driver i2c-msm-v2\n");

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dev = &pdev->dev;
	platform_set_drvdata(pdev, ctrl);
	ctrl->dbgfs.dbg_lvl         = DEFAULT_DBG_LVL;
	ctrl->dbgfs.force_xfer_mode = I2C_MSM_XFER_MODE_NONE;
	mutex_init(&ctrl->mlock);
	ctrl->pwr_state = MSM_I2C_PM_SUSPENDED;
	atomic_set(&ctrl->is_ctrl_active, 0);

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "error: null device-tree node");
		return -EBADE;
	}

	ret = i2c_msm_rsrcs_dt_to_pdata(ctrl, pdev);
	if (ret)
		return ret;

	ret = i2c_msm_rsrcs_mem_init(pdev, ctrl);
	if (ret)
		goto mem_err;

	ret = i2c_msm_rsrcs_clk_init(ctrl);
	if (ret)
		goto clk_err;

	
	i2c_msm_pm_clk_vote(ctrl);
	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_resume_clk(ctrl);
	ret = i2c_msm_ctrl_ver_detect_and_set(ctrl);
	if (ret) {
		if (ctrl->rsrcs.clk_ctl_xfer)
			i2c_msm_pm_suspend_clk(ctrl);
		i2c_msm_pm_clk_unvote(ctrl);
		goto ver_err;
	}

	ret = (*ctrl->ver.reset)(ctrl);
	if (ret)
		dev_err(ctrl->dev, "error error on qup software reset\n");

	if (ctrl->rsrcs.clk_ctl_xfer)
		i2c_msm_pm_suspend_clk(ctrl);
	i2c_msm_pm_clk_unvote(ctrl);

	ret = i2c_msm_rsrcs_gpio_pinctrl_init(ctrl);
	if (ret)
		goto err_no_pinctrl;

	i2c_msm_pm_rt_init(ctrl->dev);

	
	ret = (*ctrl->ver.create)(ctrl);
	if (ret)
		goto ver_err;

	ret = i2c_msm_rsrcs_irq_init(pdev, ctrl);
	if (ret)
		goto irq_err;

	ret = (*ctrl->ver.init_rsrcs)(pdev, ctrl);
	if (ret)
		goto rcrcs_err;

	i2c_msm_dbgfs_init(ctrl);

	ret = i2c_msm_frmwrk_reg(pdev, ctrl);
	if (ret)
		goto reg_err;

	if ((ctrl->adapter.nr < 7) && (ctrl->adapter.nr >= 0))
		error_times[ctrl->adapter.nr] = 0;

	i2c_msm_dbg(ctrl, MSM_PROF, "probe() completed with success");
	return 0;

reg_err:
	i2c_msm_dbgfs_teardown(ctrl);
rcrcs_err:
	i2c_msm_rsrcs_irq_teardown(ctrl);
irq_err:
	(*ctrl->ver.destroy)(ctrl);
err_no_pinctrl:
ver_err:
	i2c_msm_rsrcs_clk_teardown(ctrl);
clk_err:
	i2c_msm_rsrcs_mem_teardown(ctrl);
mem_err:
	dev_err(ctrl->dev, "error probe() failed with err:%d\n", ret);
	devm_kfree(&pdev->dev, ctrl);
	return ret;
}

static int i2c_msm_remove(struct platform_device *pdev)
{
	struct i2c_msm_ctrl *ctrl = platform_get_drvdata(pdev);

	
	mutex_lock(&ctrl->mlock);
	ctrl->pwr_state = MSM_I2C_PM_SYS_SUSPENDED;
	mutex_unlock(&ctrl->mlock);
	atomic_set(&ctrl->is_ctrl_active, 0);

	i2c_msm_pm_suspend_impl(ctrl->dev);
	mutex_destroy(&ctrl->mlock);

	i2c_msm_frmwrk_unreg(ctrl);
	(*ctrl->ver.teardown)(ctrl);
	i2c_msm_dbgfs_teardown(ctrl);
	i2c_msm_rsrcs_irq_teardown(ctrl);
	i2c_msm_rsrcs_clk_teardown(ctrl);
	i2c_msm_rsrcs_mem_teardown(ctrl);
	(*ctrl->ver.destroy)(ctrl);
	return 0;
}

static struct of_device_id i2c_msm_dt_match[] = {
	{
		.compatible = "qcom,i2c-msm-v2",
	},
	{}
};

static struct platform_driver i2c_msm_driver = {
	.probe  = i2c_msm_probe,
	.remove = i2c_msm_remove,
	.driver = {
		.name           = "i2c-msm-v2",
		.owner          = THIS_MODULE,
		.pm             = &i2c_msm_pm_ops,
		.of_match_table = i2c_msm_dt_match,
	},
};

static int i2c_msm_init(void)
{
	return platform_driver_register(&i2c_msm_driver);
}
arch_initcall(i2c_msm_init);

static void i2c_msm_exit(void)
{
	platform_driver_unregister(&i2c_msm_driver);
}
module_exit(i2c_msm_exit);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-msm-v2");

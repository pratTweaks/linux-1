// SPDX-License-Identifier: GPL-2.0
//
// Mediatek SPI NOR controller driver
//
// Copyright (C) 2020 Chuanhong Guo <gch981213@gmail.com>

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/string.h>

#define DRIVER_NAME "mtk-spi-nor"

#define MTK_NOR_REG_CMD			0x00
#define MTK_NOR_CMD_WRITE		BIT(4)
#define MTK_NOR_CMD_PROGRAM		BIT(2)
#define MTK_NOR_CMD_READ		BIT(0)
#define MTK_NOR_CMD_MASK		GENMASK(5, 0)

#define MTK_NOR_REG_PRG_CNT		0x04
#define MTK_NOR_REG_RDATA		0x0c

#define MTK_NOR_REG_RADR0		0x10
#define MTK_NOR_REG_RADR(n)		(MTK_NOR_REG_RADR0 + 4 * (n))
#define MTK_NOR_REG_RADR3		0xc8

#define MTK_NOR_REG_WDATA		0x1c

#define MTK_NOR_REG_PRGDATA0		0x20
#define MTK_NOR_REG_PRGDATA(n)		(MTK_NOR_REG_PRGDATA0 + 4 * (n))
#define MTK_NOR_REG_PRGDATA_MAX		5

#define MTK_NOR_REG_SHIFT0		0x38
#define MTK_NOR_REG_SHIFT(n)		(MTK_NOR_REG_SHIFT0 + 4 * (n))
#define MTK_NOR_REG_SHIFT_MAX		9

#define MTK_NOR_REG_CFG1		0x60
#define MTK_NOR_FAST_READ		BIT(0)

#define MTK_NOR_REG_CFG2		0x64
#define MTK_NOR_WR_CUSTOM_OP_EN		BIT(4)
#define MTK_NOR_WR_BUF_EN		BIT(0)

#define MTK_NOR_REG_PP_DATA		0x98

#define MTK_NOR_REG_IRQ_STAT		0xa8
#define MTK_NOR_REG_IRQ_EN		0xac
#define MTK_NOR_IRQ_DMA			BIT(7)
#define MTK_NOR_IRQ_MASK		GENMASK(7, 0)

#define MTK_NOR_REG_CFG3		0xb4
#define MTK_NOR_DISABLE_WREN		BIT(7)
#define MTK_NOR_DISABLE_SR_POLL		BIT(5)

#define MTK_NOR_REG_WP			0xc4
#define MTK_NOR_ENABLE_SF_CMD		0x30

#define MTK_NOR_REG_BUSCFG		0xcc
#define MTK_NOR_4B_ADDR			BIT(4)
#define MTK_NOR_QUAD_ADDR		BIT(3)
#define MTK_NOR_QUAD_READ		BIT(2)
#define MTK_NOR_DUAL_ADDR		BIT(1)
#define MTK_NOR_DUAL_READ		BIT(0)
#define MTK_NOR_BUS_MODE_MASK		GENMASK(4, 0)

#define MTK_NOR_REG_DMA_CTL		0x718
#define MTK_NOR_DMA_START		BIT(0)

#define MTK_NOR_REG_DMA_FADR		0x71c
#define MTK_NOR_REG_DMA_DADR		0x720
#define MTK_NOR_REG_DMA_END_DADR	0x724

#define MTK_NOR_PRG_MAX_SIZE		6
// Reading DMA src/dst addresses have to be 16-byte aligned
#define MTK_NOR_DMA_ALIGN		16
#define MTK_NOR_DMA_ALIGN_MASK		(MTK_NOR_DMA_ALIGN - 1)
// and we allocate a bounce buffer if destination address isn't aligned.
#define MTK_NOR_BOUNCE_BUF_SIZE		PAGE_SIZE

// Buffered page program can do one 128-byte transfer
#define MTK_NOR_PP_SIZE			128

#define CLK_TO_US(sp, clkcnt)		DIV_ROUND_UP(clkcnt, sp->spi_freq / 1000000)

struct mtk_nor {
	struct spi_controller *ctlr;
	struct device *dev;
	void __iomem *base;
	u8 *buffer;
	struct clk *spi_clk;
	struct clk *ctlr_clk;
	unsigned int spi_freq;
	bool wbuf_en;
	bool has_irq;
	struct completion op_done;
};

static inline void mtk_nor_rmw(struct mtk_nor *sp, u32 reg, u32 set, u32 clr)
{
	u32 val = readl(sp->base + reg);

	val &= ~clr;
	val |= set;
	writel(val, sp->base + reg);
}

static inline int mtk_nor_cmd_exec(struct mtk_nor *sp, u32 cmd, ulong clk)
{
	ulong delay = CLK_TO_US(sp, clk);
	u32 reg;
	int ret;

	writel(cmd, sp->base + MTK_NOR_REG_CMD);
	ret = readl_poll_timeout(sp->base + MTK_NOR_REG_CMD, reg, !(reg & cmd),
				 delay / 3, (delay + 1) * 200);
	if (ret < 0)
		dev_err(sp->dev, "command %u timeout.\n", cmd);
	return ret;
}

static void mtk_nor_set_addr(struct mtk_nor *sp, const struct spi_mem_op *op)
{
	u32 addr = op->addr.val;
	int i;

	for (i = 0; i < 3; i++) {
		writeb(addr & 0xff, sp->base + MTK_NOR_REG_RADR(i));
		addr >>= 8;
	}
	if (op->addr.nbytes == 4) {
		writeb(addr & 0xff, sp->base + MTK_NOR_REG_RADR3);
		mtk_nor_rmw(sp, MTK_NOR_REG_BUSCFG, MTK_NOR_4B_ADDR, 0);
	} else {
		mtk_nor_rmw(sp, MTK_NOR_REG_BUSCFG, 0, MTK_NOR_4B_ADDR);
	}
}

static bool mtk_nor_match_read(const struct spi_mem_op *op)
{
	int dummy = 0;

	if (op->dummy.buswidth)
		dummy = op->dummy.nbytes * BITS_PER_BYTE / op->dummy.buswidth;

	if ((op->data.buswidth == 2) || (op->data.buswidth == 4)) {
		if (op->addr.buswidth == 1)
			return dummy == 8;
		else if (op->addr.buswidth == 2)
			return dummy == 4;
		else if (op->addr.buswidth == 4)
			return dummy == 6;
	} else if ((op->addr.buswidth == 1) && (op->data.buswidth == 1)) {
		if (op->cmd.opcode == 0x03)
			return dummy == 0;
		else if (op->cmd.opcode == 0x0b)
			return dummy == 8;
	}
	return false;
}

static int mtk_nor_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	size_t len;

	if (!op->data.nbytes)
		return 0;

	if ((op->addr.nbytes == 3) || (op->addr.nbytes == 4)) {
		if ((op->data.dir == SPI_MEM_DATA_IN) &&
		    mtk_nor_match_read(op)) {
			// limit size to prevent timeout calculation overflow
			if (op->data.nbytes > 0x400000)
				op->data.nbytes = 0x400000;

			if ((op->addr.val & MTK_NOR_DMA_ALIGN_MASK) ||
			    (op->data.nbytes < MTK_NOR_DMA_ALIGN))
				op->data.nbytes = 1;
			else if (!((ulong)(op->data.buf.in) &
				   MTK_NOR_DMA_ALIGN_MASK))
				op->data.nbytes &= ~MTK_NOR_DMA_ALIGN_MASK;
			else if (op->data.nbytes > MTK_NOR_BOUNCE_BUF_SIZE)
				op->data.nbytes = MTK_NOR_BOUNCE_BUF_SIZE;
			return 0;
		} else if (op->data.dir == SPI_MEM_DATA_OUT) {
			if (op->data.nbytes >= MTK_NOR_PP_SIZE)
				op->data.nbytes = MTK_NOR_PP_SIZE;
			else
				op->data.nbytes = 1;
			return 0;
		}
	}

	len = MTK_NOR_PRG_MAX_SIZE - op->cmd.nbytes - op->addr.nbytes -
	      op->dummy.nbytes;
	if (op->data.nbytes > len)
		op->data.nbytes = len;

	return 0;
}

static bool mtk_nor_supports_op(struct spi_mem *mem,
				const struct spi_mem_op *op)
{
	size_t len;

	if (op->cmd.buswidth != 1)
		return false;

	if ((op->addr.nbytes == 3) || (op->addr.nbytes == 4)) {
		switch(op->data.dir) {
		case SPI_MEM_DATA_IN:
			if (!mtk_nor_match_read(op))
				return false;
			break;
		case SPI_MEM_DATA_OUT:
			if ((op->addr.buswidth != 1) ||
			    (op->dummy.nbytes != 0) ||
			    (op->data.buswidth != 1))
				return false;
			break;
		default:
			break;
		}
	}
	len = op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes;
	if ((len > MTK_NOR_PRG_MAX_SIZE) ||
	    ((op->data.nbytes) && (len == MTK_NOR_PRG_MAX_SIZE)))
		return false;

	return spi_mem_default_supports_op(mem, op);
}

static void mtk_nor_setup_bus(struct mtk_nor *sp, const struct spi_mem_op *op)
{
	u32 reg = 0;

	if (op->addr.nbytes == 4)
		reg |= MTK_NOR_4B_ADDR;

	if (op->data.buswidth == 4) {
		reg |= MTK_NOR_QUAD_READ;
		writeb(op->cmd.opcode, sp->base + MTK_NOR_REG_PRGDATA(4));
		if (op->addr.buswidth == 4)
			reg |= MTK_NOR_QUAD_ADDR;
	} else if (op->data.buswidth == 2) {
		reg |= MTK_NOR_DUAL_READ;
		writeb(op->cmd.opcode, sp->base + MTK_NOR_REG_PRGDATA(3));
		if (op->addr.buswidth == 2)
			reg |= MTK_NOR_DUAL_ADDR;
	} else {
		if (op->cmd.opcode == 0x0b)
			mtk_nor_rmw(sp, MTK_NOR_REG_CFG1, MTK_NOR_FAST_READ, 0);
		else
			mtk_nor_rmw(sp, MTK_NOR_REG_CFG1, 0, MTK_NOR_FAST_READ);
	}
	mtk_nor_rmw(sp, MTK_NOR_REG_BUSCFG, reg, MTK_NOR_BUS_MODE_MASK);
}

static int mtk_nor_read_dma(struct mtk_nor *sp, u32 from, unsigned int length,
			    u8 *buffer)
{
	int ret = 0;
	ulong delay;
	u32 reg;
	dma_addr_t dma_addr;

	dma_addr = dma_map_single(sp->dev, buffer, length, DMA_FROM_DEVICE);
	if (dma_mapping_error(sp->dev, dma_addr)) {
		dev_err(sp->dev, "failed to map dma buffer.\n");
		return -EINVAL;
	}

	writel(from, sp->base + MTK_NOR_REG_DMA_FADR);
	writel(dma_addr, sp->base + MTK_NOR_REG_DMA_DADR);
	writel(dma_addr + length, sp->base + MTK_NOR_REG_DMA_END_DADR);

	if (sp->has_irq) {
		reinit_completion(&sp->op_done);
		mtk_nor_rmw(sp, MTK_NOR_REG_IRQ_EN, MTK_NOR_IRQ_DMA, 0);
	}

	mtk_nor_rmw(sp, MTK_NOR_REG_DMA_CTL, MTK_NOR_DMA_START, 0);

	delay = CLK_TO_US(sp, (length + 5) * BITS_PER_BYTE);

	if (sp->has_irq) {
		if (!wait_for_completion_timeout(&sp->op_done,
						 (delay + 1) * 100))
			ret = -ETIMEDOUT;
	} else {
		ret = readl_poll_timeout(sp->base + MTK_NOR_REG_DMA_CTL, reg,
					 !(reg & MTK_NOR_DMA_START), delay / 3,
					 (delay + 1) * 100);
	}

	dma_unmap_single(sp->dev, dma_addr, length, DMA_FROM_DEVICE);
	if (ret < 0)
		dev_err(sp->dev, "dma read timeout.\n");

	return ret;
}

static int mtk_nor_read_bounce(struct mtk_nor *sp, u32 from,
			       unsigned int length, u8 *buffer)
{
	unsigned int rdlen;
	int ret;

	if (length & MTK_NOR_DMA_ALIGN_MASK)
		rdlen = (length + MTK_NOR_DMA_ALIGN) & ~MTK_NOR_DMA_ALIGN_MASK;
	else
		rdlen = length;

	ret = mtk_nor_read_dma(sp, from, rdlen, sp->buffer);
	if (ret)
		return ret;

	memcpy(buffer, sp->buffer, length);
	return 0;
}

static int mtk_nor_read_pio(struct mtk_nor *sp, const struct spi_mem_op *op)
{
	u8 *buf = op->data.buf.in;
	int ret;

	ret = mtk_nor_cmd_exec(sp, MTK_NOR_CMD_READ, 6 * BITS_PER_BYTE);
	if (!ret)
		buf[0] = readb(sp->base + MTK_NOR_REG_RDATA);
	return ret;
}

static int mtk_nor_write_buffer_enable(struct mtk_nor *sp)
{
	int ret;
	u32 val;

	if (sp->wbuf_en)
		return 0;

	val = readl(sp->base + MTK_NOR_REG_CFG2);
	writel(val | MTK_NOR_WR_BUF_EN, sp->base + MTK_NOR_REG_CFG2);
	ret = readl_poll_timeout(sp->base + MTK_NOR_REG_CFG2, val,
				 val & MTK_NOR_WR_BUF_EN, 0, 10000);
	if (!ret)
		sp->wbuf_en = true;
	return ret;
}

static int mtk_nor_write_buffer_disable(struct mtk_nor *sp)
{
	int ret;
	u32 val;

	if (!sp->wbuf_en)
		return 0;
	val = readl(sp->base + MTK_NOR_REG_CFG2);
	writel(val & ~MTK_NOR_WR_BUF_EN, sp->base + MTK_NOR_REG_CFG2);
	ret = readl_poll_timeout(sp->base + MTK_NOR_REG_CFG2, val,
				 !(val & MTK_NOR_WR_BUF_EN), 0, 10000);
	if (!ret)
		sp->wbuf_en = false;
	return ret;
}

static int mtk_nor_pp_buffered(struct mtk_nor *sp, const struct spi_mem_op *op)
{
	const u8 *buf = op->data.buf.out;
	u32 val;
	int ret, i;

	ret = mtk_nor_write_buffer_enable(sp);
	if (ret < 0)
		return ret;

	for (i = 0; i < op->data.nbytes; i += 4) {
		val = buf[i + 3] << 24 | buf[i + 2] << 16 | buf[i + 1] << 8 |
		      buf[i];
		writel(val, sp->base + MTK_NOR_REG_PP_DATA);
	}
	return mtk_nor_cmd_exec(sp, MTK_NOR_CMD_WRITE,
				(op->data.nbytes + 5) * BITS_PER_BYTE);
}

static int mtk_nor_pp_unbuffered(struct mtk_nor *sp,
				 const struct spi_mem_op *op)
{
	const u8 *buf = op->data.buf.out;
	int ret;

	ret = mtk_nor_write_buffer_disable(sp);
	if (ret < 0)
		return ret;
	writeb(buf[0], sp->base + MTK_NOR_REG_WDATA);
	return mtk_nor_cmd_exec(sp, MTK_NOR_CMD_WRITE, 6 * BITS_PER_BYTE);
}

static int mtk_nor_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct mtk_nor *sp = spi_controller_get_devdata(mem->spi->master);
	int ret;

	if ((op->data.nbytes == 0) ||
	    ((op->addr.nbytes != 3) && (op->addr.nbytes != 4)))
		return -ENOTSUPP;

	if (op->data.dir == SPI_MEM_DATA_OUT) {
		mtk_nor_set_addr(sp, op);
		writeb(op->cmd.opcode, sp->base + MTK_NOR_REG_PRGDATA0);
		if (op->data.nbytes == MTK_NOR_PP_SIZE)
			return mtk_nor_pp_buffered(sp, op);
		return mtk_nor_pp_unbuffered(sp, op);
	}

	if ((op->data.dir == SPI_MEM_DATA_IN) && mtk_nor_match_read(op)) {
		ret = mtk_nor_write_buffer_disable(sp);
		if (ret < 0)
			return ret;
		mtk_nor_setup_bus(sp, op);
		if (op->data.nbytes == 1) {
			mtk_nor_set_addr(sp, op);
			return mtk_nor_read_pio(sp, op);
		} else if (((ulong)(op->data.buf.in) &
			    MTK_NOR_DMA_ALIGN_MASK)) {
			return mtk_nor_read_bounce(sp, op->addr.val,
						   op->data.nbytes,
						   op->data.buf.in);
		} else {
			return mtk_nor_read_dma(sp, op->addr.val,
						op->data.nbytes,
						op->data.buf.in);
		}
	}

	return -ENOTSUPP;
}

static int mtk_nor_setup(struct spi_device *spi)
{
	struct mtk_nor *sp = spi_controller_get_devdata(spi->master);

	if (spi->max_speed_hz && (spi->max_speed_hz < sp->spi_freq)) {
		dev_err(&spi->dev, "spi clock should be %u Hz.\n",
			sp->spi_freq);
		return -EINVAL;
	}
	spi->max_speed_hz = sp->spi_freq;

	return 0;
}

static int mtk_nor_transfer_one_message(struct spi_controller *master,
					struct spi_message *m)
{
	struct mtk_nor *sp = spi_controller_get_devdata(master);
	struct spi_transfer *t = NULL;
	unsigned long trx_len = 0;
	int stat = 0;
	int reg_offset = MTK_NOR_REG_PRGDATA_MAX;
	void __iomem *reg;
	const u8 *txbuf;
	u8 *rxbuf;
	int i;

	list_for_each_entry(t, &m->transfers, transfer_list) {
		txbuf = t->tx_buf;
		for (i = 0; i < t->len; i++, reg_offset--) {
			reg = sp->base + MTK_NOR_REG_PRGDATA(reg_offset);
			if (txbuf)
				writeb(txbuf[i], reg);
			else
				writeb(0, reg);
		}
		trx_len += t->len;
	}

	writel(trx_len * BITS_PER_BYTE, sp->base + MTK_NOR_REG_PRG_CNT);

	stat = mtk_nor_cmd_exec(sp, MTK_NOR_CMD_PROGRAM,
				trx_len * BITS_PER_BYTE);
	if (stat < 0)
		goto msg_done;

	reg_offset = trx_len - 1;
	list_for_each_entry(t, &m->transfers, transfer_list) {
		rxbuf = t->rx_buf;
		for (i = 0; i < t->len; i++, reg_offset--) {
			reg = sp->base + MTK_NOR_REG_SHIFT(reg_offset);
			if (rxbuf)
				rxbuf[i] = readb(reg);
		}
	}

	m->actual_length = trx_len;
msg_done:
	m->status = stat;
	spi_finalize_current_message(master);

	return 0;
}

static void mtk_nor_disable_clk(struct mtk_nor *sp)
{
	clk_disable_unprepare(sp->spi_clk);
	clk_disable_unprepare(sp->ctlr_clk);
}

static int mtk_nor_enable_clk(struct mtk_nor *sp)
{
	int ret;

	ret = clk_prepare_enable(sp->spi_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(sp->ctlr_clk);
	if (ret) {
		clk_disable_unprepare(sp->spi_clk);
		return ret;
	}

	return 0;
}

static int mtk_nor_init(struct mtk_nor *sp)
{
	int ret;

	ret = mtk_nor_enable_clk(sp);
	if (ret)
		return ret;

	sp->spi_freq = clk_get_rate(sp->spi_clk);

	writel(MTK_NOR_ENABLE_SF_CMD, sp->base + MTK_NOR_REG_WP);
	mtk_nor_rmw(sp, MTK_NOR_REG_CFG2, MTK_NOR_WR_CUSTOM_OP_EN, 0);
	mtk_nor_rmw(sp, MTK_NOR_REG_CFG3,
		    MTK_NOR_DISABLE_WREN | MTK_NOR_DISABLE_SR_POLL, 0);

	return ret;
}

static irqreturn_t mtk_nor_irq_handler(int irq, void *data)
{
	struct mtk_nor *sp = data;
	u32 irq_status, irq_enabled;

	irq_status = readl(sp->base + MTK_NOR_REG_IRQ_STAT);
	irq_enabled = readl(sp->base + MTK_NOR_REG_IRQ_EN);
	// write status back to clear interrupt
	writel(irq_status, sp->base + MTK_NOR_REG_IRQ_STAT);

	if (!(irq_status & irq_enabled))
		return IRQ_NONE;

	if (irq_status & MTK_NOR_IRQ_DMA) {
		complete(&sp->op_done);
		writel(0, sp->base + MTK_NOR_REG_IRQ_EN);
	}

	return IRQ_HANDLED;
}

static size_t mtk_max_msg_size(struct spi_device *spi)
{
	return MTK_NOR_PRG_MAX_SIZE;
}

static const struct spi_controller_mem_ops mtk_nor_mem_ops = {
	.adjust_op_size = mtk_nor_adjust_op_size,
	.supports_op = mtk_nor_supports_op,
	.exec_op = mtk_nor_exec_op
};

static const struct of_device_id mtk_nor_match[] = {
	{ .compatible = "mediatek,mt8173-nor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_nor_match);

static int mtk_nor_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct mtk_nor *sp;
	void __iomem *base;
	u8 *buffer;
	struct clk *spi_clk, *ctlr_clk;
	int ret, irq;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	spi_clk = devm_clk_get(&pdev->dev, "spi");
	if (IS_ERR(spi_clk))
		return PTR_ERR(spi_clk);

	ctlr_clk = devm_clk_get(&pdev->dev, "sf");
	if (IS_ERR(ctlr_clk))
		return PTR_ERR(ctlr_clk);

	buffer = devm_kmalloc(&pdev->dev,
			      MTK_NOR_BOUNCE_BUF_SIZE + MTK_NOR_DMA_ALIGN,
			      GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	if ((ulong)buffer & MTK_NOR_DMA_ALIGN_MASK)
		buffer = (u8 *)(((ulong)buffer + MTK_NOR_DMA_ALIGN) &
				~MTK_NOR_DMA_ALIGN_MASK);

	ctlr = spi_alloc_master(&pdev->dev, sizeof(*sp));
	if (!ctlr) {
		dev_err(&pdev->dev, "failed to allocate spi controller\n");
		return -ENOMEM;
	}

	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->max_message_size = mtk_max_msg_size;
	ctlr->mem_ops = &mtk_nor_mem_ops;
	ctlr->mode_bits = SPI_RX_DUAL | SPI_RX_QUAD | SPI_TX_DUAL | SPI_TX_QUAD;
	ctlr->num_chipselect = 1;
	ctlr->setup = mtk_nor_setup;
	ctlr->transfer_one_message = mtk_nor_transfer_one_message;

	dev_set_drvdata(&pdev->dev, ctlr);

	sp = spi_controller_get_devdata(ctlr);
	sp->base = base;
	sp->buffer = buffer;
	sp->has_irq = false;
	sp->wbuf_en = false;
	sp->ctlr = ctlr;
	sp->dev = &pdev->dev;
	sp->spi_clk = spi_clk;
	sp->ctlr_clk = ctlr_clk;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0) {
		dev_warn(sp->dev, "IRQ not available.");
	} else {
		writel(MTK_NOR_IRQ_MASK, base + MTK_NOR_REG_IRQ_STAT);
		writel(0, base + MTK_NOR_REG_IRQ_EN);
		ret = devm_request_irq(sp->dev, irq, mtk_nor_irq_handler, 0,
				       pdev->name, sp);
		if (ret < 0) {
			dev_warn(sp->dev, "failed to request IRQ.");
		} else {
			init_completion(&sp->op_done);
			sp->has_irq = true;
		}
	}

	ret = mtk_nor_init(sp);
	if (ret < 0) {
		kfree(ctlr);
		return ret;
	}

	dev_info(&pdev->dev, "spi frequency: %d Hz\n", sp->spi_freq);

	return devm_spi_register_controller(&pdev->dev, ctlr);
}

static int mtk_nor_remove(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct mtk_nor *sp;

	ctlr = dev_get_drvdata(&pdev->dev);
	sp = spi_controller_get_devdata(ctlr);

	mtk_nor_disable_clk(sp);

	return 0;
}

static struct platform_driver mtk_nor_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mtk_nor_match,
	},
	.probe = mtk_nor_probe,
	.remove = mtk_nor_remove,
};

module_platform_driver(mtk_nor_driver);

MODULE_DESCRIPTION("Mediatek SPI NOR controller driver");
MODULE_AUTHOR("Chuanhong Guo <gch981213@gmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);

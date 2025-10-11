// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cryptographic API.
 *
 * Copyright (c) 2017-present, Facebook, Inc.
 * the source is changed by oplus for out of tree module.
 * replace zstd -> zstdn n stands for newest
 */
#include <linux/crypto.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmstat.h>
#include <linux/sched/loadavg.h>
#include <crypto/internal/scompress.h>
#include <../drivers/block/zram/zcomp.h>

#define ZSTD_MIN_LEVEL        1    /* ZSTD 最低压缩等级 */
#define ZSTD_MAX_LEVEL        22   /* ZSTD 最高压缩等级 */

#define MEM_USAGE_THRESH      75   /* 内存使用率阈值：75% */
#define CPU_USAGE_THRESH      75   /* CPU使用率阈值：75% */

#define LOW_MEM_MIN_LEVEL     1    /* 内存压力低时最低压缩等级 */
#define LOW_MEM_MAX_LEVEL     3    /* 内存压力低时最高压缩等级 */

#define HIGH_MEM_MIN_LEVEL    4    /* 内存压力高时最低压缩等级 */
#define HIGH_MEM_MAX_LEVEL    9    /* 内存压力高时最高压缩等级 */

#define HIGH_CPU_LEVEL        1    /* CPU压力高时压缩等级 */
#define LOW_CPU_LEVEL         9    /* CPU压力低时压缩等级 */

static int __read_mostly compression_level = 1;
static bool auto_adjust_enabled = true;
static struct task_struct *zstd_adjust_task;

static unsigned long get_total_mem(void)
{
	return totalram_pages() << PAGE_SHIFT;
}

static unsigned long get_free_mem(void)
{
	return global_zone_page_state(NR_FREE_PAGES) << PAGE_SHIFT;
}

static unsigned int get_mem_usage_percent(void)
{
	unsigned long used = get_total_mem() - get_free_mem();
	return (used * 100) / get_total_mem();
}

static unsigned int get_cpu_usage_percent(void)
{
	unsigned long avg = avenrun[0] >> FSHIFT;
	unsigned int usage = avg * 100 / num_online_cpus();
	if (usage > 100) usage = 100;
	return usage;
}

static int zstd_adjust_thread(void *data)
{
	while (!kthread_should_stop()) {
		if (!auto_adjust_enabled) {
			ssleep(5);
			continue;
		}

		unsigned int mem = get_mem_usage_percent();
		unsigned int cpu = get_cpu_usage_percent();
		int new_level;

		if (mem < MEM_USAGE_THRESH && cpu < CPU_USAGE_THRESH) {
			// 双低负载：1 ~ 3，越空闲越低压缩
			new_level = LOW_MEM_MAX_LEVEL -
			            (mem * (LOW_MEM_MAX_LEVEL - LOW_MEM_MIN_LEVEL) / MEM_USAGE_THRESH);
			new_level = clamp_val(new_level, LOW_MEM_MIN_LEVEL, LOW_MEM_MAX_LEVEL);
		} else {
			// 有任一压力：4 ~ 9，CPU 越高越轻压缩
			new_level = HIGH_MEM_MAX_LEVEL -
			            (cpu * (HIGH_MEM_MAX_LEVEL - HIGH_MEM_MIN_LEVEL) / 100);
			new_level = clamp_val(new_level, HIGH_MEM_MIN_LEVEL, HIGH_MEM_MAX_LEVEL);
		}

		if (new_level != compression_level) {
			pr_info("zstdn: auto-adjust level=%d (mem=%u%%, cpu=%u%%)\n", new_level, mem, cpu);
		}

		compression_level = new_level;
		ssleep(5);
	}
	return 0;
}

int zstdn_set_compression_level(const char *val, const struct kernel_param *kp)
{
	if (sysfs_streq(val, "auto")) {
		auto_adjust_enabled = true;
		pr_info("zstdn: Auto-set compression level enabled\n");
		return 0;
	}

	auto_adjust_enabled = false;

	int temp_level;
	if (kstrtoint(val, 10, &temp_level) == 0) {
		compression_level = clamp_val(temp_level, ZSTD_MIN_LEVEL, ZSTD_MAX_LEVEL);
		pr_info("zstdn: Manual compression level set to %d\n", compression_level);
		return 0;
	}

	pr_warn("zstdn: Invalid compression level input\n");
  	return -EINVAL;
}
  
static int get_compression_level(char *buffer, const struct kernel_param *kp)
{
	if (auto_adjust_enabled)
		return sprintf(buffer, "auto\n");
	return sprintf(buffer, "%d\n", compression_level);
}

static int get_compression_level_ro(char *buffer, const struct kernel_param *kp)
{
    return sprintf(buffer, "%d\n", compression_level);
}
  
module_param_call(compression_level, zstdn_set_compression_level, get_compression_level, &compression_level, 0644);
module_param_call(compression_level_ro, NULL, get_compression_level_ro, &compression_level, 0444);

struct zstd_ctx {
	zstd_cctx *cctx;
	zstd_dctx *dctx;
	void *cwksp;
	void *dwksp;
};

static zstd_parameters zstd_params(void)
{
  	return zstd_get_params(compression_level, PAGE_SIZE);
}

static int zstd_comp_init(struct zstd_ctx *ctx)
{
	int ret = 0;
	const zstd_parameters params = zstd_params();
	const size_t wksp_size = zstd_cctx_workspace_bound(&params.cParams);

	ctx->cwksp = vzalloc(wksp_size);
	if (!ctx->cwksp) {
		ret = -ENOMEM;
		goto out;
	}

	ctx->cctx = zstd_init_cctx(ctx->cwksp, wksp_size);
	if (!ctx->cctx) {
		ret = -EINVAL;
		goto out_free;
	}
out:
	return ret;
out_free:
	vfree(ctx->cwksp);
	goto out;
}

static int zstd_decomp_init(struct zstd_ctx *ctx)
{
	int ret = 0;
	const size_t wksp_size = zstd_dctx_workspace_bound();

	ctx->dwksp = vzalloc(wksp_size);
	if (!ctx->dwksp) {
		ret = -ENOMEM;
		goto out;
	}

	ctx->dctx = zstd_init_dctx(ctx->dwksp, wksp_size);
	if (!ctx->dctx) {
		ret = -EINVAL;
		goto out_free;
	}
out:
	return ret;
out_free:
	vfree(ctx->dwksp);
	goto out;
}

static void zstd_comp_exit(struct zstd_ctx *ctx)
{
	vfree(ctx->cwksp);
	ctx->cwksp = NULL;
	ctx->cctx = NULL;
}

static void zstd_decomp_exit(struct zstd_ctx *ctx)
{
	vfree(ctx->dwksp);
	ctx->dwksp = NULL;
	ctx->dctx = NULL;
}

static int __zstd_init(void *ctx)
{
	int ret;

	ret = zstd_comp_init(ctx);
	if (ret)
		return ret;
	ret = zstd_decomp_init(ctx);
	if (ret)
		zstd_comp_exit(ctx);
	return ret;
}

static void *zstd_alloc_ctx(struct crypto_scomp *tfm)
{
	int ret;
	struct zstd_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ret = __zstd_init(ctx);
	if (ret) {
		kfree(ctx);
		return ERR_PTR(ret);
	}

	return ctx;
}

static int zstd_init(struct crypto_tfm *tfm)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_init(ctx);
}

static void __zstd_exit(void *ctx)
{
	zstd_comp_exit(ctx);
	zstd_decomp_exit(ctx);
}

static void zstd_free_ctx(struct crypto_scomp *tfm, void *ctx)
{
	__zstd_exit(ctx);
	kfree(ctx);
}

static void zstd_exit(struct crypto_tfm *tfm)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	__zstd_exit(ctx);
}

static int __zstd_compress(const u8 *src, unsigned int slen,
			   u8 *dst, unsigned int *dlen, void *ctx)
{
	size_t out_len;
	struct zstd_ctx *zctx = ctx;
	const zstd_parameters params = zstd_params();

	out_len = zstd_compress_cctx(zctx->cctx, dst, *dlen, src, slen, &params);
	if (zstd_is_error(out_len))
		return -EINVAL;
	*dlen = out_len;
	return 0;
}

static int zstd_compress(struct crypto_tfm *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_compress(src, slen, dst, dlen, ctx);
}

static int zstd_scompress(struct crypto_scomp *tfm, const u8 *src,
			  unsigned int slen, u8 *dst, unsigned int *dlen,
			  void *ctx)
{
	return __zstd_compress(src, slen, dst, dlen, ctx);
}

static int __zstd_decompress(const u8 *src, unsigned int slen,
			     u8 *dst, unsigned int *dlen, void *ctx)
{
	size_t out_len;
	struct zstd_ctx *zctx = ctx;

	out_len = zstd_decompress_dctx(zctx->dctx, dst, *dlen, src, slen);
	if (zstd_is_error(out_len))
		return -EINVAL;
	*dlen = out_len;
	return 0;
}

static int zstd_decompress(struct crypto_tfm *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_decompress(src, slen, dst, dlen, ctx);
}

static int zstd_sdecompress(struct crypto_scomp *tfm, const u8 *src,
			    unsigned int slen, u8 *dst, unsigned int *dlen,
			    void *ctx)
{
	return __zstd_decompress(src, slen, dst, dlen, ctx);
}

static struct crypto_alg alg = {
	.cra_name		= "zstdn",
	.cra_driver_name	= "zstdn-generic",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct zstd_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= zstd_init,
	.cra_exit		= zstd_exit,
	.cra_u			= { .compress = {
	.coa_compress		= zstd_compress,
	.coa_decompress		= zstd_decompress } }
};

static struct scomp_alg scomp = {
	.alloc_ctx		= zstd_alloc_ctx,
	.free_ctx		= zstd_free_ctx,
	.compress		= zstd_scompress,
	.decompress		= zstd_sdecompress,
	.base			= {
		.cra_name	= "zstdn",
		.cra_driver_name = "zstdn-scomp",
		.cra_module	 = THIS_MODULE,
	}
};

static int __init zstdn_mod_init(void)
{
	int ret;

	pr_info("register comp zstdn start\n");
	ret = crypto_register_alg(&alg);
	if (ret)
		return ret;

	ret = crypto_register_scomp(&scomp);
	if (ret) {
		crypto_unregister_alg(&alg);
	    pr_info("register comp zstdn success\n");
	    return ret;
	}

    ret = zram_register_alg("zstdn");
    if (ret) {
	    crypto_unregister_alg(&alg);
	    crypto_unregister_scomp(&scomp);
        return ret;
    }

	zstd_adjust_task = kthread_run(zstd_adjust_thread, NULL, "zstd_adjust");
	if (IS_ERR(zstd_adjust_task)) {
		pr_err("zstdn: Failed to start adjust thread\n");
		zstd_adjust_task = NULL;
	}

	return 0;
}

static void __exit zstdn_mod_fini(void)
{
	if (zstd_adjust_task)
		kthread_stop(zstd_adjust_task);

    zram_unregister_alg("zstdn");
	crypto_unregister_alg(&alg);
	crypto_unregister_scomp(&scomp);
}

module_init(zstdn_mod_init);
module_exit(zstdn_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zstdn Compression Algorithm");
MODULE_ALIAS_CRYPTO("zstdn");

/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Shared glue code between AES implementations, refactored from the AES-NI's.
 *
 * The helper code is inlined for a performance reason. With the mitigation
 * for speculative executions like retpoline, indirect calls become very
 * expensive at a cost of measurable overhead.
 */

#ifndef _AES_INTEL_GLUE_H
#define _AES_INTEL_GLUE_H

#include <linux/err.h>
#include <crypto/algapi.h>
#include <crypto/aes.h>
#include <crypto/xts.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/simd.h>

#define AES_ALIGN			16
#define AES_ALIGN_ATTR			__attribute__((__aligned__(AES_ALIGN)))
#define AES_BLOCK_MASK			(~(AES_BLOCK_SIZE - 1))
#define AES_ALIGN_EXTRA			((AES_ALIGN - 1) & ~(CRYPTO_MINALIGN - 1))
#define CRYPTO_AES_CTX_SIZE		(sizeof(struct crypto_aes_ctx) + AES_ALIGN_EXTRA)
#define XTS_AES_CTX_SIZE		(sizeof(struct aes_xts_ctx) + AES_ALIGN_EXTRA)

struct aes_xts_ctx {
	struct crypto_aes_ctx tweak_ctx AES_ALIGN_ATTR;
	struct crypto_aes_ctx crypt_ctx AES_ALIGN_ATTR;
};

static inline struct crypto_aes_ctx *aes_ctx(void *raw_ctx)
{
	unsigned long addr = (unsigned long)raw_ctx;
	unsigned long align = AES_ALIGN;

	if (align <= crypto_tfm_ctx_alignment())
		align = 1;

	return (struct crypto_aes_ctx *)ALIGN(addr, align);
}

static inline int
xts_setkey_common(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen,
		  int (*fn)(struct crypto_tfm *tfm, void *ctx, const u8 *in_key,
			    unsigned int key_len))
{
	struct aes_xts_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	err = xts_verify_key(tfm, key, keylen);
	if (err)
		return err;

	keylen /= 2;

	/* first half of xts-key is for crypt */
	err = fn(crypto_skcipher_tfm(tfm), &ctx->crypt_ctx, key, keylen);
	if (err)
		return err;

	/* second half of xts-key is for tweak */
	return fn(crypto_skcipher_tfm(tfm), &ctx->tweak_ctx, key + keylen, keylen);
}

static inline int
xts_crypt_common(struct skcipher_request *req,
		 int (*crypt_fn)(const struct crypto_aes_ctx *ctx, u8 *out, const u8 *in,
				 unsigned int len, u8 *iv),
		 int (*crypt1_fn)(const void *ctx, u8 *out, const u8 *in))
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct aes_xts_ctx *ctx = crypto_skcipher_ctx(tfm);
	int tail = req->cryptlen % AES_BLOCK_SIZE;
	struct skcipher_request subreq;
	struct skcipher_walk walk;
	int err;

	if (req->cryptlen < AES_BLOCK_SIZE)
		return -EINVAL;

	err = skcipher_walk_virt(&walk, req, false);
	if (!walk.nbytes)
		return err;

	if (unlikely(tail > 0 && walk.nbytes < walk.total)) {
		int blocks = DIV_ROUND_UP(req->cryptlen, AES_BLOCK_SIZE) - 2;

		skcipher_walk_abort(&walk);

		skcipher_request_set_tfm(&subreq, tfm);
		skcipher_request_set_callback(&subreq,
					      skcipher_request_flags(req),
					      NULL, NULL);
		skcipher_request_set_crypt(&subreq, req->src, req->dst,
					   blocks * AES_BLOCK_SIZE, req->iv);
		req = &subreq;

		err = skcipher_walk_virt(&walk, req, false);
		if (!walk.nbytes)
			return err;
	} else {
		tail = 0;
	}

	kernel_fpu_begin();

	/* calculate first value of T */
	err = crypt1_fn(aes_ctx(&ctx->tweak_ctx), walk.iv, walk.iv);
	if (err) {
		kernel_fpu_end();
		return err;
	}

	while (walk.nbytes > 0) {
		int nbytes = walk.nbytes;

		if (nbytes < walk.total)
			nbytes &= ~(AES_BLOCK_SIZE - 1);

		err = crypt_fn(aes_ctx(&ctx->crypt_ctx), walk.dst.virt.addr, walk.src.virt.addr,
			       nbytes, walk.iv);
		kernel_fpu_end();
		if (err)
			return err;

		err = skcipher_walk_done(&walk, walk.nbytes - nbytes);

		if (walk.nbytes > 0)
			kernel_fpu_begin();
	}

	if (unlikely(tail > 0 && !err)) {
		struct scatterlist sg_src[2], sg_dst[2];
		struct scatterlist *src, *dst;

		dst = src = scatterwalk_ffwd(sg_src, req->src, req->cryptlen);
		if (req->dst != req->src)
			dst = scatterwalk_ffwd(sg_dst, req->dst, req->cryptlen);

		skcipher_request_set_crypt(req, src, dst, AES_BLOCK_SIZE + tail,
					   req->iv);

		err = skcipher_walk_virt(&walk, &subreq, false);
		if (err)
			return err;

		kernel_fpu_begin();
		err = crypt_fn(aes_ctx(&ctx->crypt_ctx), walk.dst.virt.addr, walk.src.virt.addr,
			       walk.nbytes, walk.iv);
		kernel_fpu_end();
		if (err)
			return err;

		err = skcipher_walk_done(&walk, 0);
	}
	return err;
}

#endif /* _AES_INTEL_GLUE_H */

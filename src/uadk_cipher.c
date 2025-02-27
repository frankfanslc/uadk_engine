/*
 * Copyright 2020-2021 Huawei Technologies Co.,Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>
#include <openssl/engine.h>
#include <uadk/wd_cipher.h>
#include "uadk.h"
#include "uadk_async.h"

#define UADK_DO_SOFT         (-0xE0)
#define CTX_SYNC_ENC		0
#define CTX_SYNC_DEC		1
#define CTX_ASYNC_ENC		2
#define CTX_ASYNC_DEC		3
#define CTX_NUM			4
#define CTR_128BIT_COUNTER	16
#define CTR_MODE_LEN_SHIFT	4
#define BYTE_BITS		8
#define IV_LEN			16
#define ENV_ENABLED		1

struct cipher_engine {
	struct wd_ctx_config ctx_cfg;
	struct wd_sched sched;
	int numa_id;
	int pid;
	pthread_spinlock_t lock;
};

static struct cipher_engine engine;

struct sw_cipher_t {
	int nid;
	const EVP_CIPHER *(*get_cipher)(void);
};

struct cipher_priv_ctx {
	handle_t sess;
	struct wd_cipher_sess_setup setup;
	struct wd_cipher_req req;
	unsigned char iv[IV_LEN];
	const unsigned char *key;
	int switch_flag;
	void *sw_ctx_data;
	/* Crypto small packet offload threshold */
	size_t switch_threshold;
	int update_iv;
};

struct cipher_info {
	int nid;
	enum wd_cipher_alg alg;
	enum wd_cipher_mode mode;
	__u32 out_bytes;
};

static int platform;

#define SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT 192

static int cipher_920_nids[] = {
	NID_aes_128_cbc,
	NID_aes_192_cbc,
	NID_aes_256_cbc,
	NID_aes_128_ecb,
	NID_aes_192_ecb,
	NID_aes_256_ecb,
	NID_aes_128_xts,
	NID_aes_256_xts,
	NID_sm4_cbc,
	NID_des_ede3_cbc,
	NID_des_ede3_ecb,
	0,
};

static int cipher_930_nids[] = {
	NID_aes_128_cbc,
	NID_aes_192_cbc,
	NID_aes_256_cbc,
	NID_aes_128_ctr,
	NID_aes_192_ctr,
	NID_aes_256_ctr,
	NID_aes_128_ecb,
	NID_aes_192_ecb,
	NID_aes_256_ecb,
	NID_aes_128_xts,
	NID_aes_256_xts,
	NID_sm4_cbc,
	NID_sm4_ecb,
	NID_des_ede3_cbc,
	NID_des_ede3_ecb,
	NID_aes_128_cfb128,
	NID_aes_192_cfb128,
	NID_aes_256_cfb128,
	NID_aes_128_ofb128,
	NID_aes_192_ofb128,
	NID_aes_256_ofb128,
	NID_sm4_cfb128,
	NID_sm4_ofb128,
	NID_sm4_ctr,
	0,
};

static EVP_CIPHER *uadk_aes_128_cbc;
static EVP_CIPHER *uadk_aes_192_cbc;
static EVP_CIPHER *uadk_aes_256_cbc;
static EVP_CIPHER *uadk_aes_128_ctr;
static EVP_CIPHER *uadk_aes_192_ctr;
static EVP_CIPHER *uadk_aes_256_ctr;
static EVP_CIPHER *uadk_aes_128_ecb;
static EVP_CIPHER *uadk_aes_192_ecb;
static EVP_CIPHER *uadk_aes_256_ecb;
static EVP_CIPHER *uadk_aes_128_xts;
static EVP_CIPHER *uadk_aes_256_xts;
static EVP_CIPHER *uadk_sm4_cbc;
static EVP_CIPHER *uadk_sm4_ecb;
static EVP_CIPHER *uadk_des_ede3_cbc;
static EVP_CIPHER *uadk_des_ede3_ecb;
static EVP_CIPHER *uadk_aes_128_cfb128;
static EVP_CIPHER *uadk_aes_192_cfb128;
static EVP_CIPHER *uadk_aes_256_cfb128;
static EVP_CIPHER *uadk_aes_128_ofb128;
static EVP_CIPHER *uadk_aes_192_ofb128;
static EVP_CIPHER *uadk_aes_256_ofb128;
static EVP_CIPHER *uadk_sm4_cfb128;
static EVP_CIPHER *uadk_sm4_ofb128;
static EVP_CIPHER *uadk_sm4_ctr;

static struct sw_cipher_t sec_ciphers_sw_table[] = {
	{ NID_aes_128_ecb, EVP_aes_128_ecb },
	{ NID_aes_192_ecb, EVP_aes_192_ecb },
	{ NID_aes_256_ecb, EVP_aes_256_ecb },
	{ NID_aes_128_cbc, EVP_aes_128_cbc },
	{ NID_aes_192_cbc, EVP_aes_192_cbc },
	{ NID_aes_256_cbc, EVP_aes_256_cbc },
	{ NID_aes_128_xts, EVP_aes_128_xts },
	{ NID_aes_256_xts, EVP_aes_256_xts },
	{ NID_sm4_cbc, EVP_sm4_cbc },
	{ NID_des_ede3_cbc, EVP_des_ede3_cbc },
	{ NID_des_ede3_ecb, EVP_des_ede3_ecb },
	{ NID_aes_128_ctr, EVP_aes_128_ctr },
	{ NID_aes_192_ctr, EVP_aes_192_ctr },
	{ NID_aes_256_ctr, EVP_aes_256_ctr },
	{ NID_aes_128_ofb128, EVP_aes_128_ofb },
	{ NID_aes_192_ofb128, EVP_aes_192_ofb },
	{ NID_aes_256_ofb128, EVP_aes_256_ofb },
	{ NID_aes_128_cfb128, EVP_aes_128_cfb },
	{ NID_aes_192_cfb128, EVP_aes_192_cfb },
	{ NID_aes_256_cfb128, EVP_aes_256_cfb },
	{ NID_sm4_ofb128, EVP_sm4_ofb },
	{ NID_sm4_cfb128, EVP_sm4_cfb },
	{ NID_sm4_ecb, EVP_sm4_ecb },
	{ NID_sm4_ctr, EVP_sm4_ctr },
};

static struct cipher_info cipher_info_table[] = {
	{ NID_aes_128_ecb, WD_CIPHER_AES, WD_CIPHER_ECB, 16},
	{ NID_aes_192_ecb, WD_CIPHER_AES, WD_CIPHER_ECB, 16},
	{ NID_aes_256_ecb, WD_CIPHER_AES, WD_CIPHER_ECB, 16},
	{ NID_aes_128_cbc, WD_CIPHER_AES, WD_CIPHER_CBC, 16},
	{ NID_aes_192_cbc, WD_CIPHER_AES, WD_CIPHER_CBC, 64},
	{ NID_aes_256_cbc, WD_CIPHER_AES, WD_CIPHER_CBC, 64},
	{ NID_aes_128_xts, WD_CIPHER_AES, WD_CIPHER_XTS, 32},
	{ NID_aes_256_xts, WD_CIPHER_AES, WD_CIPHER_XTS, 512},
	{ NID_sm4_cbc, WD_CIPHER_SM4, WD_CIPHER_CBC, 16},
	{ NID_des_ede3_cbc, WD_CIPHER_3DES, WD_CIPHER_CBC, 16},
	{ NID_des_ede3_ecb, WD_CIPHER_3DES, WD_CIPHER_ECB, 16},
	{ NID_aes_128_ctr, WD_CIPHER_AES, WD_CIPHER_CTR, 64},
	{ NID_aes_192_ctr, WD_CIPHER_AES, WD_CIPHER_CTR, 64},
	{ NID_aes_256_ctr, WD_CIPHER_AES, WD_CIPHER_CTR, 64},
	{ NID_aes_128_ofb128, WD_CIPHER_AES, WD_CIPHER_OFB, 16},
	{ NID_aes_192_ofb128, WD_CIPHER_AES, WD_CIPHER_OFB, 16},
	{ NID_aes_256_ofb128, WD_CIPHER_AES, WD_CIPHER_OFB, 16},
	{ NID_aes_128_cfb128, WD_CIPHER_AES, WD_CIPHER_CFB, 16},
	{ NID_aes_192_cfb128, WD_CIPHER_AES, WD_CIPHER_CFB, 16},
	{ NID_aes_256_cfb128, WD_CIPHER_AES, WD_CIPHER_CFB, 16},
	{ NID_sm4_ofb128, WD_CIPHER_SM4, WD_CIPHER_OFB, 16},
	{ NID_sm4_cfb128, WD_CIPHER_SM4, WD_CIPHER_CFB, 16},
	{ NID_sm4_ecb, WD_CIPHER_SM4, WD_CIPHER_ECB, 16},
	{ NID_sm4_ctr, WD_CIPHER_SM4, WD_CIPHER_CTR, 16},
};

static const EVP_CIPHER *sec_ciphers_get_cipher_sw_impl(int n_id)
{
	int sec_cipher_sw_table_size = ARRAY_SIZE(sec_ciphers_sw_table);
	int i;

	for (i = 0; i < sec_cipher_sw_table_size; i++) {
		if (n_id == sec_ciphers_sw_table[i].nid)
			return (sec_ciphers_sw_table[i].get_cipher)();
	}
	fprintf(stderr, "invalid nid %d\n", n_id);

	return (EVP_CIPHER *)NULL;
}

static int uadk_e_cipher_sw_init(EVP_CIPHER_CTX *ctx, const unsigned char *key,
				 const unsigned char *iv, int enc)
{
	/* Real implementation: Openssl soft arithmetic key initialization function */
	struct cipher_priv_ctx *priv = NULL;
	const EVP_CIPHER *sw_cipher = NULL;
	int ret, nid, sw_size;

	if (unlikely(key == NULL)) {
		fprintf(stderr, "uadk engine init parameter key is NULL.\n");
		return 0;
	}

	priv = (struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);
	if (unlikely(priv == NULL)) {
		fprintf(stderr, "uadk engine state is NULL.\n");
		return 0;
	}

	sw_cipher = sec_ciphers_get_cipher_sw_impl(EVP_CIPHER_CTX_nid(ctx));
	if (unlikely(sw_cipher == NULL)) {
		nid = EVP_CIPHER_CTX_nid(ctx);
		fprintf(stderr, "get openssl software cipher failed, nid = %d.\n", nid);
		return 0;
	}

	sw_size = EVP_CIPHER_impl_ctx_size(sw_cipher);
	if (unlikely(sw_size == 0)) {
		fprintf(stderr, "get openssl software cipher ctx size failed.\n");
		return 0;
	}

	if (priv->sw_ctx_data == NULL) {
		priv->sw_ctx_data = OPENSSL_malloc(sw_size);
		if (priv->sw_ctx_data == NULL)
			return 0;
	}

	memset(priv->sw_ctx_data, 0, sw_size);
	if (iv == NULL)
		iv = EVP_CIPHER_CTX_iv_noconst(ctx);

	EVP_CIPHER_CTX_set_cipher_data(ctx, priv->sw_ctx_data);
	ret = EVP_CIPHER_meth_get_init(sw_cipher)(ctx, key, iv, enc);
	EVP_CIPHER_CTX_set_cipher_data(ctx, priv);
	if (unlikely(ret != 1)) {
		fprintf(stderr, "failed init openssl soft work key.\n");
		OPENSSL_free(priv->sw_ctx_data);
		priv->sw_ctx_data = NULL;
		return 0;
	}

	return 1;
}

static int uadk_e_cipher_soft_work(EVP_CIPHER_CTX *ctx, unsigned char *out,
				   const unsigned char *in, size_t inl)
{
	struct cipher_priv_ctx *priv = NULL;
	const EVP_CIPHER *sw_cipher = NULL;
	unsigned char *iv;
	int ret, nid;

	priv = (struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);
	if (unlikely(priv == NULL)) {
		fprintf(stderr, "uadk engine state is NULL.\n");
		return 0;
	}

	/*
	 * The hardware input iv needs to be updated by engine, and the soft
	 * work iv can updated by self. so the hardware iv needs to be copied
	 * only once.
	 */
	if (!priv->update_iv) {
		iv = EVP_CIPHER_CTX_iv_noconst(ctx);
		if (unlikely(iv == NULL)) {
			fprintf(stderr, "get openssl software iv failed.\n");
			return 0;
		}
		memcpy(iv, priv->iv, EVP_CIPHER_CTX_iv_length(ctx));
		priv->update_iv = true;
	}
	sw_cipher = sec_ciphers_get_cipher_sw_impl(EVP_CIPHER_CTX_nid(ctx));
	if (unlikely(sw_cipher == NULL)) {
		nid = EVP_CIPHER_CTX_nid(ctx);
		fprintf(stderr, "get openssl software cipher failed, nid = %d.\n", nid);
		return 0;
	}

	EVP_CIPHER_CTX_set_cipher_data(ctx, priv->sw_ctx_data);
	ret = EVP_CIPHER_meth_get_do_cipher(sw_cipher)(ctx, out, in, inl);
	if (unlikely(ret != 1)) {
		fprintf(stderr, "OpenSSL do cipher failed.\n");
		return 0;
	}

	EVP_CIPHER_CTX_set_cipher_data(ctx, priv);

	return 1;
}

static int sec_ciphers_is_check_valid(struct cipher_priv_ctx *priv)
{
	return priv->req.in_bytes <= priv->switch_threshold ?
			 0 : 1;
}

static void uadk_e_cipher_sw_cleanup(EVP_CIPHER_CTX *ctx)
{
	struct cipher_priv_ctx *priv =
		(struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);

	if (priv->sw_ctx_data) {
		OPENSSL_free(priv->sw_ctx_data);
		priv->sw_ctx_data = NULL;
	}
}

static int uadk_get_accel_platform(char *alg_name)
{
	struct uacce_dev *dev;

	dev = wd_get_accel_dev("cipher");
	if (dev == NULL)
		return 0;

	if (!strcmp(dev->api, "hisi_qm_v2"))
		platform = KUNPENG920;
	else
		platform = KUNPENG930;
	free(dev);

	return 1;
}

static int uadk_e_engine_ciphers(ENGINE *e, const EVP_CIPHER **cipher,
				 const int **nids, int nid)
{
	int ret = 1;
	int *cipher_nids;
	int size;
	int i;

	if (platform == KUNPENG920) {
		size = (sizeof(cipher_920_nids) - 1) / sizeof(int);
		cipher_nids = cipher_920_nids;
	} else {
		size = (sizeof(cipher_930_nids) - 1) / sizeof(int);
		cipher_nids = cipher_930_nids;
	}

	if (!cipher) {
		*nids = cipher_nids;
		return size;
	}

	for (i = 0; i < size; i++) {
		if (nid == cipher_nids[i])
			break;
	}

	switch (nid) {
	case NID_aes_128_cbc:
		*cipher = uadk_aes_128_cbc;
		break;
	case NID_aes_192_cbc:
		*cipher = uadk_aes_192_cbc;
		break;
	case NID_aes_256_cbc:
		*cipher = uadk_aes_256_cbc;
		break;
	case NID_aes_128_ctr:
		*cipher = uadk_aes_128_ctr;
		break;
	case NID_aes_192_ctr:
		*cipher = uadk_aes_192_ctr;
		break;
	case NID_aes_256_ctr:
		*cipher = uadk_aes_256_ctr;
		break;
	case NID_aes_128_ecb:
		*cipher = uadk_aes_128_ecb;
		break;
	case NID_aes_192_ecb:
		*cipher = uadk_aes_192_ecb;
		break;
	case NID_aes_256_ecb:
		*cipher = uadk_aes_256_ecb;
		break;
	case NID_aes_128_xts:
		*cipher = uadk_aes_128_xts;
		break;
	case NID_aes_256_xts:
		*cipher = uadk_aes_256_xts;
		break;
	case NID_sm4_cbc:
		*cipher = uadk_sm4_cbc;
		break;
	case NID_sm4_ecb:
		*cipher = uadk_sm4_ecb;
		break;
	case NID_des_ede3_cbc:
		*cipher = uadk_des_ede3_cbc;
		break;
	case NID_des_ede3_ecb:
		*cipher = uadk_des_ede3_ecb;
		break;
	case NID_aes_128_ofb128:
		*cipher = uadk_aes_128_ofb128;
		break;
	case NID_aes_192_ofb128:
		*cipher = uadk_aes_192_ofb128;
		break;
	case NID_aes_256_ofb128:
		*cipher = uadk_aes_256_ofb128;
		break;
	case NID_aes_128_cfb128:
		*cipher = uadk_aes_128_cfb128;
		break;
	case NID_aes_192_cfb128:
		*cipher = uadk_aes_192_cfb128;
		break;
	case NID_aes_256_cfb128:
		*cipher = uadk_aes_256_cfb128;
		break;
	case NID_sm4_ofb128:
		*cipher = uadk_sm4_ofb128;
		break;
	case NID_sm4_cfb128:
		*cipher = uadk_sm4_cfb128;
		break;
	case NID_sm4_ctr:
		*cipher = uadk_sm4_ctr;
		break;
	default:
		ret = 0;
		*cipher = NULL;
		break;
	}

	return ret;
}

static handle_t sched_single_init(handle_t h_sched_ctx, void *sched_param)
{
	struct sched_params *param = (struct sched_params *)sched_param;
	struct sched_params *skey;

	skey = malloc(sizeof(struct sched_params));
	if (!skey) {
		fprintf(stderr, "fail to alloc cipher sched key!\n");
		return (handle_t)0;
	}

	skey->numa_id = param->numa_id;
	skey->type = param->type;

	return (handle_t)skey;
}

static __u32 sched_single_pick_next_ctx(handle_t sched_ctx,
		void *sched_key, const int sched_mode)
{
	struct sched_params *key = (struct sched_params *)sched_key;

	if (sched_mode) {
		/* async */
		if (key->type == WD_CIPHER_ENCRYPTION)
			return CTX_ASYNC_ENC;
		else
			return CTX_ASYNC_DEC;
	} else {
		/* sync */
		if (key->type == WD_CIPHER_ENCRYPTION)
			return CTX_SYNC_ENC;
		else
			return CTX_SYNC_DEC;
	}
}

static int sched_single_poll_policy(handle_t h_sched_ctx,
				    __u32 expect, __u32 *count)
{
	return 0;
}

static int uadk_e_cipher_poll(void *ctx)
{
	struct cipher_priv_ctx *priv = (struct cipher_priv_ctx *) ctx;
	__u32 recv = 0;
	/* Poll one packet currently */
	int expt = 1;
	int ret, idx;

	if (priv->req.op_type == WD_CIPHER_ENCRYPTION)
		idx = CTX_ASYNC_ENC;
	else
		idx = CTX_ASYNC_DEC;

	do {
		ret = wd_cipher_poll_ctx(idx, expt, &recv);
		if (recv >= expt)
			return 0;
		else if (ret < 0 && ret != -EAGAIN)
			return ret;
	} while (ret == -EAGAIN);

	return ret;
}

static int uadk_e_cipher_env_poll(void *ctx)
{
	__u32 recv = 0;
	/* poll one packet currently */
	int expt = 1;
	int ret;

	do {
		ret = wd_cipher_poll(expt, &recv);
		if (ret < 0)
			return ret;
	} while (recv < expt);

	return ret;
}

static int uadk_e_wd_cipher_env_init(struct uacce_dev *dev)
{
	int ret;

	ret = uadk_e_set_env("WD_CIPHER_CTX_NUM", dev->numa_id);
	if (ret)
		return ret;

	ret = wd_cipher_env_init(NULL);
	if (ret)
		return ret;

	return async_register_poll_fn(ASYNC_TASK_CIPHER,
				      uadk_e_cipher_env_poll);
}

static int uadk_e_wd_cipher_init(struct uacce_dev *dev)
{
	int ret, i, j;

	engine.numa_id = dev->numa_id;

	ret = uadk_e_is_env_enabled("cipher");
	if (ret == ENV_ENABLED)
		return uadk_e_wd_cipher_env_init(dev);

	memset(&engine.ctx_cfg, 0, sizeof(struct wd_ctx_config));
	engine.ctx_cfg.ctx_num = CTX_NUM;
	engine.ctx_cfg.ctxs = calloc(CTX_NUM, sizeof(struct wd_ctx));
	if (!engine.ctx_cfg.ctxs)
		return -ENOMEM;

	for (i = 0; i < CTX_NUM; i++) {
		engine.ctx_cfg.ctxs[i].ctx = wd_request_ctx(dev);
		if (!engine.ctx_cfg.ctxs[i].ctx) {
			ret = -ENOMEM;
			goto err_freectx;
		}
	}

	engine.ctx_cfg.ctxs[CTX_SYNC_ENC].op_type = CTX_TYPE_ENCRYPT;
	engine.ctx_cfg.ctxs[CTX_SYNC_DEC].op_type = CTX_TYPE_DECRYPT;
	engine.ctx_cfg.ctxs[CTX_ASYNC_ENC].op_type = CTX_TYPE_ENCRYPT;
	engine.ctx_cfg.ctxs[CTX_ASYNC_DEC].op_type = CTX_TYPE_DECRYPT;
	engine.ctx_cfg.ctxs[CTX_SYNC_ENC].ctx_mode = CTX_MODE_SYNC;
	engine.ctx_cfg.ctxs[CTX_SYNC_DEC].ctx_mode = CTX_MODE_SYNC;
	engine.ctx_cfg.ctxs[CTX_ASYNC_ENC].ctx_mode = CTX_MODE_ASYNC;
	engine.ctx_cfg.ctxs[CTX_ASYNC_DEC].ctx_mode = CTX_MODE_ASYNC;

	engine.sched.name = "sched_single";
	engine.sched.pick_next_ctx = sched_single_pick_next_ctx;
	engine.sched.poll_policy = sched_single_poll_policy;
	engine.sched.sched_init = sched_single_init;

	ret = wd_cipher_init(&engine.ctx_cfg, &engine.sched);
	if (ret)
		goto err_freectx;

	return async_register_poll_fn(ASYNC_TASK_CIPHER, uadk_e_cipher_poll);

err_freectx:
	for (j = 0; j < i; j++)
		wd_release_ctx(engine.ctx_cfg.ctxs[j].ctx);

	free(engine.ctx_cfg.ctxs);

	return ret;
}

static int uadk_e_init_cipher(void)
{
	struct uacce_dev *dev;
	int ret;

	if (engine.pid != getpid()) {
		pthread_spin_lock(&engine.lock);
		if (engine.pid == getpid()) {
			pthread_spin_unlock(&engine.lock);
			return 1;
		}

		dev = wd_get_accel_dev("cipher");
		if (!dev) {
			pthread_spin_unlock(&engine.lock);
			fprintf(stderr, "failed to get device for cipher.\n");
			return 0;
		}

		ret = uadk_e_wd_cipher_init(dev);
		if (ret)
			goto err_unlock;

		engine.pid = getpid();
		pthread_spin_unlock(&engine.lock);
		free(dev);
	}

	return 1;

err_unlock:
	pthread_spin_unlock(&engine.lock);
	free(dev);
	fprintf(stderr, "failed to init cipher(%d).\n", ret);

	return 0;
}

static void cipher_priv_ctx_setup(struct cipher_priv_ctx *priv,
	enum wd_cipher_alg alg, enum wd_cipher_mode mode, __u32 out_bytes)
{
	priv->setup.alg = alg;
	priv->setup.mode = mode;
	priv->req.out_bytes = out_bytes;
}

static int uadk_e_cipher_init(EVP_CIPHER_CTX *ctx, const unsigned char *key,
			      const unsigned char *iv, int enc)
{
	struct cipher_priv_ctx *priv =
		(struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);
	int cipher_counts = ARRAY_SIZE(cipher_info_table);
	int nid, ret, i;

	if (unlikely(!key)) {
		fprintf(stderr, "ctx init parameter key is NULL.\n");
		return 0;
	}

	nid = EVP_CIPHER_CTX_nid(ctx);
	priv->req.op_type = enc ? WD_CIPHER_ENCRYPTION : WD_CIPHER_DECRYPTION;

	if (iv)
		memcpy(priv->iv, iv, EVP_CIPHER_CTX_iv_length(ctx));

	for (i = 0; i < cipher_counts; i++) {
		if (nid == cipher_info_table[i].nid) {
			cipher_priv_ctx_setup(priv, cipher_info_table[i].alg,
					cipher_info_table[i].mode, cipher_info_table[i].out_bytes);
			break;
		}
	}

	if (i == cipher_counts) {
		fprintf(stderr, "failed to setup the private ctx.\n");
		return 0;
	}

	ret = uadk_e_cipher_sw_init(ctx, key, iv, enc);
	if (unlikely(ret != 1))
		return 0;

	priv->key = key;
	priv->switch_threshold = SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT;

	return 1;
}

static int uadk_e_cipher_cleanup(EVP_CIPHER_CTX *ctx)
{
	struct cipher_priv_ctx *priv =
		(struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);

	uadk_e_cipher_sw_cleanup(ctx);

	if (priv->sess) {
		wd_cipher_free_sess(priv->sess);
		priv->sess = 0;
	}

	return 1;
}

static void async_cb(struct wd_cipher_req *req, void *data)
{
	struct uadk_e_cb_info *cb_param;
	struct async_op *op;

	if (!req)
		return;

	cb_param = req->cb_param;
	if (!cb_param)
		return;

	op = cb_param->op;
	if (op && op->job && !op->done) {
		op->done = 1;
		async_free_poll_task(op->idx, 1);
		async_wake_job(op->job);
	}
}

/* increment counter (128-bit int) by c */
static void ctr_iv_inc(uint8_t *counter, __u32 c)
{
	uint32_t n = CTR_128BIT_COUNTER;
	uint8_t *counter1 = counter;

	/*
	 * Since the counter has been increased 1 by the hardware,
	 * so the c need to  decrease 1.
	 */
	c = c - 1;
	do {
		--n;
		c += counter1[n];
		counter1[n] = (uint8_t)c;
		c >>= BYTE_BITS;
	} while (n);
}

static void uadk_cipher_update_priv_ctx(struct cipher_priv_ctx *priv)
{
	__u16 iv_bytes = priv->req.iv_bytes;
	int offset = priv->req.in_bytes - iv_bytes;
	unsigned char K[IV_LEN] = {0};
	int i;

	switch (priv->setup.mode) {
	case WD_CIPHER_CBC:
		if (priv->req.op_type == WD_CIPHER_ENCRYPTION) {
			priv->req.dst += priv->req.in_bytes;
			memcpy(priv->iv, priv->req.dst - iv_bytes, iv_bytes);
		}
		break;
	case WD_CIPHER_OFB:
		for (i = 0; i < IV_LEN; i++) {
			K[i] = *((unsigned char *)priv->req.src + offset + i) ^
			       *((unsigned char *)priv->req.dst + offset + i);
		}
		memcpy(priv->iv, K, iv_bytes);
		break;
	case WD_CIPHER_CFB:
		if (priv->req.op_type == WD_CIPHER_ENCRYPTION) {
			priv->req.dst += priv->req.in_bytes;
			memcpy(priv->iv, priv->req.dst - iv_bytes, iv_bytes);
		} else {
			priv->req.src += priv->req.in_bytes;
			memcpy(priv->iv, priv->req.src - iv_bytes, iv_bytes);
		}
		break;
	case WD_CIPHER_CTR:
		ctr_iv_inc(priv->iv, priv->req.in_bytes >> CTR_MODE_LEN_SHIFT);
		break;
	default:
		break;
	}
}

static int do_cipher_sync(struct cipher_priv_ctx *priv)
{
	int ret;

	if (unlikely(priv->switch_flag == UADK_DO_SOFT))
		return 0;

	ret = sec_ciphers_is_check_valid(priv);
	if (!ret)
		return 0;

	ret = wd_do_cipher_sync(priv->sess, &priv->req);
	if (ret)
		return 0;
	return 1;
}

static int do_cipher_async(struct cipher_priv_ctx *priv, struct async_op *op)
{
	struct uadk_e_cb_info cb_param;
	int idx, ret;

	if (unlikely(priv->switch_flag == UADK_DO_SOFT)) {
		fprintf(stderr, "async cipher init failed.\n");
		return 0;
	}

	cb_param.op = op;
	cb_param.priv = priv;
	priv->req.cb = (void *)async_cb;
	priv->req.cb_param = &cb_param;
	ret = async_get_free_task(&idx);
	if (!ret)
		return 0;

	op->idx = idx;
	do {
		ret = wd_do_cipher_async(priv->sess, &priv->req);
		if (ret < 0 && ret != -EBUSY) {
			fprintf(stderr, "do sec cipher failed, switch to soft cipher.\n");
			async_free_poll_task(op->idx, 0);
			return 0;
		}
	} while (ret == -EBUSY);

	ret = async_pause_job(priv, op, ASYNC_TASK_CIPHER, idx);
	if (!ret)
		return 0;
	return 1;
}

static void uadk_e_ctx_init(EVP_CIPHER_CTX *ctx, struct cipher_priv_ctx *priv)
{
	struct sched_params params = {0};
	int ret;

	ret = uadk_e_init_cipher();
	if (unlikely(!ret)) {
		priv->switch_flag = UADK_DO_SOFT;
		fprintf(stderr, "uadk failed to init cipher HW!\n");
	}

	priv->req.iv_bytes = EVP_CIPHER_CTX_iv_length(ctx);
	priv->req.iv = priv->iv;

	/*
	 * The internal RR scheduler used by environment variables,
	 * the cipher algorithm does not distinguish between
	 * encryption and decryption queues
	 */
	params.type = priv->req.op_type;
	ret = uadk_e_is_env_enabled("cipher");
	if (ret)
		params.type = 0;

	params.numa_id = engine.numa_id;
	priv->setup.sched_param = &params;
	if (!priv->sess) {
		priv->sess = wd_cipher_alloc_sess(&priv->setup);
		if (!priv->sess)
			fprintf(stderr, "uadk failed to alloc session!\n");
	}

	ret = wd_cipher_set_key(priv->sess, priv->key, EVP_CIPHER_CTX_key_length(ctx));
	if (ret) {
		wd_cipher_free_sess(priv->sess);
		fprintf(stderr, "uadk failed to set key!\n");
	}
}

static int uadk_e_do_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
			    const unsigned char *in, size_t inlen)
{
	struct cipher_priv_ctx *priv =
		(struct cipher_priv_ctx *)EVP_CIPHER_CTX_get_cipher_data(ctx);
	struct async_op op;
	int ret;

	priv->req.src = (unsigned char *)in;
	priv->req.in_bytes = inlen;
	priv->req.dst = out;
	priv->req.out_buf_bytes = inlen;

	uadk_e_ctx_init(ctx, priv);
	ret = async_setup_async_event_notification(&op);
	if (!ret) {
		fprintf(stderr, "failed to setup async event notification.\n");
		return 0;
	}

	if (op.job == NULL) {
		/* Synchronous, only the synchronous mode supports soft computing */
		ret = do_cipher_sync(priv);
		if (!ret)
			goto sync_err;
	} else {
		ret = do_cipher_async(priv, &op);
		if (!ret)
			goto out_notify;
	}
	uadk_cipher_update_priv_ctx(priv);

	return 1;
sync_err:
	ret = uadk_e_cipher_soft_work(ctx, out, in, inlen);
	if (ret != 1)
		fprintf(stderr, "do soft ciphers failed.\n");
out_notify:
	async_clear_async_event_notification();
	return ret;
}

#define UADK_CIPHER_DESCR(name, block_size, key_size, iv_len, flags, ctx_size, \
	init, cipher, cleanup, set_params, get_params) \
do { \
	uadk_##name = EVP_CIPHER_meth_new(NID_##name, block_size, key_size); \
	if (uadk_##name == 0 || \
		!EVP_CIPHER_meth_set_iv_length(uadk_##name, iv_len) || \
		!EVP_CIPHER_meth_set_flags(uadk_##name, flags) || \
		!EVP_CIPHER_meth_set_impl_ctx_size(uadk_##name, ctx_size) || \
		!EVP_CIPHER_meth_set_init(uadk_##name, init) || \
		!EVP_CIPHER_meth_set_do_cipher(uadk_##name, cipher) || \
		!EVP_CIPHER_meth_set_cleanup(uadk_##name, cleanup) || \
		!EVP_CIPHER_meth_set_set_asn1_params(uadk_##name, set_params) || \
		!EVP_CIPHER_meth_set_get_asn1_params(uadk_##name, get_params)) \
		return 0; \
} while (0)

static int bind_v2_cipher(void)
{
	UADK_CIPHER_DESCR(aes_128_cbc, 16, 16, 16, EVP_CIPH_CBC_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_192_cbc, 16, 24, 16, EVP_CIPH_CBC_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_cbc, 16, 32, 16, EVP_CIPH_CBC_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_128_ecb, 16, 16, 0, EVP_CIPH_ECB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_192_ecb, 16, 24, 0, EVP_CIPH_ECB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_ecb, 16, 32, 0, EVP_CIPH_ECB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_128_xts, 1, 32, 16, EVP_CIPH_XTS_MODE | EVP_CIPH_CUSTOM_IV,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_xts, 1, 64, 16, EVP_CIPH_XTS_MODE | EVP_CIPH_CUSTOM_IV,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(sm4_cbc, 16, 16, 16, EVP_CIPH_CBC_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(des_ede3_cbc, 8, 24, 8, EVP_CIPH_CBC_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(des_ede3_ecb, 8, 24, 0, EVP_CIPH_ECB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	return 0;
}

static int bind_v3_cipher(void)
{
	UADK_CIPHER_DESCR(aes_128_ctr, 1, 16, 16, EVP_CIPH_CTR_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_192_ctr, 1, 24, 16, EVP_CIPH_CTR_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_ctr, 1, 32, 16, EVP_CIPH_CTR_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_128_ofb128, 1, 16, 16, EVP_CIPH_OFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_192_ofb128, 1, 24, 16, EVP_CIPH_OFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_ofb128, 1, 32, 16, EVP_CIPH_OFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_128_cfb128, 1, 16, 16, EVP_CIPH_CFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_192_cfb128, 1, 24, 16, EVP_CIPH_CFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(aes_256_cfb128, 1, 32, 16, EVP_CIPH_CFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(sm4_ofb128, 1, 16, 16, EVP_CIPH_OFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(sm4_cfb128, 1, 16, 16, EVP_CIPH_OFB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(sm4_ctr, 1, 16, 16, EVP_CIPH_CTR_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);
	UADK_CIPHER_DESCR(sm4_ecb, 16, 16, 16, EVP_CIPH_ECB_MODE,
			  sizeof(struct cipher_priv_ctx), uadk_e_cipher_init,
			  uadk_e_do_cipher, uadk_e_cipher_cleanup,
			  EVP_CIPHER_set_asn1_iv, EVP_CIPHER_get_asn1_iv);

	return 0;
}

int uadk_e_bind_cipher(ENGINE *e)
{
	int ret;

	ret = uadk_get_accel_platform("cipher");
	if (!ret) {
		fprintf(stderr, "failed to get accel hardware version.\n");
		return 0;
	}

	bind_v2_cipher();
	if (platform > KUNPENG920)
		bind_v3_cipher();

	pthread_spin_init(&engine.lock, PTHREAD_PROCESS_PRIVATE);

	return ENGINE_set_ciphers(e, uadk_e_engine_ciphers);
}

static void destroy_v2_cipher(void)
{
	EVP_CIPHER_meth_free(uadk_aes_128_cbc);
	uadk_aes_128_cbc = 0;
	EVP_CIPHER_meth_free(uadk_aes_192_cbc);
	uadk_aes_192_cbc = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_cbc);
	uadk_aes_256_cbc = 0;
	EVP_CIPHER_meth_free(uadk_aes_128_ecb);
	uadk_aes_128_ecb = 0;
	EVP_CIPHER_meth_free(uadk_aes_192_ecb);
	uadk_aes_192_ecb = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_ecb);
	uadk_aes_256_ecb = 0;
	EVP_CIPHER_meth_free(uadk_aes_128_xts);
	uadk_aes_128_xts = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_xts);
	uadk_aes_256_xts = 0;
	EVP_CIPHER_meth_free(uadk_sm4_cbc);
	uadk_sm4_cbc = 0;
	EVP_CIPHER_meth_free(uadk_des_ede3_cbc);
	uadk_des_ede3_cbc = 0;
	EVP_CIPHER_meth_free(uadk_des_ede3_ecb);
	uadk_des_ede3_ecb = 0;
}

static void destroy_v3_cipher(void)
{
	EVP_CIPHER_meth_free(uadk_aes_128_ctr);
	uadk_aes_128_ctr = 0;
	EVP_CIPHER_meth_free(uadk_aes_192_ctr);
	uadk_aes_192_ctr = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_ctr);
	uadk_aes_256_ctr = 0;
	EVP_CIPHER_meth_free(uadk_aes_128_ofb128);
	uadk_aes_128_ofb128 = 0;
	EVP_CIPHER_meth_free(uadk_aes_192_ofb128);
	uadk_aes_192_ofb128 = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_ofb128);
	uadk_aes_256_ofb128 = 0;
	EVP_CIPHER_meth_free(uadk_aes_128_cfb128);
	uadk_aes_128_cfb128 = 0;
	EVP_CIPHER_meth_free(uadk_aes_192_cfb128);
	uadk_aes_192_cfb128 = 0;
	EVP_CIPHER_meth_free(uadk_aes_256_cfb128);
	uadk_aes_256_cfb128 = 0;
	EVP_CIPHER_meth_free(uadk_sm4_cfb128);
	uadk_sm4_cfb128 = 0;
	EVP_CIPHER_meth_free(uadk_sm4_ofb128);
	uadk_sm4_ofb128 = 0;
	EVP_CIPHER_meth_free(uadk_sm4_ctr);
	uadk_sm4_ctr = 0;
	EVP_CIPHER_meth_free(uadk_sm4_ecb);
	uadk_sm4_ecb = 0;
}

void uadk_e_destroy_cipher(void)
{
	int i, ret;

	if (engine.pid == getpid()) {
		ret = uadk_e_is_env_enabled("cipher");
		if (ret == ENV_ENABLED) {
			wd_cipher_env_uninit();
		} else {
			wd_cipher_uninit();
			for (i = 0; i < engine.ctx_cfg.ctx_num; i++)
				wd_release_ctx(engine.ctx_cfg.ctxs[i].ctx);
			free(engine.ctx_cfg.ctxs);
		}
		engine.pid = 0;
	}

	pthread_spin_destroy(&engine.lock);

	destroy_v2_cipher();
	if (platform > KUNPENG920)
		destroy_v3_cipher();
}

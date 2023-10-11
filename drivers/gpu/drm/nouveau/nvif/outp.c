/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <nvif/outp.h>
#include <nvif/disp.h>
#include <nvif/printf.h>

#include <nvif/class.h>
#include <nvif/if0012.h>

void
nvif_outp_release(struct nvif_outp *outp)
{
	int ret = nvif_mthd(&outp->object, NVIF_OUTP_V0_RELEASE, NULL, 0);
	NVIF_ERRON(ret, &outp->object, "[RELEASE]");
	outp->or.id = -1;
}

static inline int
nvif_outp_acquire(struct nvif_outp *outp, u8 proto, struct nvif_outp_acquire_v0 *args)
{
	int ret;

	args->version = 0;
	args->proto = proto;

	ret = nvif_mthd(&outp->object, NVIF_OUTP_V0_ACQUIRE, args, sizeof(*args));
	if (ret)
		return ret;

	outp->or.id = args->or;
	outp->or.link = args->link;
	return 0;
}

int
nvif_outp_acquire_dp(struct nvif_outp *outp,  bool hda)
{
	struct nvif_outp_acquire_v0 args;
	int ret;

	args.dp.hda = hda;

	ret = nvif_outp_acquire(outp, NVIF_OUTP_ACQUIRE_V0_DP, &args);
	NVIF_ERRON(ret, &outp->object,
		   "[ACQUIRE proto:DP hda:%d] or:%d link:%d", args.dp.hda, args.or, args.link);
	return ret;
}

int
nvif_outp_acquire_lvds(struct nvif_outp *outp, bool dual, bool bpc8)
{
	struct nvif_outp_acquire_v0 args;
	int ret;

	args.lvds.dual = dual;
	args.lvds.bpc8 = bpc8;

	ret = nvif_outp_acquire(outp, NVIF_OUTP_ACQUIRE_V0_LVDS, &args);
	NVIF_ERRON(ret, &outp->object,
		   "[ACQUIRE proto:LVDS dual:%d 8bpc:%d] or:%d link:%d",
		   args.lvds.dual, args.lvds.bpc8, args.or, args.link);
	return ret;
}

int
nvif_outp_acquire_tmds(struct nvif_outp *outp, bool hda)
{
	struct nvif_outp_acquire_v0 args;
	int ret;

	args.tmds.hda = hda;

	ret = nvif_outp_acquire(outp, NVIF_OUTP_ACQUIRE_V0_TMDS, &args);
	NVIF_ERRON(ret, &outp->object,
		   "[ACQUIRE proto:TMDS hda:%d] or:%d link:%d", args.tmds.hda, args.or, args.link);
	return ret;
}

int
nvif_outp_acquire_rgb_crt(struct nvif_outp *outp)
{
	struct nvif_outp_acquire_v0 args;
	int ret;

	ret = nvif_outp_acquire(outp, NVIF_OUTP_ACQUIRE_V0_RGB_CRT, &args);
	NVIF_ERRON(ret, &outp->object, "[ACQUIRE proto:RGB_CRT] or:%d", args.or);
	return ret;
}

int
nvif_outp_load_detect(struct nvif_outp *outp, u32 loadval)
{
	struct nvif_outp_load_detect_v0 args;
	int ret;

	args.version = 0;
	args.data = loadval;

	ret = nvif_mthd(&outp->object, NVIF_OUTP_V0_LOAD_DETECT, &args, sizeof(args));
	NVIF_ERRON(ret, &outp->object, "[LOAD_DETECT data:%08x] load:%02x", args.data, args.load);
	return ret < 0 ? ret : args.load;
}

void
nvif_outp_dtor(struct nvif_outp *outp)
{
	nvif_object_dtor(&outp->object);
}

int
nvif_outp_ctor(struct nvif_disp *disp, const char *name, int id, struct nvif_outp *outp)
{
	struct nvif_outp_v0 args;
	int ret;

	args.version = 0;
	args.id = id;

	ret = nvif_object_ctor(&disp->object, name ?: "nvifOutp", id, NVIF_CLASS_OUTP,
			       &args, sizeof(args), &outp->object);
	NVIF_ERRON(ret, &disp->object, "[NEW outp id:%d]", id);
	if (ret)
		return ret;

	outp->or.id = -1;
	return 0;
}

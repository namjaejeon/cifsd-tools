/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <memory.h>
#include <glib.h>
#include <errno.h>
#include <linux/cifsd_server.h>

#include <management/share.h>

#include <rpc.h>
#include <cifsdtools.h>

/*
 * We need a proper DCE RPC (ndr/ndr64) parser. And we also need a proper
 * IDL support...
 * Maybe someone smart and cool enough can do it for us. The one you can
 * find here is just a very simple implementation, which sort of works for
 * us, but we do realize that it sucks.
 */

#define PAYLOAD_HEAD(d)	((d)->payload + (d)->offset)

#define __ALIGN(x, a)							\
	({								\
		typeof(x) ret = (x);					\
		if (((x) & ((typeof(x))(a) - 1)) != 0)			\
			ret = __ALIGN_MASK(x, (typeof(x))(a) - 1);	\
		ret;							\
	})

#define __ALIGN_MASK(x, mask)	(((x) + (mask)) & ~(mask))

static void align_offset(struct cifsd_dcerpc *dce)
{
	if (dce->flags & CIFSD_DCERPC_ALIGN8) {
		dce->offset = __ALIGN(dce->offset, 8);
	} else if (dce->flags & CIFSD_DCERPC_ALIGN4) {
		dce->offset = __ALIGN(dce->offset, 4);
	}
}

static int try_realloc_payload(struct cifsd_dcerpc *dce, size_t data_sz)
{
	char *n;

	if (dce->offset < dce->payload_sz - data_sz)
		return 0;

	if (dce->flags & CIFSD_DCERPC_FIXED_PAYLOAD_SZ) {
		pr_err("DCE RPC: fixed payload buffer overflow\n");
		return -ENOMEM;
	}

	n = realloc(dce->payload, dce->payload_sz + 4096);
	if (!n)
		return -ENOMEM;

	dce->payload = n;
	dce->payload_sz += 4096;
	memset(dce->payload + dce->offset, 0, dce->payload_sz - dce->offset);
	return 0;
}

static int dcerpc_write_int16(struct cifsd_dcerpc *dce, short value)
{
	if (try_realloc_payload(dce, sizeof(short)))
		return -ENOMEM;

	if (dce->flags & CIFSD_DCERPC_LITTLE_ENDIAN)
		*(__le16 *)PAYLOAD_HEAD(dce) = (__le16)value;
	else
		*(short *)PAYLOAD_HEAD(dce) = value;

	dce->offset += sizeof(short);
	align_offset(dce);
	return 0;
}

static int dcerpc_write_int32(struct cifsd_dcerpc *dce, int value)
{
	if (try_realloc_payload(dce, sizeof(short)))
		return -ENOMEM;

	if (dce->flags & CIFSD_DCERPC_LITTLE_ENDIAN)
		*(__le32 *)PAYLOAD_HEAD(dce) = (__le32)value;
	else
		*(int *)PAYLOAD_HEAD(dce) = value;

	dce->offset += sizeof(int);
	align_offset(dce);
	return 0;
}

static int dcerpc_write_int64(struct cifsd_dcerpc *dce, long long value)
{
	if (try_realloc_payload(dce, sizeof(short)))
		return -ENOMEM;

	if (dce->flags & CIFSD_DCERPC_LITTLE_ENDIAN)
		*(__le64*)PAYLOAD_HEAD(dce) = (__le64)value;
	else
		*(long long*)PAYLOAD_HEAD(dce) = value;

	dce->offset += sizeof(long long);
	return 0;
}

static int dcerpc_write_union(struct cifsd_dcerpc *dce, int value)
{
	int ret;

	/*
	 * For a non-encapsulated union, the discriminant is marshalled into
	 * the transmitted data stream twice: once as the field or parameter,
	 * which is referenced by the switch_is construct, in the procedure
	 * argument list; and once as the first part of the union
	 * representation.
	 */
	ret = dcerpc_write_int32(dce, value);
	if (ret)
		return ret;
	return dcerpc_write_int32(dce, value);
}

static int dcerpc_write_bytes(struct cifsd_dcerpc *dce, void *value, size_t sz)
{
	if (try_realloc_payload(dce, sizeof(short)))
		return -ENOMEM;

	memcpy(PAYLOAD_HEAD(dce), value, sz);
	dce->offset += sz;
	return 0;
}

static int dcerpc_write_vstring(struct cifsd_dcerpc *dce, char *value)
{
	gchar *out;
	gsize bytes_read = 0;
	gsize bytes_written = 0;
	GError *err = NULL;

	size_t raw_len, conv_len;
	char *raw_value = value;
	char *conv_value;
	char *charset = CHARSET_UTF16LE;
	int ret;

	if (!value)
		raw_value = "";
	raw_len = strlen(raw_value);

	if (!(dce->flags & CIFSD_DCERPC_LITTLE_ENDIAN))
		charset = CHARSET_UTF16BE;

	if (dce->flags & CIFSD_DCERPC_ASCII_STRING)
		charset = CHARSET_UTF8;

	out = g_convert(raw_value,
			raw_len,
			charset,
			CHARSET_DEFAULT,
			&bytes_read,
			&bytes_written,
			&err);

	if (err) {
		pr_err("Can't convert string: %s\n", err->message);
		g_error_free(err);
		return -EINVAL;
	}

	/*
	 * NDR represents a conformant and varying string as an ordered
	 * sequence of representations of the string elements, preceded
	 * by three unsigned long integers. The first integer gives the
	 * maximum number of elements in the string, including the terminator.
	 * The second integer gives the offset from the first index of the
	 * string to the first index of the actual subset being passed.
	 * The third integer gives the actual number of elements being
	 * passed, including the terminator.
	 */
	ret = dcerpc_write_int32(dce, bytes_written / 2);
	ret |= dcerpc_write_int32(dce, 0);
	ret |= dcerpc_write_int32(dce, bytes_written / 2);
	ret |= dcerpc_write_bytes(dce, out, bytes_written);

	align_offset(dce);
out:
	g_free(out);
	return ret;
}

static int dcerpc_write_array_of_structs(struct cifsd_dcerpc *dce,
					 struct cifsd_rpc_pipe *pipe)
{
	int current_size;
	int max_entry_nr;
	int i, ret, has_more_data = 0;

	/*
	 * In the NDR representation of a structure that contains a
	 * conformant and varying array, the maximum counts for dimensions
	 * of the array are moved to the beginning of the structure, but
	 * the offsets and actual counts remain in place at the end of the
	 * structure, immediately preceding the array elements.
	 */

	max_entry_nr = pipe->num_entries;
	if (dce->flags & CIFSD_DCERPC_FIXED_PAYLOAD_SZ && dce->entry_size) {
		current_size = 0;

		for (i = 0; i < pipe->num_entries; i++) {
			gpointer entry;

			entry = g_array_index(pipe->entries,  gpointer, i);
			current_size += dce->entry_size(dce, entry);

			if (current_size < 2 * dce->payload_sz / 3)
				continue;

			max_entry_nr = i;
			has_more_data = CIFSD_DCERPC_ERROR_MORE_DATA;
			break;
		}
	}

	/*
	 * ARRAY representation [per dimension]
	 *    max_count
	 *    offset
	 *    actual_count
	 *    element representation [1..N]
	 *    actual elements [1..N]
	 */
	dcerpc_write_int32(dce, max_entry_nr);
	dcerpc_write_int32(dce, 1);
	dcerpc_write_int32(dce, max_entry_nr);

	if (max_entry_nr == 0) {
		pr_err("DCERPC: can't fit any data, buffer is too small\n");
		return CIFSD_DCERPC_ERROR_INVALID_LEVEL;
	}

	for (i = 0; i < max_entry_nr; i++) {
		gpointer entry;

		entry = g_array_index(pipe->entries,  gpointer, i);
		ret = dce->entry_rep(dce, entry);

		if (ret != 0)
			return CIFSD_DCERPC_ERROR_INVALID_LEVEL;
	}

	for (i = 0; i < max_entry_nr; i++) {
		gpointer entry;

		entry = g_array_index(pipe->entries,  gpointer, i);
		ret = dce->entry_data(dce, entry);

		if (ret != 0)
			return CIFSD_DCERPC_ERROR_INVALID_LEVEL;
	}

	if (pipe->entry_processed) {
		for (i = 0; i < max_entry_nr; i++)
			pipe->entry_processed(pipe, 0);
	}
	return has_more_data;
}

struct cifsd_rpc_pipe *cifsd_rpc_pipe_alloc(void)
{
	struct cifsd_rpc_pipe *pipe = malloc(sizeof(struct cifsd_rpc_pipe));

	if (!pipe)
		return NULL;

	memset(pipe, 0x00, sizeof(struct cifsd_rpc_pipe));
	pipe->entries = g_array_new(0, 0, sizeof(void *));
	if (!pipe->entries) {
		free(pipe);
		return NULL;
	}

	return pipe;
}

void cifsd_rpc_pipe_free(struct cifsd_rpc_pipe *pipe)
{
	int i;

	if (pipe->entry_processed) {
		while (pipe->num_entries)
			pipe->entry_processed(pipe, 0);
	}
	g_array_free(pipe->entries, 0);
	free(pipe);
}

void cifsd_dcerpc_free(struct cifsd_dcerpc *dce)
{
	free(dce->payload);
	free(dce);
}

struct cifsd_dcerpc *cifsd_dcerpc_allocate(unsigned int flags, int sz)
{
	struct cifsd_dcerpc *dce;

	dce = malloc(sizeof(struct cifsd_dcerpc));
	if (!dce)
		return NULL;

	memset(dce, 0x00, sizeof(struct cifsd_dcerpc));
	dce->payload = malloc(sz);
	if (!dce->payload) {
		free(dce);
		return NULL;
	}

	memset(dce->payload, sz, 0x00);
	dce->payload_sz = sz;
	dce->flags = flags;

	if (sz == CIFSD_DCERPC_MAX_PREFERRED_SIZE)
		dce->flags &= ~CIFSD_DCERPC_FIXED_PAYLOAD_SZ;

	return dce;
}

static int __share_entry_size_ctr0(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;

	return strlen(share->name) * 2 + 4 * sizeof(__u32);
}

static int __share_entry_size_ctr1(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;
	int sz;

	sz = strlen(share->name) * 2 + strlen(share->comment) * 2;
	sz += 9 * sizeof(__u32);
	return sz;
}

static int __share_entry_rep_ctr0(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;

	return dcerpc_write_int32(dce, 1);
}

static int __share_entry_rep_ctr1(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;
	int ret;

	ret = dcerpc_write_int32(dce, 1);
	ret |= dcerpc_write_int32(dce, 0); // FIXME
	ret |= dcerpc_write_int32(dce, 1);
	return ret;
}

static int __share_entry_data_ctr0(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;

	return dcerpc_write_vstring(dce, share->name);
}

static int __share_entry_data_ctr1(struct cifsd_dcerpc *dce, gpointer entry)
{
	struct cifsd_share *share = entry;
	int ret;

	ret = dcerpc_write_vstring(dce, share->name);
	ret |= dcerpc_write_vstring(dce, share->comment);
	return ret;
}

static int __share_entry_processed(struct cifsd_rpc_pipe *pipe, int i)
{
	struct cifsd_share *share;

	share = g_array_index(pipe->entries,  gpointer, i);
	pipe->entries = g_array_remove_index(pipe->entries, i);
	pipe->num_entries--;
	put_cifsd_share(share);
}

static void __enum_all_shares(gpointer key, gpointer value, gpointer user_data)
{
	struct cifsd_rpc_pipe *pipe = (struct cifsd_rpc_pipe *)user_data;
	struct cifsd_share *share = (struct cifsd_share *)value;

	if (!get_cifsd_share(share))
		return;

	pipe->entries = g_array_append_val(pipe->entries, share);
	pipe->num_entries++;
}

struct cifsd_rpc_pipe *cifsd_rpc_share_enum_all(void)
{
	struct cifsd_rpc_pipe *pipe;

	pipe = cifsd_rpc_pipe_alloc();
	if (!pipe)
		return NULL;

	for_each_cifsd_share(__enum_all_shares, pipe);
	pipe->entry_processed = __share_entry_processed;
	return pipe;
}

struct cifsd_dcerpc *
cifsd_rpc_DCE_share_enum_all(struct cifsd_rpc_pipe *pipe,
			     int level,
			     unsigned int flags,
			     int max_preferred_size)
{
	struct cifsd_dcerpc *dce;
	int ret;

	dce = cifsd_dcerpc_allocate(flags, max_preferred_size);
	if (!dce)
		return NULL;

	if (level == 0) {
		dce->entry_size = __share_entry_size_ctr0;
		dce->entry_rep = __share_entry_rep_ctr0;
		dce->entry_data = __share_entry_data_ctr0;
	} else if (level == 1) {
		dce->entry_size = __share_entry_size_ctr1;
		dce->entry_rep = __share_entry_rep_ctr1;
		dce->entry_data = __share_entry_data_ctr1;
	} else {
		ret = CIFSD_DCERPC_ERROR_INVALID_LEVEL;
		goto out;
	}

	dcerpc_write_union(dce, level);
	dcerpc_write_int32(dce, pipe->num_entries);

	ret = dcerpc_write_array_of_structs(dce, pipe);

out:
	/*
	 * [out] DWORD* TotalEntries
	 * [out, unique] DWORD* ResumeHandle
	 * [out] DWORD Return value/code
	 */
	dcerpc_write_int32(dce, pipe->num_entries);
	if (ret == CIFSD_DCERPC_ERROR_MORE_DATA)
		dcerpc_write_int32(dce, 0x01);
	else
		dcerpc_write_int32(dce, 0x00);
	dcerpc_write_int32(dce, ret);

	return dce;
}
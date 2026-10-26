/*
 * Minimal compatibility header for the ARM/MT6580 bring-up build.
 *
 * The public xnu-4570.71.2 tree uses <os/firehose_buffer_private.h> from the
 * SDK's libfirehose_kernel support library.  That library is not available
 * for armv7 in the iPhoneOS 17.5 SDK, so provide the small subset of the
 * interface that the kernel sources need and implement stubs in
 * libkern/os/log.c.
 */

#ifndef __FIREHOSE_BUFFER_PRIVATE_COMPAT_H
#define __FIREHOSE_BUFFER_PRIVATE_COMPAT_H

#include <stddef.h>
#include <firehose/firehose_types_private.h>
#include <firehose/tracepoint_private.h>

/* Size of the kernel persistence buffer, in 4 KB chunks. */
#define FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT 256

__BEGIN_DECLS

firehose_buffer_t __firehose_buffer_create(size_t *size);

firehose_tracepoint_t __firehose_buffer_tracepoint_reserve(uint64_t stamp,
    firehose_stream_t stream, uint16_t publen, uint16_t flags, void *ctx);

void __firehose_buffer_tracepoint_flush(firehose_tracepoint_t ft,
    firehose_tracepoint_id_u ftid);

void __firehose_merge_updates(firehose_push_reply_t reply);

__END_DECLS

#endif /* __FIREHOSE_BUFFER_PRIVATE_COMPAT_H */

// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2010, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <assert.h>
#include <openssl/md5.h>
#include <junkie/config.h>
#include <junkie/tools/log.h>
#include <junkie/tools/miscmacs.h>
#include <junkie/tools/mallocer.h>
#include <junkie/tools/timeval.h>
#include <junkie/cpp.h>
#include "digest_queue.h"

static char const Id[] = "$Id$";

#define NB_QUEUES (CPU_MAX < 256 ? CPU_MAX : 256)   /* Must not be greater that 256 since we use only one byte from digest to hash */

struct digest_queue {
    struct queue {
        struct digest_qcell {
            unsigned char digest[DIGEST_SIZE];
            struct timeval tv;
        } *digests;
        unsigned idx;       // Index of the lastly added frame (more recent)
        unsigned length;    // Actually all queues have the same length, except during resizing
        struct mutex mutex;
    } queues[NB_QUEUES];    // The queue is chosen according to digest hash, so that several distinct threads can perform lookups simultaneously.
};

MALLOCER_DEF(digest_queues);

static int digest_queue_ctor(struct digest_queue *digest, unsigned length)
{
    for (unsigned i = 0; i < NB_ELEMS(digest->queues); i++) {
        struct queue *const q = digest->queues + i;
        q->idx = 0;
        q->length = length;
        q->digests = MALLOC(digest_queues, q->length * sizeof *q->digests);
        if (! q->digests && length > 0) return -1;
        if (q->digests) memset(q->digests, 0, q->length * sizeof q->digests);
        mutex_ctor(&q->mutex, "digest queue");
    }

    return 0;
}

struct digest_queue *digest_queue_new(unsigned length)
{
    MALLOCER_INIT(digest_queues);
    struct digest_queue *digest = MALLOC(digest_queues, sizeof(*digest));
    if (! digest) return NULL;

    if (0 != digest_queue_ctor(digest, length)) {
        FREE(digest);
        return NULL;
    }

    return digest;
}

static void digest_queue_dtor(struct digest_queue *digest)
{
    for (unsigned i = 0; i < NB_ELEMS(digest->queues); i++) {
        if (digest->queues[i].digests) FREE(digest->queues[i].digests);
        mutex_dtor(&digest->queues[i].mutex);
    }
}

void digest_queue_del(struct digest_queue *digest)
{
    digest_queue_dtor(digest);
    FREE(digest);
}

int digest_queue_resize(struct digest_queue *digest, unsigned length)
{
    for (unsigned i = 0; i < NB_ELEMS(digest->queues); i++) {
        struct queue *const q = digest->queues + i;

        void *new = MALLOC(digest_queues, length * sizeof *q->digests);
        if (! new && length > 0) return -1;

        mutex_lock(&q->mutex);
        if (q->digests) FREE(q->digests);
        q->digests = new;
        q->idx = 0;
        q->length = length;
        if (q->digests) memset(q->digests, 0, q->length * sizeof q->digests);
        mutex_unlock(&q->mutex);
    }

    return 0;
}

enum digest_status digest_queue_find(struct digest_queue *digest, unsigned char buf[DIGEST_SIZE], struct timeval const *frame_tv, unsigned delay_usec)
{
    struct timeval min_tv = *frame_tv;
    timeval_sub_usec(&min_tv, delay_usec);
    unsigned const h = buf[0] % NB_ELEMS(digest->queues);
    struct queue *const q = digest->queues + h;

    mutex_lock(&q->mutex);

    unsigned i = q->length;
    if (q->length > 0) {
        do {
            unsigned j = (q->idx + i) % q->length;

            if (!timeval_is_set(&q->digests[j].tv) || timeval_cmp(&q->digests[j].tv, &min_tv) < 0) break;

            if (0 == memcmp(q->digests[j].digest, buf, DIGEST_SIZE)) {
                mutex_unlock(&q->mutex);
                return DIGEST_MATCH;
            }
        } while (--i);

        q->idx = (q->idx + 1) % q->length;
        memcpy(q->digests[q->idx].digest, buf, DIGEST_SIZE);
        q->digests[q->idx].tv = *frame_tv;
    }

    mutex_unlock(&q->mutex);
    return i != 0 ? DIGEST_NOMATCH : DIGEST_UNKNOWN;
}

#define BUFSIZE_TO_HASH 64

#define ETHER_DST_ADDR_OFFSET   0
#define ETHER_SRC_ADDR_OFFSET   ETHER_DST_ADDR_OFFSET + 6
#define ETHER_ETHERTYPE_OFFSET  ETHER_SRC_ADDR_OFFSET + 6
#define ETHER_HEADER_SIZE       ETHER_ETHERTYPE_OFFSET + 2

#define IPV4_VERSION_OFFSET     0
#define IPV4_TOS_OFFSET         IPV4_VERSION_OFFSET + 1
#define IPV4_LEN_OFFSET         IPV4_TOS_OFFSET + 1
#define IPV4_ID_OFFSET          IPV4_LEN_OFFSET + 2
#define IPV4_OFF_OFFSET         IPV4_ID_OFFSET + 2
#define IPV4_TTL_OFFSET         IPV4_OFF_OFFSET + 2
#define IPV4_PROTO_OFFSET       IPV4_TTL_OFFSET + 1
#define IPV4_CHECKSUM_OFFSET    IPV4_PROTO_OFFSET + 1
#define IPV4_SRC_HOST_OFFSET    IPV4_CHECKSUM_OFFSET + 2
#define IPV4_DST_HOST_OFFSET    IPV4_SRC_HOST_OFFSET + 4

void digest_frame(unsigned char buf[DIGEST_SIZE], size_t size, uint8_t *restrict packet)
{
    SLOG(LOG_DEBUG, "Compute the md5 digest of relevant data in the frame");

    size_t iphdr_offset = ETHER_HEADER_SIZE;
    size_t ethertype_offset = ETHER_ETHERTYPE_OFFSET;

    if (
        size > ethertype_offset + 1 &&
        0x00 == packet[ethertype_offset] &&
        0x00 == packet[ethertype_offset+1]
    ) {  // Skip Linux Cooked Capture special header
        iphdr_offset += 2;
        ethertype_offset += 2;
    }

    if (
        size >= ethertype_offset+1 &&
        0x81 == packet[ethertype_offset] &&
        0x00 == packet[ethertype_offset+1]
    ) { // Skip VLan tag
        iphdr_offset += 4;
    }

    if (size < iphdr_offset + IPV4_CHECKSUM_OFFSET) {
        SLOG(LOG_DEBUG, "Small frame (%zu bytes), compute the digest on the whole data", size);
        ASSERT_COMPILE(sizeof(uint8_t) == 1);
        (void) MD5((unsigned char *)packet, size, buf);
        return;
    }

    size_t len = MIN(BUFSIZE_TO_HASH, size - iphdr_offset);

    assert(size >= iphdr_offset + IPV4_TOS_OFFSET);
    assert(size >= iphdr_offset + IPV4_TTL_OFFSET);
    uint8_t tos = packet[iphdr_offset + IPV4_TOS_OFFSET];
    uint8_t ttl = packet[iphdr_offset + IPV4_TTL_OFFSET];
    uint16_t checksum = *(uint16_t *)&packet[iphdr_offset + IPV4_CHECKSUM_OFFSET];

    uint8_t ipversion = (packet[iphdr_offset + IPV4_VERSION_OFFSET] & 0xf0) >> 4;
    if (4 == ipversion) {
        // We must mask different fields which may be rewritten by
        // network equipment (routers, switches, etc), eg. TTL, Diffserv
        // or IP Header Checksum
        packet[iphdr_offset + IPV4_TOS_OFFSET] = 0x00;
        packet[iphdr_offset + IPV4_TTL_OFFSET] = 0x00;
        memset(packet + iphdr_offset + IPV4_CHECKSUM_OFFSET, 0, sizeof(uint16_t));
    }

    (void) MD5((unsigned char *)&packet[iphdr_offset], len, buf);

    if (4 == ipversion) {
        // Restore the dumped IP header fields
        packet[iphdr_offset + IPV4_TOS_OFFSET] = tos;
        packet[iphdr_offset + IPV4_TTL_OFFSET] = ttl;
        memcpy(packet + iphdr_offset + IPV4_CHECKSUM_OFFSET, &checksum, sizeof checksum);
    }
}



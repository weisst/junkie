// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#include <time.h>
#include <junkie/cpp.h>
#include <junkie/tools/ext.h>
#include <junkie/tools/objalloc.h>
#include <junkie/proto/pkt_wait_list.h>
#include <junkie/proto/cap.h>
#include <junkie/proto/eth.h>
#include <junkie/proto/ip.h>
#include <junkie/proto/tcp.h>
#include <junkie/proto/http.h>
#include <junkie/proto/streambuf.h>
#include <junkie/proto/cnxtrack.h>
#include "lib_test_junkie.h"

/*
 * These are 9 eth frames for a double HTTP get in the same cnx, one for /generate_204 and one for /complete/search/etc...
 * The two answers have code 204 and then 200.
 */

struct packet {
    size_t size;
    uint8_t payload[16*46 + 5];
} pkts[] = {
    {
        .size = 16*4 + 10,
        .payload = {    // Syn
            0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x08, 0x00, 0x45, 0x00,
            0x00, 0x3c, 0xdb, 0x72, 0x40, 0x00, 0x40, 0x06, 0xdf, 0xd0, 0xc0, 0xa8, 0x0a, 0x09, 0xd1, 0x55,
            0xe3, 0x71, 0xb4, 0x71, 0x00, 0x50, 0xdd, 0x9f, 0xe3, 0xe0, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x02,
            0xfa, 0xf0, 0xff, 0xa6, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4, 0x04, 0x02, 0x08, 0x0a, 0x1f, 0x17,
            0x38, 0x92, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x03, 0x0b
        },
    }, {
        .size = 16*4 + 10,
        .payload = {    // Syn+Ack
            0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0x08, 0x00, 0x45, 0x00,
            0x00, 0x3c, 0x7a, 0xb4, 0x00, 0x00, 0x36, 0x06, 0x8a, 0x8f, 0xd1, 0x55, 0xe3, 0x71, 0xc0, 0xa8,
            0x0a, 0x09, 0x00, 0x50, 0xb4, 0x71, 0x3b, 0x5c, 0xde, 0xde, 0xdd, 0x9f, 0xe3, 0xe1, 0xa0, 0x12,
            0x16, 0x28, 0x34, 0x87, 0x00, 0x00, 0x02, 0x04, 0x05, 0x96, 0x04, 0x02, 0x08, 0x0a, 0xcf, 0x4d,
            0xc6, 0x71, 0x1f, 0x17, 0x38, 0x92, 0x01, 0x03, 0x03, 0x06
        },
    }, {
        .size = 16*4 + 2,
        .payload = {    // Ack
            0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x08, 0x00, 0x45, 0x00,
            0x00, 0x34, 0xdb, 0x73, 0x40, 0x00, 0x40, 0x06, 0xdf, 0xd7, 0xc0, 0xa8, 0x0a, 0x09, 0xd1, 0x55,
            0xe3, 0x71, 0xb4, 0x71, 0x00, 0x50, 0xdd, 0x9f, 0xe3, 0xe1, 0x3b, 0x5c, 0xde, 0xdf, 0x80, 0x10,
            0x00, 0x20, 0x79, 0x39, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0x1f, 0x17, 0x38, 0x95, 0xcf, 0x4d,
            0xc6, 0x71
        },
    }, {
        .size = 16*45 + 3,
        .payload = {    // HTTP Get /generate_404
            0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x08, 0x00, 0x45, 0x00,
			0x02, 0xc5, 0xdb, 0x74, 0x40, 0x00, 0x40, 0x06, 0xdd, 0x45, 0xc0, 0xa8, 0x0a, 0x09, 0xd1, 0x55,
			0xe3, 0x71, 0xb4, 0x71, 0x00, 0x50, 0xdd, 0x9f, 0xe3, 0xe1, 0x3b, 0x5c, 0xde, 0xdf, 0x80, 0x18,
			0x00, 0x20, 0x97, 0x65, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0x1f, 0x17, 0x38, 0x95, 0xcf, 0x4d,
			0xc6, 0x71, 0x47, 0x45, 0x54, 0x20, 0x2f, 0x67, 0x65, 0x6e, 0x65, 0x72, 0x61, 0x74, 0x65, 0x5f,
			0x32, 0x30, 0x34, 0x20, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31, 0x2e, 0x31, 0x0d, 0x0a, 0x48, 0x6f,
			0x73, 0x74, 0x3a, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x73, 0x31, 0x2e, 0x67, 0x6f, 0x6f,
			0x67, 0x6c, 0x65, 0x2e, 0x66, 0x72, 0x0d, 0x0a, 0x55, 0x73, 0x65, 0x72, 0x2d, 0x41, 0x67, 0x65,
			0x6e, 0x74, 0x3a, 0x20, 0x4d, 0x6f, 0x7a, 0x69, 0x6c, 0x6c, 0x61, 0x2f, 0x35, 0x2e, 0x30, 0x20,
			0x28, 0x58, 0x31, 0x31, 0x3b, 0x20, 0x55, 0x3b, 0x20, 0x4c, 0x69, 0x6e, 0x75, 0x78, 0x20, 0x78,
			0x38, 0x36, 0x5f, 0x36, 0x34, 0x3b, 0x20, 0x65, 0x6e, 0x2d, 0x55, 0x53, 0x3b, 0x20, 0x72, 0x76,
			0x3a, 0x31, 0x2e, 0x39, 0x2e, 0x32, 0x2e, 0x31, 0x32, 0x29, 0x20, 0x47, 0x65, 0x63, 0x6b, 0x6f,
			0x2f, 0x32, 0x30, 0x31, 0x30, 0x31, 0x30, 0x32, 0x37, 0x20, 0x55, 0x62, 0x75, 0x6e, 0x74, 0x75,
			0x2f, 0x31, 0x30, 0x2e, 0x30, 0x34, 0x20, 0x28, 0x6c, 0x75, 0x63, 0x69, 0x64, 0x29, 0x20, 0x46,
			0x69, 0x72, 0x65, 0x66, 0x6f, 0x78, 0x2f, 0x33, 0x2e, 0x36, 0x2e, 0x31, 0x32, 0x0d, 0x0a, 0x41,
			0x63, 0x63, 0x65, 0x70, 0x74, 0x3a, 0x20, 0x69, 0x6d, 0x61, 0x67, 0x65, 0x2f, 0x70, 0x6e, 0x67,
			0x2c, 0x69, 0x6d, 0x61, 0x67, 0x65, 0x2f, 0x2a, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x38, 0x2c, 0x2a,
			0x2f, 0x2a, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x35, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65, 0x70, 0x74,
			0x2d, 0x4c, 0x61, 0x6e, 0x67, 0x75, 0x61, 0x67, 0x65, 0x3a, 0x20, 0x65, 0x6e, 0x2d, 0x75, 0x73,
			0x2c, 0x65, 0x6e, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x35, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65, 0x70,
			0x74, 0x2d, 0x45, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x67, 0x7a, 0x69, 0x70,
			0x2c, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65, 0x70, 0x74,
			0x2d, 0x43, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3a, 0x20, 0x49, 0x53, 0x4f, 0x2d, 0x38, 0x38,
			0x35, 0x39, 0x2d, 0x31, 0x2c, 0x75, 0x74, 0x66, 0x2d, 0x38, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x37,
			0x2c, 0x2a, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x37, 0x0d, 0x0a, 0x4b, 0x65, 0x65, 0x70, 0x2d, 0x41,
			0x6c, 0x69, 0x76, 0x65, 0x3a, 0x20, 0x31, 0x31, 0x35, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x6e, 0x65,
			0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x6b, 0x65, 0x65, 0x70, 0x2d, 0x61, 0x6c, 0x69, 0x76,
			0x65, 0x0d, 0x0a, 0x52, 0x65, 0x66, 0x65, 0x72, 0x65, 0x72, 0x3a, 0x20, 0x68, 0x74, 0x74, 0x70,
			0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c, 0x65, 0x2e, 0x66, 0x72,
			0x2f, 0x0d, 0x0a, 0x43, 0x6f, 0x6f, 0x6b, 0x69, 0x65, 0x3a, 0x20, 0x50, 0x52, 0x45, 0x46, 0x3d,
			0x49, 0x44, 0x3d, 0x64, 0x62, 0x30, 0x64, 0x30, 0x31, 0x65, 0x36, 0x32, 0x61, 0x61, 0x66, 0x38,
			0x34, 0x36, 0x37, 0x3a, 0x55, 0x3d, 0x34, 0x33, 0x65, 0x37, 0x62, 0x31, 0x63, 0x35, 0x63, 0x33,
			0x36, 0x64, 0x65, 0x36, 0x39, 0x39, 0x3a, 0x46, 0x46, 0x3d, 0x30, 0x3a, 0x4c, 0x44, 0x3d, 0x65,
			0x6e, 0x3a, 0x4e, 0x52, 0x3d, 0x33, 0x30, 0x3a, 0x54, 0x4d, 0x3d, 0x31, 0x32, 0x36, 0x38, 0x36,
			0x34, 0x36, 0x32, 0x33, 0x32, 0x3a, 0x4c, 0x4d, 0x3d, 0x31, 0x32, 0x39, 0x34, 0x31, 0x35, 0x31,
			0x31, 0x36, 0x37, 0x3a, 0x53, 0x3d, 0x74, 0x48, 0x49, 0x33, 0x5f, 0x54, 0x38, 0x44, 0x76, 0x72,
			0x34, 0x6e, 0x78, 0x50, 0x79, 0x59, 0x3b, 0x20, 0x4e, 0x49, 0x44, 0x3d, 0x34, 0x32, 0x3d, 0x6a,
			0x51, 0x4c, 0x6b, 0x59, 0x62, 0x43, 0x59, 0x43, 0x6f, 0x32, 0x4a, 0x78, 0x47, 0x42, 0x59, 0x5f,
			0x62, 0x47, 0x4a, 0x6f, 0x6f, 0x6d, 0x4e, 0x50, 0x48, 0x79, 0x5a, 0x38, 0x42, 0x4a, 0x59, 0x4d,
			0x45, 0x36, 0x35, 0x66, 0x4e, 0x32, 0x32, 0x6a, 0x51, 0x73, 0x5f, 0x55, 0x5f, 0x4a, 0x37, 0x35,
			0x69, 0x32, 0x69, 0x6b, 0x72, 0x7a, 0x64, 0x2d, 0x6b, 0x6b, 0x2d, 0x6b, 0x76, 0x4f, 0x57, 0x52,
			0x79, 0x68, 0x53, 0x58, 0x43, 0x55, 0x37, 0x7a, 0x34, 0x57, 0x33, 0x63, 0x55, 0x6a, 0x32, 0x59,
			0x6d, 0x77, 0x6f, 0x67, 0x31, 0x5a, 0x67, 0x76, 0x78, 0x55, 0x39, 0x36, 0x2d, 0x41, 0x63, 0x72,
			0x67, 0x50, 0x4b, 0x41, 0x75, 0x2d, 0x4e, 0x35, 0x76, 0x6f, 0x6c, 0x31, 0x5f, 0x4c, 0x6e, 0x4d,
			0x56, 0x2d, 0x44, 0x72, 0x6e, 0x54, 0x59, 0x42, 0x6b, 0x32, 0x58, 0x64, 0x43, 0x63, 0x62, 0x0d,
			0x0a, 0x0d, 0x0a
        },
    }, {
        .size = 16*4 + 2,
        .payload = {    // Ack
            0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0x08, 0x00, 0x45, 0x00,
            0x00, 0x34, 0x7a, 0xb5, 0x00, 0x00, 0x36, 0x06, 0x8a, 0x96, 0xd1, 0x55, 0xe3, 0x71, 0xc0, 0xa8,
            0x0a, 0x09, 0x00, 0x50, 0xb4, 0x71, 0x3b, 0x5c, 0xde, 0xdf, 0xdd, 0x9f, 0xe6, 0x72, 0x80, 0x10,
            0x00, 0x6e, 0x76, 0x36, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0xcf, 0x4d, 0xc6, 0x95, 0x1f, 0x17,
            0x38, 0x95
        },
    }, {
        .size = 16*11 + 15,
        .payload = {    // Response with a single header (complete) with content-length=0
            0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0x08, 0x00, 0x45, 0x00,
            0x00, 0xb1, 0x7a, 0xb6, 0x00, 0x00, 0x36, 0x06, 0x8a, 0x18, 0xd1, 0x55, 0xe3, 0x71, 0xc0, 0xa8,
            0x0a, 0x09, 0x00, 0x50, 0xb4, 0x71, 0x3b, 0x5c, 0xde, 0xdf, 0xdd, 0x9f, 0xe6, 0x72, 0x80, 0x18,
            0x00, 0x6e, 0x1e, 0x27, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0xcf, 0x4d, 0xc6, 0x95, 0x1f, 0x17,
            0x38, 0x95, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31, 0x2e, 0x31, 0x20, 0x32, 0x30, 0x34, 0x20, 0x4e,
            0x6f, 0x20, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65,
            0x6e, 0x74, 0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a, 0x20, 0x30, 0x0d, 0x0a, 0x43, 0x6f,
            0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54, 0x79, 0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78, 0x74,
            0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x0d, 0x0a, 0x44, 0x61, 0x74, 0x65, 0x3a, 0x20, 0x54, 0x75, 0x65,
            0x2c, 0x20, 0x30, 0x34, 0x20, 0x4a, 0x61, 0x6e, 0x20, 0x32, 0x30, 0x31, 0x31, 0x20, 0x31, 0x37,
            0x3a, 0x31, 0x37, 0x3a, 0x33, 0x33, 0x20, 0x47, 0x4d, 0x54, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
            0x65, 0x72, 0x3a, 0x20, 0x47, 0x46, 0x45, 0x2f, 0x32, 0x2e, 0x30, 0x0d, 0x0a, 0x0d, 0x0a
        },
    }, {
        .size = 16*4 + 2,
        .payload = {    // Ack
            0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x08, 0x00, 0x45, 0x00,
            0x00, 0x34, 0xdb, 0x75, 0x40, 0x00, 0x40, 0x06, 0xdf, 0xd5, 0xc0, 0xa8, 0x0a, 0x09, 0xd1, 0x55,
            0xe3, 0x71, 0xb4, 0x71, 0x00, 0x50, 0xdd, 0x9f, 0xe6, 0x72, 0x3b, 0x5c, 0xdf, 0x5c, 0x80, 0x10,
            0x00, 0x20, 0x76, 0x03, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0x1f, 0x17, 0x38, 0x99, 0xcf, 0x4d,
            0xc6, 0x95
        },
    }, {
        .size = 16*46 + 5,
        .payload = {    // Get /complete/search/etc
            0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x08, 0x00, 0x45, 0x00,
            0x02, 0xd7, 0xdb, 0x76, 0x40, 0x00, 0x40, 0x06, 0xdd, 0x31, 0xc0, 0xa8, 0x0a, 0x09, 0xd1, 0x55,
            0xe3, 0x71, 0xb4, 0x71, 0x00, 0x50, 0xdd, 0x9f, 0xe6, 0x72, 0x3b, 0x5c, 0xdf, 0x5c, 0x80, 0x18,
            0x00, 0x20, 0x9d, 0x2b, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0x1f, 0x17, 0x3b, 0xd9, 0xcf, 0x4d,
            0xc6, 0x95, 0x47, 0x45, 0x54, 0x20, 0x2f, 0x63, 0x6f, 0x6d, 0x70, 0x6c, 0x65, 0x74, 0x65, 0x2f,
            0x73, 0x65, 0x61, 0x72, 0x63, 0x68, 0x3f, 0x68, 0x6c, 0x3d, 0x65, 0x6e, 0x26, 0x63, 0x6c, 0x69,
            0x65, 0x6e, 0x74, 0x3d, 0x68, 0x70, 0x26, 0x65, 0x78, 0x70, 0x49, 0x64, 0x73, 0x3d, 0x31, 0x37,
            0x32, 0x35, 0x39, 0x2c, 0x31, 0x38, 0x31, 0x36, 0x37, 0x26, 0x71, 0x3d, 0x74, 0x6f, 0x26, 0x63,
            0x70, 0x3d, 0x32, 0x20, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31, 0x2e, 0x31, 0x0d, 0x0a, 0x48, 0x6f,
            0x73, 0x74, 0x3a, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x73, 0x31, 0x2e, 0x67, 0x6f, 0x6f,
            0x67, 0x6c, 0x65, 0x2e, 0x66, 0x72, 0x0d, 0x0a, 0x55, 0x73, 0x65, 0x72, 0x2d, 0x41, 0x67, 0x65,
            0x6e, 0x74, 0x3a, 0x20, 0x4d, 0x6f, 0x7a, 0x69, 0x6c, 0x6c, 0x61, 0x2f, 0x35, 0x2e, 0x30, 0x20,
            0x28, 0x58, 0x31, 0x31, 0x3b, 0x20, 0x55, 0x3b, 0x20, 0x4c, 0x69, 0x6e, 0x75, 0x78, 0x20, 0x78,
            0x38, 0x36, 0x5f, 0x36, 0x34, 0x3b, 0x20, 0x65, 0x6e, 0x2d, 0x55, 0x53, 0x3b, 0x20, 0x72, 0x76,
            0x3a, 0x31, 0x2e, 0x39, 0x2e, 0x32, 0x2e, 0x31, 0x32, 0x29, 0x20, 0x47, 0x65, 0x63, 0x6b, 0x6f,
            0x2f, 0x32, 0x30, 0x31, 0x30, 0x31, 0x30, 0x32, 0x37, 0x20, 0x55, 0x62, 0x75, 0x6e, 0x74, 0x75,
            0x2f, 0x31, 0x30, 0x2e, 0x30, 0x34, 0x20, 0x28, 0x6c, 0x75, 0x63, 0x69, 0x64, 0x29, 0x20, 0x46,
            0x69, 0x72, 0x65, 0x66, 0x6f, 0x78, 0x2f, 0x33, 0x2e, 0x36, 0x2e, 0x31, 0x32, 0x0d, 0x0a, 0x41,
            0x63, 0x63, 0x65, 0x70, 0x74, 0x3a, 0x20, 0x2a, 0x2f, 0x2a, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65,
            0x70, 0x74, 0x2d, 0x4c, 0x61, 0x6e, 0x67, 0x75, 0x61, 0x67, 0x65, 0x3a, 0x20, 0x65, 0x6e, 0x2d,
            0x75, 0x73, 0x2c, 0x65, 0x6e, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x35, 0x0d, 0x0a, 0x41, 0x63, 0x63,
            0x65, 0x70, 0x74, 0x2d, 0x45, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x67, 0x7a,
            0x69, 0x70, 0x2c, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65,
            0x70, 0x74, 0x2d, 0x43, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3a, 0x20, 0x49, 0x53, 0x4f, 0x2d,
            0x38, 0x38, 0x35, 0x39, 0x2d, 0x31, 0x2c, 0x75, 0x74, 0x66, 0x2d, 0x38, 0x3b, 0x71, 0x3d, 0x30,
            0x2e, 0x37, 0x2c, 0x2a, 0x3b, 0x71, 0x3d, 0x30, 0x2e, 0x37, 0x0d, 0x0a, 0x4b, 0x65, 0x65, 0x70,
            0x2d, 0x41, 0x6c, 0x69, 0x76, 0x65, 0x3a, 0x20, 0x31, 0x31, 0x35, 0x0d, 0x0a, 0x43, 0x6f, 0x6e,
            0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x6b, 0x65, 0x65, 0x70, 0x2d, 0x61, 0x6c,
            0x69, 0x76, 0x65, 0x0d, 0x0a, 0x52, 0x65, 0x66, 0x65, 0x72, 0x65, 0x72, 0x3a, 0x20, 0x68, 0x74,
            0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c, 0x65, 0x2e,
            0x66, 0x72, 0x2f, 0x0d, 0x0a, 0x43, 0x6f, 0x6f, 0x6b, 0x69, 0x65, 0x3a, 0x20, 0x50, 0x52, 0x45,
            0x46, 0x3d, 0x49, 0x44, 0x3d, 0x64, 0x62, 0x30, 0x64, 0x30, 0x31, 0x65, 0x36, 0x32, 0x61, 0x61,
            0x66, 0x38, 0x34, 0x36, 0x37, 0x3a, 0x55, 0x3d, 0x34, 0x33, 0x65, 0x37, 0x62, 0x31, 0x63, 0x35,
            0x63, 0x33, 0x36, 0x64, 0x65, 0x36, 0x39, 0x39, 0x3a, 0x46, 0x46, 0x3d, 0x30, 0x3a, 0x4c, 0x44,
            0x3d, 0x65, 0x6e, 0x3a, 0x4e, 0x52, 0x3d, 0x33, 0x30, 0x3a, 0x54, 0x4d, 0x3d, 0x31, 0x32, 0x36,
            0x38, 0x36, 0x34, 0x36, 0x32, 0x33, 0x32, 0x3a, 0x4c, 0x4d, 0x3d, 0x31, 0x32, 0x39, 0x34, 0x31,
            0x35, 0x31, 0x31, 0x36, 0x37, 0x3a, 0x53, 0x3d, 0x74, 0x48, 0x49, 0x33, 0x5f, 0x54, 0x38, 0x44,
            0x76, 0x72, 0x34, 0x6e, 0x78, 0x50, 0x79, 0x59, 0x3b, 0x20, 0x4e, 0x49, 0x44, 0x3d, 0x34, 0x32,
            0x3d, 0x6a, 0x51, 0x4c, 0x6b, 0x59, 0x62, 0x43, 0x59, 0x43, 0x6f, 0x32, 0x4a, 0x78, 0x47, 0x42,
            0x59, 0x5f, 0x62, 0x47, 0x4a, 0x6f, 0x6f, 0x6d, 0x4e, 0x50, 0x48, 0x79, 0x5a, 0x38, 0x42, 0x4a,
            0x59, 0x4d, 0x45, 0x36, 0x35, 0x66, 0x4e, 0x32, 0x32, 0x6a, 0x51, 0x73, 0x5f, 0x55, 0x5f, 0x4a,
            0x37, 0x35, 0x69, 0x32, 0x69, 0x6b, 0x72, 0x7a, 0x64, 0x2d, 0x6b, 0x6b, 0x2d, 0x6b, 0x76, 0x4f,
            0x57, 0x52, 0x79, 0x68, 0x53, 0x58, 0x43, 0x55, 0x37, 0x7a, 0x34, 0x57, 0x33, 0x63, 0x55, 0x6a,
            0x32, 0x59, 0x6d, 0x77, 0x6f, 0x67, 0x31, 0x5a, 0x67, 0x76, 0x78, 0x55, 0x39, 0x36, 0x2d, 0x41,
            0x63, 0x72, 0x67, 0x50, 0x4b, 0x41, 0x75, 0x2d, 0x4e, 0x35, 0x76, 0x6f, 0x6c, 0x31, 0x5f, 0x4c,
            0x6e, 0x4d, 0x56, 0x2d, 0x44, 0x72, 0x6e, 0x54, 0x59, 0x42, 0x6b, 0x32, 0x58, 0x64, 0x43, 0x63,
            0x62, 0x0d, 0x0a, 0x0d, 0x0a
        },
    }, {
        .size = 16*32 + 3,
        .payload = {
       		0xa4, 0xba, 0xdb, 0xe6, 0x15, 0xfa, 0x00, 0x10, 0xf3, 0x09, 0x19, 0x48, 0x08, 0x00, 0x45, 0x00,
			0x01, 0xf5, 0x7a, 0xb7, 0x00, 0x00, 0x36, 0x06, 0x88, 0xd3, 0xd1, 0x55, 0xe3, 0x71, 0xc0, 0xa8,
			0x0a, 0x09, 0x00, 0x50, 0xb4, 0x71, 0x3b, 0x5c, 0xdf, 0x5c, 0xdd, 0x9f, 0xe9, 0x15, 0x80, 0x18,
			0x00, 0x83, 0xc5, 0x47, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a, 0xcf, 0x4d, 0xe7, 0x67, 0x1f, 0x17,
			0x3b, 0xd9, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31, 0x2e, 0x31, 0x20, 0x32, 0x30, 0x30, 0x20, 0x4f,
			0x4b, 0x0d, 0x0a, 0x44, 0x61, 0x74, 0x65, 0x3a, 0x20, 0x54, 0x75, 0x65, 0x2c, 0x20, 0x30, 0x34,
			0x20, 0x4a, 0x61, 0x6e, 0x20, 0x32, 0x30, 0x31, 0x31, 0x20, 0x31, 0x37, 0x3a, 0x31, 0x37, 0x3a,
			0x34, 0x32, 0x20, 0x47, 0x4d, 0x54, 0x0d, 0x0a, 0x45, 0x78, 0x70, 0x69, 0x72, 0x65, 0x73, 0x3a,
			0x20, 0x54, 0x75, 0x65, 0x2c, 0x20, 0x30, 0x34, 0x20, 0x4a, 0x61, 0x6e, 0x20, 0x32, 0x30, 0x31,
			0x31, 0x20, 0x31, 0x37, 0x3a, 0x31, 0x37, 0x3a, 0x34, 0x32, 0x20, 0x47, 0x4d, 0x54, 0x0d, 0x0a,
			0x43, 0x61, 0x63, 0x68, 0x65, 0x2d, 0x43, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x3a, 0x20, 0x70,
			0x72, 0x69, 0x76, 0x61, 0x74, 0x65, 0x2c, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x61, 0x67, 0x65, 0x3d,
			0x33, 0x36, 0x30, 0x30, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54, 0x79,
			0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78, 0x74, 0x2f, 0x6a, 0x61, 0x76, 0x61, 0x73, 0x63, 0x72,
			0x69, 0x70, 0x74, 0x3b, 0x20, 0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x55, 0x54, 0x46,
			0x2d, 0x38, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x45, 0x6e, 0x63, 0x6f,
			0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x67, 0x7a, 0x69, 0x70, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
			0x65, 0x72, 0x3a, 0x20, 0x67, 0x77, 0x73, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
			0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x37, 0x38, 0x0d, 0x0a, 0x58, 0x2d,
			0x58, 0x53, 0x53, 0x2d, 0x50, 0x72, 0x6f, 0x74, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
			0x31, 0x3b, 0x20, 0x6d, 0x6f, 0x64, 0x65, 0x3d, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x0d, 0x0a, 0x0d,
			0x0a, 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x6d, 0xd0, 0xdb, 0x0a, 0x83,
			0x30, 0x0c, 0x06, 0xe0, 0x57, 0x29, 0xb9, 0xda, 0xa0, 0xb8, 0x38, 0xdd, 0xe9, 0x7a, 0xec, 0x29,
			0xb4, 0x17, 0x75, 0x4a, 0x15, 0xb6, 0x46, 0xac, 0x22, 0x65, 0xec, 0xdd, 0x77, 0xbc, 0x19, 0xfb,
			0x21, 0x10, 0x08, 0x5f, 0xc2, 0xdf, 0xce, 0x9d, 0xaf, 0x65, 0x4e, 0x9c, 0x88, 0xbb, 0x34, 0x89,
			0x3d, 0x27, 0xed, 0xa2, 0xa0, 0x51, 0x48, 0x17, 0xaf, 0x56, 0x4e, 0xcc, 0xd9, 0xb1, 0x7a, 0xb7,
			0x53, 0x0c, 0x6a, 0x50, 0x53, 0xf8, 0xcc, 0xca, 0xd5, 0x77, 0x4a, 0x9a, 0x35, 0x31, 0x19, 0xfd,
			0xe7, 0xfb, 0xd0, 0x4a, 0x8f, 0x74, 0x8a, 0x74, 0x94, 0xd1, 0x22, 0xbc, 0x86, 0xa7, 0x55, 0xce,
			0x08, 0x67, 0x10, 0x5f, 0xad, 0x47, 0x38, 0x47, 0xb8, 0xb6, 0x51, 0x3d, 0x73, 0xcf, 0x68, 0x61,
			0x83, 0xa3, 0xb8, 0xc6, 0x0e, 0x88, 0x6f, 0x11, 0x0f, 0x6d, 0x57, 0xc1, 0x77, 0xee, 0x90, 0xf6,
			0x51, 0x9d, 0xa7, 0x61, 0xec, 0xe0, 0xa7, 0xef, 0x71, 0x9c, 0x94, 0x59, 0x05, 0xf1, 0x0e, 0xee,
			0x1c, 0xc8, 0x18, 0x4d, 0xf4, 0x53, 0xb7, 0xbb, 0x59, 0x3e, 0x00, 0x5f, 0x78, 0xaf, 0xdd, 0x07,
			0x02, 0x00, 0x00
        },
    }
};

/*
 * Check that whatever the order of the packets we find the HTTP get
 */

static struct timeval now;
static struct parser *eth_parser;
static unsigned nb_okfn_calls;
static unsigned nb_gets, nb_resps;
static struct proto_subscriber sub;

static void okfn(struct proto_subscriber unused_ *s, struct proto_info const *last, size_t unused_ cap_len, uint8_t const unused_ *packet, struct timeval const unused_ *now)
{
    nb_okfn_calls ++;
    SLOG(LOG_INFO, "Last info [%s]: %s", last->parser->proto->name, last->parser->proto->ops->info_2_str(last));

    if (last->parser->proto == proto_http) {
        struct http_proto_info const *info = DOWNCAST(last, info, http_proto_info);
        if ((info->set_values & HTTP_METHOD_SET) && info->method == HTTP_METHOD_GET) {
            assert(nb_gets < 2);
            if (nb_gets == 0) assert(info->strs[info->url+1] == 'g');  // check GETs order
            if (nb_gets == 1) assert(info->strs[info->url+1] == 'c');
            nb_gets ++;
        } else if (! (info->set_values & HTTP_METHOD_SET) && (info->set_values & HTTP_CODE_SET)) {
            assert(nb_resps < 2);
            if (nb_resps == 0) assert(info->code == 204);   // check resp order
            if (nb_resps == 1) assert(info->code == 200);
            nb_resps ++;
        }
    }
}

static void setup(void)
{
    timeval_set_now(&now);
    eth_parser = proto_eth->ops->parser_new(proto_eth);
    assert(eth_parser);
    nb_okfn_calls = nb_gets = nb_resps = 0;
    hook_subscriber_ctor(&pkt_hook, &sub, okfn);
}

static void teardown(void)
{
    hook_subscriber_dtor(&pkt_hook, &sub);
    parser_unref(&eth_parser);
}

// Send all fragments in order and check we have the various HTTP informations
static void simple_check(void)
{
    setup();

    for (unsigned p = 0 ; p < NB_ELEMS(pkts); p++) {
        assert(PROTO_OK == proto_parse(eth_parser, NULL, 0, pkts[p].payload, pkts[p].size, pkts[p].size, &now, pkts[p].size, pkts[p].payload));
    }
    assert(nb_okfn_calls == NB_ELEMS(pkts));
    assert(nb_gets == 2);
    assert(nb_resps == 2);

    teardown();
}

// Check that, providing we receive the SYNs first, HTTP parser is given the packet in the right order
// (ie first the GET, then the RESP)
static void random_check(void)
{
    setup();

    bool sent[NB_ELEMS(pkts)] = { };

    SLOG(LOG_DEBUG, "----------Start random check------------------");
    for (unsigned nb_sent = 0; nb_sent < NB_ELEMS(pkts); nb_sent++) {
        unsigned p = nb_sent < 2 ? nb_sent : random() % NB_ELEMS(pkts); // send the first syn first, then random
        while (sent[p]) p = (p+1) % NB_ELEMS(pkts);
        sent[p] = true;
        SLOG(LOG_DEBUG, "Sending Packet %u", p);
        assert(PROTO_OK == proto_parse(eth_parser, NULL, 0, pkts[p].payload, pkts[p].size, pkts[p].size, &now, pkts[p].size, pkts[p].payload));
    }
    assert(nb_okfn_calls == NB_ELEMS(pkts));
    assert(nb_gets == 2);
    assert(nb_resps == 2);

    teardown();
}

int main(void)
{
    log_init();
    ext_init();
    objalloc_init();
    cnxtrack_init();
    pkt_wait_list_init();
    ref_init();
    streambuf_init();
    proto_init();
    cap_init();
    eth_init();
    ip_init();
    ip6_init();
    tcp_init();
    http_init();
    log_set_level(LOG_DEBUG, NULL);
    log_set_level(LOG_ERR, "mutex");
    log_set_file("tcp_reorder_check.log");

    simple_check();
    for (unsigned nb_rand = 0; nb_rand < 100; nb_rand++) random_check();

    doomer_stop();
    http_fini();
    tcp_fini();
    ip6_fini();
    ip_fini();
    eth_fini();
    cap_fini();
    proto_fini();
    streambuf_fini();
    ref_fini();
    pkt_wait_list_fini();
    cnxtrack_fini();
    objalloc_fini();
    ext_fini();
    log_fini();
    return EXIT_SUCCESS;
}

